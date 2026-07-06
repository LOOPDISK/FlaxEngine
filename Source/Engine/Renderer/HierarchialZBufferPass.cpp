#include "HierarchialZBufferPass.h"
#include "Renderer.h"
#include "Engine/Core/Config/GraphicsSettings.h"
#include "Engine/Content/Content.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/Screen.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Threading/Task.h"
#include "Engine/Core/Utilities.h"
#include "Engine/Profiler/Profiler.h"

#define HZB_FORMAT PixelFormat::R32_Float
#define HZB_CULL_GROUP_SIZE 64

String HierarchialZBufferPass::ToString() const
{
    return TEXT("HierarchialZBufferPass");
}

bool HierarchialZBufferPass::Init()
{
    MainRenderTask::Instance->PreRender.Bind<HierarchialZBufferPass, &HierarchialZBufferPass::Render>(this);
    _supported = GPUDevice::Instance->GetFeatureLevel() >= FeatureLevel::ES2;
    return false;
}

bool HierarchialZBufferPass::setupResources()
{
    if (!_supported)
        return true;

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

    if (_shaderCull == nullptr)
    {
        _shaderCull = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/HZBCull"));
        if (_shaderCull == nullptr)
            return true;
#if COMPILE_WITH_DEV_ENV
        _shaderCull.Get()->OnReloading.Bind<HierarchialZBufferPass, &HierarchialZBufferPass::OnShaderReloading>(this);
#endif
    }
    if (!_shaderCull->IsLoaded())
        return true;

    const auto device = GPUDevice::Instance;
    const auto shader = _shader->GetShader();
    const auto shaderCull = _shaderCull->GetShader();

    _cb = shader->GetCB(0);
    _cbDebug = shader->GetCB(1);
    _cbCull = shaderCull->GetCB(0);
    if (!_cb || !_cbDebug || !_cbCull)
        return true;

    _csCull = shaderCull->GetCS("CS_HZBCull");
    if (!_csCull)
        return true;

    _psHZB = device->CreatePipelineState();
    {
        GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = shader->GetPS("PS_HZB");
        if (_psHZB->Init(psDesc))
            return true;
    }
    _psDebug = device->CreatePipelineState();
    {
        GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
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
    _csCull = nullptr;
    invalidateResources();
}
#endif

void HierarchialZBufferPass::Dispose()
{
    RendererPass::Dispose();

    SAFE_DELETE_GPU_RESOURCE(_psHZB);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    _csCull = nullptr;

    {
        ScopeLock lock(_infoLock);
        for (int i = 0; i < _info.Count(); i++)
        {
            _info[i]->Dispose();
            Delete(_info[i]);
        }
        _info.Clear();
    }
    _shader = nullptr;
    _shaderCull = nullptr;
}

HZBData* HierarchialZBufferPass::GetOrCreateInfo(RenderContext& renderContext)
{
    auto info = renderContext.Task->OcclusionInfo;
    if (info == nullptr)
    {
        info = New<HZBData>();
        renderContext.Task->OcclusionInfo = info;
        ScopeLock lock(_infoLock);
        _info.Add(info);
    }
    return info;
}

GPU_CB_STRUCT(HZBShaderData {
    Float2 Dimensions;
    Float2 DepthDimensions;
    int Level;
    int Offset;
    int PrevOffset;
    float Dummy0;
});

GPU_CB_STRUCT(HZBDebugData {
    Float4 ViewInfo;
    Float3 ViewPos;
    float ViewFar;
    Matrix InvViewMatrix;
    Matrix InvProjectionMatrix;
    Float2 Size;
    Float2 Dummy1;
    Int4 TestRect;
});

GPU_CB_STRUCT(HZBCullCBData {
    Matrix ViewProjection;
    Float3 ViewForward;
    float ViewNear;
    Float2 PyramidBase;
    uint32 TotalBounds;
    uint32 MaxLevel;
    Float3 ViewOrigin;
    uint32 NumSlots;
    float BoundsInflate;
    uint32 Pad1;
    uint32 Pad2;
    uint32 Pad3;
});

void HierarchialZBufferPass::RenderDebug(RenderContext& renderContext, GPUContext* context)
{
    if (!Graphics::OcclusionCulling)
        return;
    if (_info.Count() == 0)
        return;
    auto info = _info[0];
    if (info->CheckSkip())
        return;
    if (!info->_hasValidPyramid)
        return;

    HZBDebugData data;
    data.Size = info->_depthTexture->Size();
    data.ViewInfo = renderContext.View.ViewInfo;
    data.ViewPos = renderContext.View.Position;
    data.ViewFar = renderContext.View.Far;
    data.TestRect = Int4::Zero;
    Matrix::Transpose(renderContext.View.IV, data.InvViewMatrix);
    Matrix::Transpose(renderContext.View.IP, data.InvProjectionMatrix);

    context->UpdateCB(_cbDebug, &data);
    context->BindCB(1, _cbDebug);
    context->BindSR(0, info->_depthTexture);
    context->BindUA(1, info->_hzbTexture->View());
    context->SetState(_psDebug);
    context->DrawFullscreenTriangle();
    context->ClearState();
}

void HierarchialZBufferPass::Render(GPUContext* context, RenderContext& renderContext)
{
    if (!Graphics::OcclusionCulling)
        return;
    if (checkIfSkipPass())
        return;

    auto info = GetOrCreateInfo(renderContext);
    if (info->CheckSkip())
        return;

    // Promote any completed occlusion readback into its slots now, before this frame's draw
    // collection samples them - saves the frame of latency the per-Dispatch promotion path costs.
    info->PromoteCompletedReadbacks();

    PROFILE_GPU_CPU("HZB Build");

    Viewport viewport = renderContext.Task->GetOutputViewport();
    Float2 resolution = viewport.Size;
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2;
    sizeY += sizeY % 2;
    int32 depth = Math::Max(2, (int)Math::Log2(resolution.MaxValue()));

    if (resolution != info->_resolution)
    {
        if (info->_depthTexture->Resize(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT))
            LOG(Error, "Failed to resize HZB depth");
        if (info->_hzbTexture->Resize(sizeX, sizeY, HZB_FORMAT))
            LOG(Error, "Failed to resize HZB");
        info->_hasValidPyramid = false;
    }
    info->_resolution = resolution;

    // Capture camera state for subsequent cull dispatches this frame. The pyramid is built from the
    // PRIOR frame's depth buffer (GBuffer clears the depth later this frame), so we must project cull
    // bounds through the PRIOR frame's view-projection + origin to match that depth - using the
    // current VP pops geometry as the camera rotates. PrevViewProjection/PrevOrigin were latched at
    // the end of last frame (SceneRenderTask::OnEnd) and correspond exactly to that depth.
    info->_vp = renderContext.View.PrevViewProjection;
    info->_viewForward = renderContext.View.Direction;
    info->_viewNear = renderContext.View.Near;
    info->_pyramidBase = Float2((float)(sizeX / 2), (float)(sizeY / 2));
    info->_maxLevel = (uint32)depth;
    info->_viewOrigin = renderContext.View.PrevOrigin;

    // Camera translation since the pyramid's capture frame. Bounds tested this frame are current-
    // frame positions but the depth is from last frame, so a bound the camera has advanced toward
    // can fall behind a stale occluder edge; dilating the test radius by the travel keeps it visible.
    const Vector3 curCamWorld = renderContext.View.Origin + (Vector3)renderContext.View.Position;
    info->_boundsInflate = info->_camWorldPosValid ? (float)Vector3::Distance(curCamWorld, info->_camWorldPos) : 0.0f;
    info->_camWorldPos = curCamWorld;
    info->_camWorldPosValid = true;

    Float2 depthDimensions = renderContext.Buffers->DepthBuffer->Size();
    int currWidth = sizeX / 2;
    int currHeight = sizeY / 2;
    int offset = 0;
    int prevOffset = 0;
    context->ClearUA(info->_hzbTexture, Float4::One);
    context->SetRenderTarget(info->_depthTexture->View(), (GPUTextureView*)nullptr);
    for (int i = 0; i < depth; i++)
    {
        context->SetViewport((float)currWidth, (float)currHeight);

        HZBShaderData data;
        data.Dimensions = Float2((float)currWidth, (float)currHeight);
        data.DepthDimensions = depthDimensions;
        data.Level = i;
        data.Offset = offset;
        data.PrevOffset = prevOffset;
        context->UpdateCB(_cb, &data);
        context->BindCB(0, _cb);
        context->BindSR(0, renderContext.Buffers->DepthBuffer);
        context->BindUA(1, info->_hzbTexture->View());
        context->SetState(_psHZB);
        context->DrawFullscreenTriangle();

        prevOffset = offset;
        offset += currWidth;
        currWidth = Math::Max(1, currWidth / 2);
        currHeight = Math::Max(1, currHeight / 2);
    }
    context->ClearState();
    context->SetViewport(renderContext.Task->GetOutputViewport());

    info->_hasValidPyramid = true;
}

// Round up to a power-of-two not smaller than v, with a floor of 64.
static FORCE_INLINE int32 RoundUpPOT(int32 v, int32 floor = 64)
{
    int32 cap = Math::Max(floor, 1);
    while (cap < v)
        cap *= 2;
    return cap;
}

void HierarchialZBufferPass::FlushPendingCulls(GPUContext* context)
{
    if (!_supported || !_csCull)
        return;

    for (HZBData* pyramid : _info)
    {
        if (!pyramid)
            continue;

        const int32 numEntries = pyramid->_pendingDispatches.Count();
        if (numEntries == 0 || !pyramid->IsReady())
        {
            pyramid->ClearPending();
            continue;
        }

        // One async readback per pyramid in flight. While the previous chain is still filling
        // _batchBytes, skip this frame entirely - slots keep their last verdict.
        if (pyramid->_batchReadback && !pyramid->_batchReadback->IsChainEnded())
        {
            pyramid->ClearPending();
            continue;
        }

        // Build slot table CPU-side. Walk pending entries, compute cumulativeThreadStart.
        // Scrubbed slots (Slot == nullptr) get count=0 so they consume zero threads in the CS.
        pyramid->_slotTableStaging.Resize(numEntries);
        HZBData::SlotTableEntry* slotRows = pyramid->_slotTableStaging.Get();
        uint32 cumThread = 0;
        for (int32 i = 0; i < numEntries; i++)
        {
            const HZBData::BatchEntry& e = pyramid->_pendingDispatches[i];
            const uint32 count = (e.Slot != nullptr) ? e.BoundsCount : 0u;
            HZBData::SlotTableEntry& row = slotRows[i];
            row.BoundsOffset = e.BoundsStagingOffset;
            row.Count = count;
            row.WriteOffsetWords = e.WriteOffsetWords;
            row.CumThread = cumThread;
            cumThread += count;
        }

        const uint32 totalBounds = cumThread;
        const int32 stagedBounds = pyramid->_boundsStaging.Count();
        if (totalBounds == 0 || stagedBounds == 0)
        {
            pyramid->ClearPending();
            continue;
        }

        PROFILE_GPU_CPU("HZB Cull Batch");

        // (Re)size the shared visibility buffer to fit all slots' bit ranges concatenated.
        const int32 needBytes = (int32)pyramid->_pendingTotalWords * 4;
        if (!pyramid->_batchVisBuffer || pyramid->_batchVisBuffer->GetSize() < needBytes)
        {
            SAFE_DELETE_GPU_RESOURCE(pyramid->_batchVisBuffer);
            pyramid->_batchVisBuffer = GPUDevice::Instance->CreateBuffer(TEXT("HZBCull.BatchVis"));
            if (pyramid->_batchVisBuffer->Init(GPUBufferDescription::Raw(needBytes, GPUBufferFlags::UnorderedAccess | GPUBufferFlags::ShaderResource)))
            {
                SAFE_DELETE_GPU_RESOURCE(pyramid->_batchVisBuffer);
                pyramid->ClearPending();
                continue;
            }
        }

        // (Re)size the shared bounds buffer - Dynamic, mapped via WRITE_DISCARD once per frame.
        if (!pyramid->_sharedBoundsBuffer || pyramid->_sharedBoundsCapacity < stagedBounds)
        {
            const int32 newCap = RoundUpPOT(stagedBounds, 256);
            SAFE_DELETE_GPU_RESOURCE(pyramid->_sharedBoundsBuffer);
            pyramid->_sharedBoundsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("HZBCull.SharedBounds"));
            if (pyramid->_sharedBoundsBuffer->Init(GPUBufferDescription::Buffer((uint32)newCap * (uint32)sizeof(Float4), GPUBufferFlags::Structured | GPUBufferFlags::ShaderResource, PixelFormat::Unknown, nullptr, (uint32)sizeof(Float4), GPUResourceUsage::Dynamic)))
            {
                SAFE_DELETE_GPU_RESOURCE(pyramid->_sharedBoundsBuffer);
                pyramid->_sharedBoundsCapacity = 0;
                pyramid->ClearPending();
                continue;
            }
            pyramid->_sharedBoundsCapacity = newCap;
        }

        // (Re)size the slot table buffer.
        if (!pyramid->_slotTableBuffer || pyramid->_slotTableCapacity < numEntries)
        {
            const int32 newCap = RoundUpPOT(numEntries, 64);
            SAFE_DELETE_GPU_RESOURCE(pyramid->_slotTableBuffer);
            pyramid->_slotTableBuffer = GPUDevice::Instance->CreateBuffer(TEXT("HZBCull.SlotTable"));
            if (pyramid->_slotTableBuffer->Init(GPUBufferDescription::Buffer((uint32)newCap * (uint32)sizeof(HZBData::SlotTableEntry), GPUBufferFlags::Structured | GPUBufferFlags::ShaderResource, PixelFormat::Unknown, nullptr, (uint32)sizeof(HZBData::SlotTableEntry), GPUResourceUsage::Dynamic)))
            {
                SAFE_DELETE_GPU_RESOURCE(pyramid->_slotTableBuffer);
                pyramid->_slotTableCapacity = 0;
                pyramid->ClearPending();
                continue;
            }
            pyramid->_slotTableCapacity = newCap;
        }

        // Upload bounds: one Map(WRITE_DISCARD) per pyramid.
        {
            void* mapped = pyramid->_sharedBoundsBuffer->Map(GPUResourceMapMode::Write);
            if (mapped)
            {
                Platform::MemoryCopy(mapped, pyramid->_boundsStaging.Get(), (uint64)stagedBounds * sizeof(Float4));
                pyramid->_sharedBoundsBuffer->Unmap();
            }
        }
        // Upload slot table.
        {
            void* mapped = pyramid->_slotTableBuffer->Map(GPUResourceMapMode::Write);
            if (mapped)
            {
                Platform::MemoryCopy(mapped, pyramid->_slotTableStaging.Get(), (uint64)numEntries * sizeof(HZBData::SlotTableEntry));
                pyramid->_slotTableBuffer->Unmap();
            }
        }

        // Single clear for the whole frame's verdict bits.
        const uint32 zeros[4] = { 0, 0, 0, 0 };
        context->ClearUA(pyramid->_batchVisBuffer, zeros);

        HZBCullCBData cb;
        Matrix::Transpose(pyramid->_vp, cb.ViewProjection);
        cb.ViewForward = pyramid->_viewForward;
        cb.ViewNear = pyramid->_viewNear;
        cb.PyramidBase = pyramid->_pyramidBase;
        cb.TotalBounds = totalBounds;
        cb.MaxLevel = pyramid->_maxLevel;
        cb.ViewOrigin = pyramid->_viewOrigin;
        cb.NumSlots = (uint32)numEntries;
        cb.BoundsInflate = pyramid->_boundsInflate;
        cb.Pad1 = 0;
        cb.Pad2 = 0;
        cb.Pad3 = 0;
        context->UpdateCB(_cbCull, &cb);

        context->BindCB(0, _cbCull);
        context->BindSR(0, pyramid->_sharedBoundsBuffer->View());
        context->BindSR(1, pyramid->_slotTableBuffer->View());
        context->BindSR(2, pyramid->_hzbTexture->View());
        context->BindUA(0, pyramid->_batchVisBuffer->View());

        const uint32 groups = (totalBounds + HZB_CULL_GROUP_SIZE - 1u) / HZB_CULL_GROUP_SIZE;
        context->Dispatch(_csCull, groups, 1, 1);

        context->ResetSR();
        context->ResetUA();

        // Stamp slot slices ONLY when we actually kick a new readback - slot promotion compares
        // _batchFrame to pyramid->_batchReadbackFrame to detect "is this slice in the buffer the
        // readback is filling".
        const uint64 frame = Engine::FrameCount;
        for (int32 i = 0; i < numEntries; i++)
        {
            const HZBData::BatchEntry& e = pyramid->_pendingDispatches[i];
            if (!e.Slot)
                continue;
            e.Slot->_batchOffsetWords = e.WriteOffsetWords;
            e.Slot->_batchCount = (int32)e.BoundsCount;
            e.Slot->_batchFrame = frame;
        }
        pyramid->_batchReadbackFrame = frame;

        // Kick the single async readback for this pyramid.
        Task* dl = pyramid->_batchVisBuffer->DownloadDataAsync(pyramid->_batchBytes);
        if (dl)
        {
            pyramid->_batchReadback = dl;
            dl->Start();
        }

        pyramid->ClearPending();
    }
}

void HZBData::ClearPending()
{
    _pendingDispatches.Clear();
    _boundsStaging.Clear();
    _pendingTotalWords = 0;
}

void HZBData::CancelReadback()
{
    if (_batchReadback)
    {
        if (!Engine::IsRequestingExit)
        {
            _batchReadback->Cancel();
            _batchReadback->WaitChain();
        }
        _batchReadback = nullptr;
    }
}

bool HZBData::Init()
{
    if (_isReady)
        return false;
    const auto device = GPUDevice::Instance;

    _depthTexture = device->CreateTexture(TEXT("HZB.Depth"));
    Float2 resolution = Screen::GetSize();
    int32 sizeX = Math::RoundToInt(resolution.X * 0.5f);
    int32 sizeY = Math::RoundToInt(resolution.Y * 0.5f);
    sizeX += sizeX % 2;
    sizeY += sizeY % 2;
    if (_depthTexture->Init(GPUTextureDescription::New2D(sizeX, sizeY, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
        return true;

    _hzbTexture = device->CreateTexture(TEXT("HZB.Pyramid"));
    auto desc = GPUTextureDescription::New2D(sizeX, sizeY, HZB_FORMAT, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
    if (_hzbTexture->Init(desc))
        return true;

    _isReady = true;
    return false;
}

void HZBData::Dispose()
{
    _isReady = false;
    _isValid = false;
    _hasValidPyramid = false;

    if (_depthTexture)
        _depthTexture->ReleaseGPU();
    if (_hzbTexture)
        _hzbTexture->ReleaseGPU();

    SAFE_DELETE_GPU_RESOURCE(_depthTexture);
    SAFE_DELETE_GPU_RESOURCE(_hzbTexture);
    _depthTexture = nullptr;
    _hzbTexture = nullptr;

    CancelReadback();
    SAFE_DELETE_GPU_RESOURCE(_batchVisBuffer);
    SAFE_DELETE_GPU_RESOURCE(_sharedBoundsBuffer);
    SAFE_DELETE_GPU_RESOURCE(_slotTableBuffer);
    _sharedBoundsCapacity = 0;
    _slotTableCapacity = 0;
    _batchBytes.Release();
    ClearPending();
    _slotTableStaging.Clear();
}

bool HZBData::CheckSkip()
{
    if (!_isValid)
        return true;
    if (!_isReady)
    {
        if (Init())
        {
            Dispose();
            return true;
        }
    }
    return false;
}

HZBData::~HZBData()
{
    // Defensive: HierarchialZBufferPass::Dispose calls our Dispose() before Delete; but if anything
    // ever destroys an HZBData without going through Dispose, _batchReadback would dangle.
    CancelReadback();
    SAFE_DELETE_GPU_RESOURCE(_batchVisBuffer);
    SAFE_DELETE_GPU_RESOURCE(_sharedBoundsBuffer);
    SAFE_DELETE_GPU_RESOURCE(_slotTableBuffer);

    ScopeLock lock(_consumersLock);
    for (auto& kv : _consumers)
        Delete(kv.Value);
    _consumers.Clear();
}

HZBCullSlot* HZBData::GetOrCreateConsumer(void* owner, int32 subId)
{
    const uint64 key = PackConsumerKey(owner, subId);
    ScopeLock lock(_consumersLock);
    HZBCullSlot** existing = _consumers.TryGet(key);
    if (existing)
        return *existing;
    HZBCullSlot* slot = New<HZBCullSlot>();
    slot->_owner = owner;
    _consumers.Add(key, slot);
    return slot;
}

void HierarchialZBufferPass::ReleaseConsumer(void* owner)
{
    if (!owner)
        return;
    HierarchialZBufferPass* pass = HierarchialZBufferPass::Instance();
    if (!pass)
        return;
    ScopeLock lock(pass->_infoLock);
    for (HZBData* pyramid : pass->_info)
    {
        if (pyramid)
            pyramid->ReleaseConsumersOf(owner);
    }
}

void HZBData::ReleaseConsumersOf(void* owner)
{
    ScopeLock lock(_consumersLock);
    Array<uint64, InlinedAllocation<8>> toRemove;
    for (auto& kv : _consumers)
    {
        if (kv.Value->_owner == owner)
        {
            // Scrub pending batch entries that reference the slot we're about to delete; otherwise
            // FlushPendingCulls would touch freed memory when stamping slot fields.
            for (auto& entry : _pendingDispatches)
                if (entry.Slot == kv.Value)
                    entry.Slot = nullptr;
            Delete(kv.Value);
            toRemove.Add(kv.Key);
        }
    }
    for (uint64 k : toRemove)
        _consumers.Remove(k);
}

void HZBData::PromoteCompletedReadbacks()
{
    if (!_batchReadback || !_batchReadback->IsChainEnded() || _batchReadbackFrame == 0)
        return;
    const uint64 frame = Engine::FrameCount;
    ScopeLock lock(_consumersLock);
    for (auto& kv : _consumers)
    {
        HZBCullSlot* slot = kv.Value;
        if (!slot || slot->_batchCount <= 0)
            continue;
        // Only promote slots that were part of the most recent kicked batch (its frame is stamped
        // on the slot); a stale _batchFrame means the slot missed that flush (skipped/scrubbed).
        if (slot->_batchFrame == 0 || slot->_batchFrame != _batchReadbackFrame)
            continue;
        const int32 wordsSlot = (slot->_batchCount + 31) / 32;
        const int32 byteOffset = (int32)slot->_batchOffsetWords * 4;
        const int32 byteLen = wordsSlot * 4;
        if (_batchBytes.Length() >= byteOffset + byteLen)
        {
            slot->VisBits.Allocate(byteLen);
            Platform::MemoryCopy(slot->VisBits.Get(), _batchBytes.Get() + byteOffset, byteLen);
            slot->VisBitsCount = slot->_batchCount;
            slot->_lastPromoteFrame = frame;
        }
    }
}

int32 HZBCullSlot::CountVisible() const
{
    if (VisBitsCount <= 0)
        return 0;
    const uint32* bits = (const uint32*)VisBits.Get();
    if (!bits)
        return 0;
    const int32 fullWords = VisBitsCount >> 5;
    const int32 tailBits = VisBitsCount & 31;
    int32 total = 0;
    for (int32 i = 0; i < fullWords; i++)
        total += Utilities::CountBits(bits[i]);
    if (tailBits)
    {
        const uint32 mask = (1u << (uint32)tailBits) - 1u;
        total += Utilities::CountBits(bits[fullWords] & mask);
    }
    return total;
}

void HierarchialZBufferPass::CollectStats(int32& pyramidsActive, int32& consumerSlots, int32& boundsTested, int32& visible)
{
    pyramidsActive = _info.Count();
    consumerSlots = 0;
    boundsTested = 0;
    visible = 0;
    for (HZBData* pyramid : _info)
    {
        if (!pyramid)
            continue;
        ScopeLock lock(pyramid->_consumersLock);
        for (auto& kv : pyramid->_consumers)
        {
            HZBCullSlot* slot = kv.Value;
            if (!slot)
                continue;
            consumerSlots++;
            boundsTested += slot->GetBoundsTested();
            visible += slot->CountVisible();
        }
    }
}

HZBCullSlot::~HZBCullSlot()
{
    VisBits.Release();
}

void HZBCullSlot::Dispatch(HZBData* pyramid, const Float4* worldBoundsCpu, uint32 boundsCount)
{
    if (!pyramid)
        return;

    // (1) Promote: if the pyramid's last-kicked readback included this slot AND has now drained,
    // copy our slice out of pyramid->_batchBytes into our own VisBits. _batchFrame is stamped by
    // FlushPendingCulls only when it actually kicked, so a stale _batchFrame survives skip frames.
    const uint64 frame = Engine::FrameCount;
    if (pyramid->_batchReadback && pyramid->_batchReadback->IsChainEnded() &&
        _batchFrame != 0 && _batchFrame == pyramid->_batchReadbackFrame && _batchCount > 0)
    {
        const int32 wordsSlot = (_batchCount + 31) / 32;
        const int32 byteOffset = (int32)_batchOffsetWords * 4;
        const int32 byteLen = wordsSlot * 4;
        if (pyramid->_batchBytes.Length() >= byteOffset + byteLen)
        {
            VisBits.Allocate(byteLen);
            Platform::MemoryCopy(VisBits.Get(), pyramid->_batchBytes.Get() + byteOffset, byteLen);
            VisBitsCount = _batchCount;
            _lastPromoteFrame = frame;
        }
    }

    // Age-out: if we hold a verdict but promotion has stalled (wedged readback, skipped flushes),
    // drop it so TestVisibility fails open rather than culling on an ever-staler mask. 8 frames is
    // well beyond the normal 1-2 frame promote latency.
    if (VisBitsCount > 0 && _lastPromoteFrame != 0 && frame - _lastPromoteFrame > 8)
        VisBitsCount = 0;

    if (!pyramid->IsReady() || boundsCount == 0 || !worldBoundsCpu)
        return;

    // (2) Copy bounds into the pyramid's per-frame staging, reserve a verdict slice, and queue
    // the entry. FlushPendingCulls walks _pendingDispatches once after DrainDelayedDraws to run
    // a single CS for the whole frame. Single-threaded - DrainDelayedDraws is render-thread only.
    const uint32 words = (boundsCount + 31u) / 32u;
    const int32 stagingOffset = pyramid->_boundsStaging.Count();
    pyramid->_boundsStaging.Add(worldBoundsCpu, (int32)boundsCount);
    HZBData::BatchEntry e;
    e.BoundsStagingOffset = (uint32)stagingOffset;
    e.BoundsCount = boundsCount;
    e.WriteOffsetWords = pyramid->_pendingTotalWords;
    e.Slot = this;
    pyramid->_pendingDispatches.Add(e);
    pyramid->_pendingTotalWords += words;
}
