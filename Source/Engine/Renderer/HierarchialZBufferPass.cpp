#include "HierarchialZBufferPass.h"
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
#include "Engine/Scripting/Scripting.h"
#include "Engine/Input/Input.h"

#define HZB_FORMAT PixelFormat::R32_Float
#define HZB_BOUNDS_BIAS 20.0f // adds this many pixels to a query objects bounding box on the screen. Increase this to reduce pop-in, at the cost of more conservative occlusion.

/// <summary>
/// Custom task called after downloading HZB texture data to save it.
/// </summary>
class UploadHZBTask : public ThreadPoolTask
{
private:
    int _index = 0;
    HZBData* _info;

public:
    UploadHZBTask(HZBData* info, int i) : _info(info), _index(i) { }
    bool Run() override
    {
        _info->CompleteDownload(_index);
        return true;
    }
};

String HierarchialZBufferPass::ToString()const
{
    return TEXT("HierarchialZBufferPass");
}

bool HierarchialZBufferPass::Init()
{
    // Active only on MainRenderTask. To use on all render tasks, uncomment the corresponding line in Renderer.cpp and remove this.
    MainRenderTask::Instance->PreRender.Bind<HierarchialZBufferPass, &HierarchialZBufferPass::Render>(this);

    // Check platform support
    const auto device = GPUDevice::Instance;
    _supported = device->GetFeatureLevel() >= FeatureLevel::ES2;
    return false;
}

bool HierarchialZBufferPass::setupResources()
{
    if (!_supported)
        return true;

    // Load shader
    if (_shader == nullptr)
    {
        _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/HZB"));
        if (_shader == nullptr)
            return true;
#if COMPILE_WITH_DEV_ENV
        _shader.Get()->OnReloading.Bind<HierarchialZBufferPass, &HierarchialZBufferPass::OnShaderReloading>(this);
#endif
    }
    if (!_shader->IsLoaded())
        return true;

    const auto device = GPUDevice::Instance;
    const auto shader = _shader->GetShader();

    _cb = shader->GetCB(0);

    // Create pipeline stages
    _psHZB = device->CreatePipelineState();
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    {
        psDesc.PS = shader->GetPS("PS_HZB");
        if (_psHZB->Init(psDesc))
            return true;
    }
    _psDebug = device->CreatePipelineState();
    psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    {
        psDesc.PS = shader->GetPS("PS_DebugView");
        if (_psDebug->Init(psDesc))
            return true;
    }

    return false;
}

#if COMPILE_WITH_DEV_ENV

void HierarchialZBufferPass::OnShaderReloading(Asset* obj)
{
    SAFE_DELETE_GPU_RESOURCE(_psHZB);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    invalidateResources();
}

#endif

void HierarchialZBufferPass::Dispose()
{
    RendererPass::Dispose();

    // Release data
    SAFE_DELETE_GPU_RESOURCE(_psHZB);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);

    for (int i = 0; i < _info.Count(); i++)
    {
        auto info = _info[i];
        info->Dispose();
        Delete(info);
    }
    _info.Clear();
    _shader = nullptr;
}

HZBData* HierarchialZBufferPass::GetOrCreateInfo(RenderContext& renderContext)
{
    auto info = renderContext.Task->OcclusionInfo;
    if (info == nullptr)
    {
        // create a new HZBData to be associated with this SceneRenderTask
        info = New<HZBData>();
        info->Id = _info.Count();
        renderContext.Task->OcclusionInfo = info;
        _info.Add(info);
    }
    return info;
}

GPU_CB_STRUCT(HZBShaderData{
    Float4 ViewInfo;
    Float3 ViewPos;
    float ViewFar;
    Matrix InvViewMatrix;
    Matrix InvProjectionMatrix;
    Float2 Dimensions;
    int Level;
    int Offset;
    //float Dummy0;
    }
);

void HierarchialZBufferPass::SetInputs(const RenderView& view, HZBShaderData& data, Float2 dimensions, int level, int offset)
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

void HierarchialZBufferPass::RenderDebug(RenderContext& renderContext, GPUContext* context)
{
    if (!Graphics::OcclusionCulling) return;
    // draws the HZB pyramid over the depth buffer

    // auto info = GetOrCreateInfo(renderContext);
    // get the first HZBInfo, the main render tasks, instead of the debug one.
    if (_info.Count() == 0)
        return;
    auto info = _info[0];

    if (info->CheckSkip())
    {
        return;
    }

    if (info->CurrentFrameIndex < 0)
        return;

    // Set constants buffer
    HZBShaderData data;
    SetInputs(renderContext.View, data, info->_depthTexture->Size(), 0, 0);
    context->UpdateCB(_cb, &data);
    context->BindCB(0, _cb);

    context->BindSR(0, info->_depthTexture);
    context->BindUA(1, info->_hzbTexture->View());
    context->SetState(_psDebug);

    context->DrawFullscreenTriangle();

    // Cleanup
    context->ClearState();
}

void HierarchialZBufferPass::Render(GPUContext* context, RenderContext& renderContext)
{
    if (!Graphics::OcclusionCulling) return;

    // Skip if not supported
    if (checkIfSkipPass())
        return;

    // Get and/or init
    auto info = GetOrCreateInfo(renderContext);
    if (info->CheckSkip())
    {
        return;
    }

    HZBFrame* renderFrame = &info->_frames[info->_nextRenderFrameIndex];
    info->_nextRenderFrameIndex = (info->_nextRenderFrameIndex + 1) % HZB_FRAME_COUNT;

    if (renderFrame->IsDownloading)
        return;

    // save view settings
    Viewport viewport = renderContext.Task->GetOutputViewport();

    renderFrame->Viewport = viewport;
    renderFrame->ViewPosition = renderContext.View.WorldPosition;
    renderFrame->VP = renderContext.View.ViewProjection();
    renderFrame->ViewDirection = renderContext.View.Direction;
    renderFrame->ViewDirectionPerpendicular = Float3::Cross(renderContext.View.Direction, Float3::Up).GetNormalized();
    if (renderFrame->ViewDirectionPerpendicular.LengthSquared() < 0.001f)
    { // looking up, choose different direction
        renderFrame->ViewDirectionPerpendicular = Float3::Cross(renderContext.View.Direction, Float3::Left).GetNormalized();
    }

    // Resize if screen resolution changed
    Float2 resolution = viewport.Size;
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2; // round to nearest even number
    sizeY += sizeY % 2; // round to nearest even number
    int32 depth = Math::Max(2, (int)Math::Log2(resolution.MaxValue()));
    if (resolution != info->_resolution)
    {
        if (info->_depthTexture->Resize(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT))
        {
            LOG(Error, "Failed to resize HZB depth");
        }

        if (info->_hzbTexture->Resize(sizeX, sizeY, PixelFormat::R32_Float))
        {
            LOG(Error, "Failed to resize HZB");
        }

        for (int i = 0; i < HZB_FRAME_COUNT; i++)
        {
            if (info->_frames[i].StagingTexture->Resize(sizeX, sizeY, PixelFormat::R32_Float))
            {
                LOG(Error, "Failed to resize HZB staging");
            }
        }
    }
    info->_resolution = resolution;


    // Draw depth
    PROFILE_GPU("HZB depth");
    StaticFlags oldMask = renderContext.View.StaticFlagsMask;
    StaticFlags oldCompare = renderContext.View.StaticFlagsCompare;
    renderContext.Task->View.StaticFlagsMask = StaticFlags::Occluder;
    renderContext.Task->View.StaticFlagsCompare = StaticFlags::Occluder;
    context->ClearDepth(info->_depthTexture->View());
    Renderer::DrawSceneDepth(context, renderContext.Task, info->_depthTexture, _emptyArray);
    context->ClearState();
    renderContext.Task->View.StaticFlagsMask = oldMask;
    renderContext.Task->View.StaticFlagsCompare = oldCompare;

    // Render hierarchy
    Float2 dimensions = Float2(sizeX, sizeY);
    int offset = 0;
    context->Clear(info->_hzbTexture->View(), Color::White);
    for (int i = 0; i < depth; i++)
    {
        dimensions *= 0.5f;
        context->SetViewport(dimensions.X, dimensions.Y);

        HZBShaderData data;
        SetInputs(renderContext.View, data, dimensions, i, offset);
        context->UpdateCB(_cb, &data);
        context->BindCB(0, _cb);
        context->BindSR(0, info->_depthTexture);
        context->BindUA(1, info->_hzbTexture->View());
        context->SetState(_psHZB);
        context->DrawFullscreenTriangle();

        context->ClearState();
        offset += dimensions.X;
    }

    // Reset to the original viewport
    context->SetViewport(renderContext.Task->GetOutputViewport());

    // Create async job to gather hzb data from the GPU
    context->CopyTexture(renderFrame->StagingTexture, 0, 0, 0, 0, info->_hzbTexture, 0);
    renderFrame->IsDownloading = true;
    Task* uploadTask = New<UploadHZBTask>(info, renderFrame->Index);
    Task* downloadTask = renderFrame->StagingTexture->DownloadDataAsync(renderFrame->TextureData);

    if (downloadTask == nullptr)
    {
        LOG(Fatal, "Failed to create async task to download HZB texture data from the GPU.");
    }
    downloadTask->ContinueWith(uploadTask);
    downloadTask->Start();
}


bool HZBData::Init()
{
    if (_isReady)
        return false;
    const auto device = GPUDevice::Instance;

    // Init depth texture
    _depthTexture = device->CreateTexture(TEXT("HZB.Depth"));
    Float2 resolution = Screen::GetSize();
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2; // round to nearest even number
    sizeY += sizeY % 2; // round to nearest even number
    if (_depthTexture->Init(GPUTextureDescription::New2D(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
        return true;

    // Init hzb
    _hzbTexture = device->CreateTexture(TEXT("HZB.Pyramid"));
    auto desc = GPUTextureDescription::New2D(sizeX, sizeY, HZB_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
    if (_hzbTexture->Init(desc))
        return true;

    // Init staging textures
    for (int i = 0; i < HZB_FRAME_COUNT; i++)
    {
        _frames[i].Index = i;
        _frames[i].StagingTexture = device->CreateTexture(TEXT("HZB.Staging"));
        desc = desc.ToStagingReadback();
        if (_frames[i].StagingTexture->Init(desc))
            return true;
    }
    _isReady = true;
    return false;
}

void HZBData::Dispose()
{
    _isReady = false;
    _isValid = false;

    // Release GPU data
    if (_depthTexture)
        _depthTexture->ReleaseGPU();
    if (_hzbTexture)
        _hzbTexture->ReleaseGPU();

    // Release data
    SAFE_DELETE_GPU_RESOURCE(_depthTexture);
    SAFE_DELETE_GPU_RESOURCE(_hzbTexture);
    _depthTexture = nullptr;
    _hzbTexture = nullptr;

    for (int i = 0; i < HZB_FRAME_COUNT; i++)
    {
        auto stagingTexture = _frames[i].StagingTexture;
        if (stagingTexture)
        {
            stagingTexture->ReleaseGPU();
            SAFE_DELETE_GPU_RESOURCE(stagingTexture);
            _frames[i].StagingTexture = nullptr;
        }
    }
}

bool HZBData::CheckSkip()
{
    if (_isValid == false)
    {
        return true;
    }
    if (_isReady == false)
    {
        if (Init())
        {
            Dispose();
            return true;
        }
    }

    if (_nextRenderFrameIndex == CurrentFrameIndex)
    { // already far ahead, skip to let downloads catch up
        return true;
    }
    return false;
}

void HZBData::CompleteDownload(int index)
{
    CurrentFrameIndex = index;
    _frames[index].IsDownloading = false;
}

bool HZBData::CheckOcclusion(const BoundingSphere& bounds)
{
    if (!Graphics::OcclusionCulling) return false;
    if (!_isReady) return false;
    if (CurrentFrameIndex < 0) return false;

    HZBFrame* activeFrame = &_frames[CurrentFrameIndex];
    // no data yet
    if (activeFrame->TextureData.GetArraySize() == 0)
        return false;

    auto data = activeFrame->TextureData.GetData(0, 0);
    if (data->Data.Length() == 0)
    {
        return false;
    }
    // get sphere center and radius in screen space
    Vector3 centerProj, radiusProj, closestProj;
    activeFrame->Viewport.Project(bounds.Center, activeFrame->VP, centerProj);
    activeFrame->Viewport.Project(bounds.Center + activeFrame->ViewDirectionPerpendicular * bounds.Radius, activeFrame->VP, radiusProj);
    activeFrame->Viewport.Project(bounds.Center - activeFrame->ViewDirection * bounds.Radius, activeFrame->VP, closestProj);

    // increase this to reduce pop in, at the expense of less occlusion
    float radiusLength = HZB_BOUNDS_BIAS + Float2::Distance(Float2(centerProj.X, centerProj.Y), Float2(radiusProj.X, radiusProj.Y));
    // all the halving is because the buffer is already 50% of the full screen, and level 0 is half of that. The other levels are stacked horizontally to the right of it
    centerProj *= 0.5f;
    radiusLength *= 0.5f;
    if (closestProj.Z > 1.0f || closestProj.Z < 0)
    { // early exit if object is too close or far.
        return false;
    }
    float targetDistance = closestProj.Z;
    int level = Math::Max(0.0f, Math::Log2(radiusLength * 2.0f) - 1.0f - 2);
    
    float offset = 0; // horizontal offset for finding the other levels
    float width = activeFrame->TextureData.Width * 0.5f;
    float height = activeFrame->TextureData.Height * 0.5f;
    centerProj *= 0.5f;
    radiusLength *= 0.5f;

    // set calculations to appropriate level, which are stacked horizontally on the top right
    for (int i = 0; i < level; i++)
    {
        offset += width;
        width *= 0.5f;
        height *= 0.5f;
        radiusLength *= 0.5f;
        centerProj *= 0.5f;
        if ((int)(width * 0.5f) == 1 || (int)(height * 0.5f) == 1)
        { // break early if next iteration will be too small
            level = i;
            break;
        }
    }

    int widthIndex = (int)width - 1;
    int heightIndex = (int)height - 1;
    int startX = Math::Clamp(offset + (int)(centerProj.X - radiusLength), offset, offset + widthIndex);
    int endX = Math::Clamp(offset + (int)(centerProj.X + radiusLength), offset, offset + widthIndex);
    int startY = Math::Clamp((int)(centerProj.Y - radiusLength), 0, heightIndex);
    int endY = Math::Clamp((int)(centerProj.Y + radiusLength), 0, heightIndex);
    if (startX - endX == 0)
    { // needs to be at least 1 wide
        if (startX == offset + widthIndex) startX--;
        else endX++;
    }
    if (startY - endY == 0)
    { // needs to be at least 1 tall
        if (startY == heightIndex) startY--;
        else endY++;
    }
    
    // check each pixel (roughly 2x2) for occlusion
    for (int x = startX; x < endX; x++)
    {
        for (int y = startY; y < endY; y++)
        {
            float value = data->Get<float>(x, y);
            if (targetDistance < value)
            { // the object is closer than this pixel, so don't occlude it
                return false;
            }
        }
    }

    // occlusion detected
    return true;
}


