// Copyright (c) Wojciech Figat. All rights reserved.

#include "ShadowsPass.h"
#include "GBufferPass.h"
#include "VolumetricFogPass.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Content/Content.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Profiler/ProfilingTools.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Engine/Units.h"
#include "Engine/Graphics/RenderTools.h"
#include "Engine/Level/Scene/SceneRendering.h"
#include "Engine/Level/Actors/AnimatedModel.h"
#include "Engine/Foliage/Foliage.h"
#include "Engine/Terrain/Terrain.h"
#include "Engine/Particles/ParticleEffect.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/Enums.h"
#include "Engine/Serialization/FileWriteStream.h"
#include "Engine/Utilities/RectPack.h"
#include "Engine/Threading/JobSystem.h"
#include "Engine/Threading/Task.h"
#include "Engine/Threading/ThreadPoolTask.h"
#include "Engine/Core/Math/Mathd.h"
#if USE_EDITOR
#include "Engine/Renderer/Lightmaps.h"
#endif

#define SHADOWS_POSITION_ERROR METERS_TO_UNITS(0.1f)
#define SHADOWS_ROTATION_ERROR 0.9999f
#define SHADOWS_MAX_TILES 6
#define SHADOWS_MIN_RESOLUTION 32
#define SHADOWS_MAX_STATIC_ATLAS_CAPACITY_TO_DEFRAG 0.7f
#define SHADOWS_BASE_LIGHT_RESOLUTION(atlasResolution) (atlasResolution / MAX_CSM_CASCADES) // Allow to store 4 CSM cascades in a single row in all cases
#define NormalOffsetScaleTweak METERS_TO_UNITS(1)
#define LocalLightNearPlane METERS_TO_UNITS(0.1f)

// Clipmap depth-interval COLD FALLBACK pad. The clipmap's depth interval along the sun axis is
// normally derived dynamically from the scene's static-caster bounds (ShadowClipmap::SceneDepth*,
// fed by ShadowsCustomBuffer::StaticCastersBounds) so any verticality is captured exactly. This
// pad only sizes the legacy origin-anchored envelope [-(2*WE + PAD), +2*WE] used on cold frames
// before the first caster scan produces valid bounds. Depth-quantum impact is negligible with D24
// (~0.6mm per level at 10km). See: D:\code\notes\shadow_clipmap_assumptions.md (invariant I8).
#define SHADOW_CLIPMAP_NEAR_PAD METERS_TO_UNITS(10000.0f) // 10km of vertical headroom toward the sun

// The clipmap is an explicit static-shadow cache: a full redraw fires only on cold-init,
// scroll-overflow self-heal, or Renderer::InvalidateStaticShadows(). Cheap L-strip scroll
// updates always run regardless. Engine-detected sun/basis/drift changes request an amortized
// self-heal rather than a spiky one-frame rebuild.
//
// Scroll-overflow self-heal: when the camera outruns a level's window in one frame (|delta| >= R),
// rebuild that level amortized over this many frames rather than smear stale
// content or spike a one-frame full redraw.
#define SHADOW_CLIPMAP_OVERFLOW_AMORTIZE 4

// Explicit static-shadow redraw request, driven by Renderer::InvalidateStaticShadows(). The game
// bumps Generation when the cached static shadows must rebuild (level settled after load, sun moved,
// warp/teleport). Each view's clipmap services the latest Generation, amortizing the rebuild over
// Amortize frames. Countdown reports "still redrawing" to the warp/fade gate (AreStaticShadowsRedrawing).
// Writers are game threads; the render thread reads, services, and ticks the countdown. All access
// goes through Platform::AtomicRead/AtomicStore/Interlocked* (volatile alone is not a memory fence).
static volatile int64 StaticShadowRedrawGeneration = 0;
static volatile int32 StaticShadowRedrawAmortize = 1;
static volatile int64 StaticShadowRedrawCountdown = 0; // int64: InterlockedDecrement has no int32 overload
static volatile int64 StaticShadowRedrawCountdownFrame = -1;
// Frame index of the most recent clipmap rebuild band rasterized. AreStaticShadowsRedrawing also
// reports true while bands are landing, covering rebuilds that outlive the countdown (self-heal,
// scroll-overflow amortize) or that started without an explicit request.
static volatile int64 StaticShadowLastBandFrame = -1;

// Light-space basis used by the toroidal shadow clipmap. Every site that constructs a
// (LightRight, LightUp) pair from a sun direction MUST go through this helper - otherwise
// the rasterizer, compositor, and cascade matrices can disagree in the sun-angle band
// between two different threshold values and the cached texture's content rotates 90deg vs
// the sampling math (silent shadow corruption that "swims" with camera rotation).
// See: D:\code\notes\shadow_clipmap_assumptions.md (invariant I1).
static void ComputeLightBasis(const Float3& sunDir, Float3& outRight, Float3& outUp)
{
    Float3 up = Float3::Up;
    if (Math::Abs(Float3::Dot(sunDir, up)) > 0.9f)
        up = Float3::Right;
    outRight = Float3::Normalize(Float3::Cross(up, sunDir));
    outUp = Float3::Cross(sunDir, outRight);
}

// CSM cascade bounding sphere: world-space center (snapped to the light texel grid) + radius.
// Shared by the dynamic cascade loop and the clipmap init so the two cannot drift - the lock-step
// requirement in shadow_clipmap_assumptions.md (invariants I1 + I12). Caller passes the basis from
// ComputeLightBasis. Double-precision snap is mandatory at cm world scale (see I12).
// stableRadius is persistent per-cascade state (hysteresis); both call sites must share one slot.
static void ComputeCascadeSphere(const Float3* frustumCornersVs, const Matrix& invView, const Float3& lightRight, const Float3& lightUp, const Float3& lightDir, int32 resolution, float splitMinRatio, float splitMaxRatio, float oldSplitMinRatio, float csmOverlap, float& stableRadius, float& lastRawRadius, uint64& radiusMotionFrame, uint64& radiusHighFrame, Float3& outCenter, float& outRadius)
{
    // Cascade split frustum corners in view space, then world space.
    Float3 cornersVs[8];
    const float overlap = csmOverlap * (splitMinRatio - oldSplitMinRatio);
    for (int32 j = 0; j < 4; j++)
    {
        const Float3 rangeVS = frustumCornersVs[j + 4] - frustumCornersVs[j];
        cornersVs[j] = frustumCornersVs[j] + rangeVS * (splitMinRatio - overlap);
        cornersVs[j + 4] = frustumCornersVs[j] + rangeVS * splitMaxRatio;
    }
    Float3 cornersWs[8];
    for (int32 i = 0; i < 8; i++)
        Float3::Transform(cornersVs[i], invView, cornersWs[i]);

    // Bounding-sphere center + radius.
    Float3 center = Float3::Zero;
    for (int32 i = 0; i < 8; i++)
        center += cornersWs[i];
    center *= 1.0f / 8.0f;
    float radius = 0.0f;
    for (int32 i = 0; i < 8; i++)
        radius = Math::Max(radius, (cornersWs[i] - center).LengthSquared());
    radius = Math::Sqrt(radius);

    // Decouple the radius from per-frame churn. Raw radius tracks tan(fov/2) with sensitivity
    // growing as sec^2(fov/2), plus world-scale float noise under camera rotation. Gameplay FOV
    // lerps (flinch/reveal/ADS) are absorbed UPSTREAM now - the frustum corners come from the
    // FOV-stable reference projection (ShadowsCustomBuffer::StableShadowProjection), so under a
    // pure zoom the raw radius here is constant and this hysteresis is a backstop (aspect changes,
    // FOV latch growth, rotation noise). Any radius change rescales the texel-snap grid below (tpu), which
    // reads as sub-texel shimmer on ALL shadow edges - worst at wide FOV - AND forces a full redraw
    // of the matching clipmap level (TexelSize re-anchor). Quantize to 1/16-magnitude buckets (same
    // scheme as the clipmap WorldExtent) with motion-aware hysteresis:
    // - growth applies immediately (coverage must never clip), but while the radius is actively
    //   animating it over-provisions +4 buckets so a continuous zoom-out crosses the range in a few
    //   large jumps instead of one bucket-jump (and clipmap rebuild) nearly every frame;
    // - shrink (tighten) fires only after the raw radius has DWELT below the threshold for several
    //   seconds. Tightening between ADS flips would force a rescale + clipmap rebuild on every
    //   cycle: shrink right after zoom-in settles, then regrow through bucket jumps on zoom-out.
    //   With the dwell, rapid ADS cycling latches the wide-FOV radius and both animation directions
    //   become event-free after the first zoom-out warms the latch; only a sustained zoom (scoped
    //   for >RadiusShrinkDwellFrames) pays one tighten to reclaim texel density.
    // stableRadius >= raw always, so cascade coverage is preserved; cost is coarser texel density
    // while the latch holds (up to ~25% right after an animated zoom-out).
    // Motion/dwell tracking is idempotent across the two per-frame call sites (dynamic cascade loop
    // + clipmap init share the same state slots): the second call sees an identical raw radius.
    const float magnitude = Math::Pow(2.0f, Math::Floor(Math::Log2(Math::Max(radius, 1.0f))));
    const float bucketSize = magnitude * (1.0f / 16.0f);
    const uint64 frame = Engine::FrameCount;
    const uint64 RadiusShrinkDwellFrames = 600; // ~10s at 60fps below the threshold before tightening
    if (Math::Abs(radius - lastRawRadius) > radius * 0.002f)
        radiusMotionFrame = frame;
    lastRawRadius = radius;
    const bool settled = frame - radiusMotionFrame > 30;
    if (stableRadius <= 0.0f)
    {
        stableRadius = Math::Ceil(radius / bucketSize) * bucketSize + bucketSize;
        radiusHighFrame = frame;
    }
    else if (radius > stableRadius)
    {
        stableRadius = Math::Ceil(radius / bucketSize) * bucketSize + (settled ? 1.0f : 4.0f) * bucketSize;
        radiusHighFrame = frame;
    }
    else if (radius > stableRadius - 1.5f * bucketSize)
    {
        radiusHighFrame = frame; // Raw is near the latch: reset the tighten dwell
    }
    else if (settled && frame - radiusHighFrame > RadiusShrinkDwellFrames)
    {
        stableRadius = Math::Ceil(radius / bucketSize) * bucketSize + bucketSize;
        radiusHighFrame = frame;
    }
    radius = stableRadius;

    // Snap center to the texel grid in light space (X/Y); Z is the light depth axis.
    const double tpu = (double)resolution / ((double)radius * 2.0);
    const double dotR = (double)center.X * lightRight.X + (double)center.Y * lightRight.Y + (double)center.Z * lightRight.Z;
    const double dotU = (double)center.X * lightUp.X + (double)center.Y * lightUp.Y + (double)center.Z * lightUp.Z;
    const double cxL = Math::Floor(dotR * tpu) / tpu;
    const double cyL = Math::Floor(dotU * tpu) / tpu;
    const double czL = (double)center.X * lightDir.X + (double)center.Y * lightDir.Y + (double)center.Z * lightDir.Z;
    outCenter = Float3(
        (float)(lightRight.X * cxL + lightUp.X * cyL + lightDir.X * czL),
        (float)(lightRight.Y * cxL + lightUp.Y * cyL + lightDir.Y * czL),
        (float)(lightRight.Z * cxL + lightUp.Z * cyL + lightDir.Z * czL));
    outRadius = radius;
}

GPU_CB_STRUCT(Data {
    ShaderGBufferData GBuffer;
    ShaderLightData Light;
    Matrix WVP;
    Matrix ViewProjectionMatrix;
    float Dummy0;
    float TemporalTime;
    float ContactShadowsDistance;
    float ContactShadowsLength;
    });

struct ShadowsAtlasRectTile : RectPackNode<uint16>
{
    bool IsStatic;

    ShadowsAtlasRectTile(Size x, Size y, Size width, Size height)
        : RectPackNode(x, y, width, height)
    {
    }

    void OnInsert(class ShadowsCustomBuffer* buffer, bool isStatic);
    void OnFree(ShadowsCustomBuffer* buffer);
};

uint16 QuantizeResolution(float input)
{
    uint16 output = Math::FloorToInt(input);
    uint16 alignment = 32;
    if (output >= 512)
        alignment = 128;
    else if (output >= 256)
        alignment = 64;
    output = Math::AlignDown<uint16>(output, alignment);
    return output;
}

// State for shadow projection
struct ShadowAtlasLightTile
{
    ShadowsAtlasRectTile* RectTile;
    ShadowsAtlasRectTile* StaticRectTile;
    const ShadowsAtlasRectTile* LinkedRectTile;
    Matrix WorldToShadow;
    float FramesToUpdate; // Amount of frames (with fraction) until the next shadow update can happen
    bool SkipUpdate;
    mutable bool HasStaticGeometry;
    Viewport CachedViewport; // The viewport used the last time to render shadow to the atlas

    void FreeDynamic(ShadowsCustomBuffer* buffer);
    void FreeStatic(ShadowsCustomBuffer* buffer);

    void Free(ShadowsCustomBuffer* buffer)
    {
        FreeDynamic(buffer);
        FreeStatic(buffer);
    }

    void ClearDynamic()
    {
        RectTile = nullptr;
        FramesToUpdate = 0;
        SkipUpdate = false;
    }

    void ClearStatic()
    {
        StaticRectTile = nullptr;
        LinkedRectTile = nullptr;
        FramesToUpdate = 0;
        SkipUpdate = false;
    }

    void SetWorldToShadow(const Matrix& shadowViewProjection)
    {
        // Transform Clip Space [-1,+1]^2 to UV Space [0,1]^2 (saves MAD instruction in shader)
        const Matrix ClipToUV(
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f);
        Matrix m;
        Matrix::Multiply(shadowViewProjection, ClipToUV, m);
        Matrix::Transpose(m, WorldToShadow);
    }
};

// Shadow clipmap level for caching static geometry shadows
struct ShadowClipmapLevel
{
    GPUTexture* DepthTexture = nullptr; // RxR depth texture for this level
    int32 Resolution = 0;               // Texels per side
    float TexelSize = 0.0f;             // World units per texel
    float WorldExtent = 0.0f;           // = Resolution * TexelSize
    // Rasterized depth interval along the sun axis (sun-z = dot(p, SunDir)), world-anchored like
    // the XY texel grid (arm centers carry no sun-z component). Ortho near plane sits at sun-z =
    // DepthMinZ, far plane at DepthMinZ + DepthRange. Scene-bounds-driven when available so ALL
    // static casters fit regardless of verticality; legacy origin-anchored envelope as cold fallback.
    float DepthMinZ = 0.0f;             // Sun-z of ortho near plane
    float DepthRange = 0.0f;            // Total ortho depth range (DepthMaxZ - DepthMinZ)
    Int2 ScrollTexels = Int2::Zero;      // Current camera position in texel coords (level center)
    Int2 PrevScrollTexels = Int2::Zero;  // Previous frame scroll
    Int2 DirtyStrip = Int2::Zero;        // Delta scroll (texels in X / Y to render in strip pass)
    // TextureOriginTexels anchors the toroidal mapping. Texture pixel for world-texel w:
    //   px = ((w.X - origin.X) mod R + R) mod R
    //   py = ((origin.Y - 1 - w.Y) mod R + R) mod R   (Y-flip matches existing ortho convention)
    // Set on full-redraw to (ScrollTexels.X - R/2, ScrollTexels.Y + R/2). Strip updates leave it unchanged.
    Int2 TextureOriginTexels = Int2::Zero;
    bool NeedsFullRedraw = true;         // Pending (re)build request: cold-init, overflow, or explicit API
    bool Populated = false;              // Set true when a rebuild completes; cleared content until then
    // Amortized rebuild schedule. NeedsFullRedraw requests a build; the render loop then bands it:
    // each frame rasterizes RedrawRowsPerFrame texel rows at RedrawAnchorScroll, top-to-bottom, until
    // RedrawRowCursor reaches Resolution. RowCursor < 0 = idle (not rebuilding). ScrollTexels is held
    // at the anchor while a rebuild is in flight so the compositor samples coherently (I4). amortize=1
    // -> one full-window band == the old single-frame full redraw.
    Int2  RedrawAnchorScroll = Int2::Zero;
    int32 RedrawRowCursor = -1;          // next texel row to rasterize; <0 = idle
    int32 RedrawRowsPerFrame = 0;        // band height (rows) per frame
    // R4 content-proof: static occluders rasterized so far by the in-flight rebuild, accumulated
    // across bands. Reset when a rebuild starts; checked at completion. Populated latches true ONLY
    // if this is >0 - a fresh proc-gen world whose geometry hasn't streamed in yet collects zero
    // casters, and latching "valid" over that empty rasterize is what made statics silently vanish
    // (and the editor "only casts when I move a prop" symptom). Mirrors master's own
    // non-empty-draw-list gate before committing a static cache.
    int32 RedrawCasterCount = 0;
    bool  SelfHealRequested = false;     // engine-detected genuine invalidation (cumulative cascade-scale
                                         // drift the game can't see). Honored as an amortized rebuild even
                                         // in explicit mode - survives the populated-clear, unlike the
                                         // DirtyBounds / per-frame / basis advisory noise.
    // SunDir snapshot at the moment of last full rasterize. The per-frame basis-coherence check
    // in Init() compares current SunDir against this to detect cumulative drift below the
    // sunChanged heuristic's threshold - sub-0.81deg rotations would otherwise let the strip update
    // write current-basis pixels into a stale-basis texture, producing the "rotation glitch" that
    // settles only after a full rebuild fires. Re-stamped on every successful full rebuild.
    Float3 LastRedrawSunDir = Float3::Zero;
    // Stale-anchor instrumentation: snapshot at end of previous Init. If a subsequent Init
    // produces a different TexelSize or DepthRange without a full redraw, the cached texture
    // pixels are anchored to old units and the compositor will sample garbage.
    float PrevTexelSize = 0.0f;
    float PrevDepthRange = 0.0f;
    float PrevDepthMinZ = 0.0f;
    // Cumulative-drift instrumentation: snapshot at time of last full redraw. Per-frame Init only
    // compares to Prev*, so gradual drift slides under the 0.1% threshold while the cached texture
    // diverges from current cascade math over many frames. LastRedraw* captures the anchor the
    // texture content was actually drawn against, so we can measure true divergence.
    float LastRedrawTexelSize = 0.0f;
    float LastRedrawDepthRange = 0.0f;
    float LastRedrawDepthMinZ = 0.0f;
    float LastRedrawWorldExtent = 0.0f;
    Float4 CompositingColor;             // xy=UV scale, zw=UV offset (logical) for PS_ClipmapComposite
    Float2 DepthRemap;                   // x=scale, y=bias for depth remapping
    Float2 WrapOffsetUV;                 // frac((leftEdgeWorldTexel - origin)/R); added inside frac() in shader
    // Desired (target) params, recomputed by Init each frame from current cascades/scene. CONTENT
    // params (WorldExtent/TexelSize/DepthMinZ/DepthRange) describe what the texture actually holds
    // and are adopted from these ONLY at full-rebuild start (render loop) - so the compositor always
    // samples content with the params it was rendered under, and any FOV/radius change is safe by
    // construction (the per-frame composite mapping absorbs it; no desync window exists).
    float DesiredWorldExtent = 0.0f;
    float DesiredDepthMinZ = 0.0f;
    float DesiredDepthRange = 0.0f;
    uint64 DensityLowFrame = 0;          // Last frame content extent was NOT oversized vs desired (density-reclaim dwell)
    // When true this cascade renders static geometry dynamically and skips the clipmap composite:
    // the content can't serve it (coverage gap, rebuild in flight, mis-anchored, or cold).
    bool FallbackActive = true;
    // Per-arm/sub-rect view+proj are built locally in RenderClipmapStrip - no cached matrices kept on the level.

    ~ShadowClipmapLevel() { SAFE_DELETE_GPU_RESOURCE(DepthTexture); }
};

// Toroidal shadow clipmap for caching static geometry across multiple levels
struct ShadowClipmap
{
    ShadowClipmapLevel Levels[MAX_CSM_CASCADES]; // One cached level per CSM cascade
    int32 LevelCount = 0;
    Float3 LightRight, LightUp, SunDir; // Light-space basis vectors
    Float3 CachedSunDirection;           // For detecting sun rotation
    Float3 LastCameraPos = Float3::Zero; // Camera pos from this frame's ComputeScroll; rebuild-start re-anchors scroll on the adopted texel grid with it
    // Stale-anchor instrumentation: snapshot of light basis at end of previous Init. A flip
    // (eg. up-vector threshold crossing) without a sun-rotation trigger means level textures
    // were rendered with a different (R_L, U_L) basis than the compositor now assumes.
    Float3 PrevLightRight = Float3::Zero;
    Float3 PrevLightUp = Float3::Zero;
    bool Enabled = false;
    Guid LightId;                         // ID of directional light using this clipmap
    // Explicit-redraw bookkeeping (Renderer::InvalidateStaticShadows). ServicedRedrawGeneration is
    // the last global request this clipmap honored; PendingAmortize carries the frame budget for the
    // next rebuild start (from the API or overflow self-heal), consumed when a band begins.
    int64 ServicedRedrawGeneration = 0;
    int32 PendingAmortize = 1;
    // Quantized sun-axis depth interval covering all known static casters. Expand-only (never
    // shrinks within a session) so streaming-in geometry can only widen it; expansion crosses a
    // bucket boundary -> per-level depth-drift gate fires a self-heal rebuild that captures the
    // new content. Generous bucket + headroom keep that rare. Valid==false until the first
    // caster bounds arrive (cold frames fall back to the legacy origin-anchored envelope).
    float SceneDepthMinZ = 0.0f;
    float SceneDepthMaxZ = 0.0f;
    bool SceneDepthValid = false;

    void Init(PixelFormat format, int32 cascadeCount, const float* cascadeRadii, const float* splitDistances,
              int32 cascadeResolution, const Float3& sunDir,
              bool sceneZValid, float sceneMinZ, float sceneMaxZ)
    {
        // Compute light-space basis (see ComputeLightBasis - single source of truth).
        SunDir = Float3::Normalize(sunDir);
        ComputeLightBasis(SunDir, LightRight, LightUp);

        // Check if sun direction changed significantly
        bool sunChanged = Float3::Dot(CachedSunDirection, SunDir) < 0.9999f;
        if (sunChanged)
            CachedSunDirection = SunDir;

        // Instrumentation: detect basis flip not driven by sun rotation. If the up-vector
        // threshold inside ComputeLightBasis crosses, R_L/U_L flip while sunChanged stays false,
        // and cached levels become rotation-inconsistent unless we force redraw.
        const bool hasPrevBasis = PrevLightRight != Float3::Zero;
        const bool basisFlipped = hasPrevBasis && (
            Float3::Dot(LightRight, PrevLightRight) < 0.9999f ||
            Float3::Dot(LightUp, PrevLightUp) < 0.9999f);
        if (basisFlipped && !sunChanged)
        {
            LOG(Warning, "[ShadowClipmap] light basis flipped without sunChanged: prevR=({0},{1},{2}) curR=({3},{4},{5})",
                PrevLightRight.X, PrevLightRight.Y, PrevLightRight.Z,
                LightRight.X, LightRight.Y, LightRight.Z);
            for (int32 i = 0; i < LevelCount; i++) Levels[i].NeedsFullRedraw = true;
        }
        PrevLightRight = LightRight;
        PrevLightUp = LightUp;

        LevelCount = cascadeCount;

        // Resolve the depth interval along the sun axis from actual scene content. The old
        // origin-anchored envelope [-(2*WE + NEAR_PAD), +2*WE] missed casters in three real cases:
        // tall geometry beyond the fixed pad, deep geometry below +2*WE, and any world far from the
        // origin plane along a tilted sun axis (sun-z mixes horizontal distance). Scene bounds make
        // coverage exact. Quantized to coarse buckets with 1-bucket headroom and EXPAND-ONLY within
        // a session: streaming-in casters can only widen the interval, and only a bucket crossing
        // changes the mapping (-> per-level depth-drift gate -> amortized self-heal rebuild).
        if (sceneZValid)
        {
            const float range = Math::Max(sceneMaxZ - sceneMinZ, METERS_TO_UNITS(10.0f));
            const float bucket = Math::Max(METERS_TO_UNITS(100.0f), Math::Pow(2.0f, Math::Floor(Math::Log2(range))) * 0.125f);
            if (!SceneDepthValid)
            {
                SceneDepthMinZ = (Math::Floor(sceneMinZ / bucket) - 1.0f) * bucket;
                SceneDepthMaxZ = (Math::Ceil(sceneMaxZ / bucket) + 1.0f) * bucket;
                SceneDepthValid = true;
                //LOG(Info, "[ShadowClipmap] scene depth interval init: [{0}, {1}] (raw [{2}, {3}])",
                //    SceneDepthMinZ, SceneDepthMaxZ, sceneMinZ, sceneMaxZ);
            }
            else
            {
                //const float oldMinZ = SceneDepthMinZ, oldMaxZ = SceneDepthMaxZ;
                if (sceneMinZ < SceneDepthMinZ)
                    SceneDepthMinZ = (Math::Floor(sceneMinZ / bucket) - 1.0f) * bucket;
                if (sceneMaxZ > SceneDepthMaxZ)
                    SceneDepthMaxZ = (Math::Ceil(sceneMaxZ / bucket) + 1.0f) * bucket;
                //if (SceneDepthMinZ != oldMinZ || SceneDepthMaxZ != oldMaxZ)
                //    LOG(Info, "[ShadowClipmap] scene depth interval expand: [{0}, {1}] -> [{2}, {3}]",
                //        oldMinZ, oldMaxZ, SceneDepthMinZ, SceneDepthMaxZ);
            }
        }

        for (int32 i = 0; i < cascadeCount; i++)
        {
            auto& level = Levels[i];
            level.Resolution = cascadeResolution;
            // Clipmap must cover the cascade regardless of camera rotation.
            // The cascade center can be at most splitDistance from camera in light-space XY,
            // and the cascade sphere extends cascadeRadius beyond that.
            float requiredHalfExtent = splitDistances[i] + cascadeRadii[i];
            float rawExtent = requiredHalfExtent * 2.0f;

            // CONTENT params (WorldExtent/TexelSize/DepthMinZ/DepthRange) are IMMUTABLE here: they
            // describe what the texture holds and are adopted from the Desired* values only when a
            // full rebuild starts (render loop). The compositor therefore always samples content
            // with the params it was rendered under - FOV/radius changes need NO rebuild for
            // correctness (the per-frame composite mapping absorbs them); rebuilds are requested
            // only to restore coverage or reclaim texel density, and FallbackActive (computed in
            // the compositing-params loop from the actual mapping) bridges any gap by rendering
            // statics dynamically for the affected cascades.
            // Desired extent is quantized to 1/4-magnitude buckets + 1 bucket headroom so FOV noise
            // and gameplay lerps (Homunculus reveal 90->97.2deg, FlinchFov) don't thrash requests.
            const float magnitude = Math::Pow(2.0f, Math::Floor(Math::Log2(Math::Max(rawExtent, 1.0f))));
            const float bucketSize = Math::Max(1.0f, magnitude * (1.0f / 4.0f));
            level.DesiredWorldExtent = Math::Ceil(rawExtent / bucketSize) * bucketSize + bucketSize;
            if (SceneDepthValid)
            {
                level.DesiredDepthMinZ = SceneDepthMinZ;
                level.DesiredDepthRange = SceneDepthMaxZ - SceneDepthMinZ;
            }
            else
            {
                // Cold fallback before the first caster scan: legacy origin-anchored envelope
                level.DesiredDepthMinZ = -(level.DesiredWorldExtent * 2.0f + SHADOW_CLIPMAP_NEAR_PAD);
                level.DesiredDepthRange = level.DesiredWorldExtent * 4.0f + SHADOW_CLIPMAP_NEAR_PAD;
            }

            if (level.WorldExtent <= 0.0f)
            {
                // Cold init: no content to preserve - adopt immediately (first build renders at these)
                level.WorldExtent = level.DesiredWorldExtent;
                level.TexelSize = level.WorldExtent / level.Resolution;
                level.DepthMinZ = level.DesiredDepthMinZ;
                level.DepthRange = level.DesiredDepthRange;
                level.NeedsFullRedraw = true;
            }
            else
            {
                // Divergence-driven rebuild requests. Content stays valid (and composited) while a
                // request waits or bands - except coverage shortfall, which FallbackActive detects
                // per-frame from the actual composite mapping. Explicit mode routes via self-heal so
                // the play-mode advisory clear can't drop these (they are correctness/quality, not noise).
                const uint64 frame = Engine::FrameCount;
                bool rebuild = false;
                if (rawExtent > level.WorldExtent)
                    rebuild = true; // Coverage shortfall: cascade footprint exceeds the cached window (zoom-out)
                if (rawExtent > level.WorldExtent * 0.55f)
                    level.DensityLowFrame = frame;
                else if (frame - level.DensityLowFrame > 600)
                    rebuild = true; // Density reclaim: content ~2x oversized for ~10s (settled zoom-in)
                const float depthEps = 0.25f * Math::Max(level.DepthRange, 1.0f);
                if (Math::Abs(level.DesiredDepthMinZ - level.DepthMinZ) > depthEps ||
                    Math::Abs(level.DesiredDepthRange - level.DepthRange) > depthEps)
                    rebuild = true; // Static caster depth window moved substantially (streaming/scene edits)
                if (rebuild && !level.NeedsFullRedraw && !level.SelfHealRequested && level.RedrawRowCursor < 0)
                {
                    // Static caster depth window / extent moved substantially (streaming or scene
                    // edits): request an amortized self-heal rather than a spiky one-frame redraw.
                    level.SelfHealRequested = true;
                }
            }

            if (sunChanged)
                level.NeedsFullRedraw = true;

            // Per-level basis-coherence: the cache content was rasterized under level.LastRedrawSunDir.
            // If the current basis has drifted from that, the cached texels are mis-anchored and must
            // be rebuilt. The existing `sunChanged` check uses 0.9999 (~0.81deg) against CachedSunDirection
            // (updated only on jump), letting sub-threshold drift accumulate while strip updates
            // write current-basis pixels into a stale-basis texture - the rotation-glitch bug.
            // Threshold 1 - 1e-7 ~ 0.026deg drift; sized so worst-case misalignment at the largest
            // clipmap extent stays sub-texel (extent ~10km / 2048 res = 4.88m/texel -> arcsin(4.88/10000)
            // ~ 0.028deg before a visible 1-texel shift). Any drift above this triggers a same-frame
            // full rebuild so the compositor never reads mis-anchored content.
            const bool basisCoherent = level.LastRedrawSunDir != Float3::Zero &&
                                       Float3::Dot(level.LastRedrawSunDir, SunDir) > 1.0f - 1e-7f;
            if (!basisCoherent)
                level.NeedsFullRedraw = true;

            // Create or resize texture if needed
            if (!level.DepthTexture)
            {
                level.DepthTexture = GPUDevice::Instance->CreateTexture(TEXT("Shadow Clipmap Level"));
                auto desc = GPUTextureDescription::New2D(cascadeResolution, cascadeResolution, format, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil);
                if (level.DepthTexture->Init(desc))
                {
                    LOG(Warning, "Failed to create shadow clipmap level texture");
                    Enabled = false;
                    return;
                }
                level.NeedsFullRedraw = true;
            }
            else if (level.DepthTexture->Width() != cascadeResolution)
            {
                auto desc = GPUTextureDescription::New2D(cascadeResolution, cascadeResolution, format, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil);
                if (level.DepthTexture->Init(desc))
                {
                    LOG(Warning, "Failed to resize shadow clipmap level texture");
                    Enabled = false;
                    return;
                }
                // The realloc leaves undefined contents, so the level is no longer populated. Clearing
                // this (not just NeedsFullRedraw) is required: the play-mode explicit-redraw gate (~L1944)
                // suppresses NeedsFullRedraw on populated levels, which would otherwise cancel the
                // mandatory post-resize rebuild and composite an empty texture as if valid.
                level.Populated = false;
                level.NeedsFullRedraw = true;
            }
        }

        Enabled = true;
    }

    void ComputeScroll(const Float3& cameraPos)
    {
        LastCameraPos = cameraPos;
        for (int32 i = 0; i < LevelCount; i++)
        {
            auto& level = Levels[i];
            // Hold the anchor while a rebuild is in flight: no scroll, no strip (keeps the compositor
            // sampling coherent with the fixed RedrawAnchorScroll - I4).
            if (level.RedrawRowCursor >= 0)
            {
                level.DirtyStrip = Int2::Zero;
                continue;
            }
            level.PrevScrollTexels = level.ScrollTexels;

            // Project camera position into light-space XY in DOUBLE precision. Single-precision
            // dot+divide+FloorToInt flips off-by-1 between frames once camera is far from origin
            // (Flax world units are cm, so the threshold is small - ~500m / 50,000 cm), making
            // ScrollTexels jump erratically and the entire clipmap level shift by a texel
            // mid-stride. The visible artifact is the same shadow edge shimmer that the main
            // cascade snap was originally trying to suppress.
            const double camX = (double)cameraPos.X * LightRight.X + (double)cameraPos.Y * LightRight.Y + (double)cameraPos.Z * LightRight.Z;
            const double camY = (double)cameraPos.X * LightUp.X    + (double)cameraPos.Y * LightUp.Y    + (double)cameraPos.Z * LightUp.Z;
            const double invTs = 1.0 / (double)level.TexelSize;
            level.ScrollTexels = Int2(
                (int32)Math::Floor(camX * invTs),
                (int32)Math::Floor(camY * invTs)
            );

            // Compute delta - strip update path consumes this in the render loop.
            // If |delta| >= Resolution, the prior content is entirely stale -> fall back to full redraw.
            level.DirtyStrip = level.ScrollTexels - level.PrevScrollTexels;
            if (Math::Abs(level.DirtyStrip.X) >= level.Resolution || Math::Abs(level.DirtyStrip.Y) >= level.Resolution)
            {
                level.NeedsFullRedraw = true;
                level.DirtyStrip = Int2::Zero;
                // Spread the forced rebuild so a fast fly-through can't spike.
                PendingAmortize = Math::Max(PendingAmortize, SHADOW_CLIPMAP_OVERFLOW_AMORTIZE);
            }
        }
    }

    void MarkFullRedraw()
    {
        for (int32 i = 0; i < LevelCount; i++)
            Levels[i].NeedsFullRedraw = true;
    }

    // Honor an explicit redraw request. On a new generation, flag every level for a banded rebuild
    // and latch the amortize budget. No-op if already serviced. Levels mid-rebuild keep their flag
    // and restart once the current band schedule drains (the render loop only starts when idle).
    void ServiceRedrawRequest(int64 globalGeneration, int32 amortize)
    {
        if (ServicedRedrawGeneration == globalGeneration)
            return;
        ServicedRedrawGeneration = globalGeneration;
        PendingAmortize = Math::Max(PendingAmortize, Math::Max(amortize, 1));
        for (int32 i = 0; i < LevelCount; i++)
            Levels[i].NeedsFullRedraw = true;
    }

    void DirtyBounds(const BoundingSphere& bounds)
    {
        // Project bounds into light-space and mark overlapping texels dirty
        // For now, just mark full redraw on any change (can be optimized later)
        for (int32 i = 0; i < LevelCount; i++)
        {
            float cx = Float3::Dot((Float3)bounds.Center, LightRight);
            float cy = Float3::Dot((Float3)bounds.Center, LightUp);
            float r = (float)bounds.Radius;

            float minX = (cx - r) / Levels[i].TexelSize;
            float maxX = (cx + r) / Levels[i].TexelSize;
            float minY = (cy - r) / Levels[i].TexelSize;
            float maxY = (cy + r) / Levels[i].TexelSize;

            // Check if bounds overlap with this level's coverage
            float scrollX = (float)Levels[i].ScrollTexels.X;
            float scrollY = (float)Levels[i].ScrollTexels.Y;
            float halfRes = Levels[i].Resolution * 0.5f;

            if (maxX >= scrollX - halfRes && minX <= scrollX + halfRes &&
                maxY >= scrollY - halfRes && minY <= scrollY + halfRes)
            {
                Levels[i].NeedsFullRedraw = true;
            }
        }
    }
};

// State for shadow cache sed to invalidate any prerendered shadow depths
struct ShadowAtlasLightCache
{
    bool StaticValid;
    bool DynamicValid;
    float ShadowsUpdateRate;
    float ShadowsUpdateRateAtDistance;
    uint32 ShadowFrame;
    float OuterConeAngle;
    Float3 Position;
    float Radius;
    Float3 Direction;
    float Distance;
    Float4 CascadeSplits;
    Float3 ViewDirection;
    // Stable shadow projection M11/M22 (reference FOV + aspect): cascade spheres are built from the
    // reference frustum (ShadowsCustomBuffer::StableShadowProjection), so this only changes when the
    // cascades actually move - aspect change, or the FOV latch growing past the reference. A gameplay
    // FOV lerp (ADS zoom) below the reference no longer touches it, keeping cached shadows valid
    // through the whole animation.
    Float2 ProjectionScale;
    int32 ShadowsResolution;

    void Set(const RenderView& view, const RenderLightData& light, const Float4& cascadeSplits = Float4::Zero, const Float2& projectionScale = Float2::Zero)
    {
        StaticValid = true;
        DynamicValid = true;
        Distance = light.ShadowsDistance;
        ShadowsUpdateRate = light.ShadowsUpdateRate;
        ShadowsUpdateRateAtDistance = light.ShadowsUpdateRateAtDistance;
        Direction = light.Direction;
        ShadowFrame = light.ShadowFrame;
        ShadowsResolution = light.ShadowsResolution;
        if (light.IsDirectionalLight)
        {
            // Sun
            Position = view.Position;
            ViewDirection = view.Direction;
            CascadeSplits = cascadeSplits;
            ProjectionScale = projectionScale;
        }
        else
        {
            // Local light
            const auto& localLight = (const RenderLocalLightData&)light;
            Position = light.Position;
            Radius = localLight.Radius;
            if (light.IsSpotLight)
                OuterConeAngle = ((const RenderSpotLightData&)light).OuterConeAngle;
        }
    }
};

// State for light's shadows rendering
struct ShadowAtlasLight
{
    // Static shadow map is created in 2 passes:
    // - once to check if any static objects are in-use per tile (ShadowAtlasLightTile::HasStaticGeometry)
    // - then to render those objects into the shadow map.
    // When any static objects gets modified in the light range the second step is repeated.
    // When light is changed then both steps are repeated.
    enum StaticStates
    {
        // Not using static shadow map at all.
        Unused,
        // Static objects are rendered separately to dynamic objects to check if light projections need to allocate static shadow map.
        WaitForGeometryCheck,
        // Static objects will be rendered into static shadow map.
        UpdateStaticShadow,
        // Static objects are up-to-date and can be copied from static shadow map.
        CopyStaticShadow,
        // None of the tiles has static geometry nearby.
        NoStaticGeometry,
        // One of the tiles failed to insert into static atlas so fallback to default dynamic logic.
        FailedToInsertTiles,
    };

    uint64 LastFrameUsed;
    int32 ContextIndex;
    int32 ContextCount;
    uint16 Resolution;
    uint16 StaticResolution;
    uint8 TilesNeeded;
    uint8 TilesCount;
    bool HasStaticShadowContext;
    bool BlendCSM;
    mutable StaticStates StaticState;
    BoundingSphere Bounds;
    float Sharpness, Fade, NormalOffsetScale, Bias, FadeDistance, Distance, TileBorder;
    // Sticky snapshot of light.ShadowsDistance at the last Distance recompute. Used to keep
    // atlasLight.Distance stable against per-frame View.Far transients (sub-passes, probes,
    // dynamic far auto-adjust) while still tracking genuine setting changes.
    float DistanceSettingAtSnapshot;
    float Softness; // PCSS apparent source size, in shadow-projection units
    bool RenderDynamic; // When false, skip per-frame dynamic cascade rendering (clipmap composite + static copy still run)
    float StableCascadeRadius[MAX_CSM_CASCADES]; // Hysteresis state for ComputeCascadeSphere (0 = unset)
    float LastRawCascadeRadius[MAX_CSM_CASCADES]; // Previous frame's raw (unquantized) cascade radius, for motion detection
    uint64 CascadeRadiusMotionFrame[MAX_CSM_CASCADES]; // Last frame the raw radius was actively changing (FOV animation)
    uint64 CascadeRadiusHighFrame[MAX_CSM_CASCADES]; // Last frame the raw radius was near the stable latch (tighten-dwell tracking)
    Float4 CascadeSplits;
    ShadowAtlasLightTile Tiles[SHADOWS_MAX_TILES];
    ShadowAtlasLightCache Cache;

    ShadowAtlasLight()
    {
        Platform::MemoryClear(this, sizeof(ShadowAtlasLight));
        RenderDynamic = true; // Default for non-directional lights; directional path overrides per-frame
    }

    POD_COPYABLE(ShadowAtlasLight);

    bool HasStaticGeometry() const
    {
        for (auto& tile : Tiles)
        {
            if (tile.HasStaticGeometry)
                return true;
        }
        return false;
    }

    float CalculateUpdateRateInv(const RenderLightData& light, float distanceFromView, bool& freezeUpdate) const
    {
        if (!GPU_SPREAD_WORKLOAD)
        {
            freezeUpdate = false;
            return 1.0f;
        }
        const float shadowsUpdateRate = light.ShadowsUpdateRate;
        const float shadowsUpdateRateAtDistance = shadowsUpdateRate * light.ShadowsUpdateRateAtDistance;
        float updateRate = Math::Lerp(shadowsUpdateRate, shadowsUpdateRateAtDistance, Math::Saturate(distanceFromView / Distance));
        updateRate *= Graphics::ShadowUpdateRate;
        freezeUpdate = updateRate <= ZeroTolerance;
        if (freezeUpdate)
            return 0.0f;
        return 1.0f / updateRate;
    }

    void ValidateCache(const RenderView& view, const RenderLightData& light, const Float2& projectionScale = Float2::Zero)
    {
        if (!Cache.StaticValid || !Cache.DynamicValid)
            return;
        if (!Math::NearEqual(Cache.Distance, light.ShadowsDistance) ||
            !Math::NearEqual(Cache.ShadowsUpdateRate, light.ShadowsUpdateRate) ||
            !Math::NearEqual(Cache.ShadowsUpdateRateAtDistance, light.ShadowsUpdateRateAtDistance) ||
            Cache.ShadowFrame != light.ShadowFrame ||
            Cache.ShadowsResolution != light.ShadowsResolution ||
            Float3::Dot(Cache.Direction, light.Direction) < SHADOWS_ROTATION_ERROR)
        {
            // Invalidate
            Cache.StaticValid = false;
        }
        if (light.IsDirectionalLight)
        {
            // Sun. Stable projection scale (reference FOV/aspect) is part of the key: cascade spheres
            // are built from the reference frustum, so the cache must drop when that frustum changes
            // (aspect change, FOV latch growth) - but a gameplay FOV lerp below the reference no
            // longer moves the cascades, so it no longer invalidates cached shadows either.
            if (!Float3::NearEqual(Cache.Position, view.Position, SHADOWS_POSITION_ERROR) ||
                !Float4::NearEqual(Cache.CascadeSplits, CascadeSplits) ||
                Float3::Dot(Cache.ViewDirection, view.Direction) < SHADOWS_ROTATION_ERROR ||
                !Float2::NearEqual(Cache.ProjectionScale, projectionScale))
            {
                // Invalidate
                Cache.StaticValid = false;
            }
        }
        else
        {
            // Local light
            const auto& localLight = (const RenderLocalLightData&)light;
            if (!Float3::NearEqual(Cache.Position, light.Position, SHADOWS_POSITION_ERROR) ||
                !Math::NearEqual(Cache.Radius, localLight.Radius))
            {
                // Invalidate
                Cache.StaticValid = false;
            }
            if (light.IsSpotLight && !Math::NearEqual(Cache.OuterConeAngle, ((const RenderSpotLightData&)light).OuterConeAngle))
            {
                // Invalidate
                Cache.StaticValid = false;
            }
        }
        Cache.DynamicValid &= Cache.StaticValid;
        for (int32 i = 0; i < TilesCount && !Cache.DynamicValid; i++)
        {
            auto& tile = Tiles[i];
            if (tile.CachedViewport != Viewport(tile.RectTile->X, tile.RectTile->Y, tile.RectTile->Width, tile.RectTile->Height))
            {
                // Invalidate
                Cache.DynamicValid = false;
            }
        }
    }
};

class ShadowsCustomBuffer : public RenderBuffers::CustomBuffer, public ISceneRenderingListener
{
public:
    int32 MaxShadowsQuality = 0;
    int32 Resolution = 0;
    int32 AtlasPixelsUsed = 0;
    int32 StaticAtlasPixelsUsed = 0;
    bool EnableStaticShadows = true;
    mutable bool ClearShadowMapAtlas = true;
    mutable bool ClearStaticShadowMapAtlas = false;
    Vector3 ViewOrigin;
    GPUTexture* ShadowMapAtlas = nullptr;
    GPUTexture* StaticShadowMapAtlas = nullptr;
    DynamicTypedBuffer ShadowsBuffer;
    GPUBufferView* ShadowsBufferView = nullptr;
    const ShadowsCustomBuffer* LinkedShadows = nullptr;
    RectPackAtlas<ShadowsAtlasRectTile> Atlas;
    RectPackAtlas<ShadowsAtlasRectTile> StaticAtlas;
    Dictionary<Guid, ShadowAtlasLight> Lights;
    ShadowClipmap Clipmap;
    // Per-frame clipmap ownership: there is a single ShadowClipmap per buffer, so with multiple
    // shadowed suns the first StaticShadows-enabled one each frame claims it and the others fall
    // back to plain CSM rendering (no static exclusion, no composite).
    uint64 ClipmapOwnerFrame = 0;
    Guid ClipmapOwnerLight = Guid::Empty;

    // FOV-stable shadow reference (see Camera.ReferenceFieldOfView). World-anchored shadow structures
    // (cascade spheres -> snap grids, clipmap extents, static-cache keys) must not track a per-frame
    // FOV lerp (ADS zoom, flinch): every radius change re-anchors texel grids and forces
    // clipmap/static-cache work that reads as shadows jumping. Latches the widest tan(fov/2) seen,
    // seeded by the camera property (session-max: an intentional base-FOV decrease keeps the wider
    // latch - slightly coarser texel density - until this buffer is recreated). Live FOV always wins
    // when wider so cascade coverage never clips. Model LOD and screen-size culling are stabilized
    // separately via RenderView::ReferenceFovScreenScaleSq.
    float LatchedShadowTanHalfFov = 0.0f;
    // Same idea for the weapon self-shadow ortho bounds: WeaponFOV lerps during ADS (eg. 30->12),
    // which would rescale the weapon shadow texel grid every frame of the zoom animation.
    float LatchedWeaponFov = 0.0f;
    // Main view's NonJitteredProjection widened to the reference FOV (== live projection when the
    // live FOV is the widest seen, or for ortho views). Refreshed at the top of SetupShadows.
    Matrix StableShadowProjection = Matrix::Identity;

    // Weapon self-shadowing support
    GPUTexture* WeaponShadowMapAtlas = nullptr;
    DynamicTypedBuffer WeaponShadowsBuffer;
    GPUBufferView* WeaponShadowsBufferView = nullptr;
    RectPackAtlas<ShadowsAtlasRectTile> WeaponAtlas;
    mutable bool ClearWeaponShadowMapAtlas = true;
    ShadowsAtlasRectTile* WeaponDirectionalLightTile = nullptr;

    ShadowsCustomBuffer()
        : ShadowsBuffer(1024, PixelFormat::R32G32B32A32_Float, false, TEXT("ShadowsBuffer"))
        , WeaponShadowsBuffer(128, PixelFormat::R32G32B32A32_Float, false, TEXT("WeaponShadowsBuffer"))
    {
        ShadowMapAtlas = GPUDevice::Instance->CreateTexture(TEXT("Shadow Map Atlas"));
        WeaponShadowMapAtlas = GPUDevice::Instance->CreateTexture(TEXT("Weapon Shadow Map Atlas"));
    }

    void ClearDynamic()
    {
        ClearShadowMapAtlas = true;
        ClearWeaponShadowMapAtlas = true;
        for (auto it = Lights.Begin(); it.IsNotEnd(); ++it)
        {
            auto& atlasLight = it->Value;
            atlasLight.Cache.DynamicValid = false;
            for (int32 i = 0; i < atlasLight.TilesCount; i++)
                atlasLight.Tiles[i].ClearDynamic();
        }
        Atlas.Clear();
        WeaponAtlas.Clear();
        WeaponDirectionalLightTile = nullptr;
        AtlasPixelsUsed = 0;
    }

    void ClearStatic()
    {
        ClearStaticShadowMapAtlas = true;
        for (auto it = Lights.Begin(); it.IsNotEnd(); ++it)
        {
            auto& atlasLight = it->Value;
            atlasLight.StaticState = ShadowAtlasLight::Unused;
            atlasLight.Cache.StaticValid = false;
            // Clear the STATIC tile pointers: StaticAtlas.Clear() below re-inits the rect-pack
            // nodes, so keeping StaticRectTile would leave it dangling (hit by the static-atlas
            // defrag path). Dynamic tiles stay valid - they live in the dynamic Atlas.
            for (int32 i = 0; i < atlasLight.TilesCount; i++)
                atlasLight.Tiles[i].ClearStatic();
        }
        StaticAtlas.Clear();
        StaticAtlasPixelsUsed = 0;
    }

    void Reset()
    {
        Lights.Clear();
        ClearDynamic();
        ClearStatic();
        ClipmapOwnerFrame = 0; // Re-run the ownership claim if this buffer sets up again this frame
    }

    void InitStaticAtlas()
    {
        const int32 atlasResolution = Math::Min(Resolution * 2, GPUDevice::Instance->Limits.MaximumTexture2DSize);
        if (StaticAtlas.Width == atlasResolution)
            return;
        StaticAtlas.Init(atlasResolution, atlasResolution);
        if (!StaticShadowMapAtlas)
            StaticShadowMapAtlas = GPUDevice::Instance->CreateTexture(TEXT("Static Shadow Map Atlas"));
        auto desc = ShadowMapAtlas->GetDescription();
        desc.Width = desc.Height = atlasResolution;
        if (StaticShadowMapAtlas->Init(desc))
        {
            LOG(Fatal, "Failed to setup shadow map of size {0}x{1} and format {2}", desc.Width, desc.Height, ScriptingEnum::ToString(desc.Format));
            return;
        }
        ClearStaticShadowMapAtlas = true;
    }

    void DirtyStaticBounds(const BoundingSphere& bounds)
    {
        PROFILE_CPU();
        // TODO: use octree to improve bounds-testing
        // TODO: build list of modified bounds and dirty them in batch on next frame start (ideally in async within shadows setup job)
        for (auto& e : Lights)
        {
            auto& atlasLight = e.Value;
            if ((atlasLight.StaticState == ShadowAtlasLight::CopyStaticShadow || atlasLight.StaticState == ShadowAtlasLight::NoStaticGeometry)
                && atlasLight.Bounds.Intersects(bounds))
            {
                // Invalidate static shadow
                atlasLight.Cache.StaticValid = false;
            }
        }

        // Also dirty clipmap if bounds intersect
        if (Clipmap.Enabled)
            Clipmap.DirtyBounds(bounds);
    }

    // Expand-only world-space AABB of all effectively-Shadow-static casters observed (scene scans
    // + listener events). Drives the clipmap's dynamic sun-axis depth interval so geometry of any
    // height/depth is captured. Never shrinks while listened scenes live; a scene clear resets it
    // and rescans the remaining scenes.
    BoundingBox StaticCastersBounds;
    bool StaticCastersBoundsValid = false;
    // Generation of Renderer::InvalidateStaticShadows last seen; a bump purges + rescans the bounds.
    // Expand-only bounds capture transient world-gen staging positions (actors parked kilometers
    // away mid-generation) - the game's explicit invalidate marks the settle point to rebuild from.
    int64 StaticCastersBoundsGeneration = 0;
    Array<SceneRendering*> ListenedScenes;

    void MergeStaticCasterBounds(const BoundingSphere& sphere)
    {
        BoundingBox box;
        BoundingBox::FromSphere(sphere, box);
        if (StaticCastersBoundsValid)
            BoundingBox::Merge(StaticCastersBounds, box, StaticCastersBounds);
        else
        {
            StaticCastersBounds = box;
            StaticCastersBoundsValid = true;
        }
    }

    // takeLock=false when the caller already runs inside the scene's PreRender..PostRender window
    // (SceneRendering::Draw(PreRender) holds Locker.ReadLock for the whole frame on this thread;
    // SRWLOCK shared acquisition is NOT recursion-safe when a writer is queued - deadlock).
    void ScanStaticCastersBounds(SceneRendering* scene, bool takeLock)
    {
        PROFILE_CPU();
        if (takeLock)
            scene->Locker.ReadLock();
        for (int32 category = 0; category < SceneRendering::MAX; category++)
        {
            for (const auto& e : scene->Actors[category])
            {
                if (e.Actor && IsDepthCasterType(e.Actor) && IsEffectivelyShadowStatic(e.Actor))
                {
                    // A single mis-flagged giant caster widens the depth interval for the whole
                    // session (D16 precision cost) - surface it so the asset can be fixed.
                    if (e.Bounds.Radius > METERS_TO_UNITS(1000.0f))
                        LOG(Warning, "[ClipmapStatic] huge Shadow-static caster bounds r={0} drives depth interval: path='{1}'", e.Bounds.Radius, BuildActorChain(e.Actor));
                    MergeStaticCasterBounds(e.Bounds);
                }
            }
        }
        if (takeLock)
            scene->Locker.ReadUnlock();
    }

    // Listen + seed: ListenSceneRendering does NOT replay actors already streamed in, so the first
    // attach scans the scene's current population (the on-load casters the listener never saw).
    // Caller must be inside rendering (scene came from List->Scenes => its read lock is held).
    void ListenScene(SceneRendering* scene)
    {
        if (ListenedScenes.Contains(scene))
            return;
        ListenedScenes.Add(scene);
        ListenSceneRendering(scene);
        ScanStaticCastersBounds(scene, false);
    }

    // Sun-axis (sun-z = dot(p, sunDir)) interval of all known static casters, origin-relative to
    // match the cascade/clipmap math. Returns false until the first caster has been observed.
    bool GetStaticCastersSunRange(const Float3& sunDir, const Vector3& origin, float& outMinZ, float& outMaxZ) const
    {
        if (!StaticCastersBoundsValid)
            return false;
        const Float3 center = (Float3)((StaticCastersBounds.Minimum + StaticCastersBounds.Maximum) * 0.5 - origin);
        const Float3 extent = (Float3)((StaticCastersBounds.Maximum - StaticCastersBounds.Minimum) * 0.5);
        const float cz = Float3::Dot(center, sunDir);
        const float ez = Math::Abs(extent.X * sunDir.X) + Math::Abs(extent.Y * sunDir.Y) + Math::Abs(extent.Z * sunDir.Z);
        outMinZ = cz - ez;
        outMaxZ = cz + ez;
        return true;
    }

    ~ShadowsCustomBuffer()
    {
        Reset();
        SAFE_DELETE_GPU_RESOURCE(ShadowMapAtlas);
        SAFE_DELETE_GPU_RESOURCE(StaticShadowMapAtlas);
        SAFE_DELETE_GPU_RESOURCE(WeaponShadowMapAtlas);
    }

    // An actor's own Shadow-static flag is only meaningful if its entire parent chain is also static
    // for Transform. A Shadow-static StaticModel under a moving BoneSocket (or any non-Transform-static
    // ancestor) effectively moves every frame and would dirty the static clipmap on every move - the
    // asset is mis-flagged. Returns false in that case so we treat the actor as dynamic for clipmap
    // purposes only (it still casts shadows via the dynamic path).
    static bool IsEffectivelyShadowStatic(Actor* a)
    {
        if (!a->HasStaticFlag(StaticFlags::Shadow))
            return false;
        // An animated model deforms every frame it plays - its depth can never be cached as
        // static, so Shadow-static on it is an oxymoron. Treat as dynamic regardless of flags.
        if (a->Is<AnimatedModel>())
            return false;
        // A particle effect spawns/moves/dies every frame - same oxymoron, treat as dynamic.
        if (a->Is<ParticleEffect>())
            return false;
        Actor* parent = a->GetParent();
        int32 depth = 0;
        while (parent && depth < 16)
        {
            if (!parent->HasStaticFlag(StaticFlags::Transform))
                return false;
            // An animated ancestor (skinned mesh, bone-socket rig) moves this actor every frame
            // even when the actor's own flags say static - so it can't be cached. Catches static
            // props/weapons parented under a droid or any AnimatedModel.
            if (parent->Is<AnimatedModel>())
                return false;
            parent = parent->GetParent();
            depth++;
        }
        return true;
    }

    // Bounds-merge filter for the clipmap's dynamic depth interval: only actor types that actually
    // rasterize into shadow depth. Lights, sky, probes, volumes etc. can carry the Shadow static
    // flag with giant bounds (sky sphere ~10km at origin) and never cast depth - merging them
    // permanently balloons the expand-only interval and destroys D16 shadow depth precision.
    static bool IsDepthCasterType(Actor* a)
    {
        return a->Is<ModelInstanceActor>() || a->Is<Foliage>() || a->Is<Terrain>();
    }

    // Full scene path (ancestor names joined). The bare actor Name is usually a generic type
    // label ("AnimatedModel"), useless for locating the offending instance - walk to the root.
    static String BuildActorChain(Actor* a)
    {
        String chain = a->GetName();
        Actor* parent = a->GetParent();
        int32 depth = 0;
        while (parent && depth < 8)
        {
            chain = parent->GetName() + TEXT("/") + chain;
            parent = parent->GetParent();
            depth++;
        }
        return chain;
    }

    // [ISceneRenderingListener]
    void OnSceneRenderingAddActor(Actor* a) override
    {
        // Surface the mis-flagged asset so it can be fixed at the source (the flag is ignored above).
        // Log the scene path + the backing model asset - the actor Name alone is just the type label.
        if (a->HasStaticFlag(StaticFlags::Shadow) && a->Is<AnimatedModel>())
        {
            auto* am = (AnimatedModel*)a;
            LOG(Warning, "[ClipmapStatic] AnimatedModel Shadow-static (ignored): path='{0}' model='{1}'. Clear the Shadow flag on this asset.",
                BuildActorChain(a), am->SkinnedModel ? am->SkinnedModel->GetPath() : String(TEXT("<none>")));
        }
        if (a->HasStaticFlag(StaticFlags::Shadow) && a->Is<ParticleEffect>())
            LOG(Warning, "[ClipmapStatic] ParticleEffect Shadow-static (ignored): path='{0}'. Clear the Shadow flag on this asset.", BuildActorChain(a));
        if (IsEffectivelyShadowStatic(a))
        {
            DirtyStaticBounds(a->GetSphere());
            if (IsDepthCasterType(a))
                MergeStaticCasterBounds(a->GetSphere());
        }
    }

    void OnSceneRenderingUpdateActor(Actor* a, const BoundingSphere& prevBounds, UpdateFlags flags) override
    {
        // Dirty static objects to redraw when changed (eg. material modification)
        if (IsEffectivelyShadowStatic(a))
        {
            const BoundingSphere curBounds = a->GetSphere();
            if (IsDepthCasterType(a))
                MergeStaticCasterBounds(curBounds);
            DirtyStaticBounds(prevBounds);
            DirtyStaticBounds(curBounds);
        }
        else if (flags & StaticFlags)
        {
            DirtyStaticBounds(a->GetSphere());
        }
    }

    void OnSceneRenderingRemoveActor(Actor* a) override
    {
        if (IsEffectivelyShadowStatic(a))
            DirtyStaticBounds(a->GetSphere());
    }

    void OnSceneRenderingClear(SceneRendering* scene) override
    {
        // Scene unloaded: reset and rescan what remains; the depth-mapping change routes through
        // the clipmap drift gate -> self-heal rebuild. Clipmap's quantized interval is expand-only
        // while valid, so drop it too or the shrink would never propagate.
        // Locked scan: this runs outside rendering (Clear holds only the CLEARED scene's write lock).
        ListenedScenes.Remove(scene);
        StaticCastersBoundsValid = false;
        Clipmap.SceneDepthValid = false;
        for (SceneRendering* s : ListenedScenes)
            ScanStaticCastersBounds(s, true);
    }
};

void ShadowsAtlasRectTile::OnInsert(ShadowsCustomBuffer* buffer, bool isStatic)
{
    IsStatic = isStatic;
    const int32 pixels = (int32)Width * (int32)Height;
    if (isStatic)
        buffer->StaticAtlasPixelsUsed += pixels;
    else
        buffer->AtlasPixelsUsed += pixels;
}

void ShadowsAtlasRectTile::OnFree(ShadowsCustomBuffer* buffer)
{
    const int32 pixels = (int32)Width * (int32)Height;
    if (IsStatic)
        buffer->StaticAtlasPixelsUsed -= pixels;
    else
        buffer->AtlasPixelsUsed -= pixels;
}

void ShadowAtlasLightTile::FreeDynamic(ShadowsCustomBuffer* buffer)
{
    if (RectTile)
    {
        buffer->Atlas.Free(RectTile, buffer);
        RectTile = nullptr;
    }
}

void ShadowAtlasLightTile::FreeStatic(ShadowsCustomBuffer* buffer)
{
    if (StaticRectTile)
    {
        buffer->StaticAtlas.Free(StaticRectTile, buffer);
        StaticRectTile = nullptr;
    }
}

String ShadowsPass::ToString() const
{
    return TEXT("ShadowsPass");
}

bool ShadowsPass::Init()
{
    // Create pipeline states
    _psShadowDir.CreatePipelineStates();
    _psShadowPoint.CreatePipelineStates();
    _psShadowPointInside.CreatePipelineStates();
    _psShadowSpot.CreatePipelineStates();
    _psShadowSpotInside.CreatePipelineStates();
    _depthBounds = GPUDevice::Instance->Limits.HasDepthBounds && GPUDevice::Instance->Limits.HasReadOnlyDepth;

    // Load assets
    _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Shadows"));
    _sphereModel = Content::LoadAsyncInternal<Model>(TEXT("Engine/Models/Sphere"));
    if (_shader == nullptr || _sphereModel == nullptr)
        return true;

#if COMPILE_WITH_DEV_ENV
    _shader.Get()->OnReloading.Bind<ShadowsPass, &ShadowsPass::OnShaderReloading>(this);
#endif

    // Select format for shadow maps
    _shadowMapFormat = PixelFormat::Unknown;
    for (const PixelFormat format : { PixelFormat::D16_UNorm, PixelFormat::D24_UNorm_S8_UInt, PixelFormat::D32_Float })
    {
        const auto formatTexture = PixelFormatExtensions::FindShaderResourceFormat(format, false);
        const auto formatFeaturesDepth = GPUDevice::Instance->GetFormatFeatures(format);
        const auto formatFeaturesTexture = GPUDevice::Instance->GetFormatFeatures(formatTexture);
        if (EnumHasAllFlags(formatFeaturesDepth.Support, FormatSupport::DepthStencil | FormatSupport::Texture2D | FormatSupport::TextureCube) &&
            EnumHasAllFlags(formatFeaturesTexture.Support, FormatSupport::ShaderSample | FormatSupport::ShaderSampleComparison))
        {
            _shadowMapFormat = format;
            break;
        }
    }
    if (_shadowMapFormat == PixelFormat::Unknown)
        LOG(Warning, "GPU doesn't support shadows rendering");

    return false;
}

bool ShadowsPass::setupResources()
{
    // Wait for the assets
    if (!_sphereModel->CanBeRendered() || !_shader->IsLoaded())
        return true;
    auto shader = _shader->GetShader();
    CHECK_INVALID_SHADER_PASS_CB_SIZE(shader, 0, Data);

    // Create pipeline stages
    GPUPipelineState::Description psDesc;
    if (!_psShadowDir.IsValid())
    {
        psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RG;
        psDesc.DepthWriteEnable = false;
        psDesc.DepthEnable = psDesc.DepthBoundsEnable = _depthBounds;
        if (_psShadowDir.Create(psDesc, shader, "PS_DirLight"))
            return true;
    }
    if (!_psShadowPoint.IsValid())
    {
        psDesc = GPUPipelineState::Description::DefaultNoDepth;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RG;
        psDesc.VS = shader->GetVS("VS_Model");
        psDesc.DepthEnable = true;
        psDesc.DepthBoundsEnable = _depthBounds;
        psDesc.CullMode = CullMode::Normal;
        if (_psShadowPoint.Create(psDesc, shader, "PS_PointLight"))
            return true;
        psDesc.DepthFunc = ComparisonFunc::Greater;
        psDesc.CullMode = CullMode::Inverted;
        if (_psShadowPointInside.Create(psDesc, shader, "PS_PointLight"))
            return true;
    }
    if (!_psShadowSpot.IsValid())
    {
        psDesc = GPUPipelineState::Description::DefaultNoDepth;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RG;
        psDesc.VS = shader->GetVS("VS_Model");
        psDesc.DepthEnable = true;
        psDesc.DepthBoundsEnable = _depthBounds;
        psDesc.CullMode = CullMode::Normal;
        if (_psShadowSpot.Create(psDesc, shader, "PS_SpotLight"))
            return true;
        psDesc.DepthFunc = ComparisonFunc::Greater;
        psDesc.CullMode = CullMode::Inverted;
        if (_psShadowSpotInside.Create(psDesc, shader, "PS_SpotLight"))
            return true;
    }
    if (_psDepthClear == nullptr)
    {
        psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = GPUDevice::Instance->QuadShader->GetPS("PS_DepthClear");
        psDesc.DepthEnable = true;
        psDesc.DepthWriteEnable = true;
        psDesc.DepthFunc = ComparisonFunc::Always;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::None;
        _psDepthClear = GPUDevice::Instance->CreatePipelineState();
        if (_psDepthClear->Init(psDesc))
            return true;
    }
    if (_psDepthCopy == nullptr)
    {
        psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = GPUDevice::Instance->QuadShader->GetPS("PS_DepthCopy");
        psDesc.DepthEnable = true;
        psDesc.DepthWriteEnable = true;
        psDesc.DepthFunc = ComparisonFunc::Always;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::None;
        _psDepthCopy = GPUDevice::Instance->CreatePipelineState();
        if (_psDepthCopy->Init(psDesc))
            return true;
    }
    if (_psClipmapComposite == nullptr)
    {
        psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = GPUDevice::Instance->QuadShader->GetPS("PS_ClipmapComposite");
        psDesc.DepthEnable = true;
        psDesc.DepthWriteEnable = true;
        psDesc.DepthFunc = ComparisonFunc::Always;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::None;
        _psClipmapComposite = GPUDevice::Instance->CreatePipelineState();
        if (_psClipmapComposite->Init(psDesc))
            return true;
    }

    return false;
}

void ShadowsPass::SetupRenderContext(RenderContext& renderContext, RenderContext& shadowContext, ShadowAtlasLight* atlasLight, RenderContext* dynamicContext)
{
    const auto& view = renderContext.View;

    // Use the current render view to sync model LODs with the shadow maps rendering stage
    // (LOD selection through it is FOV-stable via RenderView::ReferenceFovScreenScaleSq)
    shadowContext.LodProxyView = &renderContext.View;

    // Prepare properties
    auto& shadowView = shadowContext.View;
    if (dynamicContext)
    {
        // Duplicate dynamic view but with static only geometry
        shadowView = dynamicContext->View;
        shadowView.StaticFlagsMask = StaticFlags::Shadow;
        shadowView.StaticFlagsCompare = StaticFlags::Shadow;
    }
    else
    {
        shadowView.Flags = view.Flags;
        shadowView.StaticFlagsMask = view.StaticFlagsMask;
        shadowView.StaticFlagsCompare = view.StaticFlagsCompare;
        shadowView.RenderLayersMask = view.RenderLayersMask;
        shadowView.IsOfflinePass = view.IsOfflinePass;
        shadowView.ModelLODBias = view.ModelLODBias;
        shadowView.ModelLODDistanceFactor = view.ModelLODDistanceFactor;
        shadowView.Pass = DrawPass::Depth;
        shadowView.Origin = view.Origin;
        shadowView.CascadeIndex = -1; // default; cascade setup overrides per-cascade below
        if (atlasLight && atlasLight->StaticState != ShadowAtlasLight::Unused && atlasLight->StaticState != ShadowAtlasLight::FailedToInsertTiles)
        {
            // Draw only dynamic geometry
            shadowView.StaticFlagsMask = StaticFlags::Shadow;
            shadowView.StaticFlagsCompare = StaticFlags::None;
        }
    }
    shadowContext.List = RenderList::GetFromPool();
    shadowContext.Buffers = renderContext.Buffers;
    shadowContext.Task = renderContext.Task;
    shadowContext.List->Clear();
}

// Smallest depth difference the shadow map can reliably resolve: ~2 ulps of the depth format, in
// normalized depth units. With the analytic receiver-plane references this is the entire flat bias
// a directional light needs; everything slope-dependent is derived per-pixel in the shader.
static float GetShadowMapFormatUlp(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::D16_UNorm:
        return 1.0f / 65535.0f;
    case PixelFormat::D24_UNorm_S8_UInt:
        return 1.0f / 16777215.0f;
    default:
        return 1.0f / 8388608.0f; // D32_Float: 23-bit mantissa step below 1.0
    }
}

void ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderLightData& light, ShadowAtlasLight& atlasLight)
{
    // Copy light properties (NormalOffsetScale and Bias are packed per light type by the callers:
    // directional uses texel-relative units resolved in the shader, local lights keep legacy units)
    atlasLight.Sharpness = light.ShadowsSharpness;
    atlasLight.Fade = light.ShadowsStrength;
    atlasLight.Softness = light.ShadowsSoftness;
    atlasLight.FadeDistance = Math::Max(light.ShadowsFadeDistance, 0.1f);
    // Snapshot the View.Far-clamped distance only when the ShadowsDistance setting actually
    // changes (or on first frame). View.Far gets mutated mid-frame by sub-render passes
    // (probes, SDF, weapon view, etc.); without this stickiness, cascade radii ping-pong in
    // lockstep on every View.Far transient and force a full clipmap redraw every frame.
    // The cache invariant (Cache.Distance == light.ShadowsDistance, see ValidateCache) is
    // unaffected - we re-snapshot whenever the setting genuinely changes.
    if (atlasLight.Distance <= 0.0f || !Math::NearEqual(atlasLight.DistanceSettingAtSnapshot, light.ShadowsDistance))
    {
        atlasLight.Distance = Math::Min(renderContext.View.Far, light.ShadowsDistance);
        atlasLight.DistanceSettingAtSnapshot = light.ShadowsDistance;
    }
    atlasLight.Bounds.Center = light.Position + renderContext.View.Origin; // Keep bounds in world-space to properly handle DirtyStaticBounds
    atlasLight.Bounds.Radius = 0.0f;
}

bool ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderLocalLightData& light, ShadowAtlasLight& atlasLight)
{
    SetupLight(shadows, renderContext, renderContextBatch, (RenderLightData&)light, atlasLight);
    atlasLight.Bounds.Radius = light.Radius;

    // Local lights keep the legacy bias semantics existing content is tuned against: world-space
    // normal offset normalized by tile resolution, constant depth bias applied pre-divide in-shader.
    atlasLight.NormalOffsetScale = light.ShadowsNormalOffsetScale * NormalOffsetScaleTweak * (1.0f / (float)atlasLight.Resolution);
    atlasLight.Bias = light.ShadowsDepthBias;
    if (shadows.MaxShadowsQuality == 0)
        atlasLight.Bias *= 1.5f; // Adjust bias to account for lower shadow quality

    // Fade shadow on distance
    const float fadeDistance = Math::Max(light.ShadowsFadeDistance, 0.1f);
    const float dstLightToView = Float3::Distance(light.Position, renderContext.View.Position) - light.Radius;
    const float fade = 1 - Math::Saturate((dstLightToView - atlasLight.Distance + fadeDistance) / fadeDistance);
    atlasLight.Fade *= fade;

    // Update cached state (invalidate it if the light changed)
    atlasLight.ValidateCache(renderContext.View, light);

    // Update static shadow logic
    atlasLight.HasStaticShadowContext = shadows.EnableStaticShadows && EnumHasAllFlags(light.StaticFlags, StaticFlags::Shadow);
    if (atlasLight.HasStaticShadowContext)
    {
        // Calculate static resolution for the light based on the world-bounds, not view-dependant
        shadows.InitStaticAtlas();
        const int32 baseLightResolution = SHADOWS_BASE_LIGHT_RESOLUTION(shadows.Resolution) / 2;
        int32 staticResolution = Math::RoundToInt(Math::Saturate(light.Radius / METERS_TO_UNITS(10)) * baseLightResolution);
        staticResolution = Math::Clamp<int32>(staticResolution, atlasLight.Resolution, atlasLight.Resolution * 2); // Limit static shadow to be max x2 the current dynamic shadow res
        if (!Math::IsPowerOfTwo(staticResolution))
            staticResolution = Math::RoundUpToPowerOf2(staticResolution); // Round up to power of two to reduce fragmentation of the static atlas and redraws
        if (staticResolution != atlasLight.StaticResolution)
        {
            atlasLight.StaticResolution = staticResolution;
            atlasLight.StaticState = ShadowAtlasLight::Unused;
            for (auto& tile : atlasLight.Tiles)
                tile.FreeStatic(&shadows);
        }
    }
    else
        atlasLight.StaticState = ShadowAtlasLight::Unused;
    switch (atlasLight.StaticState)
    {
    case ShadowAtlasLight::Unused:
        if (atlasLight.HasStaticShadowContext)
            atlasLight.StaticState = ShadowAtlasLight::WaitForGeometryCheck;
        break;
    case ShadowAtlasLight::WaitForGeometryCheck:
        if (atlasLight.HasStaticGeometry())
        {
            shadows.InitStaticAtlas();

            // Allocate static shadow map slot for all used tiles
            for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
            {
                auto& tile = atlasLight.Tiles[tileIndex];
                if (tile.StaticRectTile == nullptr)
                {
                    tile.StaticRectTile = shadows.StaticAtlas.Insert(atlasLight.StaticResolution, atlasLight.StaticResolution, &shadows, true);
                    if (!tile.StaticRectTile)
                    {
                        // Failed to insert tile to switch back to the default rendering
                        atlasLight.StaticState = ShadowAtlasLight::FailedToInsertTiles;
                        for (int32 i = 0; i < tileIndex; i++)
                            atlasLight.Tiles[i].FreeStatic(&shadows);
                        break;
                    }
                }
            }
            if (atlasLight.StaticState == ShadowAtlasLight::WaitForGeometryCheck)
            {
                // Now we know the tiles with static geometry and we can render those
                atlasLight.StaticState = ShadowAtlasLight::UpdateStaticShadow;
            }
        }
        else
        {
            // Not using static geometry for this light shadows
            atlasLight.StaticState = ShadowAtlasLight::NoStaticGeometry;
        }
        break;
    case ShadowAtlasLight::CopyStaticShadow:
        // Light was modified so update the static shadows
        if (!atlasLight.Cache.StaticValid && atlasLight.HasStaticShadowContext)
            atlasLight.StaticState = ShadowAtlasLight::UpdateStaticShadow;
        break;
    }
    switch (atlasLight.StaticState)
    {
    case ShadowAtlasLight::NoStaticGeometry:
        // Light was modified so attempt to find the static shadow again
        if (!atlasLight.Cache.StaticValid && atlasLight.HasStaticShadowContext)
        {
            atlasLight.StaticState = ShadowAtlasLight::WaitForGeometryCheck;
            break;
        }
    case ShadowAtlasLight::CopyStaticShadow:
    case ShadowAtlasLight::FailedToInsertTiles:
        // Skip collecting static draws
        atlasLight.HasStaticShadowContext = false;
        break;
    }
    if (atlasLight.HasStaticShadowContext)
    {
        // If rendering finds any static draws then it will be set to true
        for (auto& tile : atlasLight.Tiles)
            tile.HasStaticGeometry = false;
    }

    // Calculate update rate based on the distance to the view
    bool freezeUpdate;
    const float updateRateInv = atlasLight.CalculateUpdateRateInv(light, dstLightToView, freezeUpdate);
    float& framesToUpdate = atlasLight.Tiles[0].FramesToUpdate; // Use the first tile for all local light projections to be in sync
    if ((framesToUpdate > 0.0f || freezeUpdate) && atlasLight.Cache.DynamicValid && !atlasLight.HasStaticShadowContext)
    {
        // Light state matches the cached state and the update rate allows us to reuse the cached shadow map so skip update
        if (!freezeUpdate)
            framesToUpdate -= 1.0f;
        for (auto& tile : atlasLight.Tiles)
            tile.SkipUpdate = true;
        return true;
    }
    framesToUpdate += updateRateInv - 1.0f;

    // Cache the current state
    atlasLight.Cache.Set(renderContext.View, light);
    for (int32 i = 0; i < atlasLight.TilesCount; i++)
    {
        auto& tile = atlasLight.Tiles[i];
        tile.SkipUpdate = false;
        tile.CachedViewport = Viewport(tile.RectTile->X, tile.RectTile->Y, tile.RectTile->Width, tile.RectTile->Height);
    }

    return false;
}

void ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderDirectionalLightData& light, ShadowAtlasLight& atlasLight)
{
    SetupLight(shadows, renderContext, renderContextBatch, (RenderLightData&)light, atlasLight);

    // Directional shadows use analytic receiver-plane references in the shader, so the flat bias is
    // just depth-format quantization (a known constant) plus whatever the user authors on top (for
    // LOD-mismatched or alpha-tested casters). Normal offset is authored in texels (10 = 1 texel)
    // and converted per-cascade in the shader, so both track cascade extent, resolution and FOV
    // instead of being tuned for one projection.
    atlasLight.NormalOffsetScale = light.ShadowsNormalOffsetScale * 0.1f;
    atlasLight.Bias = light.ShadowsDepthBias + 2.0f * GetShadowMapFormatUlp(Instance()->_shadowMapFormat);

    const auto& view = renderContext.View;
    // Graphics::Shadows::EnableClipmap off => no clipmap at all: statics render into the CSM cascades
    // every frame (the exclusion filter below is skipped since ownsEnabledClipmap stays false), i.e.
    // plain "CSM for everything". Toggleable live (DebugCommand) and via Graphics Settings.
    bool useClipmap = Graphics::Shadows::EnableClipmap && light.StaticShadows && shadows.EnableStaticShadows && EnumHasAllFlags(light.StaticFlags, StaticFlags::Shadow);
    // Single clipmap per shadows buffer: the first StaticShadows sun each frame claims ownership;
    // any other shadowed sun falls back to plain CSM rendering (its cascades keep static geometry).
    if (useClipmap)
    {
        if (shadows.ClipmapOwnerFrame != Engine::FrameCount)
        {
            shadows.ClipmapOwnerFrame = Engine::FrameCount;
            shadows.ClipmapOwnerLight = light.ID;
        }
        else if (shadows.ClipmapOwnerLight != light.ID)
            useClipmap = false;
    }
    const int32 csmCount = atlasLight.TilesCount;
    const auto shadowMapsSize = (float)atlasLight.Resolution;
    atlasLight.BlendCSM = Graphics::AllowCSMBlending;
#if USE_EDITOR
    // Disable cascades blending when baking lightmaps
    if (IsRunningRadiancePass)
        atlasLight.BlendCSM = false;
#elif PLATFORM_WEB || PLATFORM_SWITCH || PLATFORM_IOS || PLATFORM_ANDROID
    // Disable cascades blending on low-end platforms
    atlasLight.BlendCSM = false;
#endif

    // Calculate cascade splits
    const float minDistance = renderContext.View.Near;
    const float maxDistance = renderContext.View.Near + atlasLight.Distance;
    const float viewRange = renderContext.View.Far - renderContext.View.Near;
    float cascadeSplits[MAX_CSM_CASCADES];
    {
        PartitionMode partitionMode = light.PartitionMode;
        float splitDistance0 = light.Cascade1Spacing;
        float splitDistance1 = Math::Max(splitDistance0, light.Cascade2Spacing);
        float splitDistance2 = Math::Max(splitDistance1, light.Cascade3Spacing);
        float splitDistance3 = Math::Max(splitDistance2, light.Cascade4Spacing);

        // Compute the split distances based on the partitioning mode
        if (partitionMode == PartitionMode::Manual)
        {
            if (csmCount == 1)
            {
                cascadeSplits[0] = minDistance + splitDistance3 * maxDistance;
            }
            else if (csmCount == 2)
            {
                cascadeSplits[0] = minDistance + splitDistance1 * maxDistance;
                cascadeSplits[1] = minDistance + splitDistance3 * maxDistance;
            }
            else if (csmCount == 3)
            {
                cascadeSplits[0] = minDistance + splitDistance1 * maxDistance;
                cascadeSplits[1] = minDistance + splitDistance2 * maxDistance;
                cascadeSplits[2] = minDistance + splitDistance3 * maxDistance;
            }
            else if (csmCount == 4)
            {
                cascadeSplits[0] = minDistance + splitDistance0 * maxDistance;
                cascadeSplits[1] = minDistance + splitDistance1 * maxDistance;
                cascadeSplits[2] = minDistance + splitDistance2 * maxDistance;
                cascadeSplits[3] = minDistance + splitDistance3 * maxDistance;
            }
        }
        else if (partitionMode == PartitionMode::Logarithmic || partitionMode == PartitionMode::PSSM)
        {
            const float pssmFactor = 0.5f;
            const float lambda = partitionMode == PartitionMode::PSSM ? pssmFactor : 1.0f;
            const auto range = maxDistance - minDistance;
            const auto ratio = maxDistance / minDistance;
            // Blend weight toward the logarithmic split: 1 = fully logarithmic (tight near cascades),
            // 0 = uniform. Master used (1 - lambda) here, which inverted it - "Logarithmic" (lambda=1)
            // collapsed to uniform, so cascade 0 spanned the whole ShadowsDistance and near shadows went
            // coarse/blobby (only small objects kept a crisp shadow). Use lambda directly; PSSM stays 0.5.
            const auto logRatio = Math::Clamp(lambda, 0.0f, 1.0f);
            for (int32 cascadeLevel = 0; cascadeLevel < csmCount; cascadeLevel++)
            {
                // Compute cascade split (between znear and zfar)
                const float distribute = static_cast<float>(cascadeLevel + 1) / csmCount;
                float logZ = minDistance * Math::Pow(ratio, distribute);
                float uniformZ = minDistance + range * distribute;
                cascadeSplits[cascadeLevel] = Math::Lerp(uniformZ, logZ, logRatio);
            }
        }

        // Convert distance splits to ratios cascade in the range [0, 1]
        for (int32 i = 0; i < MAX_CSM_CASCADES; i++)
            cascadeSplits[i] = (cascadeSplits[i] - renderContext.View.Near) / viewRange;
    }
    atlasLight.CascadeSplits = renderContext.View.Near + Float4(cascadeSplits) * viewRange;

    // Update cached state (invalidate it if the light changed). Directional cascades are keyed on
    // the FOV-stable reference projection so a gameplay FOV lerp doesn't invalidate cached shadows.
    const Float2 stableProjectionScale(shadows.StableShadowProjection.M11, shadows.StableShadowProjection.M22);
    atlasLight.ValidateCache(renderContext.View, light, stableProjectionScale);

    // Update cascades to check which should be updated this frame
    atlasLight.ContextIndex = renderContextBatch.Contexts.Count();
    atlasLight.ContextCount = 0;
    for (int32 cascadeIndex = 0; cascadeIndex < csmCount; cascadeIndex++)
    {
        const float dstToCascade = atlasLight.CascadeSplits.Raw[cascadeIndex];
        bool freezeUpdate;
        const float updateRateInv = atlasLight.CalculateUpdateRateInv(light, dstToCascade, freezeUpdate);
        auto& tile = atlasLight.Tiles[cascadeIndex];
        if ((tile.FramesToUpdate > 0.0f || freezeUpdate) && atlasLight.Cache.DynamicValid)
        {
            // Light state matches the cached state and the update rate allows us to reuse the cached shadow map so skip update
            if (!freezeUpdate)
                tile.FramesToUpdate -= 1.0f;
            tile.SkipUpdate = true;
            continue;
        }
        tile.FramesToUpdate += updateRateInv - 1.0f;

        // Cache the current state
        tile.SkipUpdate = false;
        tile.CachedViewport = Viewport(tile.RectTile->X, tile.RectTile->Y, tile.RectTile->Width, tile.RectTile->Height);
        atlasLight.ContextCount++;
    }

    // Latch dynamic-shadow toggle; the clipmap-composite and static-copy paths still run
    // when DynamicShadows is off - only per-cascade dynamic geometry gets skipped.
    atlasLight.RenderDynamic = light.DynamicShadows;
    if (!atlasLight.RenderDynamic)
        atlasLight.ContextCount = 0;

    // Init shadow data. Allow non-dynamic paths (clipmap composite, static atlas copy) to still
    // process even when no dynamic cascade contexts will be added.
    if (atlasLight.ContextCount == 0 && !useClipmap && !atlasLight.HasStaticShadowContext)
        return;
    if (atlasLight.ContextCount > 0)
        renderContextBatch.Contexts.AddDefault(atlasLight.ContextCount);
    atlasLight.Cache.Set(renderContext.View, light, atlasLight.CascadeSplits, stableProjectionScale);

    // Get the 8 points of the view frustum in view-space (unproject from clip-space)
    Float3 frustumCornersVs[8];
    {
        Float3 frustumCornersCs[8] =
        {
            Float3(-1.0f,  1.0f, 0.0f),
            Float3(1.0f,  1.0f, 0.0f),
            Float3(1.0f, -1.0f, 0.0f),
            Float3(-1.0f, -1.0f, 0.0f),
            Float3(-1.0f,  1.0f, 1.0f),
            Float3(1.0f,  1.0f, 1.0f),
            Float3(1.0f, -1.0f, 1.0f),
            Float3(-1.0f, -1.0f, 1.0f),
        };
        // Unproject through the FOV-stable reference projection (not the live one): cascade spheres,
        // their texel-snap grids and the clipmap extents derived from these corners must not track a
        // per-frame FOV lerp (ADS zoom). The reference is only ever as narrow as the live FOV, so
        // coverage of the actual view frustum is preserved.
        Matrix invProjectionMatrix;
        Matrix::Invert(shadows.StableShadowProjection, invProjectionMatrix);
        for (int32 i = 0; i < 8; i++)
            Float3::TransformCoordinate(frustumCornersCs[i], invProjectionMatrix, frustumCornersVs[i]);
    }

    // Create the different view and projection matrices for each split.
    // Skipped wholesale when DynamicShadows is off - no dynamic contexts will be rendered,
    // so there's no point computing per-cascade view/proj or allocating shadow contexts.
    float splitMinRatio = 0;
    float splitMaxRatio = (minDistance - renderContext.View.Near) / viewRange;
    int32 contextIndex = 0;
    for (int32 cascadeIndex = 0; cascadeIndex < csmCount && atlasLight.RenderDynamic; cascadeIndex++)
    {
        const auto oldSplitMinRatio = splitMinRatio;
        splitMinRatio = splitMaxRatio;
        splitMaxRatio = cascadeSplits[cascadeIndex];

        auto& tile = atlasLight.Tiles[cascadeIndex];
        if (tile.SkipUpdate)
            continue;

        // Light-space basis (lightUp also feeds the LookAt below; shared with the snap + clipmap, see I1).
        Float3 lightRight, lightUp;
        ComputeLightBasis(light.Direction, lightRight, lightUp);

        // Cascade bounding sphere (center light-texel-snapped + radius), shared with the clipmap
        // init below so the two stay in lock-step (I1 + I12).
        const float csmOverlap = atlasLight.BlendCSM ? 0.2f : 0.1f;
        Float3 frustumCenter;
        float frustumRadius;
        ComputeCascadeSphere(frustumCornersVs, renderContext.View.IV, lightRight, lightUp, light.Direction, atlasLight.Resolution, splitMinRatio, splitMaxRatio, oldSplitMinRatio, csmOverlap, atlasLight.StableCascadeRadius[cascadeIndex], atlasLight.LastRawCascadeRadius[cascadeIndex], atlasLight.CascadeRadiusMotionFrame[cascadeIndex], atlasLight.CascadeRadiusHighFrame[cascadeIndex], frustumCenter, frustumRadius);

        // Cascade bounds are built around the sphere at the frustum center to reduce shadow shimmering
        Float3 maxExtents = Float3(frustumRadius);
        Float3 minExtents = -maxExtents;
        Float3 cascadeExtents = maxExtents - minExtents;

        Matrix shadowView, shadowProjection, shadowVP, cullingVP;

        // Create view matrix using the basis built above
        Matrix::LookAt(frustumCenter + light.Direction * minExtents.Z, frustumCenter, lightUp, shadowView);

        // Create viewport for culling with extended near/far planes due to culling issues (aka pancaking)
        const float cullRangeExtent = METERS_TO_UNITS(1000.0f);
        Matrix::OrthoOffCenter(minExtents.X, maxExtents.X, minExtents.Y, maxExtents.Y, -cullRangeExtent, cascadeExtents.Z + cullRangeExtent, shadowProjection);
        Matrix::Multiply(shadowView, shadowProjection, cullingVP);

        // Create projection matrix
        Matrix::OrthoOffCenter(minExtents.X, maxExtents.X, minExtents.Y, maxExtents.Y, 0.0f, cascadeExtents.Z, shadowProjection);

        // Snap the projection so the world origin lands exactly on a shadow texel (kills the sub-texel
        // edge crawl seen when the camera moves/rotates slowly). Folded into the ortho's row 4 and
        // computed in DOUBLE precision. The world origin's shadow-clip position carries camera-distance
        // magnitude, so the previous form (project (0,0,0) through shadowVP, Round in single precision)
        // lost ~0.1 texel once the camera was a few hundred metres from world origin (Flax world units
        // are cm) - reintroducing the exact shimmer the double-precision cascade-centre snap (I12) exists
        // to remove, and sliding the dynamic cascade off the clipmap composite (which anchors static
        // content to the same double-snapped centre). Recompute the origin's texel-space position
        // directly from frustumCenter in double - algebraically identical to the old origin projection
        // (ortho is symmetric, so origin texelX = -(frustumCenter.lightRight) * tpu), minus the
        // precision loss - so dynamic and static shadows share one world-anchored texel grid.
        // See: D:\code\notes\shadow_clipmap_assumptions.md (invariant I12).
        const double snapTpu = (double)shadowMapsSize / ((double)frustumRadius * 2.0);
        const double snapTexelX = ((double)frustumCenter.X * lightRight.X + (double)frustumCenter.Y * lightRight.Y + (double)frustumCenter.Z * lightRight.Z) * snapTpu;
        const double snapTexelY = ((double)frustumCenter.X * lightUp.X + (double)frustumCenter.Y * lightUp.Y + (double)frustumCenter.Z * lightUp.Z) * snapTpu;
        const double snapFracX = snapTexelX - Math::Round(snapTexelX);
        const double snapFracY = snapTexelY - Math::Round(snapTexelY);
        const Float4 roundOffset((float)(snapFracX * 2.0 / (double)shadowMapsSize), (float)(snapFracY * 2.0 / (double)shadowMapsSize), 0.0f, 0.0f);
        shadowProjection.SetRow4(shadowProjection.GetRow4() + roundOffset);

        // Calculate view*projection matrix
        Matrix::Multiply(shadowView, shadowProjection, shadowVP);
        tile.SetWorldToShadow(shadowVP);

        // Setup context for cascade
        auto& shadowContext = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
        SetupRenderContext(renderContext, shadowContext);
        shadowContext.View.Position = light.Direction * -atlasLight.Distance + renderContext.View.Position;
        shadowContext.View.Direction = light.Direction;
        shadowContext.View.SetUp(shadowView, shadowProjection);
        shadowContext.View.CullingFrustum.SetMatrix(cullingVP);
        // Tag cascade index (for RenderList best-fit grouping) and bias distant cascades to coarser LODs.
        // Safe because ModelDraw skips the LOD dither-transition in Pass=Depth (ShadowModelLODBias deprecated).
        // Starts at cascade 2: a coarser-LOD caster differs from the G-buffer receiver mesh by a
        // constant world-space amount, which the analytic receiver-plane bias cannot predict - at
        // narrow FOV (zoom) the visible scene lands on far cascades with tiny texels, so LOD-biased
        // near-ish cascades read as shadow acne no sampling math can remove.
        shadowContext.View.CascadeIndex = (int8)cascadeIndex;
        shadowContext.View.ModelLODBias += Math::Max(cascadeIndex - 1, 0);
        shadowContext.View.PrepareCache(shadowContext, shadowMapsSize, shadowMapsSize, Float2::Zero, &renderContext.View);
    }

    // Setup shadow clipmap for static geometry caching
    auto& clipmap = shadows.Clipmap;

    if (useClipmap)
    {
        // Per-cascade radii + centers for clipmap init. Uses the SAME ComputeCascadeSphere as the
        // dynamic cascade loop, so clipmap centers cannot drift from the rendered cascades (I1/I12
        // lock-step is now structural, not by-convention). Mismatch would misalign the composite.
        float cascadeRadii[MAX_CSM_CASCADES];
        Float3 cascadeFrustumCenters[MAX_CSM_CASCADES];
        {
            Float3 lightRightClip, lightUpClip;
            ComputeLightBasis(light.Direction, lightRightClip, lightUpClip);

            float splitMinRatio2 = 0;
            float splitMaxRatio2 = (minDistance - view.Near) / viewRange;
            for (int32 ci = 0; ci < csmCount; ci++)
            {
                const float oldSplitMin2 = splitMinRatio2;
                splitMinRatio2 = splitMaxRatio2;
                splitMaxRatio2 = cascadeSplits[ci];

                const float csmOverlap = atlasLight.BlendCSM ? 0.2f : 0.1f;
                ComputeCascadeSphere(frustumCornersVs, renderContext.View.IV, lightRightClip, lightUpClip, light.Direction, atlasLight.Resolution, splitMinRatio2, splitMaxRatio2, oldSplitMin2, csmOverlap, atlasLight.StableCascadeRadius[ci], atlasLight.LastRawCascadeRadius[ci], atlasLight.CascadeRadiusMotionFrame[ci], atlasLight.CascadeRadiusHighFrame[ci], cascadeFrustumCenters[ci], cascadeRadii[ci]);
            }
        }

        const PixelFormat clipmapFormat = shadows.ShadowMapAtlas->GetDescription().Format;
        float splitDistances[MAX_CSM_CASCADES];
        for (int32 i = 0; i < csmCount; i++)
            splitDistances[i] = atlasLight.CascadeSplits.Raw[i];
        // Sun-axis interval of all known static casters (origin-relative to match cascade math);
        // drives the clipmap's dynamic depth range so any verticality is captured.
        float sceneMinZ = 0.0f, sceneMaxZ = 0.0f;
        const bool sceneZValid = shadows.GetStaticCastersSunRange(light.Direction, renderContext.View.Origin, sceneMinZ, sceneMaxZ);
        clipmap.Init(clipmapFormat, csmCount, cascadeRadii, splitDistances, atlasLight.Resolution, light.Direction, sceneZValid, sceneMinZ, sceneMaxZ);
        clipmap.LightId = light.ID;

        if (clipmap.Enabled)
        {
            // Explicit-redraw policy, PLAY MODE ONLY: Init's heuristic triggers (sun/basis/drift/scene
            // edits) are advisory there. Clear them on populated, idle levels BEFORE ComputeScroll and
            // the compositor anchor (I4), so only cold-init (not yet populated), scroll-overflow
            // self-heal (set in ComputeScroll), and Renderer::InvalidateStaticShadows drive a rebuild.
            // In editor edit-mode the triggers ARE honored - scene edits and sun rotation must
            // regenerate the cache without the game's explicit request - but amortized on populated
            // levels so an edit never hitches the frame. In-flight rebuilds keep their request so a
            // mid-build re-request restarts once the current band schedule drains.
            {
                const bool playMode = Engine::IsPlayMode();
                for (int32 li = 0; li < clipmap.LevelCount; li++)
                {
                    auto& l = clipmap.Levels[li];
                    if (l.Populated && l.RedrawRowCursor < 0)
                    {
                        if (playMode)
                            l.NeedsFullRedraw = false;
                        else if (l.NeedsFullRedraw)
                            clipmap.PendingAmortize = Math::Max(clipmap.PendingAmortize, SHADOW_CLIPMAP_OVERFLOW_AMORTIZE);
                    }
                    // Honor an engine-detected self-heal (cumulative scale drift) AFTER the noise clear:
                    // genuine invalidation the game can't know about, rebuilt amortized then reconciled.
                    if (l.SelfHealRequested && l.RedrawRowCursor < 0)
                    {
                        l.NeedsFullRedraw = true;
                        l.SelfHealRequested = false;
                        clipmap.PendingAmortize = Math::Max(clipmap.PendingAmortize, SHADOW_CLIPMAP_OVERFLOW_AMORTIZE);
                    }
                }
            }

            clipmap.ComputeScroll(view.Position);

            // Service an explicit redraw request (generation bump). The warp/fade countdown ticks at
            // the top of SetupShadows - independent of any clipmap being enabled.
            clipmap.ServiceRedrawRequest(Platform::AtomicRead(&StaticShadowRedrawGeneration), Platform::AtomicRead(&StaticShadowRedrawAmortize));

            // Compute compositing parameters per cascade level
            for (int32 ci = 0; ci < csmCount; ci++)
            {
                auto& level = clipmap.Levels[ci];

                // Pre-set TextureOriginTexels to the value the rasterizer's full-rebuild branch
                // will assign later this frame. Compositor params (WrapOffsetUV especially) are
                // computed here, BEFORE the rasterize runs - if we used the stale origin, the
                // composite would read freshly-rebuilt content through last-frame's toroidal-wrap
                // math, producing a 1-frame glitch on every invalidation event. The full-rebuild
                // branch in the rasterize loop redundantly re-assigns the same value (kept there
                // for locality with the rect bounds that derive from it).
                // See: D:\code\notes\shadow_clipmap_assumptions.md (invariant I4 - WrapOffsetUV
                // coherence with the rasterizer's anchor).
                if (level.NeedsFullRedraw)
                {
                    level.TextureOriginTexels = Int2(
                        level.ScrollTexels.X - level.Resolution / 2,
                        level.ScrollTexels.Y + level.Resolution / 2);
                }

                // Cascade center in light-space XY
                Float2 cascadeCenterLight(
                    Float3::Dot(cascadeFrustumCenters[ci], clipmap.LightRight),
                    Float3::Dot(cascadeFrustumCenters[ci], clipmap.LightUp)
                );

                // Clipmap center in light-space XY
                Float2 clipCenterLight(
                    level.ScrollTexels.X * level.TexelSize,
                    level.ScrollTexels.Y * level.TexelSize
                );

                // UV scale = cascade diameter / clipmap extent
                float scale = 2.0f * cascadeRadii[ci] / level.WorldExtent;

                // UV offset: maps cascade tile TexCoord to clipmap texture UV
                // X uses +dx, Y uses -dy due to texture Y-down convention
                float dx = cascadeCenterLight.X - clipCenterLight.X;
                float dy = cascadeCenterLight.Y - clipCenterLight.Y;
                float halfScaleComplement = 0.5f * (1.0f - scale);
                Float2 uvOffset(dx / level.WorldExtent + halfScaleComplement,
                                -dy / level.WorldExtent + halfScaleComplement);

                level.CompositingColor = Float4(scale, scale, uvOffset.X, uvOffset.Y);

                // Content-serve check: the cascade window must lie fully inside the cached window
                // (logical UV in [0,1]) and the content must be complete and correctly anchored.
                // Otherwise this cascade falls back: statics render dynamically, composite skipped.
                // This is the correctness backstop that makes any radius/FOV change safe regardless
                // of rebuild timing - the hysteresis above is purely a cost optimization.
                const bool covered = uvOffset.X >= 0.0f && uvOffset.Y >= 0.0f &&
                    uvOffset.X + scale <= 1.0f && uvOffset.Y + scale <= 1.0f;
                const bool fallback = !level.Populated || level.RedrawRowCursor >= 0 || level.NeedsFullRedraw || !covered;
                if (fallback != level.FallbackActive)
                {
                    // Serve-state flip: tiles that skip updates hold content built for the other
                    // mode; force a re-render next frame. Radius-driven flips already re-render
                    // this frame via the projection-scale cache key.
                    atlasLight.Cache.DynamicValid = false;
                    level.FallbackActive = fallback;
                }

                // Toroidal wrap offset: shader does texUV = frac(logicalUV + WrapOffsetUV).
                // Logical uv.x=0 maps to world-texel-X = ScrollTexels.X - R/2 (left edge of rect).
                // Texture pixel X for world-texel-X w.X = ((w.X - origin.X) mod R + R) mod R.
                // Continuous: texU = frac((leftEdgeWorldTexel.X - origin.X)/R + uv.x).
                // For Y (Y-flipped): texV = frac((origin.Y - topEdgeWorldTexel.Y)/R + uv.y).
                // topEdgeWorldTexel.Y = ScrollTexels.Y + R/2 - 1; using origin.Y as "row above top".
                const float rInv = 1.0f / (float)level.Resolution;
                level.WrapOffsetUV = Float2(
                    Math::Frac((float)(level.ScrollTexels.X - level.Resolution / 2 - level.TextureOriginTexels.X) * rInv),
                    Math::Frac((float)(level.TextureOriginTexels.Y - (level.ScrollTexels.Y + level.Resolution / 2 - 1) - 1) * rInv));

                // Depth remap: cascadeDepth = clipmapDepth * A + B
                // Clipmap covers [DepthMinZ, DepthMinZ + DepthRange] along SunDir axis (ortho near=0
                // at sun-z DepthMinZ; see the level fields + RenderClipmapStrip - MUST stay in sync).
                // Cascade covers [frustumCenter - R, frustumCenter + R] along SunDir axis (near=0, far=2R).
                // General formula: A = clipmapWorldRange / cascadeWorldRange,
                //                  B = (clipmapNearWorldZ - cascadeNearWorldZ) / cascadeWorldRange.
                const float cascadeRadius = cascadeRadii[ci];
                const float clipmapWorldRange = level.DepthRange;
                const float cascadeWorldRange = 2.0f * cascadeRadius;
                const float clipmapNearWorldZ = level.DepthMinZ;
                const float frustumCenterLightZ = Float3::Dot(cascadeFrustumCenters[ci], clipmap.SunDir);
                const float cascadeNearWorldZ = frustumCenterLightZ - cascadeRadius;
                const float depthA = clipmapWorldRange / cascadeWorldRange;
                const float depthB = (clipmapNearWorldZ - cascadeNearWorldZ) / cascadeWorldRange;
                level.DepthRemap = Float2(depthA, depthB);
            }

        }
    }
    else if (clipmap.LightId == light.ID)
    {
        // Only the clipmap's owner may disable it - a second shadowed sun without static shadows
        // must not clobber the owner's enabled clipmap mid-frame.
        clipmap.Enabled = false;
    }

    // Cascade static-exclusion filter. Applied when:
    //   - the clipmap is providing static shadows FOR THIS LIGHT (don't double-render in cascade), OR
    //   - StaticShadows is off (user explicitly wants no static contribution from any path).
    // NOT applied when StaticShadows is on but clipmap failed to enable (e.g. light not flagged
    // Static, atlas alloc failed) or another sun owns the clipmap - in those cases let the cascade
    // render static the old way so we don't silently drop shadows (a non-owning sun gets no
    // composite, so excluding static geometry would lose its static shadows entirely). Skipped
    // when DynamicShadows is off (no contexts allocated).
    const bool ownsEnabledClipmap = clipmap.Enabled && clipmap.LightId == light.ID;
    if (atlasLight.RenderDynamic && (ownsEnabledClipmap || !light.StaticShadows))
    {
        int32 ctxIdx = 0;
        for (int32 ci = 0; ci < csmCount; ci++)
        {
            auto& tile = atlasLight.Tiles[ci];
            if (tile.SkipUpdate)
                continue;
            auto& shadowCtx = renderContextBatch.Contexts[atlasLight.ContextIndex + ctxIdx++];
            // While this cascade's clipmap level can't serve it (FallbackActive: coverage gap after
            // a zoom, rebuild in flight, cold), keep statics in the dynamic draw - the composite is
            // skipped for it in the render loop, so excluding them would drop static shadows.
            // Not applicable when the user disabled static shadows entirely (exclude regardless).
            if (ownsEnabledClipmap && light.StaticShadows && ci < clipmap.LevelCount && clipmap.Levels[ci].FallbackActive)
                continue;
            shadowCtx.View.StaticFlagsMask = StaticFlags::Shadow;
            shadowCtx.View.StaticFlagsCompare = StaticFlags::None;
        }
    }
}

void ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderPointLightData& light, ShadowAtlasLight& atlasLight)
{
    if (SetupLight(shadows, renderContext, renderContextBatch, (RenderLocalLightData&)light, atlasLight))
        return;

    // Prevent sampling shadow map at borders that includes nearby data due to filtering of virtual cubemap sides
    atlasLight.TileBorder = 1.0f * (shadows.MaxShadowsQuality + 1);
    const float borderScale = (float)atlasLight.Resolution / (atlasLight.Resolution + 2 * atlasLight.TileBorder);
    Matrix borderScaleMatrix;
    Matrix::Scaling(borderScale, borderScale, 1.0f, borderScaleMatrix);

    atlasLight.ContextIndex = renderContextBatch.Contexts.Count();
    atlasLight.ContextCount = atlasLight.HasStaticShadowContext ? 12 : 6;
    renderContextBatch.Contexts.AddDefault(atlasLight.ContextCount);

    // Render depth to all 6 faces of the cube map
    int32 contextIndex = 0;
    for (int32 faceIndex = 0; faceIndex < 6; faceIndex++)
    {
        auto& shadowContext = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
        SetupRenderContext(renderContext, shadowContext, &atlasLight);
        shadowContext.View.SetUpCube(LocalLightNearPlane, light.Radius, light.Position);

        // Apply border to the projection matrix
        shadowContext.View.Projection = shadowContext.View.Projection * borderScaleMatrix;
        shadowContext.View.NonJitteredProjection = shadowContext.View.Projection;
        Matrix::Invert(shadowContext.View.Projection, shadowContext.View.IP);

        shadowContext.View.SetFace(faceIndex);
        const auto shadowMapsSize = (float)atlasLight.Resolution;
        shadowContext.View.PrepareCache(shadowContext, shadowMapsSize, shadowMapsSize, Float2::Zero, &renderContext.View);
        atlasLight.Tiles[faceIndex].SetWorldToShadow(shadowContext.View.ViewProjection());

        // Draw static geometry separately to be cached
        if (atlasLight.HasStaticShadowContext)
        {
            auto& shadowContextStatic = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
            SetupRenderContext(renderContext, shadowContextStatic, &atlasLight, &shadowContext);
        }
    }
}

void ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderSpotLightData& light, ShadowAtlasLight& atlasLight)
{
    if (SetupLight(shadows, renderContext, renderContextBatch, (RenderLocalLightData&)light, atlasLight))
        return;

    atlasLight.ContextIndex = renderContextBatch.Contexts.Count();
    atlasLight.ContextCount = atlasLight.HasStaticShadowContext ? 2 : 1;
    renderContextBatch.Contexts.AddDefault(atlasLight.ContextCount);

    // Render depth to a single projection
    auto& shadowContext = renderContextBatch.Contexts[atlasLight.ContextIndex];
    SetupRenderContext(renderContext, shadowContext, &atlasLight);
    shadowContext.View.SetProjector(LocalLightNearPlane, light.Radius, light.Position, light.Direction, light.UpVector, light.OuterConeAngle * 2.0f);
    shadowContext.View.PrepareCache(shadowContext, atlasLight.Resolution, atlasLight.Resolution, Float2::Zero, &renderContext.View);
    atlasLight.Tiles[0].SetWorldToShadow(shadowContext.View.ViewProjection());

    // Draw static geometry separately to be cached
    if (atlasLight.HasStaticShadowContext)
    {
        auto& shadowContextStatic = renderContextBatch.Contexts[atlasLight.ContextIndex + 1];
        SetupRenderContext(renderContext, shadowContextStatic, &atlasLight, &shadowContext);
    }
}

void ShadowsPass::ClearShadowMapTile(GPUContext* context, GPUConstantBuffer* quadShaderCB, QuadShaderData& quadShaderData) const
{
    // Color.r is used by PS_DepthClear in Quad shader to clear depth
    quadShaderData.Color = Float4::One;
    context->UpdateCB(quadShaderCB, &quadShaderData);
    context->BindCB(0, quadShaderCB);

    // Clear tile depth
    context->SetState(_psDepthClear);
    context->DrawFullscreenTriangle();
}

void ShadowsPass::CopyShadowMapTile(GPUContext* context, GPUConstantBuffer* quadShaderCB, QuadShaderData& quadShaderData, const GPUTexture* srcShadowMap, const ShadowsAtlasRectTile* srcTile) const
{
    // Color.xyzw is used by PS_DepthCopy in Quad shader to scale input texture UVs
    const float staticAtlasResolutionInv = 1.0f / (float)srcShadowMap->Width();
    quadShaderData.Color = Float4(srcTile->Width, srcTile->Height, srcTile->X, srcTile->Y) * staticAtlasResolutionInv;
    context->UpdateCB(quadShaderCB, &quadShaderData);
    context->BindCB(0, quadShaderCB);

    // Copy tile depth
    context->BindSR(0, srcShadowMap->View());
    context->SetState(_psDepthCopy);
    context->DrawFullscreenTriangle();
}

void ShadowsPass::Dispose()
{
    // Base
    RendererPass::Dispose();

    // Cleanup
    _psShadowDir.Delete();
    _psShadowPoint.Delete();
    _psShadowPointInside.Delete();
    _psShadowSpot.Delete();
    _psShadowSpotInside.Delete();
    _shader = nullptr;
    _sphereModel = nullptr;
    SAFE_DELETE_GPU_RESOURCE(_psDepthClear);
    SAFE_DELETE_GPU_RESOURCE(_psDepthCopy);
    SAFE_DELETE_GPU_RESOURCE(_psClipmapComposite);
}

void ShadowsPass::SetupShadows(RenderContext& renderContext, RenderContextBatch& renderContextBatch)
{
    PROFILE_CPU();

    // Early out and skip shadows setup if no lights is actively casting shadows
    // RenderBuffers will automatically free any old ShadowsCustomBuffer after a few frames if we don't update LastFrameUsed
    Array<RenderLightData*, RendererAllocation> shadowedLights;
    if (_shadowMapFormat != PixelFormat::Unknown && EnumHasAllFlags(renderContext.View.Flags, ViewFlags::Shadows) && !checkIfSkipPass())
    {
        for (auto& light : renderContext.List->DirectionalLights)
        {
            if (light.CanRenderShadow(renderContext.View))
                shadowedLights.Add(&light);
        }
        for (auto& light : renderContext.List->SpotLights)
        {
            if (light.CanRenderShadow(renderContext.View))
                shadowedLights.Add(&light);
        }
        for (auto& light : renderContext.List->PointLights)
        {
            if (light.CanRenderShadow(renderContext.View))
                shadowedLights.Add(&light);
        }
    }
    const auto currentFrame = Engine::FrameCount;

    // Tick the explicit-redraw warp/fade countdown once per frame, BEFORE the no-lights early-out and
    // unconditional on any clipmap enabling. The old site ticked only inside the clipmap.Enabled block,
    // so a request made while the clipmap never enabled (between levels, before the owner sun is set up,
    // StaticShadows momentarily off, or the sun culled the same frame) left the countdown stuck > 0
    // forever - wedging AreStaticShadowsRedrawing() and any warp/fade cover gated on it. Frame-guarded
    // so multiple views/tasks in one frame don't over-decrement.
    if (StaticShadowRedrawCountdownFrame != (int64)currentFrame)
    {
        StaticShadowRedrawCountdownFrame = (int64)currentFrame;
        if (Platform::AtomicRead(&StaticShadowRedrawCountdown) > 0)
            Platform::InterlockedDecrement(&StaticShadowRedrawCountdown);
    }

    if (shadowedLights.IsEmpty())
    {
        // Invalidate any existing custom buffer that could have been used by the same task (eg. when rendering 6 sides of env probe)
        if (auto* old = (ShadowsCustomBuffer*)renderContext.Buffers->FindCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false))
        {
            if (old->LastFrameUsed == currentFrame)
                old->LastFrameUsed = 0;
        }
        return;
    }

    // Initialize shadow atlas
    auto& shadows = *renderContext.Buffers->GetCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false);
    shadows.LinkedShadows = renderContext.Buffers->FindLinkedBuffer<ShadowsCustomBuffer>(TEXT("Shadows"));
    if (shadows.LinkedShadows && (shadows.LinkedShadows->LastFrameUsed != currentFrame || shadows.LinkedShadows->ViewOrigin != renderContext.View.Origin))
        shadows.LinkedShadows = nullptr; // Don't use incompatible linked shadows buffer
    if (shadows.LastFrameUsed == currentFrame)
        shadows.Reset();
    shadows.LastFrameUsed = currentFrame;

    // Refresh the FOV-stable shadow reference (see the field comments): widen the main projection
    // laterally to the reference FOV, preserving aspect ratio and the depth rows.
    shadows.StableShadowProjection = renderContext.View.NonJitteredProjection;
    if (shadows.StableShadowProjection.M44 == 0.0f && shadows.StableShadowProjection.M22 > ZeroTolerance) // Perspective views only
    {
        const float liveTanHalfFov = 1.0f / shadows.StableShadowProjection.M22;
        const float refTanHalfFov = renderContext.View.ReferenceFOV > 0.0f ? Math::Tan(renderContext.View.ReferenceFOV * DegreesToRadians * 0.5f) : 0.0f;
        shadows.LatchedShadowTanHalfFov = Math::Max(shadows.LatchedShadowTanHalfFov, Math::Max(liveTanHalfFov, refTanHalfFov));
        const float fovScale = liveTanHalfFov / shadows.LatchedShadowTanHalfFov;
        if (fovScale < 1.0f - ZeroTolerance)
        {
            shadows.StableShadowProjection.M11 *= fovScale;
            shadows.StableShadowProjection.M22 *= fovScale;
        }
    }
    // Listen to scene changes early (first attach also seeds the static-caster bounds from the
    // scene's current population) so the clipmap's dynamic depth interval is valid the same frame
    // the lights below set up. Also invalidates static shadows on scene edits.
    // An InvalidateStaticShadows generation bump (game's world-settle signal) purges the caster
    // bounds and rescans: expand-only bounds would otherwise keep world-gen staging positions
    // forever, ballooning the depth interval (and with it the D16 depth quantum). Scenes in
    // List->Scenes are read-locked for this render, so the rescan takes no lock.
    const int64 redrawGeneration = Platform::AtomicRead(&StaticShadowRedrawGeneration);
    const bool purgeCasterBounds = shadows.StaticCastersBoundsGeneration != redrawGeneration;
    if (purgeCasterBounds)
    {
        shadows.StaticCastersBoundsGeneration = redrawGeneration;
        shadows.StaticCastersBoundsValid = false;
        shadows.Clipmap.SceneDepthValid = false; // allow the quantized interval to shrink
    }
    for (SceneRendering* scene : renderContext.List->Scenes)
        shadows.ListenScene(scene);
    if (purgeCasterBounds)
        for (SceneRendering* scene : renderContext.List->Scenes)
            shadows.ScanStaticCastersBounds(scene, false);
    shadows.MaxShadowsQuality = Math::Clamp(Math::Min<int32>((int32)Graphics::ShadowsQuality, (int32)renderContext.View.MaxShadowsQuality), 0, (int32)Quality::MAX - 1);
    shadows.EnableStaticShadows = !renderContext.View.IsOfflinePass && !renderContext.View.IsSingleFrame && !shadows.LinkedShadows;
    int32 atlasResolution;
    switch (Graphics::ShadowMapsQuality)
    {
    case Quality::Low:
        atlasResolution = 1024;
        break;
    case Quality::Medium:
        atlasResolution = 2048;
        break;
    case Quality::High:
        atlasResolution = 4096;
        break;
    case Quality::Ultra:
        atlasResolution = 8192;
        break;
    default:
        return;
    }
    atlasResolution = Math::Min(atlasResolution, GPUDevice::Instance->Limits.MaximumTexture2DSize);
    if (shadows.Resolution != atlasResolution)
    {
        shadows.Reset();
        shadows.Atlas.Reset();
        shadows.StaticAtlas.Reset();
        auto desc = GPUTextureDescription::New2D(atlasResolution, atlasResolution, _shadowMapFormat, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil);
        if (shadows.ShadowMapAtlas->Init(desc))
        {
            LOG(Fatal, "Failed to setup shadow map of size {0}x{1} and format {2}", desc.Width, desc.Height, ScriptingEnum::ToString(desc.Format));
            return;
        }
#if PLATFORM_WEB
        // Hack to fix WebGPU limitation that requires to specify different sampler type manually to sample depth texture
        SetWebGPUTextureViewSampler(shadows.ShadowMapAtlas->View(), GPU_WEBGPU_SAMPLER_TYPE_DEPTH);
#endif
        shadows.ClearShadowMapAtlas = true;
        shadows.Resolution = atlasResolution;
        shadows.ViewOrigin = renderContext.View.Origin;
    }
    if (renderContext.View.Origin != shadows.ViewOrigin)
    {
        // Large Worlds chunk movement so invalidate cached shadows
        shadows.Reset();
        shadows.ViewOrigin = renderContext.View.Origin;
    }
    if (shadows.StaticAtlas.Width != 0 && (float)shadows.StaticAtlasPixelsUsed / (shadows.StaticAtlas.Width * shadows.StaticAtlas.Height) < SHADOWS_MAX_STATIC_ATLAS_CAPACITY_TO_DEFRAG)
    {
        // Defragment static shadow atlas if it failed to insert any light but it's still should have space
        bool anyStaticFailed = false;
        for (auto& e : shadows.Lights)
        {
            if (e.Value.StaticState == ShadowAtlasLight::FailedToInsertTiles)
            {
                anyStaticFailed = true;
                break;
            }
        }
        if (anyStaticFailed)
        {
            shadows.ClearStatic();
        }
    }
    if (!shadows.Atlas.IsInitialized())
        shadows.Atlas.Init(atlasResolution, atlasResolution);

    // Update/add lights
    const int32 baseLightResolution = SHADOWS_BASE_LIGHT_RESOLUTION(atlasResolution);
    for (const RenderLightData* light : shadowedLights)
    {
        auto& atlasLight = shadows.Lights[light->ID];

        // Calculate resolution for this light
        atlasLight.Resolution = light->ShadowsResolution;
        if (atlasLight.Resolution == 0)
        {
            // ScreenSize-based automatic shadowmap resolution
            atlasLight.Resolution = QuantizeResolution(baseLightResolution * light->ScreenSize);
        }

        // Cull too small lights
        if (atlasLight.Resolution < SHADOWS_MIN_RESOLUTION)
            continue;

        if (light->IsDirectionalLight)
        {
            const auto* dirLight = (const RenderDirectionalLightData*)light;
            atlasLight.TilesNeeded = Math::Clamp(dirLight->CascadeCount, 1, MAX_CSM_CASCADES);

            // Views with orthographic cameras cannot use cascades, we force it to 1 shadow map here
            if (renderContext.View.IsOrthographicProjection())
                atlasLight.TilesNeeded = 1;
        }
        else if (light->IsPointLight)
            atlasLight.TilesNeeded = 6;
        else
            atlasLight.TilesNeeded = 1;
        atlasLight.LastFrameUsed = currentFrame;
    }

    // Remove unused lights (before inserting any new ones to make space in the atlas)
    for (auto it = shadows.Lights.Begin(); it.IsNotEnd(); ++it)
    {
        if (it->Value.LastFrameUsed != currentFrame)
        {
            for (ShadowAtlasLightTile& tile : it->Value.Tiles)
                tile.Free(&shadows);
            shadows.Lights.Remove(it);
        }
    }

    // Calculate size requirements for atlas
    int32 atlasPixelsNeeded = 0;
    for (auto it = shadows.Lights.Begin(); it.IsNotEnd(); ++it)
    {
        const auto& atlasLight = it->Value;
        atlasPixelsNeeded += atlasLight.Resolution * atlasLight.Resolution * atlasLight.TilesNeeded;
    }
    const int32 atlasPixelsAllowed = atlasResolution * atlasResolution;
    const float atlasPixelsCoverage = (float)atlasPixelsNeeded / atlasPixelsAllowed;

    // If atlas is overflown then scale down the shadows resolution
    float resolutionScale = 1.0f;
    if (atlasPixelsCoverage > 1.0f)
        resolutionScale /= atlasPixelsCoverage;
    float finalScale = 1.0f;
    bool defragDone = false;
RETRY_ATLAS_SETUP:

    // Apply additional scale to the shadows resolution
    if (!Math::IsOne(resolutionScale))
    {
        finalScale *= resolutionScale;
        for (const RenderLightData* light : shadowedLights)
        {
            auto& atlasLight = shadows.Lights[light->ID];
            if (light->IsDirectionalLight && !defragDone)
                continue; // Reduce scaling on directional light shadows (before defrag)
            atlasLight.Resolution = QuantizeResolution(atlasLight.Resolution * resolutionScale);
        }
    }

    // Macro checks if light has proper amount of tiles already assigned and the resolution is matching
#define IS_LIGHT_TILE_REUSABLE (atlasLight.TilesCount == atlasLight.TilesNeeded && atlasLight.Tiles[0].RectTile && atlasLight.Tiles[0].RectTile->Width == atlasLight.Resolution)

    // Remove incorrect tiles before allocating new ones
    for (RenderLightData* light : shadowedLights)
    {
        ShadowAtlasLight& atlasLight = shadows.Lights[light->ID];
        if (IS_LIGHT_TILE_REUSABLE)
            continue;

        // Remove existing tiles
        atlasLight.Cache.DynamicValid = false;
        for (ShadowAtlasLightTile& tile : atlasLight.Tiles)
            tile.FreeDynamic(&shadows);
    }

    // Insert tiles into the atlas (already sorted to favor the first ones)
    for (RenderLightData* light : shadowedLights)
    {
        auto& atlasLight = shadows.Lights[light->ID];
        if (IS_LIGHT_TILE_REUSABLE || atlasLight.Resolution < SHADOWS_MIN_RESOLUTION)
            continue;

        // Try to insert tiles
        bool failedToInsert = false;
        for (int32 tileIndex = 0; tileIndex < atlasLight.TilesNeeded; tileIndex++)
        {
            auto rectTile = shadows.Atlas.Insert(atlasLight.Resolution, atlasLight.Resolution, &shadows, false);
            if (!rectTile)
            {
                // Free any previous tiles that were added
                for (int32 i = 0; i < tileIndex; i++)
                    atlasLight.Tiles[i].FreeDynamic(&shadows);
                failedToInsert = true;
                break;
            }
            atlasLight.Tiles[tileIndex].RectTile = rectTile;
        }
        if (failedToInsert)
        {
            if (defragDone)
            {
                // Already defragmented atlas so scale it down
                resolutionScale = 0.8f;
            }
            else
            {
                // Defragment atlas without changing scale
                defragDone = true;
                resolutionScale = 1.0f;
            }

            // Rebuild atlas
            shadows.ClearDynamic();
            goto RETRY_ATLAS_SETUP;
        }
    }

    // Setup shadows for all lights
    for (RenderLightData* light : shadowedLights)
    {
        auto& atlasLight = shadows.Lights[light->ID];

        // Reset frame-data
        atlasLight.ContextIndex = 0;
        atlasLight.ContextCount = 0;

        if (atlasLight.Tiles[0].RectTile && atlasLight.Tiles[0].RectTile->Width == atlasLight.Resolution)
        {
            // Invalidate cache when whole atlas will be cleared
            if (shadows.ClearShadowMapAtlas)
                atlasLight.Cache.DynamicValid = false;
            if (shadows.ClearStaticShadowMapAtlas)
                atlasLight.Cache.StaticValid = false;

            light->HasShadow = true;
            atlasLight.TilesCount = atlasLight.TilesNeeded;
            if (light->IsPointLight)
                SetupLight(shadows, renderContext, renderContextBatch, *(RenderPointLightData*)light, atlasLight);
            else if (light->IsSpotLight)
                SetupLight(shadows, renderContext, renderContextBatch, *(RenderSpotLightData*)light, atlasLight);
            else //if (light->IsDirectionalLight)
                SetupLight(shadows, renderContext, renderContextBatch, *(RenderDirectionalLightData*)light, atlasLight);

            // Check if that light exists in linked shadows buffer to reuse shadow maps
            const ShadowAtlasLight* linkedAtlasLight;
            if (shadows.LinkedShadows && ((linkedAtlasLight = shadows.LinkedShadows->Lights.TryGet(light->ID))) && linkedAtlasLight->TilesCount == atlasLight.TilesCount)
            {
                for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
                {
                    auto& tile = atlasLight.Tiles[tileIndex];
                    tile.LinkedRectTile = nullptr;
                    auto& linkedTile = linkedAtlasLight->Tiles[tileIndex];

                    // Link tile and use its projection
                    if (linkedTile.RectTile)
                    {
                        tile.LinkedRectTile = linkedTile.RectTile;
                        tile.WorldToShadow = linkedTile.WorldToShadow;
                    }
                }
            }
            else
            {
                for (auto& tile : atlasLight.Tiles)
                    tile.LinkedRectTile = nullptr;
            }
        }
    }
    // (scene-rendering listeners are attached at the top of SetupShadows, before lights setup)

#undef IS_LIGHT_TILE_REUSABLE

    // Update shadows buffer (contains packed data with all shadow projections in the atlas)
    const float atlasResolutionInv = 1.0f / (float)atlasResolution;
    shadows.ShadowsBuffer.Clear();
    shadows.ShadowsBuffer.Write(Float4::Zero); // Insert dummy prefix so ShadowsBufferAddress=0 indicates no shadow
    for (RenderLightData* light : shadowedLights)
    {
        auto& atlasLight = shadows.Lights[light->ID];
        if (atlasLight.Tiles[0].RectTile == nullptr)
        {
            light->ShadowsBufferAddress = 0; // Clear to indicate no shadow
            continue;
        }

        // Cache start of the shadow data for this light
        light->ShadowsBufferAddress = shadows.ShadowsBuffer.Data.Count() / sizeof(Float4);

        // Write shadow data (this must match HLSL)
        {
            // Shadow info
            auto* packed = shadows.ShadowsBuffer.WriteReserve<Float4>(2);
            Color32 packed0x((byte)(atlasLight.Sharpness * (255.0f / 10.0f)), (byte)(atlasLight.Fade * 255.0f), (byte)atlasLight.TilesCount, (byte)Math::Clamp(atlasLight.Softness * 255.0f, 0.0f, 255.0f));
            packed[0] = Float4(*(const float*)&packed0x, atlasLight.FadeDistance, atlasLight.NormalOffsetScale, atlasLight.Bias);
            packed[1] = atlasLight.CascadeSplits;
        }
        const float tileBorder = atlasLight.TileBorder;
        for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
        {
            // Shadow projection info
            const ShadowAtlasLightTile& tile = atlasLight.Tiles[tileIndex];
            ASSERT(tile.RectTile);
            auto* packed = shadows.ShadowsBuffer.WriteReserve<Float4>(5);
            // UV to AtlasUV via a single MAD instruction
            packed[0] = Float4(tile.RectTile->Width - tileBorder * 2, tile.RectTile->Height - tileBorder * 2, tile.RectTile->X + tileBorder, tile.RectTile->Y + tileBorder) * atlasResolutionInv;
            packed[1] = tile.WorldToShadow.GetColumn1();
            packed[2] = tile.WorldToShadow.GetColumn2();
            packed[3] = tile.WorldToShadow.GetColumn3();
            packed[4] = tile.WorldToShadow.GetColumn4();
        }
    }
    GPUContext* context = GPUDevice::Instance->GetMainContext();
    shadows.ShadowsBuffer.Flush(context);
    shadows.ShadowsBufferView = shadows.ShadowsBuffer.GetBuffer()->View();
}

// Render one rectangular world-texel region of static geometry into a shadow clipmap level,
// honouring toroidal addressing (texture pixel = world-texel - level.TextureOriginTexels, mod R,
// with Y flipped to match the existing ortho convention).
//
// Collects scene draw calls ONCE over the union world-rect (SceneDraw + SceneDrawAsync, async waited),
// then issues up to 4 sub-rect draws that mutate ctx.View per sub-rect and re-bind per-view CB.
// Caller must have called context->SetRenderTarget(level.DepthTexture->View(), nullptr).
//
// wMin/wMax are absolute world-texel coords (light-XY plane). wMax exclusive. Width & height must each be in [1, R].
// Rasterizes one clipmap sub-rect (arm) and returns the number of static occluders collected for
// it (0 on any early-out). Callers accumulate this across a banded rebuild to content-prove the
// cache before latching Populated (R4).
static int32 RenderClipmapStrip(
    GPUContext* context,
    const RenderContext& mainRenderContext,
    const ShadowClipmap& clipmap,
    ShadowClipmapLevel& level,
    Int2 wMin,
    Int2 wMax,
    GPUPipelineState* psDepthClear,
    GPUConstantBuffer* quadShaderCB)
{
    const int32 R = level.Resolution;
    const int32 wWidth = wMax.X - wMin.X;
    const int32 wHeight = wMax.Y - wMin.Y;
    if (wWidth <= 0 || wHeight <= 0 || wWidth > R || wHeight > R)
        return 0;
    const float ts = level.TexelSize;
    const Int2& origin = level.TextureOriginTexels;

    // World-rect bounds of the arm (used as the collection frustum)
    const float wx0 = (float)wMin.X * ts;
    const float wx1 = (float)wMax.X * ts;
    const float wy0 = (float)wMin.Y * ts;
    const float wy1 = (float)wMax.Y * ts;
    const float armCx = 0.5f * (wx0 + wx1);
    const float armCy = 0.5f * (wy0 + wy1);
    const float armHx = 0.5f * (wx1 - wx0);
    const float armHy = 0.5f * (wy1 - wy0);
    const Float3 armCenterW = clipmap.LightRight * armCx + clipmap.LightUp * armCy;
    // armCenterW carries no sun-z component, so the depth interval is world-anchored: ortho near
    // plane at sun-z = level.DepthMinZ, far at DepthMinZ + DepthRange (scene-bounds-driven; the
    // compositor's depth remap derives from the same fields - MUST stay in sync).
    const Float3 armEye = armCenterW + clipmap.SunDir * level.DepthMinZ;

    // Use the SAME LightUp the clipmap stored at Init time (single source of truth via
    // ComputeLightBasis). Recomputing it here from a world-axis "up hint" with a threshold
    // check would risk disagreeing with the compositor in the threshold band, leaving the
    // rasterized texture content rotated 90deg relative to the sampling math.
    // LookAt target is direction-based: DepthMinZ can be any sign, so armCenterW may sit on
    // either side of (or exactly at) the eye.
    Matrix armView, armProj;
    Matrix::LookAt(armEye, armEye + clipmap.SunDir, clipmap.LightUp, armView);
    Matrix::OrthoOffCenter(-armHx, armHx, -armHy, armHy, 0.0f, level.DepthRange, armProj);
    Matrix armVP;
    Matrix::Multiply(armView, armProj, armVP);

    // No task = no scene to collect from. Bail before pulling a RenderList from the pool so we
    // don't leak it on this early return.
    if (!mainRenderContext.Task)
        return 0;

    // Collect once over the arm (both sync + async actor categories; matches weapon-shadow pattern)
    RenderContext armCtx;
    armCtx.Buffers = mainRenderContext.Buffers;
    armCtx.Task = mainRenderContext.Task;
    armCtx.List = RenderList::GetFromPool();
    armCtx.List->Clear();
    armCtx.LodProxyView = const_cast<RenderView*>(&mainRenderContext.View);
    armCtx.View.Position = armEye;
    armCtx.View.Direction = clipmap.SunDir;
    armCtx.View.SetUp(armView, armProj);
    armCtx.View.Pass = DrawPass::Depth;
    armCtx.View.Flags = mainRenderContext.View.Flags;
    armCtx.View.StaticFlagsMask = StaticFlags::Shadow;
    armCtx.View.StaticFlagsCompare = StaticFlags::Shadow; // Static only
    armCtx.View.IsStaticShadowCache = true; // include all casters by extent; skip per-level apparent-size cull
    armCtx.View.RenderLayersMask = mainRenderContext.View.RenderLayersMask;
    armCtx.View.Origin = mainRenderContext.View.Origin;
    armCtx.View.CullingFrustum.SetMatrix(armVP);
    armCtx.View.PrepareCache(armCtx, (float)wWidth, (float)wHeight, Float2::Zero, &mainRenderContext.View);

    RenderContextBatch armBatch;
    armBatch.Contexts.Add(armCtx);
    mainRenderContext.Task->OnCollectDrawCalls(armBatch, SceneRendering::DrawCategory::SceneDraw);
    mainRenderContext.Task->OnCollectDrawCalls(armBatch, SceneRendering::DrawCategory::SceneDrawAsync);
    for (const uint64 label : armBatch.WaitLabels)
        JobSystem::Wait(label);
    armBatch.WaitLabels.Clear();
    auto& ctx = armBatch.Contexts[0];
    if (!ctx.List)
    {
        return 0;
    }

    // R4 content-proof: how many static occluders this arm actually collected. Matches the
    // Depth + ShadowDepth non-empty test master uses (DrawCallsList::IsEmpty semantics: Indices +
    // PreBatchedDrawCalls). The same collected list is executed for every sub-rect below, so this
    // count is the arm's occluder tally regardless of the wrap split.
    const auto& depthList = ctx.List->DrawCallsLists[(int32)DrawCallsListType::Depth];
    const int32 casterCount = depthList.Indices.Count() + depthList.PreBatchedDrawCalls.Count()
        + ctx.List->ShadowDepthDrawCallsList.Indices.Count() + ctx.List->ShadowDepthDrawCallsList.PreBatchedDrawCalls.Count();

// Split world-texel X range into up to 2 texture-pixel sub-ranges based on wrap around R.
    // Texture pixel px for world-texel w.X = ((w.X - origin.X) mod R + R) mod R.
    struct SubRange { int32 worldStart; int32 texStart; int32 count; };
    auto modR = [R](int32 v) -> int32 { int32 m = v % R; return m < 0 ? m + R : m; };

    SubRange xR[2]; int32 nx;
    {
        const int32 tStart = modR(wMin.X - origin.X);
        if (tStart + wWidth <= R)
        {
            xR[0] = { wMin.X, tStart, wWidth };
            nx = 1;
        }
        else
        {
            const int32 first = R - tStart;
            xR[0] = { wMin.X, tStart, first };
            xR[1] = { wMin.X + first, 0, wWidth - first };
            nx = 2;
        }
    }

    // For Y the texture pixel py for world-texel w.Y is ((origin.Y - 1 - w.Y) mod R + R) mod R.
    // That's descending in py as w.Y ascends. For w.Y in [wMin.Y, wMax.Y) the texture pixel range
    // is [py(wMax.Y-1), py(wMin.Y)] inclusive = [tStart, tStart + wHeight) with tStart = py(wMax.Y-1).
    SubRange yR[2]; int32 ny;
    {
        const int32 tStart = modR(origin.Y - wMax.Y);
        if (tStart + wHeight <= R)
        {
            // Texture pixel tStart corresponds to world-texel wMax.Y - 1 (the topmost row of this strip in light-Y).
            // We want yR[i].worldStart to be the LOWEST world-Y in the sub-range (so a +Y-flipped ortho centers cleanly).
            // Sub-range covers world-Y in [wMin.Y, wMax.Y), texture pixels [tStart, tStart + wHeight).
            yR[0] = { wMin.Y, tStart, wHeight };
            ny = 1;
        }
        else
        {
            // The Y-axis wraps: top texture-pixels [tStart, R) cover the upper world-Y slice,
            // and texture-pixels [0, ...) cover the lower world-Y slice.
            const int32 first = R - tStart;            // count of top sub-range
            // Top sub-range (texture rows [tStart, R)) covers the highest world-Y rows: w.Y in [wMax.Y - first, wMax.Y).
            yR[0] = { wMax.Y - first, tStart, first };
            // Bottom sub-range (texture rows [0, wHeight - first)) covers the rest: w.Y in [wMin.Y, wMax.Y - first).
            yR[1] = { wMin.Y, 0, wHeight - first };
            ny = 2;
        }
    }

    // Per-sub-rect: build a tight ortho over its world-rect, viewport+scissor to its texture-pixel rect, ExecuteDrawCalls.
    for (int32 yi = 0; yi < ny; yi++)
    {
        const SubRange& yr = yR[yi];
        for (int32 xi = 0; xi < nx; xi++)
        {
            const SubRange& xr = xR[xi];

            const float subWx0 = (float)xr.worldStart * ts;
            const float subWx1 = (float)(xr.worldStart + xr.count) * ts;
            const float subWy0 = (float)yr.worldStart * ts;
            const float subWy1 = (float)(yr.worldStart + yr.count) * ts;
            const float subCx = 0.5f * (subWx0 + subWx1);
            const float subCy = 0.5f * (subWy0 + subWy1);
            const float subHx = 0.5f * (subWx1 - subWx0);
            const float subHy = 0.5f * (subWy1 - subWy0);
            const Float3 subCenterW = clipmap.LightRight * subCx + clipmap.LightUp * subCy;
            // Same depth interval as the parent arm: near plane at sun-z = level.DepthMinZ.
            const Float3 subEye = subCenterW + clipmap.SunDir * level.DepthMinZ;

            Matrix subView, subProj;
            Matrix::LookAt(subEye, subEye + clipmap.SunDir, clipmap.LightUp, subView);
            Matrix::OrthoOffCenter(-subHx, subHx, -subHy, subHy, 0.0f, level.DepthRange, subProj);

            ctx.View.Position = subEye;
            ctx.View.SetUp(subView, subProj);
            ctx.View.PrepareCache(ctx, (float)xr.count, (float)yr.count, Float2::Zero, &mainRenderContext.View);

            const Viewport vp((float)xr.texStart, (float)yr.texStart, (float)xr.count, (float)yr.count);
            context->SetViewportAndScissors(vp);

            // Clear this sub-rect's depth to 1.0 before rasterizing into it.
            // Critical for toroidal addressing: when scroll re-anchors a texture pixel to a NEW
            // world position, the cached depth value from the OLD world point will survive a
            // standard LESS depth test if the new world point has higher depth - manifests as
            // bright "stale closer-to-sun" bands across recently-scrolled strips. Full-redraw
            // doesn't hit this because the caller ClearDepth's the whole texture first.
            // Uses PS_DepthClear (writes SV_Depth = Color.r = 1.0), the same pattern as
            // ClearShadowMapTile - viewport+scissor confines the clear to this sub-rect.
            if (psDepthClear && quadShaderCB)
            {
                QuadShaderData clearData;
                clearData.Color = Float4::One; // Color.r = 1.0 -> SV_Depth = 1.0
                clearData.Params = Float4::Zero;
                context->UpdateCB(quadShaderCB, &clearData);
                context->BindCB(0, quadShaderCB);
                context->SetState(psDepthClear);
                context->DrawFullscreenTriangle();
            }

            ctx.List->ExecuteDrawCalls(ctx, DrawCallsListType::Depth);
        }
    }

    context->ResetSR();
    RenderList::ReturnToPool(ctx.List);
    return casterCount;
}

void ShadowsPass::RequestStaticShadowRedraw(int32 amortizeFrames)
{
    amortizeFrames = Math::Max(amortizeFrames, 1);
    Platform::AtomicStore(&StaticShadowRedrawAmortize, amortizeFrames);
    Platform::AtomicStore(&StaticShadowRedrawCountdown, amortizeFrames);
    Platform::InterlockedIncrement(&StaticShadowRedrawGeneration); // bump last so the new amortize/countdown are visible when serviced
}

bool ShadowsPass::AreStaticShadowsRedrawing()
{
    if (Platform::AtomicRead(&StaticShadowRedrawCountdown) > 0)
        return true;
    // A rebuild band can outlive the countdown (self-heal, scroll-overflow amortize) or start with
    // no explicit request. Report "redrawing" while bands are still landing so the warp/fade cover
    // holds until the cache is coherent. One-frame slack absorbs the render-thread/main-thread lag.
    const int64 lastBand = Platform::AtomicRead(&StaticShadowLastBandFrame);
    return lastBand >= 0 && (int64)Engine::FrameCount - lastBand <= 1;
}

void ShadowsPass::RenderShadowMaps(RenderContextBatch& renderContextBatch)
{
    const RenderContext& renderContext = renderContextBatch.GetMainContext();
    const ShadowsCustomBuffer* shadowsPtr = renderContext.Buffers->FindCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false);
    if (shadowsPtr == nullptr || shadowsPtr->Lights.IsEmpty() || shadowsPtr->LastFrameUsed != Engine::FrameCount)
        return;
    PROFILE_GPU_CPU("Shadow Maps");
#if COMPILE_WITH_PROFILER
    // Tally per-model shadow caster triangles across all submits below (CSM, clipmap, atlas, weapon).
    ProfilingTools::ShadowTallyScope shadowTallyScope;
#endif
    const ShadowsCustomBuffer& shadows = *shadowsPtr;
    GPUContext* context = GPUDevice::Instance->GetMainContext();
    context->ResetSR();
    GPUConstantBuffer* quadShaderCB = GPUDevice::Instance->QuadShader->GetCB(0);
    QuadShaderData quadShaderData;

    // Update static shadows
    if (shadows.StaticShadowMapAtlas)
    {
        PROFILE_GPU_CPU("Static");
        if (shadows.ClearStaticShadowMapAtlas)
            context->ClearDepth(shadows.StaticShadowMapAtlas->View());
        bool renderedAny = false;
        for (auto& e : shadows.Lights)
        {
            const ShadowAtlasLight& atlasLight = e.Value;
            if (!atlasLight.HasStaticShadowContext || atlasLight.ContextCount == 0)
                continue;
            int32 contextIndex = 0;

            if (atlasLight.StaticState == ShadowAtlasLight::WaitForGeometryCheck)
            {
                // Check for any static geometry to use in static shadow map
                for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
                {
                    const ShadowAtlasLightTile& tile = atlasLight.Tiles[tileIndex];
                    contextIndex++; // Skip dynamic context
                    auto& shadowContextStatic = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
                    if (!shadowContextStatic.List->DrawCallsLists[(int32)DrawCallsListType::Depth].IsEmpty() || !shadowContextStatic.List->ShadowDepthDrawCallsList.IsEmpty())
                    {
                        tile.HasStaticGeometry = true;
                    }
                }
            }

            if (atlasLight.StaticState != ShadowAtlasLight::UpdateStaticShadow)
                continue;

            contextIndex = 0;
            for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
            {
                const ShadowAtlasLightTile& tile = atlasLight.Tiles[tileIndex];
                if (!tile.RectTile)
                    break;
                if (!tile.StaticRectTile)
                    continue;
                if (!renderedAny)
                {
                    renderedAny = true;
                    context->SetRenderTarget(shadows.StaticShadowMapAtlas->View(), (GPUTextureView*)nullptr);
                }

                // Set viewport for tile
                context->SetViewportAndScissors(Viewport(tile.StaticRectTile->X, tile.StaticRectTile->Y, tile.StaticRectTile->Width, tile.StaticRectTile->Height));
                if (!shadows.ClearStaticShadowMapAtlas)
                {
                    // Color.r is used by PS_DepthClear in Quad shader to clear depth
                    quadShaderData.Color = Float4::One;
                    context->UpdateCB(quadShaderCB, &quadShaderData);
                    context->BindCB(0, quadShaderCB);

                    // Clear tile depth
                    context->SetState(_psDepthClear);
                    context->DrawFullscreenTriangle();
                }

                // Draw objects depth
                contextIndex++; // Skip dynamic context
                auto& shadowContextStatic = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
                if (!shadowContextStatic.List->DrawCallsLists[(int32)DrawCallsListType::Depth].IsEmpty() || !shadowContextStatic.List->ShadowDepthDrawCallsList.IsEmpty())
                {
                    shadowContextStatic.List->ExecuteDrawCalls(shadowContextStatic, DrawCallsListType::Depth);
                    shadowContextStatic.List->ExecuteDrawCalls(shadowContextStatic, shadowContextStatic.List->ShadowDepthDrawCallsList, renderContext.List, nullptr);
                    tile.HasStaticGeometry = true;
                }
            }

            // Go into copying shadow for the next draw
            atlasLight.StaticState = ShadowAtlasLight::CopyStaticShadow;
        }
        shadows.ClearStaticShadowMapAtlas = false;
        if (renderedAny)
        {
            context->ResetSR();
            context->ResetRenderTarget();
        }
    }

    // Render shadow clipmap updates for static geometry
    ShadowsCustomBuffer& shadowsMutable = const_cast<ShadowsCustomBuffer&>(shadows);
    auto& clipmap = shadowsMutable.Clipmap;
    if (clipmap.Enabled)
    {
        PROFILE_GPU_CPU("Shadow Clipmap");

        for (int32 levelIdx = 0; levelIdx < clipmap.LevelCount; levelIdx++)
        {
            auto& level = clipmap.Levels[levelIdx];
            if (!level.DepthTexture)
                continue;

            const int32 R = level.Resolution;

            // Start a banded rebuild when one is requested and none is running. Anchor the toroidal
            // mapping at the current scroll; ComputeScroll then holds that anchor until the rebuild
            // drains. amortize=1 collapses to a single full-window band (the old full redraw).
            if (level.NeedsFullRedraw && level.RedrawRowCursor < 0)
            {
                level.NeedsFullRedraw = false;
                // Adopt the desired params NOW: the whole rebuild rasterizes on the new grid.
                // Content params are immutable everywhere else (see Init), so the compositor can
                // never read texels through math they weren't rendered under. The scroll must be
                // re-anchored on the adopted texel grid (this frame's ComputeScroll used the old
                // one; same double-precision projection - see ComputeScroll).
                if (level.DesiredWorldExtent > 0.0f)
                {
                    level.WorldExtent = level.DesiredWorldExtent;
                    level.TexelSize = level.WorldExtent / (float)R;
                    level.DepthMinZ = level.DesiredDepthMinZ;
                    level.DepthRange = level.DesiredDepthRange;
                    const double camX = (double)clipmap.LastCameraPos.X * clipmap.LightRight.X + (double)clipmap.LastCameraPos.Y * clipmap.LightRight.Y + (double)clipmap.LastCameraPos.Z * clipmap.LightRight.Z;
                    const double camY = (double)clipmap.LastCameraPos.X * clipmap.LightUp.X + (double)clipmap.LastCameraPos.Y * clipmap.LightUp.Y + (double)clipmap.LastCameraPos.Z * clipmap.LightUp.Z;
                    const double invTs = 1.0 / (double)level.TexelSize;
                    level.ScrollTexels = Int2((int32)Math::Floor(camX * invTs), (int32)Math::Floor(camY * invTs));
                    level.PrevScrollTexels = level.ScrollTexels;
                    level.DirtyStrip = Int2::Zero;
                }
                level.RedrawAnchorScroll = level.ScrollTexels;
                level.TextureOriginTexels = Int2(level.ScrollTexels.X - R / 2, level.ScrollTexels.Y + R / 2);
                const int32 amortize = Math::Clamp(clipmap.PendingAmortize, 1, R);
                level.RedrawRowsPerFrame = (R + amortize - 1) / amortize;
                level.RedrawRowCursor = 0;
                level.RedrawCasterCount = 0; // R4: start the content tally for this rebuild
            }

            // Banded rebuild in flight: rasterize the next horizontal band at the fixed anchor.
            // RenderClipmapStrip clears+writes its own sub-rect via _psDepthClear, so no full clear
            // is needed; un-rendered bands hold prior content until their turn (hidden by warp cover).
            if (level.RedrawRowCursor >= 0)
            {
                const Int2 a = level.RedrawAnchorScroll;
                const int32 y0 = level.RedrawRowCursor;
                const int32 y1 = Math::Min(y0 + level.RedrawRowsPerFrame, R);
                context->SetRenderTarget(level.DepthTexture->View(), (GPUTextureView*)nullptr);
                level.RedrawCasterCount += RenderClipmapStrip(context, renderContext, clipmap, level,
                                   Int2(a.X - R / 2, a.Y - R / 2 + y0),
                                   Int2(a.X + R / 2, a.Y - R / 2 + y1),
                                   _psDepthClear, quadShaderCB);
                context->ResetRenderTarget();
                // Stamp the band frame so AreStaticShadowsRedrawing() reports true while rebuild
                // bands are still landing, even past the explicit-request countdown.
                Platform::AtomicStore(&StaticShadowLastBandFrame, (int64)Engine::FrameCount);
                level.RedrawRowCursor = y1;
                if (y1 >= R)
                {
                    // Rebuild complete - stamp the math/basis state the content was rendered against.
                    level.RedrawRowCursor = -1;
                    // R4: content-proven validity. Only claim Populated if the rebuild actually
                    // rasterized >=1 static occluder across the level. On a fresh proc-gen world the
                    // cold rebuild (the hot path - nothing is baked, a new world every run) can fire
                    // before geometry has streamed in and collect nothing; latching Populated then
                    // composites an empty depth texture as a valid cache and statics silently vanish.
                    // Leaving Populated false keeps FallbackActive set (see the serve-check in
                    // SetupLight), so statics stay in the CSM dynamic draw - visible, never dropped -
                    // until a later rebuild (invalidation / self-heal / editor edit) proves content.
                    level.Populated = level.RedrawCasterCount > 0;
                    level.LastRedrawSunDir = clipmap.SunDir;
                    level.LastRedrawTexelSize = level.TexelSize;
                    level.LastRedrawDepthRange = level.DepthRange;
                    level.LastRedrawDepthMinZ = level.DepthMinZ;
                    level.LastRedrawWorldExtent = level.WorldExtent;
                }
                level.DirtyStrip = Int2::Zero;
                continue;
            }

            // Not rebuilding: strip-scroll the leading edge if the camera moved this frame.
            if (level.DirtyStrip.X == 0 && level.DirtyStrip.Y == 0)
                continue;
            const Int2 newScroll = level.ScrollTexels;
            {
                // Strip update - render only the L-shaped strip of new texels at the leading edges.
                // X-strip: full Y extent, |dx| wide. Y-strip: |dy| tall, with width = R - |dx| so the
                // corner (already covered by X-strip) isn't double-rendered.
                context->SetRenderTarget(level.DepthTexture->View(), (GPUTextureView*)nullptr);

                const int32 dx = level.DirtyStrip.X;
                const int32 dy = level.DirtyStrip.Y;
                const int32 absDx = Math::Abs(dx);
                const int32 absDy = Math::Abs(dy);

                if (absDx != 0)
                {
                    // X-arm spans full Y range, |dx| wide at the leading X edge.
                    const Int2 wMin(dx > 0 ? newScroll.X + R / 2 - absDx : newScroll.X - R / 2,
                                    newScroll.Y - R / 2);
                    const Int2 wMax(wMin.X + absDx,
                                    newScroll.Y + R / 2);
                    RenderClipmapStrip(context, renderContext, clipmap, level, wMin, wMax, _psDepthClear, quadShaderCB);
                }
                if (absDy != 0)
                {
                    // Y-arm spans Y range = |dy|, X range excludes the X-arm's columns (avoid corner overdraw).
                    const int32 yArmWidth = R - absDx;
                    if (yArmWidth > 0)
                    {
                        // X range of Y-arm: the part of the level NOT covered by the X-arm.
                        // If dx>0, X-arm sits at the +X edge so Y-arm is on the -X side.
                        // If dx<0, X-arm sits at the -X edge so Y-arm is on the +X side.
                        // If dx==0, Y-arm covers full X range.
                        const int32 yArmXMin = (dx > 0) ? (newScroll.X - R / 2)
                                              : (dx < 0) ? (newScroll.X - R / 2 + absDx)
                                                         : (newScroll.X - R / 2);
                        const Int2 wMin(yArmXMin,
                                        dy > 0 ? newScroll.Y + R / 2 - absDy : newScroll.Y - R / 2);
                        const Int2 wMax(yArmXMin + yArmWidth,
                                        wMin.Y + absDy);
                        RenderClipmapStrip(context, renderContext, clipmap, level, wMin, wMax, _psDepthClear, quadShaderCB);
                    }
                }

                context->ResetRenderTarget();
            }

            // Consume strip delta so the next frame's ComputeScroll starts from this baseline
            level.DirtyStrip = Int2::Zero;
        }

        // Consume the amortize budget: a fresh request (API / overflow self-heal) re-latches it.
        clipmap.PendingAmortize = 1;
    }

    // Render depth to all shadow map tiles
    if (shadows.ClearShadowMapAtlas)
        context->ClearDepth(shadows.ShadowMapAtlas->View());
    context->SetRenderTarget(shadows.ShadowMapAtlas->View(), (GPUTextureView*)nullptr);
    for (auto& e : shadows.Lights)
    {
        const ShadowAtlasLight& atlasLight = e.Value;

        // Check if this light uses the shadow clipmap
        const bool useClipmapForLight = clipmap.Enabled && e.Key == clipmap.LightId;

        // Allow non-dynamic paths (clipmap composite / static atlas copy) to still process.
        if (atlasLight.ContextCount == 0 && !useClipmapForLight && atlasLight.StaticState != ShadowAtlasLight::CopyStaticShadow)
            continue;

        int32 contextIndex = 0;
        for (int32 tileIndex = 0; tileIndex < atlasLight.TilesCount; tileIndex++)
        {
            const ShadowAtlasLightTile& tile = atlasLight.Tiles[tileIndex];
            if (!tile.RectTile)
                break;
            if (tile.SkipUpdate)
                continue;

            // Set viewport for tile
            context->SetViewportAndScissors(tile.CachedViewport);

            if (useClipmapForLight && tileIndex < clipmap.LevelCount && clipmap.Levels[tileIndex].DepthTexture &&
                !(clipmap.Levels[tileIndex].FallbackActive && atlasLight.RenderDynamic))
            {
                // (When FallbackActive, this cascade's statics were kept in the dynamic draw below and
                // the composite is skipped; with DynamicShadows off there's no fallback path, so a
                // stale composite still beats missing static shadows.)
                // Composite from shadow clipmap level into this cascade tile.
                // CRITICAL: clear the tile first when the atlas wasn't globally cleared this frame.
                // PS_ClipmapComposite discards no-occluder pixels and out-of-cascade-range pixels;
                // discarded pixels retain whatever was in the tile from the previous frame, which
                // was rendered for a *different camera orientation* -> "solitaire trailing" ghosting
                // of shadows from prior frames as the camera rotates. All non-clipmap branches in
                // the else-if chain below do this same conditional clear.
                if (!shadows.ClearShadowMapAtlas)
                    ClearShadowMapTile(context, quadShaderCB, quadShaderData);

                auto& level = clipmap.Levels[tileIndex];
                quadShaderData.Color = level.CompositingColor;
                quadShaderData.Params = Float4(level.DepthRemap.X, level.DepthRemap.Y, level.WrapOffsetUV.X, level.WrapOffsetUV.Y);
                context->UpdateCB(quadShaderCB, &quadShaderData);
                context->BindCB(0, quadShaderCB);
                context->BindSR(0, level.DepthTexture->View());
                context->SetState(_psClipmapComposite);
                context->DrawFullscreenTriangle();
                context->UnBindSR(0);
            }
            else if (tile.LinkedRectTile)
            {
                // Copy linked shadow
                ASSERT(shadows.LinkedShadows);
                CopyShadowMapTile(context, quadShaderCB, quadShaderData, shadows.LinkedShadows->ShadowMapAtlas, tile.LinkedRectTile);
            }
            else if (tile.StaticRectTile && atlasLight.StaticState == ShadowAtlasLight::CopyStaticShadow)
            {
                // Copy static shadow
                CopyShadowMapTile(context, quadShaderCB, quadShaderData, shadows.StaticShadowMapAtlas, tile.StaticRectTile);
            }
            else if (!shadows.ClearShadowMapAtlas)
            {
                // Clear shadow
                ClearShadowMapTile(context, quadShaderCB, quadShaderData);
            }

            // Draw objects depth (dynamic-only when clipmap is active for this light).
            // Skipped when DynamicShadows is off - no per-cascade contexts exist to draw from.
            if (atlasLight.RenderDynamic && atlasLight.ContextCount > 0)
            {
                auto& shadowContext = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
                shadowContext.List->ExecuteDrawCalls(shadowContext, DrawCallsListType::Depth);
                shadowContext.List->ExecuteDrawCalls(shadowContext, shadowContext.List->ShadowDepthDrawCallsList, renderContext.List, nullptr);
            }
            if (atlasLight.HasStaticShadowContext)
            {
                auto& shadowContextStatic = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
                if (!shadowContextStatic.List->DrawCallsLists[(int32)DrawCallsListType::Depth].IsEmpty() || !shadowContextStatic.List->ShadowDepthDrawCallsList.IsEmpty())
                {
                    if (atlasLight.StaticState != ShadowAtlasLight::CopyStaticShadow)
                    {
                        // Draw static objects directly to the shadow map
                        shadowContextStatic.List->ExecuteDrawCalls(shadowContextStatic, DrawCallsListType::Depth);
                        shadowContextStatic.List->ExecuteDrawCalls(shadowContextStatic, shadowContextStatic.List->ShadowDepthDrawCallsList, renderContext.List, nullptr);
                    }
                    tile.HasStaticGeometry = true;
                }
            }
        }
    }

    // Render weapon self-shadows for directional light (single shadow map, no cascades)
    // Initialize weapon shadow buffer (always, even if no shadows - for dummy entry)
    shadowsMutable.WeaponShadowsBuffer.Clear();
    shadowsMutable.WeaponShadowsBuffer.Write(Float4::Zero); // WeaponShadowsBufferAddress=0 indicates no weapon shadow

    if (shadowsMutable.WeaponShadowMapAtlas && renderContextBatch.Contexts.HasItems())
    {
        PROFILE_GPU_CPU("Weapon Shadows");

        // Find directional light
        RenderDirectionalLightData* dirLight = nullptr;
        for (int32 i = 0; i < renderContext.List->DirectionalLights.Count(); i++)
        {
            if (renderContext.List->DirectionalLights[i].HasShadow)
            {
                dirLight = &renderContext.List->DirectionalLights[i];
                break;
            }
        }

        if (!dirLight)
        {
            
            // No directional light found, skip weapon shadows
        }
        else
        {
            // Initialize weapon shadow atlas if needed
            const int32 weaponAtlasResolution = 2048; // Fixed size for weapon shadows
            if (shadowsMutable.WeaponAtlas.Width != weaponAtlasResolution)
            {
                shadowsMutable.WeaponAtlas.Init(weaponAtlasResolution, weaponAtlasResolution);
                auto desc = shadowsMutable.ShadowMapAtlas->GetDescription();
                desc.Width = desc.Height = weaponAtlasResolution;
                if (shadowsMutable.WeaponShadowMapAtlas->Init(desc))
                {
                    LOG(Error, "Failed to setup weapon shadow map of size {0}x{1}", desc.Width, desc.Height);
                    goto skip_weapon_shadows;
                }
            }

            // Allocate weapon shadow tile if needed
            const uint16 weaponShadowRes = 1024; // Single 512x512 shadow map for all weapon geometry
            if (!shadowsMutable.WeaponDirectionalLightTile)
            {
                shadowsMutable.WeaponDirectionalLightTile = shadowsMutable.WeaponAtlas.Insert(weaponShadowRes, weaponShadowRes, &shadowsMutable, false);
                if (!shadowsMutable.WeaponDirectionalLightTile)
                {
                    LOG(Error, "Failed to allocate weapon shadow tile");
                    goto skip_weapon_shadows;
                }
            }

            // Setup shadow view for weapons (use same logic as regular directional shadows)
            RenderView weaponShadowView;
            const Float3 weaponCenter = renderContext.View.Position + renderContext.View.Direction * 100.0f; // 1 meter (100cm) in front
            const float weaponRadius = 500.0f; // 1 meter radius to cover weapon bounds

            // Build shadow bounds around weapon sphere (same as cascade shadow logic)
            Float3 maxExtents = Float3(weaponRadius);
            Float3 minExtents = -maxExtents;
            Float3 cascadeExtents = maxExtents - minExtents;

            Matrix weaponShadowView_, weaponShadowProjection, weaponShadowVP;

            // Create view matrix (position light behind the weapon bounds)
            Matrix::LookAt(weaponCenter + dirLight->Direction * minExtents.Z, weaponCenter, Float3::Up, weaponShadowView_);

            // REVISED APPROACH: Generate light-space shadows but account for perspective projection differences
            // The key insight is that we need to match the depth precision and distribution between
            // shadow generation (orthographic) and weapon rendering (perspective)

            const float nearPlane = 1.0f;  // Near plane for shadow generation
            const float farPlane = cascadeExtents.Z;  // Far plane based on weapon bounds

            // CORRECTED APPROACH: Use proper bounds that match the weapon's visual FOV
            // Calculate the actual visible area at the weapon distance using the configured weapon FOV.
            // WeaponFOV lerps during ADS (eg. 30->12), which would rescale these ortho bounds - and the
            // texel grid they imply - every frame of the zoom animation (weapon shadow shimmer), so
            // latch the widest value seen instead; a narrower live FOV is always covered by it.
            const float weaponFovLive = renderContext.View.WeaponFOV > 0.0f ? renderContext.View.WeaponFOV : 54.0f;
            shadowsMutable.LatchedWeaponFov = Math::Max(shadowsMutable.LatchedWeaponFov, weaponFovLive);
            const float weaponFovDegrees = shadowsMutable.LatchedWeaponFov;
            const float weaponFOV = weaponFovDegrees * PI / 180.0f;
            const float aspect = 1.0f; // Square shadow map
            const float distanceToWeapon = 100.0f; // Same distance used for weaponCenter

            // Calculate the visible area at this distance with weapon FOV
            // tan(FOV/2) gives the half-height of the view frustum at distance 1
            const float halfHeightAtDistance = distanceToWeapon * tan(weaponFOV * 0.5f);
            const float halfWidthAtDistance = halfHeightAtDistance * aspect;

            // Add margin to ensure full weapon coverage
            const float margin = 1.2f; // 20% margin
            const Float2 shadowBounds = Float2(halfWidthAtDistance * margin, halfHeightAtDistance * margin);

            // Create standard orthographic projection with perspective-adjusted bounds
            Matrix::OrthoOffCenter(-shadowBounds.X, shadowBounds.X, -shadowBounds.Y, shadowBounds.Y, nearPlane, farPlane, weaponShadowProjection);

  
            weaponShadowView.SetUp(weaponShadowView_, weaponShadowProjection);
            weaponShadowView.Pass = DrawPass::WeaponDepth;
            weaponShadowView.Flags = renderContext.View.Flags;
            weaponShadowView.RenderLayersMask = renderContext.View.RenderLayersMask;

            // CRITICAL FIX: Use ViewProjection() from the RenderView, not manual multiply!
            // SetUp() stores the matrices, and we must use the EXACT same matrix that rendering uses
            weaponShadowVP = weaponShadowView.ViewProjection();

            // Create render context for weapon shadows (zero-initialize first)
            RenderContext weaponShadowContext = {};
            weaponShadowContext.Buffers = renderContext.Buffers;
            weaponShadowContext.Task = renderContext.Task;
            weaponShadowContext.List = RenderList::GetFromPool();
            weaponShadowContext.List->Clear();
            weaponShadowContext.LodProxyView = const_cast<RenderView*>(&renderContext.View);
            weaponShadowContext.View = weaponShadowView;
            weaponShadowContext.View.Origin = renderContext.View.Origin;
            // Don't overwrite CullingFrustum - SetUp() already configured it correctly
            // Don't pass main view to avoid FOV override contamination
            weaponShadowContext.View.PrepareCache(weaponShadowContext, (float)weaponShadowRes, (float)weaponShadowRes, Float2::Zero, nullptr);

            // Collect weapon geometry from both SceneDraw and SceneDrawAsync categories
            // We need both because Camera is in SceneDraw, but StaticModel/AnimatedModel are in SceneDrawAsync
            // CRITICAL: Must do them sequentially (not parallel) to avoid race condition with shared _drawListData
            RenderContextBatch weaponBatch;
            weaponBatch.Contexts.Add(weaponShadowContext);

            // Safety check before collection
            if (!renderContext.Task)
            {
                LOG(Warning, "Weapon shadows: renderContext.Task is null, skipping collection");
                goto skip_weapon_shadows;
            }

            renderContext.Task->OnCollectDrawCalls(weaponBatch, SceneRendering::DrawCategory::SceneDraw);
            renderContext.Task->OnCollectDrawCalls(weaponBatch, SceneRendering::DrawCategory::SceneDrawAsync);

            // CRITICAL: Wait for all async draw call collection jobs to complete before batch goes out of scope
            for (const uint64 label : weaponBatch.WaitLabels)
                JobSystem::Wait(label);
            weaponBatch.WaitLabels.Clear();

            auto& weaponCtx = weaponBatch.Contexts[0];

            // Safety check after collection
            if (!weaponCtx.List)
            {
                LOG(Warning, "Weapon shadows: weaponCtx.List is null after collection, skipping rendering");
                goto skip_weapon_shadows;
            }

            // Render weapon shadows
            if (shadowsMutable.ClearWeaponShadowMapAtlas)
                context->ClearDepth(shadowsMutable.WeaponShadowMapAtlas->View());

            context->SetRenderTarget(shadowsMutable.WeaponShadowMapAtlas->View(), (GPUTextureView*)nullptr);
            auto* tile = shadowsMutable.WeaponDirectionalLightTile;
            context->SetViewportAndScissors(Viewport(tile->X, tile->Y, tile->Width, tile->Height));

            if (!shadowsMutable.ClearWeaponShadowMapAtlas)
                ClearShadowMapTile(context, quadShaderCB, quadShaderData);

            weaponCtx.List->ExecuteDrawCalls(weaponCtx, DrawCallsListType::Depth);
            weaponCtx.List->ExecuteDrawCalls(weaponCtx, weaponCtx.List->ShadowDepthDrawCallsList, renderContext.List, nullptr);

            // Store weapon shadow data in buffer - EXACTLY like CSM does it
            // Step 1: ViewProjection * ClipToUV (same as CSM's SetWorldToShadow)
            const Matrix ClipToUV(
                0.5f, 0.0f, 0.0f, 0.0f,
                0.0f, -0.5f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.5f, 0.5f, 0.0f, 1.0f);
            Matrix weaponWorldToShadow;
            Matrix::Multiply(weaponShadowVP, ClipToUV, weaponWorldToShadow);

            // Step 2: CRITICAL FIX - Weapon shadows should use tile-local coordinates, not atlas scaling
            // The problem: CSM atlas transform scales by 1/atlasResolution, crushing our values
            // Solution: For weapon shadows, treat the tile as the full shadow map (no atlas scaling)

            const float weaponAtlasInv = 1.0f / weaponAtlasResolution;
            const float scaleX = tile->Width * weaponAtlasInv;
            const float scaleY = tile->Height * weaponAtlasInv;
            const float offsetX = tile->X * weaponAtlasInv;
            const float offsetY = tile->Y * weaponAtlasInv;

    
            Matrix atlasTransform(
                scaleX, 0.0f, 0.0f, 0.0f,
                0.0f, scaleY, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                offsetX, offsetY, 0.0f, 1.0f);
            Matrix::Multiply(weaponWorldToShadow, atlasTransform, weaponWorldToShadow);

            // Step 3: Transpose then extract columns (EXACTLY like CSM - lines 108-118, 1564-1567)
            // This effectively stores the pre-transpose matrix, which is correct for HLSL
            Matrix weaponWorldToShadowTransposed;
            Matrix::Transpose(weaponWorldToShadow, weaponWorldToShadowTransposed);

            // Cache weapon shadow buffer address for this light (before writing data)
            dirLight->WeaponShadowsBufferAddress = (uint32)(shadowsMutable.WeaponShadowsBuffer.Data.Count() / sizeof(Float4));

            // Write weapon shadow data to buffer - EXACTLY like CSM stores columns after transpose
            // This stores the original (pre-transpose) matrix, which is what HLSL expects
            shadowsMutable.WeaponShadowsBuffer.Write(weaponWorldToShadowTransposed.GetColumn1());
            shadowsMutable.WeaponShadowsBuffer.Write(weaponWorldToShadowTransposed.GetColumn2());
            shadowsMutable.WeaponShadowsBuffer.Write(weaponWorldToShadowTransposed.GetColumn3());
            shadowsMutable.WeaponShadowsBuffer.Write(weaponWorldToShadowTransposed.GetColumn4());

      
            // Cleanup - Return list to pool (it will be cleared automatically)
            RenderList::ReturnToPool(weaponCtx.List);
            weaponCtx.List = nullptr; // Null out to prevent use-after-free
            weaponShadowContext.List = nullptr; // Also null the original
            shadowsMutable.ClearWeaponShadowMapAtlas = false;
        }
    }
    skip_weapon_shadows:;

    // Always flush weapon shadow buffer and set up view (even if empty/dummy)
    shadowsMutable.WeaponShadowsBuffer.Flush(context);
    shadowsMutable.WeaponShadowsBufferView = shadowsMutable.WeaponShadowsBuffer.GetBuffer() ? shadowsMutable.WeaponShadowsBuffer.GetBuffer()->View() : nullptr;

    // Restore GPU context
    context->ResetSR();
    context->ResetRenderTarget();
    context->SetViewportAndScissors(renderContext.Task->GetViewport());
    shadows.ClearShadowMapAtlas = false;
}

void ShadowsPass::RenderShadowMask(RenderContextBatch& renderContextBatch, RenderLightData& light, GPUTextureView* shadowMask)
{
    ASSERT(light.HasShadow);
    PROFILE_GPU_CPU("Shadow");
    GPUContext* context = GPUDevice::Instance->GetMainContext();
    RenderContext& renderContext = renderContextBatch.GetMainContext();
    const ShadowsCustomBuffer& shadows = *renderContext.Buffers->FindCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false);
    ASSERT(shadows.LastFrameUsed == Engine::FrameCount);
    auto& view = renderContext.View;
    auto shader = _shader->GetShader();
    const bool isLocalLight = light.IsPointLight || light.IsSpotLight;
    int32 shadowQuality = shadows.MaxShadowsQuality;
    if (isLocalLight)
    {
        // Reduce shadows quality for smaller lights
        if (light.ScreenSize < 0.25f)
            shadowQuality--;
        if (light.ScreenSize < 0.1f)
            shadowQuality--;
        shadowQuality = Math::Max(shadowQuality, 0);
    }

    // Setup shader data
    Data sperLight;
    GBufferPass::SetInputs(view, sperLight.GBuffer);
    if (light.IsDirectionalLight)
        ((RenderDirectionalLightData&)light).SetShaderData(sperLight.Light, true);
    else if (light.IsPointLight)
        ((RenderPointLightData&)light).SetShaderData(sperLight.Light, true);
    else if (light.IsSpotLight)
        ((RenderSpotLightData&)light).SetShaderData(sperLight.Light, true);
    Matrix::Transpose(view.ViewProjection(), sperLight.ViewProjectionMatrix);
    sperLight.TemporalTime = renderContext.List->Setup.UseTemporalAAJitter ? RenderTools::ComputeTemporalTime() : 0.0f;
    sperLight.ContactShadowsDistance = light.ShadowsDistance;
    sperLight.ContactShadowsLength = EnumHasAnyFlags(view.Flags, ViewFlags::ContactShadows) ? light.ContactShadowsLength : 0.0f;

    bool isViewInside;
    if (isLocalLight)
    {
        // Calculate world view projection matrix for the light sphere
        Matrix world, wvp;
        RenderTools::ComputeSphereModelDrawMatrix(renderContext.View, light.Position, ((RenderLocalLightData&)light).Radius, world, isViewInside);
        Matrix::Multiply(world, view.ViewProjection(), wvp);
        Matrix::Transpose(wvp, sperLight.WVP);
    }

    // Render shadow in screen space
    GPUConstantBuffer* cb0 = shader->GetCB(0);
    context->UpdateCB(cb0, &sperLight);
    context->BindCB(0, cb0);
    context->BindSR(5, shadows.ShadowsBufferView);
    context->BindSR(6, shadows.ShadowMapAtlas);
    context->BindSR(8, shadows.WeaponShadowsBufferView);
    context->BindSR(9, shadows.WeaponShadowMapAtlas);

  
    const int32 permutationIndex = shadowQuality + (sperLight.ContactShadowsLength > ZeroTolerance ? 4 : 0);
    GPUTexture* depthBuffer = renderContext.Buffers->DepthBuffer;
    const bool depthBufferReadOnly = EnumHasAnyFlags(depthBuffer->Flags(), GPUTextureFlags::ReadOnlyDepthView);
    context->SetRenderTarget(depthBufferReadOnly ? depthBuffer->ViewReadOnlyDepth() : nullptr, shadowMask);
    if (_depthBounds)
    {
        Float2 minMaxDepth;
        if (light.IsPointLight || light.IsSpotLight)
            minMaxDepth = RenderTools::GetDepthBounds(view, BoundingSphere(light.Position, ((RenderLocalLightData&)light).Radius));
        else //if (light.IsDirectionalLight)
            minMaxDepth = Float2(0.0f, RenderTools::DepthBoundMaxBackground);
        context->SetDepthBounds(minMaxDepth.X, minMaxDepth.Y);
    }
    if (light.IsPointLight)
    {
        context->SetState((isViewInside ? _psShadowPointInside : _psShadowPoint).Get(permutationIndex));
        _sphereModel->LODs.Get()[0].Meshes.Get()[0].Render(context);
    }
    else if (light.IsSpotLight)
    {
        context->SetState((isViewInside ? _psShadowSpotInside : _psShadowSpot).Get(permutationIndex));
        _sphereModel->LODs.Get()[0].Meshes.Get()[0].Render(context);
    }
    else //if (light.IsDirectionalLight)
    {
        auto* atlasLight = shadows.Lights.TryGet(light.ID);
        ASSERT_LOW_LAYER(atlasLight);
        context->SetState(_psShadowDir.Get(permutationIndex + (atlasLight->BlendCSM ? 8 : 0)));
        context->DrawFullscreenTriangle();
    }

    // Cleanup
    context->ResetRenderTarget();
    context->UnBindSR(5);
    context->UnBindSR(6);
    context->UnBindSR(8);
    context->UnBindSR(9);
}

void ShadowsPass::GetShadowAtlas(const RenderBuffers* renderBuffers, GPUTexture*& shadowMapAtlas, GPUBufferView*& shadowsBuffer)
{
    const ShadowsCustomBuffer* shadowsPtr = renderBuffers->FindCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false);
    if (shadowsPtr && shadowsPtr->ShadowMapAtlas && shadowsPtr->LastFrameUsed == Engine::FrameCount)
    {
        shadowMapAtlas = shadowsPtr->ShadowMapAtlas;
        shadowsBuffer = shadowsPtr->ShadowsBufferView;
    }
    else
    {
        shadowMapAtlas = nullptr;
        shadowsBuffer = nullptr;
    }
}
