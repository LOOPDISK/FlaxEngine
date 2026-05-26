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
/// Compute pre-skinning pass. Once per frame per visible skinned mesh, dispatches a CS that skins the
/// source VB into static-layout output VBs so draw calls run the static VS variant with no VS-time
/// skinning. Dormant skeletons reuse the cached output and skip dispatch.
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

    /// <summary>
    /// Called from per-mesh draw setup. Lazy-allocates the per-slot output VBs, queues a dispatch for
    /// this frame (deduped via the dormant-version check) and returns the output VB pointers. Returns
    /// false when compute skinning isn't applicable (shader not ready, source VB lacks ShaderResource,
    /// zero vertices) and the caller should fall back to VS-time skinning. The dispatch itself runs in
    /// FlushPending, after bone matrices are uploaded this frame.
    /// </summary>
    bool PrepareForDraw(SkinnedMeshDrawData* skinning, const SkinnedMesh* mesh, int32 slot, GPUBuffer*& outVB0, GPUBuffer*& outVB1, GPUBuffer*& outVB2);

    /// <summary>
    /// Dispatches all pending work queued via PrepareForDraw this frame. Call once per frame, after bone
    /// matrices are uploaded and before any draw pass consumes the output VBs. Always clears the queue.
    /// </summary>
    void FlushPending(GPUContext* context);

    /// <summary>
    /// Drops the pending dispatch queue without running it. Call at frame start to recover from a frame
    /// that queued via PrepareForDraw but never reached FlushPending.
    /// </summary>
    void ClearPending();

    /// <summary>
    /// Registers a SkinnedMeshDrawData + SkinnedModel for one-shot GPU prewarm next render frame. Moves
    /// the lazy alloc + first-use cost off the first dormant->active wake (a ~50 ms GBuffer spike) onto
    /// scene streaming. Game-thread safe; deduped by Skinning pointer. BoneMatrices must already exist.
    /// </summary>
    void QueuePrewarm(SkinnedMeshDrawData* skinning, SkinnedModel* model);

    /// <summary>
    /// Removes a pending prewarm entry. Call from AnimatedModel::EndPlay so the render thread can't
    /// dereference a freed SkinnedMeshDrawData. Safe when no entry exists.
    /// </summary>
    void CancelPrewarm(SkinnedMeshDrawData* skinning);

    /// <summary>
    /// Drains the prewarm queue (from the AnimatedModel render-list extension PreDraw). The dispatch may
    /// run against stale bones, so OutputVersion[slot] is reset to force a re-dispatch on the first real
    /// frame; only the alloc + first-use cost is amortized, the prewarm output is discarded.
    /// </summary>
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

#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj);
#endif
};
