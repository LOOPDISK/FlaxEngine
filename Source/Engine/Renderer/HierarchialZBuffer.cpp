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
#include <Engine/Input/Input.h>
#include "Engine/Debug/DebugDraw.h"
#include "Engine/Render2D/Render2D.h"

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
    bool _enabled = true;
    TimeSpan _lastUpdate(0);
    bool _isReady = false;
    AssetReference<Shader> _shaderAsset;
    GPUShader* _shader;
    GPUPipelineState* _psHZB = nullptr;
    GPUPipelineState* _psDebug = nullptr;
    GPUTexture* _depthTexture = nullptr;
    GPUTexture* _hzbTexture = nullptr;
    GPUTexture* _stagingTexture = nullptr;
    Float2 _lastResolution;
    uint64 _lastUpdatedFrame = 0;

    bool _needsUpdate = false;
    // data
    bool _usingA = true;
    struct ViewData
    {
        TextureData textureData;
        Viewport viewport;
        Matrix vp;
        Float3 pos;
        Float3 dir;
        Float3 perpDir;
    };
    ViewData _viewA;
    ViewData _viewB;
    Array<Rectangle> _debugArray;
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

    // Init staging texture
    _stagingTexture = GPUDevice::Instance->CreateTexture(TEXT("HZB.Staging"));
    desc.ToStagingReadback();
    if (_stagingTexture->Init(desc))
        return true;

    MainRenderTask::Instance->PreRender.Bind(TryRender);
    //   MainRenderTask::Instance->PostRender.Bind(DebugDraw);
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
    SAFE_DELETE_GPU_RESOURCE(_stagingTexture);

    _isReady = false;
}

void HZBService::Update()
{
   // ASSERT(_updateFrameNumber == 0);
    if (Input::GetKeyDown(KeyboardKeys::H))
    {
        _enabled = !_enabled;
        _isReady = !_isReady;
        if (_enabled)
        {
            LOG(Info, "HZB has been enabled.");
        }
        else
        {
            LOG(Info, "HZB has been disabled.");
        }
    }
    // Check if render job is done
    if (_isReady && _lastUpdatedFrame > 0 && _lastUpdatedFrame < Engine::FrameCount)
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
    if (actor && !_actors.Contains(actor))
    {
        _actors.Add(actor);
    }
}

void HZBRenderer::RemoveOccluder(Actor* actor)
{
    if (actor)
    {
        _actors.Remove(actor);
    }
}

void HZBRenderer::ClearOccluders()
{
    _actors.Clear();
}

bool HZBRenderer::CheckOcclusion(Actor* actor, const BoundingSphere& bounds)
{
    if (!_enabled) return false;

    ViewData* activeView;
    // swap data
    if (_lastUpdatedFrame == 0)
    { // a download is in progress, or has been seen
        if (_usingA)
            activeView = &_viewA;
        else
            activeView = &_viewB;
    }
    else
    { // the download finished but a frame hasn't passed yet, so don't swap yet
        if (_usingA)
            activeView = &_viewB;
        else
            activeView = &_viewA;
    }
    // no data yet
    if (activeView->textureData.GetArraySize() == 0)
        return false;
    auto data = activeView->textureData.GetData(0, 0);
    if (data->Data.Length() == 0)
    {
        return false;
    }
    // get sphere center and radius in screen space
    Vector3 centerProj, radiusProj, closestProj;
    activeView->viewport.Project(bounds.Center, activeView->vp, centerProj);
    activeView->viewport.Project(bounds.Center + activeView->perpDir * bounds.Radius, activeView->vp, radiusProj);
    Vector3 closestPoint = bounds.Center - activeView->dir * bounds.Radius; 

    activeView->viewport.Project(closestPoint, activeView->vp, closestProj);

    // increase this to reduce pop in, at the expense of less occlusion
    const float extraSize = 10.0f;
    float radiusLength = extraSize + Float2::Distance(Float2(centerProj.X, centerProj.Y), Float2(radiusProj.X, radiusProj.Y));
    // all the halving is because the buffer is already 50% of the full screen, and level 0 is half of that. The other levels are stacked horizontally to the right of it
    centerProj *= 0.5f;
    radiusLength *= 0.5f;
    if (closestProj.Z > 1.0f || closestProj.Z < 0)
    { // early exit
        return false;
    }
    int level = Math::Max(0.0f, Math::Log2(radiusLength * 2.0f) - 1.0f - 2);
 //   level = 1;
    float offset = 0; // horizontal offset for finding the other levels
    float width = activeView->textureData.Width * 0.5f;
    float height = activeView->textureData.Height * 0.5f;
    centerProj *= 0.5f;
    radiusLength *= 0.5f;
   // _debugArray.Add(Rectangle(0, 0, width * 2.0f, height * 2.0f));

    for (int i = 0; i < level; i++)
    {
        offset += width;
        width *= 0.5f;
        height *= 0.5f;
        radiusLength *= 0.5f;
        centerProj *= 0.5f;
        if ((int)(width * 0.5f) == 0 || (int)(height * 0.5f) == 0)
        { // break early if next iteration will be too small
            level = i;
            break;
        }
    }
 //   _debugArray.Add(Rectangle(offset + centerProj.X, centerProj.Y, 2, 2));

    float targetDistance = closestProj.Z;
    int startX = Math::Clamp(offset + (int)(centerProj.X - radiusLength), offset, offset + width);
    int endX = Math::Clamp(offset + (int)(centerProj.X + radiusLength), offset, offset + width);
    int startY = Math::Clamp((int)(centerProj.Y - radiusLength), 0, (int)height);
    int endY = Math::Clamp((int)(centerProj.Y + radiusLength), 0, (int)height);
    if (startX - endX == 0)
    { // needs to be at least 1 wide
        if (startX == offset + width) startX--;
        else endX++;
    }
    if (startY - endY == 0)
    { // needs to be at least 1 tall
        if (startY == height) startY--;
        else endY++;
    }
    //     _debugArray.Add(Rectangle(startX, startY, endX - startX, endY - startY));

  //  LOG(Info, "Actor: {0}; startX: {1}; endX: {2}; startY: {3}; endY: {4}; width: {5}; height: {6}; level: {7}; r: {8}; center: {9}; offset: {10}", actor->GetNamePath(), startX, endX, startY, endY, width, height, level, radiusLength, centerProj, offset);
    for (int x = startX; x < endX; x++)
    {
        for (int y = startY; y < endY; y++)
        {
            float value = data->Get<float>(x, y);
            if (value == 0)
            {
        //        value = 1;
            }
      //      LOG(Info, "Comparing x:{0},y:{1}: {2} to {3} | {4}", x, y, targetDistance, value, actor->GetNamePath());
            if (targetDistance < value)
            {
                return false;
            }
        }
    }
    return true;
}

void HZBRenderer::DebugDraw(GPUContext* context, RenderContext& renderContext)
{
    RenderDebug(renderContext, context);
    auto output = renderContext.Task->Output;
    Render2D::Begin(context, output);

    int count = _debugArray.Count();
    for (int i = 0; i < count; i++)
    {
        auto rect = _debugArray[i];
        rect.Location *= 2.0f;
        rect.Size *= 2.0f;
        Render2D::DrawRectangle(rect, Color::Red);
    }

    Render2D::End();
    _debugArray.Clear();
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
    // saveview settings
    ViewData* activeView;
    if (_usingA)
    {
        activeView = &_viewA;
    }
    else
    {
        activeView = &_viewB;
    }
    Viewport viewport = renderContext.Task->GetOutputViewport();

    activeView->viewport = viewport;
    activeView->pos = renderContext.View.WorldPosition;
    activeView->vp = renderContext.View.ViewProjection();
    activeView->dir = renderContext.View.Direction;
    activeView->perpDir = Float3::Cross(renderContext.View.Direction, Float3::Up).GetNormalized();
    if (activeView->perpDir.LengthSquared() < 0.001f)
    { // looking up, choose different direction
        activeView->perpDir = Float3::Cross(renderContext.View.Direction, Float3::Left).GetNormalized();
    }

    // Resize if screen resolution changed
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

        if (_stagingTexture->Resize(sizeX, sizeY, PixelFormat::R32_Float))
        {
            LOG(Error, "Failed to resize HZB staging");
        }
    }
    _lastResolution = resolution;

    //for (int i = _actors.Count() - 1; i >= 0; i--)
    //{
    //    if (!_actors[i] || _actors[i] == nullptr)
    //    {
    //        _actors.RemoveAt(i);
    //    }
    //}
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

    context->CopyTexture(_stagingTexture, 0, 0, 0, 0, _hzbTexture, 0);

    Task* uploadTask = New<UploadHZBTask>();
    Task* downloadTask = _stagingTexture->DownloadDataAsync(activeView->textureData);

    if (downloadTask == nullptr)
    {
        LOG(Fatal, "Failed to create async task to download HZB texture data from the GPU.");
    }
    downloadTask->ContinueWith(uploadTask);
    downloadTask->Start();
}
