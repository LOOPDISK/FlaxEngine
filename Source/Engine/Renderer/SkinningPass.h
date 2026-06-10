// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RendererPass.h"
#include "RenderListBuffer.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Platform/CriticalSection.h"

class GPUContext;
class GPUBuffer;
class GPUConstantBuffer;
class GPUShaderProgramCS;
class SkinnedMesh;
class SkinnedMeshDrawData;
class SkinnedModel;

/// <summary>
/// Compute pre-skinning pass: a CS skins the source VB into static-layout output VBs so draws use the
/// static VS (no VS-time skinning). Dormant skeletons reuse the cached output and skip dispatch.
/// </summary>
class SkinningPass : public RendererPass<SkinningPass>
{
public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

    /// <summary>
    /// True when the shader is loaded and the compute pipeline is ready to dispatch.
    /// </summary>
    bool IsReady() const { return _csSkin != nullptr; }

    /// <summary>Ensures output VBs and queues this mesh's skinning dispatch (deduped per slot); false if not applicable so the caller falls back to VS-time skinning.</summary>
    bool PrepareForDraw(SkinnedMeshDrawData* skinning, const SkinnedMesh* mesh, int32 slot, GPUBuffer*& outVB0, GPUBuffer*& outVB1, GPUBuffer*& outVB2);

    /// <summary>Dispatches all work queued this frame. Call once after bone upload, before any pass reads the output VBs.</summary>
    void FlushPending(GPUContext* context);

    /// <summary>Drops the pending dispatch queue unrun (frame-start reset).</summary>
    void ClearPending();

    /// <summary>Queues a one-shot GPU prewarm for a new model, moving alloc + first skin off the first dormant->active wake. Game-thread safe.</summary>
    void QueuePrewarm(SkinnedMeshDrawData* skinning, SkinnedModel* model);

    /// <summary>Removes a pending prewarm entry; call from EndPlay so the render thread can't touch freed data.</summary>
    void CancelPrewarm(SkinnedMeshDrawData* skinning);

    /// <summary>Drains the prewarm queue (allocs output VBs); OutputVersion stays 0 so the first real frame still dispatches.</summary>
    void FlushPrewarm(GPUContext* context);

protected:
    // [RendererPass]
    bool setupResources() override;

private:
    struct PendingDispatch
    {
        SkinnedMeshDrawData* Skinning;
        const SkinnedMesh* Mesh;
        int32 Slot;
        uint64 PrevVersion; // OutputVersion before PrepareForDraw stamped it; restored if the dispatch drops
    };

    bool _supported = false;
    AssetReference<Shader> _shader;
    GPUShaderProgramCS* _csSkin = nullptr;
    // Per-dispatch constant buffer pool; grows to max pending count, one entry per dispatch per frame.
    Array<GPUConstantBuffer*> _cbPool;
    // Thread-safe: per-actor Draws call PrepareForDraw concurrently on the job system.
    RenderListBuffer<PendingDispatch> _pending;

    // Guards lazy output-VB allocation in PrepareForDraw (one-time per actor+mesh).
    CriticalSection _allocLock;

    // Prewarm queue: game thread pushes (under _prewarmLock), render thread drains in FlushPrewarm.
    struct PrewarmEntry
    {
        SkinnedMeshDrawData* Skinning;
        SkinnedModel* Model;
    };
    Array<PrewarmEntry> _pendingPrewarm;
    CriticalSection _prewarmLock;

    GPUConstantBuffer* GetOrCreateCB(int32 index);
    // prevBones = bone buffer bound by the previous dispatch this flush; DispatchOne skips BindSR(0)
    // when it matches (common for multi-slot actors), null for the first.
    void DispatchOne(GPUContext* context, const PendingDispatch& p, GPUConstantBuffer* cb, GPUBuffer* prevBones);
    // Un-stamps OutputVersion when a queued dispatch is dropped, else dormant skeletons (no version bump)
    // never retry and render from a never-written output VB. Same-frame only: Skinning must still be alive.
    void RollbackStamp(const PendingDispatch& p);

#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj);
#endif
};
