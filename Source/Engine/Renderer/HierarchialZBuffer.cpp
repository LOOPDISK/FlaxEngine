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
class UploadHZBTask : public ThreadPoolTask
{
public:
    bool Run() override
    {
        HZBRenderer::CompleteDownload();
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
    Float2 _lastResolution;
    uint64 _lastUpdatedFrame = 0;

    bool _needsUpdate = false;
    // data
    bool _usingA = true;
    TextureData _dataA;
    TextureData _dataB;
    Viewport _viewA;
    Viewport _viewB;

    FORCE_INLINE bool isUpdateSynced()
    {
        return _isReady && _lastUpdatedFrame > 0 && _lastUpdatedFrame < Engine::FrameCount;
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

    if (GPUDevice::Instance->Limits.HasCompute == false)
    {
        LOG(Info, "Compute shaders are not supported. Cannot use HZB occlusion.");
        return true;
    }

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
    _depthTexture = GPUDevice::Instance->CreateTexture(TEXT("HZB.Depth"));
    Float2 resolution = Screen::GetSize();
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2; // round to nearest even number
    sizeY += sizeY % 2; // round to nearest even number
    if (_depthTexture->Init(GPUTextureDescription::New2D(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
        return true;
    

    // Init hzb
    _hzbTexture = GPUDevice::Instance->CreateTexture(TEXT("HZB.Pyramid"));
    auto desc = GPUTextureDescription::New2D(sizeX, sizeY, PixelFormat::R32_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
    if (_hzbTexture->Init(desc))
        return true;

    // Mark as ready
    _isReady = true;
    _needsUpdate = true;

    return false;
}

void HZBRenderer::Release()
{
    if (!_isReady)
        return;

    ASSERT(_lastUpdatedFrame == 0);

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
   // ASSERT(_updateFrameNumber == 0);

    // Check if render job is done
    if (isUpdateSynced())
    {
        _lastUpdatedFrame = 0;
    }
    else
    {
        // Init service
        if (HZBRenderer::Init())
        {
            LOG(Warning, "Cannot setup HZB Renderer!");
            Dispose();
            return;
        }
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

bool HZBRenderer::CheckOcclusion()
{
    bool checkA = false;
    if (_lastUpdatedFrame == 0)
    { // a download is in progress, or has been seen
        checkA = _usingA;
    }
    else
    { // the download finished but a frame hasn't passed yet
        checkA = !_usingA;
    }
    if (checkA)
    {
        // TODO check against A
    }
    else
    {
        // TODO check against B
    }
    return false;
}

void HZBRenderer::CompleteDownload()
{
    _usingA = !_usingA;
    _needsUpdate = true;
    _lastUpdatedFrame = Engine::FrameCount;
    
    //LOG(Info, "Completed HZB download on frame {0}. Data size: {1}", _lastUpdatedFrame, _dataA.GetArraySize());
}

void HZBRenderer::SetInputs(const RenderView& view, HZBData& data, Float2 dimensions, int level, int offset)
{
    data.Dimensions = dimensions;
    data.ViewInfo = view.ViewInfo;
    data.ViewPos = view.Position;
    data.ViewFar = view.Far;
    data.Level = level;
    data.Offset = offset;
    Matrix::Transpose(view.IV, data.InvViewMatrix);
    Matrix::Transpose(view.IP, data.InvProjectionMatrix);
}

void HZBRenderer::RenderDebug(RenderContext& renderContext, GPUContext* context)
{
    if (!_isReady)
        return;

    // Set constants buffer
    HZBData data;
    SetInputs(renderContext.View, data, _depthTexture->Size(), 0, 0);
    auto cb = _shader->GetCB(0);
    context->UpdateCB(cb, &data);
    context->BindCB(0, cb);

    context->BindSR(0, _depthTexture);
    context->BindUA(1, _hzbTexture->View());
    context->SetState(_psDebug);

    context->DrawFullscreenTriangle();

    // Cleanup
    context->ClearState();
}

void HZBRenderer::TryRender(GPUContext* context, RenderContext& renderContext)
{
    if (!_isReady)
        return;

    if (!_needsUpdate)
        return;

  // ASSERT(_updateFrameNumber == 0);

    // Resize if screen resolution changed
    Viewport viewport = renderContext.Task->GetOutputViewport();
    Float2 resolution = viewport.Size;
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2; // round to nearest even number
    sizeY += sizeY % 2; // round to nearest even number
    int32 depth = Math::Max(2, (int)Math::Log2(resolution.MaxValue()));
    if (resolution != _lastResolution)
    {
        if (_depthTexture->Resize(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT))
        {
            LOG(Error, "Failed to resize HZB depth");
        }

        if (_hzbTexture->Resize(sizeX, sizeY, PixelFormat::R32_Float))
        {
            LOG(Error, "Failed to resize HZB");
        }
    }
    _lastResolution = resolution;

    // Draw depth
    PROFILE_GPU("HZB depth");    
    context->ClearDepth(_depthTexture->View());
    Renderer::DrawSceneDepth(context, renderContext.Task, _depthTexture, _actors);
    context->ClearState();

    // Render hierarchy
    Float2 dimensions = Float2(sizeX, sizeY);
    int offset = 0;
    context->Clear(_hzbTexture->View(), Color::White);
    for (int i = 0; i < depth; i++)
    {
        dimensions *= 0.5f;
        context->SetViewport(dimensions.X, dimensions.Y);

        HZBData data;
        SetInputs(renderContext.View, data, dimensions, i, offset);
        auto cb = _shader->GetCB(0);
        context->UpdateCB(cb, &data);
        context->BindCB(0, cb);
        context->BindSR(0, _depthTexture);
        context->BindUA(1, _hzbTexture->View());
        context->SetState(_psHZB);
        context->DrawFullscreenTriangle();

        context->ClearState();
        offset += dimensions.X;
    }

    // Reset to the original viewport
    context->SetViewport(renderContext.Task->GetOutputViewport());
    // Create async job to gather hzb data from the GPU
    _lastUpdatedFrame = 0;
    _needsUpdate = false;
    Task* uploadTask = New<UploadHZBTask>();
    Task* downloadTask;
    if (_usingA)
    {
        downloadTask = _hzbTexture->DownloadDataAsync(_dataB);
        _viewB = viewport;
    }
    else
    {
        downloadTask = _hzbTexture->DownloadDataAsync(_dataA);
        _viewA = viewport;
    }
    if (downloadTask == nullptr)
    {
        LOG(Fatal, "Failed to create async task to download HZB texture data from the GPU.");
    }
    downloadTask->ContinueWith(uploadTask);
    downloadTask->Start();
}
