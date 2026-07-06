#pragma once

#include "RendererPass.h"
#include "Engine/Graphics/PixelFormat.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Level/Actor.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Core/Math/Vector4.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/CriticalSection.h"

class GPUBuffer;
class GPUContext;
class HZBData;
class HZBCullSlot;
class Task;

/// <summary>
/// Hierarchial Z-Buffer rendering pass.
///
/// Builds an HZB pyramid each frame from the prior-frame depth buffer (the depth buffer at
/// PreRender time contains last-frame's depth - GBufferPass::Fill clears it later inside
/// RenderInner). The pyramid is then available for GPU occlusion-cull dispatches in the
/// same frame; results are consumed by next frame's draw collection.
/// </summary>
class HierarchialZBufferPass : public RendererPass<HierarchialZBufferPass>
{
public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

    /// <summary>
    /// Manages creation and disposal of an HZBData, linked to a SceneRenderTask.
    /// </summary>
    HZBData* GetOrCreateInfo(RenderContext& renderContext);

    /// <summary>
    /// Builds the HZB pyramid for this view.
    /// </summary>
    void Render(GPUContext* context, RenderContext& renderContext);

    /// <summary>
    /// Visualizes the HZB pyramid for debugging.
    /// </summary>
    void RenderDebug(RenderContext& renderContext, GPUContext* context);

    /// <summary>
    /// Collect aggregate stats across all active pyramids and slots. Cheap walk; safe to call
    /// per UI frame.
    /// </summary>
    void CollectStats(int32& pyramidsActive, int32& consumerSlots, int32& boundsTested, int32& visible);

    /// <summary>
    /// Drain all pending cull dispatches queued by HZBCullSlot::Dispatch this frame into a single
    /// per-pyramid batched CS pass: one ClearUA on a shared visibility buffer, N dispatches sharing
    /// the same UAV + CS bind, and one async readback per pyramid. Must be called after all consumers
    /// have submitted their dispatches (i.e. after DrainDelayedDraws).
    /// </summary>
    void FlushPendingCulls(GPUContext* context);

    /// <summary>
    /// Releases all HZB cull slots owned by the given consumer across every active pyramid. Call on
    /// consumer teardown (scene clear, foliage disable/destroy) so a recycled allocation reusing the
    /// same pointer can't inherit the dead consumer's stale occlusion verdicts. Thread-safe; a no-op
    /// if the pass or its pyramids aren't alive.
    /// </summary>
    static void ReleaseConsumer(void* owner);

protected:
    // [RendererPass]
    bool setupResources() override;

private:
    bool _supported = false;
    AssetReference<Shader> _shader;
    AssetReference<Shader> _shaderCull;
    GPUConstantBuffer* _cb = nullptr;
    GPUConstantBuffer* _cbDebug = nullptr;
    GPUConstantBuffer* _cbCull = nullptr;
    GPUPipelineState* _psHZB = nullptr;
    GPUPipelineState* _psDebug = nullptr;
    GPUShaderProgramCS* _csCull = nullptr;
    Array<HZBData*> _info;
    // Guards _info: the render thread appends via GetOrCreateInfo while main-thread consumer
    // teardown (ReleaseConsumer) sweeps it. Rendering itself walks _info single-threaded.
    CriticalSection _infoLock;

#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj);
#endif
};

/// <summary>
/// Per-view HZB pyramid + the camera state used to build it. Owned by SceneRenderTask::OcclusionInfo.
/// Also owns a directory of HZBCullSlot consumers - one per (owner, sub-id) - keyed by the caller
/// (e.g. (SceneRendering*, DrawCategory) for the main scene, (FoliageType*, typeIndex) for foliage).
/// Slots are heap-allocated for pointer stability across Dictionary rehash. They live as long as the
/// pyramid; the pyramid's destructor deletes them.
/// </summary>
class FLAXENGINE_API HZBData
{
    friend HierarchialZBufferPass;
    friend HZBCullSlot;

public:
    HZBData() = default;
    ~HZBData();

    bool Init();
    void Dispose();
    bool CheckSkip();

    /// <summary>
    /// Whether the pyramid is built and ready to be sampled (e.g. cull dispatches).
    /// </summary>
    bool IsReady() const { return _isReady && _hasValidPyramid; }

    /// <summary>
    /// The HZB pyramid texture (levels stacked horizontally).
    /// </summary>
    GPUTexture* GetHZBTexture() const { return _hzbTexture; }

    /// <summary>
    /// Get-or-allocate a per-consumer cull slot on this pyramid. Returned pointer is stable until
    /// this HZBData is destroyed. Thread-safe.
    /// </summary>
    /// <param name="owner">Opaque consumer identity (e.g. SceneRendering*, FoliageType*).</param>
    /// <param name="subId">Sub-identifier (e.g. DrawCategory, foliage type index). Two different
    /// (owner, subId) on the same pyramid yield two different slots.</param>
    HZBCullSlot* GetOrCreateConsumer(void* owner, int32 subId);

    /// <summary>
    /// Proactively releases all slots owned by the given consumer on this pyramid. Optional;
    /// normally slots live until the pyramid dies. Used by SceneRendering on Clear() to avoid
    /// stale verdicts surviving a scene swap. Thread-safe.
    /// </summary>
    void ReleaseConsumersOf(void* owner);

    /// <summary>
    /// If this pyramid's outstanding async readback has completed, copy every participating slot's
    /// verdict slice into its VisBits immediately. Called early in the frame (pyramid build) so the
    /// verdict is live for THIS frame's draw collection rather than waiting for each slot's next
    /// Dispatch (which is a frame later). Thread-safe.
    /// </summary>
    void PromoteCompletedReadbacks();

    // Per-frame batched cull state. Slot::Dispatch enqueues into _pendingDispatches and copies the
    // caller's CPU bounds into _boundsStaging; FlushPendingCulls then runs a single CS dispatch per
    // pyramid against a shared bounds buffer, with a single readback to _batchBytes. Slots promote
    // their slice on their next Dispatch().
    struct BatchEntry
    {
        uint32 BoundsStagingOffset;  // start index into HZBData::_boundsStaging
        uint32 BoundsCount;
        uint32 WriteOffsetWords;     // start word offset into _batchVisBuffer for this slot's bit range
        HZBCullSlot* Slot;
    };

    // Mirrors the SlotTable StructuredBuffer<uint4> layout in HZBCull.shader.
    struct SlotTableEntry
    {
        uint32 BoundsOffset;        // .x - start index into _sharedBoundsBuffer
        uint32 Count;               // .y - bound count (0 for scrubbed slots)
        uint32 WriteOffsetWords;    // .z - start word into _batchVisBuffer
        uint32 CumThread;           // .w - prefix-sum thread index of this slot's first element
    };

private:
    bool _isReady = false;
    bool _isValid = true;
    bool _hasValidPyramid = false;
    Float2 _resolution;
    GPUTexture* _depthTexture = nullptr;
    GPUTexture* _hzbTexture = nullptr;

    // Camera state captured at pyramid build time, used by FlushPendingCulls. The pyramid is built
    // from the PRIOR frame's depth buffer, so _vp/_viewOrigin are that frame's view-projection and
    // origin - projecting current-frame bounds through the current VP against last-frame depth pops
    // geometry in/out under camera rotation.
    Matrix _vp;
    Float3 _viewForward = Float3::Forward;
    float _viewNear = 0.0f;
    Float2 _pyramidBase = Float2::Zero;
    uint32 _maxLevel = 0;

    // World-space view origin captured at pyramid build time (prior frame's). The cull CS subtracts
    // this from world-space bound centers before projecting via _vp.
    Float3 _viewOrigin = Float3::Zero;

    // Absolute camera world position at the last pyramid build, and the camera translation between
    // the pyramid's capture frame and the current frame. Test-sphere radii are dilated by this so a
    // bound the camera has moved toward isn't wrongly culled against the now-stale depth pyramid.
    Vector3 _camWorldPos = Vector3::Zero;
    bool _camWorldPosValid = false;
    float _boundsInflate = 0.0f;

    // Batch state populated by HZBCullSlot::Dispatch each frame; drained by FlushPendingCulls.
    // _boundsStaging accumulates each slot's CPU bounds via memcpy in Dispatch(); FlushPendingCulls
    // uploads the whole thing once via Map(WRITE_DISCARD) into _sharedBoundsBuffer. _slotTableStaging
    // is built at flush time from _pendingDispatches and uploaded into _slotTableBuffer.
    Array<BatchEntry> _pendingDispatches;
    Array<Float4> _boundsStaging;
    Array<SlotTableEntry> _slotTableStaging;
    uint32 _pendingTotalWords = 0;
    GPUBuffer* _batchVisBuffer = nullptr;
    GPUBuffer* _sharedBoundsBuffer = nullptr;   // Dynamic StructuredBuffer<float4>, sized to max-seen totalBounds
    GPUBuffer* _slotTableBuffer = nullptr;      // Dynamic StructuredBuffer<uint4>, sized to max-seen slot count
    int32 _sharedBoundsCapacity = 0;
    int32 _slotTableCapacity = 0;
    Task* _batchReadback = nullptr;
    BytesContainer _batchBytes;
    uint64 _batchReadbackFrame = 0;

    // Resets the per-frame batch accumulation (pending entries + staged bounds + word cursor).
    void ClearPending();

    // Cancels the in-flight async readback and waits for its chain, so no continuation can still be
    // writing into _batchBytes when buffers are released. No-op during engine exit (the deferred-
    // delete queue may have already freed the task).
    void CancelReadback();

    // (owner-pointer, sub-id) packed into a single uint64 - Dictionary already has GetHash(uint64).
    // Collisions only matter within one pyramid's directory (handful of entries); the multiplicative
    // combine below is plenty.
    static FORCE_INLINE uint64 PackConsumerKey(void* owner, int32 subId)
    {
        return ((uint64)(uintptr)owner) * 0x9E3779B97F4A7C15ull + (uint64)(uint32)subId;
    }
    Dictionary<uint64, HZBCullSlot*> _consumers;
    CriticalSection _consumersLock;
};

/// <summary>
/// One occlusion-cull verdict store + readback for a (pyramid, consumer) pair. Heap-allocated and
/// owned by the HZBData::_consumers directory. Multiple slots can share the same pyramid (different
/// scenes / draw categories / foliage types) and multiple slots can share the same consumer (one
/// per pyramid the consumer is dispatching against - e.g. main camera, sniper scope, light pyramids).
///
/// The slot does NOT store bounds. The dispatching caller provides the bounds GPU buffer in
/// Dispatch(); typically that buffer is owned by the consumer (e.g. SceneRendering's persistent
/// per-category bounds buffer, shared across all pyramids).
///
/// The visibility buffer and async readback are owned by the parent HZBData and batched across
/// all slots on that pyramid (HierarchialZBufferPass::FlushPendingCulls). The slot only stores
/// its slice offset/count plus the bit array of the most recent verdict.
/// </summary>
class FLAXENGINE_API HZBCullSlot
{
public:
    HZBCullSlot() = default;
    ~HZBCullSlot();
    HZBCullSlot(const HZBCullSlot&) = delete;
    HZBCullSlot& operator=(const HZBCullSlot&) = delete;

    /// <summary>
    /// Returns true if the entry at <paramref name="key"/> was visible in the most recent completed
    /// readback. Returns true when the slot has no verdict yet (first frames after creation) or
    /// when the key is out of range - fail-open, never falsely cull.
    /// </summary>
    FORCE_INLINE bool TestVisibility(int32 key) const
    {
        if (key < 0 || key >= VisBitsCount)
            return true;
        const uint32* bits = (const uint32*)VisBits.Get();
        return (bits[key >> 5] & (1u << ((uint32)key & 31u))) != 0;
    }

    /// <summary>Number of bounds entries tested in the most recent dispatch.</summary>
    FORCE_INLINE int32 GetBoundsTested() const { return _batchCount > 0 ? _batchCount : VisBitsCount; }

    /// <summary>Population count of the most recent completed visibility bitmask.</summary>
    int32 CountVisible() const;

    /// <summary>
    /// Promote any completed batched readback into VisBits, then enqueue a cull dispatch onto the
    /// pyramid's pending list. The bounds are copied into the pyramid's per-frame staging here;
    /// no GPU work is recorded - FlushPendingCulls runs the single dispatch after DrainDelayedDraws.
    /// No-op if pyramid is null or boundsCount is zero. Caller must hold a stable pointer to
    /// <paramref name="worldBoundsCpu"/> for the duration of this call (the data is memcpy'd).
    /// Must be invoked on the main render thread (DrainDelayedDraws is single-threaded).
    /// </summary>
    /// <param name="pyramid">Pyramid to dispatch against.</param>
    /// <param name="worldBoundsCpu">CPU pointer to world-space sphere bounds (xyz=center, w=radius).</param>
    /// <param name="boundsCount">Number of valid bounds entries.</param>
    void Dispatch(HZBData* pyramid, const Float4* worldBoundsCpu, uint32 boundsCount);

private:
    friend HZBData;
    friend HierarchialZBufferPass;

    BytesContainer VisBits;
    int32 VisBitsCount = 0;

    // Slice into the pyramid's shared _batchBytes; written by FlushPendingCulls only when a fresh
    // readback was actually kicked. Slot reads the slice on its next Dispatch() once the chain ends.
    uint32 _batchOffsetWords = 0;
    int32 _batchCount = 0;
    uint64 _batchFrame = 0;

    // Frame of the most recent successful verdict promotion. If a readback wedges (chain never ends)
    // the slot would hold its last verdict forever and permanently cull whatever it last marked
    // occluded. When promotion stalls for too many frames we drop the verdict (VisBitsCount=0) so
    // TestVisibility fails open until a fresh readback lands.
    uint64 _lastPromoteFrame = 0;

    // Owner pointer carried only so HZBData::ReleaseConsumersOf can sweep by owner.
    // Otherwise unused by the slot itself.
    void* _owner = nullptr;
};
