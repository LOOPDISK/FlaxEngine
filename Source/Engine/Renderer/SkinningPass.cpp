// Copyright (c) Wojciech Figat. All rights reserved.

#include "SkinningPass.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUBufferDescription.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUVertexLayout.h"
#include "Engine/Graphics/Models/SkinnedMesh.h"
#include "Engine/Graphics/Models/SkinnedMeshDrawData.h"
#include "Engine/Content/Assets/SkinnedModel.h"

namespace
{
    // Mirror the static mesh VB0/VB1 layouts so the static VS variant can bind our compute-output VBs.
    GPUVertexLayout* GetComputeSkinVB0Layout()
    {
        return GPUVertexLayout::Get({
            { VertexElement::Types::Position, 0, 0, 0, PixelFormat::R32G32B32_Float },
        });
    }

    GPUVertexLayout* GetComputeSkinVB1Layout()
    {
        return GPUVertexLayout::Get({
            { VertexElement::Types::TexCoord, 1, 0, 0, PixelFormat::R16G16_Float },
            { VertexElement::Types::Normal, 1, 0, 0, PixelFormat::R10G10B10A2_UNorm },
            { VertexElement::Types::Tangent, 1, 0, 0, PixelFormat::R10G10B10A2_UNorm },
            { VertexElement::Types::TexCoord1, 1, 0, 0, PixelFormat::R16G16_Float },
        });
    }

    GPUVertexLayout* GetComputeSkinVB2Layout()
    {
        return GPUVertexLayout::Get({
            { VertexElement::Types::Color, 2, 0, 0, PixelFormat::R8G8B8A8_UNorm },
        });
    }
}

#define SKINNING_GROUP_SIZE 64
#define SKINNING_OUTPUT0_STRIDE 12 // Position R32G32B32_Float
#define SKINNING_OUTPUT1_STRIDE 16 // TexCoord + Normal + Tangent + TexCoord1
#define SKINNING_OUTPUT2_STRIDE 4  // Color R8G8B8A8_UNorm

GPU_CB_STRUCT(SkinningCBData {
    uint32 VertexCount;
    uint32 BoneCount;
    uint32 InputStride;
    uint32 Flags;
    uint32 OffsetPosition;
    uint32 OffsetTexCoord;
    uint32 OffsetNormal;
    uint32 OffsetTangent;
    uint32 OffsetBlendIndices;
    uint32 OffsetBlendWeights;
    uint32 OffsetColor;
    uint32 _padding0;
});

// Matches the FLAG_* defines in SkinningCS.shader.
#define SKINNING_FLAG_WEIGHTS_R8G8B8A8_UNORM        0x1u
#define SKINNING_FLAG_INDICES_R16G16B16A16_UINT     0x2u
#define SKINNING_FLAG_POSITION_R16G16B16A16_FLOAT   0x4u
#define SKINNING_FLAG_HAS_VERTEX_COLOR              0x8u

namespace
{
    // Read per-element offsets + stride from the source VB layout (cooked skinned VBs vary, so stride isn't hardcoded).
    bool ResolveInputLayout(GPUBuffer* sourceVB, SkinningCBData& cb)
    {
        GPUVertexLayout* layout = sourceVB ? sourceVB->GetVertexLayout() : nullptr;
        if (!layout)
            return false;
        cb.InputStride = layout->GetStride();
        cb.Flags = 0;
        uint32 found = 0;
        for (const VertexElement& e : layout->GetElements())
        {
            switch (e.Type)
            {
            case VertexElement::Types::Position:
                cb.OffsetPosition = e.Offset;
                if (e.Format == PixelFormat::R16G16B16A16_Float)
                    cb.Flags |= SKINNING_FLAG_POSITION_R16G16B16A16_FLOAT;
                found |= 1 << 0;
                break;
            case VertexElement::Types::TexCoord0:
                cb.OffsetTexCoord = e.Offset;
                found |= 1 << 1;
                break;
            case VertexElement::Types::Normal:
                cb.OffsetNormal = e.Offset;
                found |= 1 << 2;
                break;
            case VertexElement::Types::Tangent:
                cb.OffsetTangent = e.Offset;
                found |= 1 << 3;
                break;
            case VertexElement::Types::BlendIndices:
                cb.OffsetBlendIndices = e.Offset;
                if (e.Format == PixelFormat::R16G16B16A16_UInt)
                    cb.Flags |= SKINNING_FLAG_INDICES_R16G16B16A16_UINT;
                found |= 1 << 4;
                break;
            case VertexElement::Types::BlendWeights:
                cb.OffsetBlendWeights = e.Offset;
                if (e.Format == PixelFormat::R8G8B8A8_UNorm)
                    cb.Flags |= SKINNING_FLAG_WEIGHTS_R8G8B8A8_UNORM;
                found |= 1 << 5;
                break;
            case VertexElement::Types::Color:
                cb.OffsetColor = e.Offset;
                cb.Flags |= SKINNING_FLAG_HAS_VERTEX_COLOR;
                // Cooked skinned VBs always write Color as R8G8B8A8_UNorm.
                // If a future format variant lands, branch here on e.Format.
                break;
            default:
                break;
            }
        }
        // All six required; TexCoord0 absent can be synthesized, the rest must exist or it's not a skinned mesh.
        const uint32 required = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);
        if ((found & required) != required)
            return false;
        if ((found & (1 << 1)) == 0)
            cb.OffsetTexCoord = 0; // no UVs: read offset 0, harmless for shadow/unlit meshes
        return true;
    }

    const GPUBufferFlags ComputeSkinVBFlags = GPUBufferFlags::VertexBuffer | GPUBufferFlags::UnorderedAccess | GPUBufferFlags::RawBuffer | GPUBufferFlags::ShaderResource;

    // Source VB has a Color element? (CS passes color through; output VB2 is allocated only when present.)
    bool HasVertexColor(GPUBuffer* sourceVB)
    {
        if (GPUVertexLayout* layout = sourceVB ? sourceVB->GetVertexLayout() : nullptr)
        {
            for (const VertexElement& e : layout->GetElements())
            {
                if (e.Type == VertexElement::Types::Color)
                    return true;
            }
        }
        return false;
    }

    // Grow the per-slot output arrays to cover `slot`, null-initializing new entries (Resize leaves trivial elements garbage).
    void EnsureComputeSkinSlots(SkinnedMeshDrawData* skinning, int32 slot, bool hasColor)
    {
        if (skinning->OutputVB0.Count() <= slot)
        {
            const int32 oldCount = skinning->OutputVB0.Count();
            skinning->OutputVB0.Resize(slot + 1);
            skinning->OutputVB1.Resize(slot + 1);
            skinning->OutputVersion.Resize(slot + 1);
            for (int32 i = oldCount; i <= slot; i++)
            {
                skinning->OutputVB0[i] = nullptr;
                skinning->OutputVB1[i] = nullptr;
                skinning->OutputVersion[i] = 0;
            }
        }
        if (hasColor && skinning->OutputVB2.Count() <= slot)
        {
            const int32 oldCount = skinning->OutputVB2.Count();
            skinning->OutputVB2.Resize(slot + 1);
            for (int32 i = oldCount; i <= slot; i++)
                skinning->OutputVB2[i] = nullptr;
        }
    }

    // Create one compute-skin output VB; returns null on failure (caller decides whether to abort or retry next frame).
    GPUBuffer* CreateComputeSkinVB(const Char* name, uint32 vertexCount, uint32 stride, GPUVertexLayout* layout)
    {
        GPUBuffer* buf = GPUDevice::Instance->CreateBuffer(name);
        auto desc = GPUBufferDescription::Buffer(vertexCount * stride, ComputeSkinVBFlags, PixelFormat::R32_Typeless, nullptr, stride, GPUResourceUsage::Default);
        desc.VertexLayout = layout;
        if (buf && !buf->Init(desc))
            return buf;
        SAFE_DELETE_GPU_RESOURCE(buf);
        return nullptr;
    }
}

String SkinningPass::ToString() const
{
    return TEXT("SkinningPass");
}

bool SkinningPass::Init()
{
    _supported = GPUDevice::Instance->GetFeatureLevel() >= FeatureLevel::SM5;
    // Driven on demand from AnimatedModel. Kick setup early so shader errors surface at startup; dispatch retries via IsReady().
    if (_supported)
        checkIfSkipPass();
    return false;
}

void SkinningPass::Dispose()
{
    RendererPass::Dispose();
    _csSkin = nullptr;
    for (GPUConstantBuffer* cb : _cbPool)
        SAFE_DELETE_GPU_RESOURCE(cb);
    _cbPool.Clear();
    _shader = nullptr;
}

GPUConstantBuffer* SkinningPass::GetOrCreateCB(int32 index)
{
    if (index < _cbPool.Count() && _cbPool[index] != nullptr)
        return _cbPool[index];
    const int32 oldCount = _cbPool.Count();
    if (index >= oldCount)
    {
        _cbPool.Resize(index + 1);
        for (int32 i = oldCount; i <= index; i++)
            _cbPool[i] = nullptr;
    }
    if (_cbPool[index] == nullptr)
    {
        _cbPool[index] = GPUDevice::Instance->CreateConstantBuffer(sizeof(SkinningCBData), TEXT("SkinningPass.CB"));
    }
    return _cbPool[index];
}

bool SkinningPass::setupResources()
{
    if (!_supported)
        return true;

    if (_shader == nullptr)
    {
        _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/SkinningCS"));
        if (_shader == nullptr)
            return true;
#if COMPILE_WITH_DEV_ENV
        _shader.Get()->OnReloading.Bind<SkinningPass, &SkinningPass::OnShaderReloading>(this);
#endif
    }
    if (!_shader->IsLoaded())
        return true;

    const auto shader = _shader->GetShader();
    // Guard against shader/HLSL drift: check slot 0 reflects a CB at least our CB size.
    GPUConstantBuffer* reflectedCB = shader->GetCB(0);
    if (!reflectedCB || reflectedCB->GetSize() < sizeof(SkinningCBData))
        return true;

    _csSkin = shader->GetCS("CS_Skin");
    if (!_csSkin)
        return true;

    return false;
}

#if COMPILE_WITH_DEV_ENV
void SkinningPass::OnShaderReloading(Asset* obj)
{
    _csSkin = nullptr;
    // Pool entries survive reloads (size unchanged); leave them.
    invalidateResources();
}
#endif

bool SkinningPass::PrepareForDraw(SkinnedMeshDrawData* skinning, const SkinnedMesh* mesh, int32 slot, GPUBuffer*& outVB0, GPUBuffer*& outVB1, GPUBuffer*& outVB2)
{
    outVB0 = nullptr;
    outVB1 = nullptr;
    outVB2 = nullptr;
    if (checkIfSkipPass() || !skinning || !mesh || slot < 0)
        return false;
    if (!skinning->IsReady())
        return false;
    GPUBuffer* sourceVB = mesh->GetVertexBuffer(0);
    if (!sourceVB || !sourceVB->IsAllocated())
        return false;
    const uint32 vertexCount = (uint32)mesh->GetVertexCount();
    if (vertexCount == 0)
        return false;
    // Source VB must be ShaderResource-bindable so the CS can read it as ByteAddressBuffer; if not, fall back.
    if (!sourceVB->GetDescription().IsShaderResource())
        return false;

    // Only allocate the Color output VB when the source has a Color element.
    const bool hasVertexColor = HasVertexColor(sourceVB);

    // Lazy alloc under a lock (concurrent Draws; CreateBuffer isn't thread-safe); existing buffers skip the lock below.
    if (skinning->OutputVB0.Count() <= slot || skinning->OutputVB0[slot] == nullptr || skinning->OutputVB1[slot] == nullptr ||
        (hasVertexColor && (skinning->OutputVB2.Count() <= slot || skinning->OutputVB2[slot] == nullptr)))
    {
        ScopeLock lock(_allocLock);
        EnsureComputeSkinSlots(skinning, slot, hasVertexColor);
        if (skinning->OutputVB0[slot] == nullptr)
        {
            skinning->OutputVB0[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB0"), vertexCount, SKINNING_OUTPUT0_STRIDE, GetComputeSkinVB0Layout());
            if (skinning->OutputVB0[slot] == nullptr)
                return false;
        }
        if (skinning->OutputVB1[slot] == nullptr)
        {
            skinning->OutputVB1[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB1"), vertexCount, SKINNING_OUTPUT1_STRIDE, GetComputeSkinVB1Layout());
            if (skinning->OutputVB1[slot] == nullptr)
                return false;
        }
        if (hasVertexColor && skinning->OutputVB2[slot] == nullptr)
        {
            skinning->OutputVB2[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB2"), vertexCount, SKINNING_OUTPUT2_STRIDE, GetComputeSkinVB2Layout());
            if (skinning->OutputVB2[slot] == nullptr)
                return false;
        }
    }

    outVB0 = skinning->OutputVB0[slot];
    outVB1 = skinning->OutputVB1[slot];
    outVB2 = (hasVertexColor && slot < skinning->OutputVB2.Count()) ? skinning->OutputVB2[slot] : nullptr;

    // Enqueue if the cached output is stale (dormant-skip). OutputVersion advances only once the dispatch
    // actually runs (see DispatchOne), so a dropped dispatch retries next frame instead of locking the pose.
    if (skinning->OutputVersion[slot] != skinning->SkinningVersion)
        _pending.Add({ skinning, mesh, slot });

    return true;
}

void SkinningPass::FlushPending(GPUContext* context)
{
    if (_pending.Count() == 0)
        return;
    if (checkIfSkipPass() || !context)
    {
        _pending.Clear();
        return;
    }
    PROFILE_GPU_CPU_NAMED("Compute Skinning");
    // Elide BindSR(0) when consecutive dispatches share the same actor's BoneMatrices (multi-slot meshes).
    GPUBuffer* lastBones = nullptr;
    int32 cbIndex = 0;
    for (const PendingDispatch& p : _pending)
    {
        // Coalesce: a prior entry this frame (another context drawing the same mesh) already skinned this slot.
        if (!p.Skinning || p.Skinning->OutputVersion[p.Slot] == p.Skinning->SkinningVersion)
            continue;
        GPUConstantBuffer* cb = GetOrCreateCB(cbIndex++);
        if (!cb)
            continue;
        DispatchOne(context, p, cb, lastBones);
        lastBones = p.Skinning->BoneMatrices;
    }
    // Clear bindings once at end of pass; downstream passes rebind what they need.
    context->ResetSR();
    context->ResetUA();
    _pending.Clear();
}

void SkinningPass::ClearPending()
{
    _pending.Clear();
}

void SkinningPass::QueuePrewarm(SkinnedMeshDrawData* skinning, SkinnedModel* model)
{
    if (!_supported || !skinning || !model)
        return;
    ScopeLock lock(_prewarmLock);
    for (const PrewarmEntry& e : _pendingPrewarm)
    {
        if (e.Skinning == skinning)
            return;
    }
    _pendingPrewarm.Add({ skinning, model });
}

void SkinningPass::CancelPrewarm(SkinnedMeshDrawData* skinning)
{
    if (!skinning)
        return;
    ScopeLock lock(_prewarmLock);
    for (int32 i = 0; i < _pendingPrewarm.Count(); i++)
    {
        if (_pendingPrewarm[i].Skinning == skinning)
        {
            _pendingPrewarm.RemoveAt(i);
            return;
        }
    }
}

void SkinningPass::FlushPrewarm(GPUContext* context)
{
    // Don't drain on skip - retry next frame when the shader is ready.
    if (!context || checkIfSkipPass())
        return;
    Array<PrewarmEntry, InlinedAllocation<32>> entries;
    {
        ScopeLock lock(_prewarmLock);
        if (_pendingPrewarm.IsEmpty())
            return;
        entries.Add(_pendingPrewarm.Get(), _pendingPrewarm.Count());
        _pendingPrewarm.Clear();
    }
    PROFILE_GPU_CPU_NAMED("SkinningPass.FlushPrewarm");
    // Allocate output VBs directly (PrepareForDraw corrupts BoneMatrices during prewarm). OutputVersion stays 0 so the
    // first real draw still dispatches; only the alloc + first-use cost is paid up front. LOD0 mesh0 only.
    GPUVertexLayout* layoutVB0 = GetComputeSkinVB0Layout();
    GPUVertexLayout* layoutVB1 = GetComputeSkinVB1Layout();
    GPUVertexLayout* layoutVB2 = GetComputeSkinVB2Layout();
    for (const PrewarmEntry& entry : entries)
    {
        SkinnedMeshDrawData* skinning = entry.Skinning;
        SkinnedModel* model = entry.Model;
        if (!skinning || !skinning->IsReady() || !model || !model->IsLoaded())
            continue;
        if (model->LODs.Count() == 0 || model->LODs[0].Meshes.Count() == 0)
            continue;
        const SkinnedMesh& mesh = model->LODs[0].Meshes[0];
        if (!mesh.IsInitialized() || mesh.GetVertexCount() == 0)
            continue;
        GPUBuffer* sourceVB = mesh.GetVertexBuffer(0);
        if (!sourceVB || !sourceVB->GetDescription().IsShaderResource())
            continue;
        const bool hasVertexColor = HasVertexColor(sourceVB);
        const uint32 vertexCount = (uint32)mesh.GetVertexCount();
        const int32 slot = 0;
        EnsureComputeSkinSlots(skinning, slot, hasVertexColor);
        if (skinning->OutputVB0[slot] == nullptr)
            skinning->OutputVB0[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB0"), vertexCount, SKINNING_OUTPUT0_STRIDE, layoutVB0);
        if (skinning->OutputVB1[slot] == nullptr)
            skinning->OutputVB1[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB1"), vertexCount, SKINNING_OUTPUT1_STRIDE, layoutVB1);
        if (hasVertexColor && skinning->OutputVB2[slot] == nullptr)
            skinning->OutputVB2[slot] = CreateComputeSkinVB(TEXT("ComputeSkin VB2"), vertexCount, SKINNING_OUTPUT2_STRIDE, layoutVB2);
    }
}

void SkinningPass::DispatchOne(GPUContext* context, const PendingDispatch& p, GPUConstantBuffer* cbResource, GPUBuffer* prevBones)
{
    const uint32 vertexCount = (uint32)p.Mesh->GetVertexCount();
    GPUBuffer* sourceVB = p.Mesh->GetVertexBuffer(0);
    GPUBuffer* outputVB0 = p.Skinning->OutputVB0[p.Slot];
    GPUBuffer* outputVB1 = p.Skinning->OutputVB1[p.Slot];
    if (!sourceVB || !outputVB0 || !outputVB1 || !p.Skinning->BoneMatrices)
        return;

    SkinningCBData cb;
    cb.VertexCount = vertexCount;
    cb.BoneCount = (uint32)p.Skinning->BonesCount;
    cb.InputStride = 0;
    cb.Flags = 0;
    cb.OffsetPosition = 0;
    cb.OffsetTexCoord = 0;
    cb.OffsetNormal = 0;
    cb.OffsetTangent = 0;
    cb.OffsetBlendIndices = 0;
    cb.OffsetBlendWeights = 0;
    cb.OffsetColor = 0;
    cb._padding0 = 0;
    if (!ResolveInputLayout(sourceVB, cb))
        return;

    context->UpdateCB(cbResource, &cb);
    context->BindCB(0, cbResource);
    // Hoisted: skip rebinding bones when this dispatch shares the previous one's actor (see FlushPending).
    if (p.Skinning->BoneMatrices != prevBones)
        context->BindSR(0, p.Skinning->BoneMatrices->View());
    context->BindSR(1, sourceVB->View());
    context->BindUA(0, outputVB0->View());
    context->BindUA(1, outputVB1->View());
    // VB2 (Color) only bound when the source has Color; the CS skips u2 otherwise.
    if (p.Slot < p.Skinning->OutputVB2.Count() && p.Skinning->OutputVB2[p.Slot] != nullptr)
        context->BindUA(2, p.Skinning->OutputVB2[p.Slot]->View());

    const uint32 groups = (vertexCount + SKINNING_GROUP_SIZE - 1) / SKINNING_GROUP_SIZE;
    context->Dispatch(_csSkin, groups, 1, 1);

    // Mark the cached output resident only after a real dispatch; the early-outs above leave it stale to retry.
    p.Skinning->OutputVersion[p.Slot] = p.Skinning->SkinningVersion;
}
