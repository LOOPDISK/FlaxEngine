#include "HierarchialZBuffer.h"
#include "Renderer.h"
#include "ReflectionsPass.h"
#include "Engine/Core/Config/GraphicsSettings.h"
#include "Engine/Threading/ThreadPoolTask.h"
#include "Engine/Content/Content.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Level/Actors/PointLight.h"
#include "Engine/Level/Actors/EnvironmentProbe.h"
#include "Engine/Level/Actors/SkyLight.h"
#include "Engine/Level/SceneQuery.h"
#include "Engine/Level/LargeWorlds.h"
#include "Engine/ContentExporters/AssetExporters.h"
#include "Engine/Serialization/FileWriteStream.h"
#include "Engine/Engine/Time.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/Screen.h"
#include "Engine/Renderer/RenderList.h"

/// <summary>
/// Custom task called after downloading HZB texture data to save it.
/// </summary>
class DownloadHZBTask : public ThreadPoolTask
{
private:
    GPUTexture* _texture;
    TextureData _data;

public:
    DownloadHZBTask(GPUTexture* target)
        : _texture(target)
    {
    }

    FORCE_INLINE TextureData& GetData()
    {
        return _data;
    }

    bool Run() override
    {
        return true;
    }
};

namespace HZBRendererImpl
{
    TimeSpan _lastUpdate(0);
    bool _isReady = false;
    AssetReference<Shader> _shaderAsset;
    GPUShader* _shader;
    GPUPipelineState* _psHZB = nullptr;
    GPUPipelineState* _psDebug = nullptr;
    GPUTexture* _depthTexture = nullptr;
    GPUTexture* _hzbTexture = nullptr;
    uint64 _updateFrameNumber = 0;
    Float2 _lastResolution;

    FORCE_INLINE bool isUpdateSynced()
    {
        return _updateFrameNumber > 0 && _updateFrameNumber < Engine::FrameCount;
    }
}
Array<Actor*> HZBRenderer::_actors;

using namespace HZBRendererImpl;

class HZBService : public EngineService
{
public:
    HZBService()
        : EngineService(TEXT("Hierarchial Z-Buffer"), 400)
    {
    }

    void Update() override;
    void Dispose() override;
};

HZBService HZBServiceInstance;

bool HZBRenderer::HasReadyResources()
{
    return _isReady && _shaderAsset->IsLoaded();
}

bool HZBRenderer::Init()
{
    if (_isReady)
        return false;

    // Load shader
    if (_shaderAsset == nullptr)
    {
        _shaderAsset = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/HZB"));
        if (_shaderAsset == nullptr)
            return true;
    }
    if (!_shaderAsset->IsLoaded())
        return false;
    _shader = _shaderAsset->GetShader();
    //if (_shader->GetCB(0)->GetSize() != sizeof(HZBData))
    //{
    //    REPORT_INVALID_SHADER_PASS_CB_SIZE(_shader, 0, HZBData);
    //    return true;
    //}

    // Create pipeline stages
    _psHZB = GPUDevice::Instance->CreatePipelineState();
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    {
        psDesc.PS = _shader->GetPS("PS_HZB");
        if (_psHZB->Init(psDesc))
            return true;
    }
    _psDebug = GPUDevice::Instance->CreatePipelineState();
    psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    {
        psDesc.PS = _shader->GetPS("PS_DebugView");
        if (_psDebug->Init(psDesc))
            return true;
    }

    // Init rendering pipeline
    _depthTexture = GPUDevice::Instance->CreateTexture(TEXT("Depth"));
    Float2 resolution = Screen::GetSize();
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2; // round to nearest even number
    sizeY += sizeY % 2; // round to nearest even number
    if (_depthTexture->Init(GPUTextureDescription::New2D(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
        return true;
    

    // Init render targets
    _hzbTexture = GPUDevice::Instance->CreateTexture(TEXT("HZB"));
    sizeX = sizeX * 0.5f;
    sizeY = sizeY * 0.5f;
    int32 depth = Math::Log2(resolution.MaxValue());
    if (_hzbTexture->Init(GPUTextureDescription::New3D(sizeX, sizeY, depth, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget, 0)))
        return true;

    // Mark as ready
    _isReady = true;

    return false;
}

void HZBRenderer::Release()
{
    if (!_isReady)
        return;
    ASSERT(_updateFrameNumber == 0);

    // Release GPU data
    if (_depthTexture)
        _depthTexture->ReleaseGPU();

    // Release data
    SAFE_DELETE_GPU_RESOURCE(_psHZB);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    _shaderAsset = nullptr;
    SAFE_DELETE_GPU_RESOURCE(_depthTexture);
    SAFE_DELETE_GPU_RESOURCE(_hzbTexture);

    _isReady = false;
}

void HZBService::Update()
{
    // Calculate time delta since last update
    auto timeNow = Time::Update.UnscaledTime;
    auto timeSinceUpdate = timeNow - _lastUpdate;
    if (timeSinceUpdate < 0)
    {
        _lastUpdate = timeNow;
        timeSinceUpdate = 0;
    }

    // Check if render job is done
    if (isUpdateSynced())
    {
        // Create async job to gather hzb data from the GPU
        GPUTexture* texture = _hzbTexture;
        ASSERT(texture);

        auto taskB = New<DownloadHZBTask>(texture);
        auto taskA = texture->DownloadDataAsync(taskB->GetData());
        if (taskA == nullptr)
        {
            LOG(Fatal, "Failed to create async task to download HZB texture data from the GPU.");
        }
        taskA->ContinueWith(taskB);
        taskA->Start();

        // Clear flag
        _updateFrameNumber = 0;
    }
    else
    {
        // Init service
        if (HZBRenderer::Init())
        {
            LOG(Fatal, "Cannot setup HZB Renderer!");
        }
        if (HZBRenderer::HasReadyResources() == false)
            return;

        _updateFrameNumber = 0;
    }
}

void HZBService::Dispose()
{
    HZBRenderer::Release();
}

void HZBRenderer::AddOccluder(Actor* actor)
{
    if (!_actors.Contains(actor))
    {
        _actors.Add(actor);
    }
}

void HZBRenderer::RemoveOccluder(Actor* actor)
{
    _actors.Remove(actor);
}

void HZBRenderer::ClearOccluders()
{
    _actors.Clear();
}

void HZBRenderer::SetInputs(const RenderView& view, HZBData& data)
{
    data.ViewInfo = view.ViewInfo;
    data.ViewPos = view.Position;
    data.ViewFar = view.Far;
    Matrix::Transpose(view.IV, data.InvViewMatrix);
    Matrix::Transpose(view.IP, data.InvProjectionMatrix);
}

void HZBRenderer::RenderDebug(RenderContext& renderContext, GPUContext* context)
{
    if (!_isReady)
        return;

    // Set constants buffer
    HZBData data;
    SetInputs(renderContext.View, data);
    auto cb = _shader->GetCB(0);
    context->UpdateCB(cb, &data);
    context->BindCB(0, cb);

    // Bind inputs
    context->BindSR(0, _depthTexture);

    // Combine frame
    context->SetState(_psDebug);
    context->DrawFullscreenTriangle();

    // Cleanup
    context->ResetSR();
}

void HZBRenderer::TryRender(GPUContext* context, RenderContext& renderContext)
{
    if (!_isReady)
        return;

    // Resize if screen resolution changed
    Float2 resolution = Screen::GetSize();
    if (resolution != _lastResolution)
    {
        int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
        int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
        sizeX += sizeX % 2; // round to nearest even number
        sizeY += sizeY % 2; // round to nearest even number
        const PixelFormat format = PixelFormat::D32_Float;
        bool resizeFailed = _depthTexture->Resize(sizeX, sizeY, format);
        if (resizeFailed)
            LOG(Error, "Failed to resize HZB depth");
    }
    _lastResolution = resolution;

    // Draw depth
    PROFILE_GPU("HZB depth");    
    context->ClearDepth(_depthTexture->View());
    Renderer::DrawSceneDepth(context, renderContext.Task, _depthTexture, _actors);
    
    // Cleanup
    context->ClearState();
    // Reset to the original viewport
    context->SetViewport(renderContext.Task->GetOutputViewport());

    // Mark as rendered
    _updateFrameNumber = Engine::FrameCount;
}
