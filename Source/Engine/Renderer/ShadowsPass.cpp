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

// Clipmap rasterization pancaking pad. The clipmap's natural ortho [armCenter - DepthRange,
// armCenter + DepthRange] clips tall structures above the cascade center when DepthRange is
// small (near cascades). Pad the near (toward-sun) side generously so massive geometry
// (skyscrapers, cliffs, mountains) casts shadow into near cascades even when far from camera.
//
// Sized at 10km because:
//   - This is a per-strip-render cost, not a per-frame cost - cached pixels are unaffected,
//     so widening the rasterization extent is "free" in steady-state.
//   - Real-world tall structures top out around 1km (Burj Khalifa ~830m); 10km covers any
//     plausible cinematic / sci-fi megastructure with margin.
//   - Depth-quantum impact at the smallest cascade: with D24 and DepthRange=50m + pad=10000m,
//     ~0.6mm per depth level - well below shadow-bias scales.
//   - Mirrors and exceeds the cascade view-projection's `cullRangeExtent = 100000.0f` (1km)
//     pancaking. The clipmap can afford to go further because it caches.
//
// If shadows from very tall structures STILL fail to appear in near cascades, bump this.
// See: D:\code\notes\shadow_clipmap_assumptions.md (invariant I8 / verticality).
#define SHADOW_CLIPMAP_NEAR_PAD METERS_TO_UNITS(10000.0f) // 10km of vertical headroom toward the sun

// HACK debug toggles for shadow clipmap investigation. Flip and rebuild.
// g_ClipmapIsolateStatic: when true and clipmap is active for the directional light, skip
// the per-cascade dynamic shadow draw so the cascade tile shows ONLY composited clipmap content.
// g_ClipmapDebugDraw: when true, ShadowsPass::DrawClipmapDebugOverlay paints clipmap level depth
// textures as grayscale thumbnails down the right edge of the output (called from Renderer.cpp).
static bool g_ClipmapIsolateStatic = false;
static bool g_ClipmapDebugDraw = false;

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
static void ComputeCascadeSphere(const Float3* frustumCornersVs, const Matrix& invView, const Float3& lightRight, const Float3& lightUp, const Float3& lightDir, int32 resolution, float splitMinRatio, float splitMaxRatio, float oldSplitMinRatio, float csmOverlap, Float3& outCenter, float& outRadius)
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

    // Bounding-sphere center + radius (radius quantized to 1/16 to reduce shimmer).
    Float3 center = Float3::Zero;
    for (int32 i = 0; i < 8; i++)
        center += cornersWs[i];
    center *= 1.0f / 8.0f;
    float radius = 0.0f;
    for (int32 i = 0; i < 8; i++)
        radius = Math::Max(radius, (cornersWs[i] - center).LengthSquared());
    radius = Math::Ceil(Math::Sqrt(radius) * 16.0f) / 16.0f;

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
    Float4 ClipmapSunDir;      // xyz=sunDir, w=levelCount (as float)
    Float4 ClipmapLightRight;  // xyz=lightRight, w=bias
    Float4 ClipmapLightUp;     // xyz=lightUp, w=unused
    Float4 ClipmapParams[MAX_CSM_CASCADES]; // xy=center, z=extent, w=depthRange
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
    float DepthRange = 0.0f;            // Far plane of ortho projection
    Int2 ScrollTexels = Int2::Zero;      // Current camera position in texel coords (level center)
    Int2 PrevScrollTexels = Int2::Zero;  // Previous frame scroll
    Int2 DirtyStrip = Int2::Zero;        // Delta scroll (texels in X / Y to render in strip pass)
    // TextureOriginTexels anchors the toroidal mapping. Texture pixel for world-texel w:
    //   px = ((w.X - origin.X) mod R + R) mod R
    //   py = ((origin.Y - 1 - w.Y) mod R + R) mod R   (Y-flip matches existing ortho convention)
    // Set on full-redraw to (ScrollTexels.X - R/2, ScrollTexels.Y + R/2). Strip updates leave it unchanged.
    Int2 TextureOriginTexels = Int2::Zero;
    bool NeedsFullRedraw = true;         // First frame, sun changed, or |scroll delta| >= R
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
    // Cumulative-drift instrumentation: snapshot at time of last full redraw. Per-frame Init only
    // compares to Prev*, so gradual drift slides under the 0.1% threshold while the cached texture
    // diverges from current cascade math over many frames. LastRedraw* captures the anchor the
    // texture content was actually drawn against, so we can measure true divergence.
    float LastRedrawTexelSize = 0.0f;
    float LastRedrawDepthRange = 0.0f;
    float LastRedrawWorldExtent = 0.0f;
    Float4 CompositingColor;             // xy=UV scale, zw=UV offset (logical) for PS_ClipmapComposite
    Float2 DepthRemap;                   // x=scale, y=bias for depth remapping
    Float2 WrapOffsetUV;                 // frac((leftEdgeWorldTexel - origin)/R); added inside frac() in shader
    // Per-arm/sub-rect view+proj are built locally in RenderClipmapStrip - no cached matrices kept on the level.

    ~ShadowClipmapLevel() { SAFE_DELETE_GPU_RESOURCE(DepthTexture); }
};

// Toroidal shadow clipmap for caching static geometry across multiple levels
struct ShadowClipmap
{
    ShadowClipmapLevel Levels[MAX_CSM_CASCADES + 1]; // +1 for beyond-CSM level
    int32 LevelCount = 0;
    Float3 LightRight, LightUp, SunDir; // Light-space basis vectors
    Float3 CachedSunDirection;           // For detecting sun rotation
    // Stale-anchor instrumentation: snapshot of light basis at end of previous Init. A flip
    // (eg. up-vector threshold crossing) without a sun-rotation trigger means level textures
    // were rendered with a different (R_L, U_L) basis than the compositor now assumes.
    Float3 PrevLightRight = Float3::Zero;
    Float3 PrevLightUp = Float3::Zero;
    float BeyondCSMExtent = 0.0f;        // World extent of the extra level
    bool Enabled = false;
    Guid LightId;                         // ID of directional light using this clipmap

    void Init(PixelFormat format, int32 cascadeCount, const float* cascadeRadii, const float* splitDistances,
              int32 cascadeResolution, const Float3& sunDir, float beyondExtent)
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

        BeyondCSMExtent = beyondExtent;
        LevelCount = cascadeCount + (beyondExtent > 0.0f ? 1 : 0);

        for (int32 i = 0; i < cascadeCount; i++)
        {
            auto& level = Levels[i];
            const float prevTexelSize = level.PrevTexelSize;
            const float prevDepthRange = level.PrevDepthRange;
            level.Resolution = cascadeResolution;
            // Clipmap must cover the cascade regardless of camera rotation.
            // The cascade center can be at most splitDistance from camera in light-space XY,
            // and the cascade sphere extends cascadeRadius beyond that.
            float requiredHalfExtent = splitDistances[i] + cascadeRadii[i];
            float rawExtent = requiredHalfExtent * 2.0f;
            // Decouple from camera FOV churn. cascadeRadii is the bounding-sphere radius of
            // the cascade frustum slice's view-space corners - by construction FOV-dependent
            // via tan(FOV/2). Derelict's Homunculus drives FOVFactor between 1.0 and 1.08
            // (BASE_FOV x REVEAL = 90deg -> 97.2deg), which scales bounding radius by tan(48.6deg)/
            // tan(45deg) ~ 1.134 - a 13% swing - and FlinchFov modulates on top. Observed
            // per-frame rawExtent growth in profiled play: ~8-10% (orders of magnitude beyond
            // the 0.1-0.5% the 1/16 bucket was tuned for), driving a full per-cascade redraw
            // every frame for the duration of the lerp. The clipmap is a STATIC SHADOW CACHE
            // - it must over-cover plausible cascade footprints rather than reshape to fit
            // current FOV. Quantize to 1/4 (25% bucket -> 37.5% asymmetric hysteresis) so any
            // realistic gameplay FOV swing stays within one bucket. Cost: cache covers up to
            // ~25% more world area than the cascade tile, so static-shadow texel density at
            // the fringe is ~25% lower than dynamic-shadow density. Buckets grow with level
            // scale so all cascades share the same relative headroom.
            const float magnitude = Math::Pow(2.0f, Math::Floor(Math::Log2(Math::Max(rawExtent, 1.0f))));
            const float bucketSize = Math::Max(1.0f, magnitude * (1.0f / 4.0f));
            const float candidate = Math::Ceil(rawExtent / bucketSize) * bucketSize;
            // Asymmetric hysteresis: when raw oscillates across a bucket boundary, naive
            // ceil-snap flip-flops every frame. Growth grants headroom (candidate + 1 bucket)
            // so subsequent dips don't immediately re-cross. Shrink only when raw drops more
            // than 1.5 buckets below current - guarantees the post-shrink value still has
            // 0.5 buckets of margin before the next up-crossing. Net headroom against
            // oscillation ~ 1.5 buckets (~9%) - absorbs sustained FOV lerps without redrawing.
            float newExtent;
            if (level.WorldExtent <= 0.0f)
                newExtent = candidate + bucketSize;
            else if (rawExtent > level.WorldExtent)
                newExtent = candidate + bucketSize;
            else if (rawExtent < level.WorldExtent - 1.5f * bucketSize)
                newExtent = candidate + bucketSize;
            else
                newExtent = level.WorldExtent;
            level.WorldExtent = newExtent;
            level.TexelSize = level.WorldExtent / level.Resolution;
            level.DepthRange = level.WorldExtent * 2.0f; // Conservative depth range

            // Instrumentation: TexelSize/DepthRange drift while texture content is anchored
            // to the previous values. Either the assumption "camera intrinsics stable ->
            // cascadeRadii stable" is wrong, or something else (atlas scaledown, view.Far drift,
            // partition change) is moving these. Force a full redraw so composite math stays
            // consistent until we identify root cause.
            const bool tsDrift = prevTexelSize > 0.0f && Math::Abs(level.TexelSize - prevTexelSize) > 1e-3f * level.TexelSize;
            const bool drDrift = prevDepthRange > 0.0f && Math::Abs(level.DepthRange - prevDepthRange) > 1e-3f * level.DepthRange;
            if ((tsDrift || drDrift) && !level.NeedsFullRedraw && !sunChanged)
            {
                LOG(Warning, "[ShadowClipmap] level {0} per-frame drift: TexelSize {1}->{2}, DepthRange {3}->{4}",
                    i, prevTexelSize, level.TexelSize, prevDepthRange, level.DepthRange);
                level.NeedsFullRedraw = true;
            }
            level.PrevTexelSize = level.TexelSize;
            level.PrevDepthRange = level.DepthRange;

            // Cumulative drift vs. the values used to render the cached texture content. Gradual
            // FOV/near changes can slide the per-frame check forever; this catches the actual
            // divergence between cached pixels and current sampling math.
            const float lastTs = level.LastRedrawTexelSize;
            const float lastDr = level.LastRedrawDepthRange;
            const bool tsCum = lastTs > 0.0f && Math::Abs(level.TexelSize - lastTs) > 0.01f * lastTs;
            const bool drCum = lastDr > 0.0f && Math::Abs(level.DepthRange - lastDr) > 0.01f * lastDr;
            if ((tsCum || drCum) && !level.NeedsFullRedraw && !sunChanged)
            {
                LOG(Warning, "[ShadowClipmap] level {0} CUMULATIVE drift since last redraw: TexelSize {1}->{2} ({3}%), DepthRange {4}->{5} ({6}%)",
                    i,
                    lastTs, level.TexelSize, (lastTs > 0.0f ? 100.0f * (level.TexelSize - lastTs) / lastTs : 0.0f),
                    lastDr, level.DepthRange, (lastDr > 0.0f ? 100.0f * (level.DepthRange - lastDr) / lastDr : 0.0f));
                level.NeedsFullRedraw = true;
            }

            if (sunChanged)
            {
                LOG(Info, "[ClipmapTrigger] L{0} sunChanged: cached=({1},{2},{3}) cur=({4},{5},{6}) dot={7}",
                    i, CachedSunDirection.X, CachedSunDirection.Y, CachedSunDirection.Z,
                    SunDir.X, SunDir.Y, SunDir.Z, Float3::Dot(CachedSunDirection, SunDir));
                level.NeedsFullRedraw = true;
            }

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
            {
                LOG(Info, "[ClipmapTrigger] L{0} basisCoherent FAIL: dot={1} last=({2},{3},{4}) cur=({5},{6},{7})",
                    i, Float3::Dot(level.LastRedrawSunDir, SunDir),
                    level.LastRedrawSunDir.X, level.LastRedrawSunDir.Y, level.LastRedrawSunDir.Z,
                    SunDir.X, SunDir.Y, SunDir.Z);
                level.NeedsFullRedraw = true;
            }

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
                level.NeedsFullRedraw = true;
            }
        }

        // Setup beyond-CSM level
        if (beyondExtent > 0.0f)
        {
            auto& level = Levels[cascadeCount];
            const float prevTexelSize = level.PrevTexelSize;
            const float prevDepthRange = level.PrevDepthRange;
            level.Resolution = cascadeResolution;
            level.WorldExtent = beyondExtent;
            level.TexelSize = level.WorldExtent / level.Resolution;
            level.DepthRange = level.WorldExtent * 2.0f;

            const bool tsDrift = prevTexelSize > 0.0f && Math::Abs(level.TexelSize - prevTexelSize) > 1e-3f * level.TexelSize;
            const bool drDrift = prevDepthRange > 0.0f && Math::Abs(level.DepthRange - prevDepthRange) > 1e-3f * level.DepthRange;
            if ((tsDrift || drDrift) && !level.NeedsFullRedraw && !sunChanged)
            {
                LOG(Warning, "[ShadowClipmap] beyond-CSM stale-anchor drift: TexelSize {0}->{1}, DepthRange {2}->{3}",
                    prevTexelSize, level.TexelSize, prevDepthRange, level.DepthRange);
                level.NeedsFullRedraw = true;
            }
            level.PrevTexelSize = level.TexelSize;
            level.PrevDepthRange = level.DepthRange;

            if (sunChanged)
                level.NeedsFullRedraw = true;

            // Per-level basis-coherence (same rationale as the cascade-level check above).
            const bool basisCoherent = level.LastRedrawSunDir != Float3::Zero &&
                                       Float3::Dot(level.LastRedrawSunDir, SunDir) > 1.0f - 1e-7f;
            if (!basisCoherent)
            {
                LOG(Info, "[ClipmapTrigger] Lbeyond basisCoherent FAIL: dot={0} last=({1},{2},{3}) cur=({4},{5},{6})",
                    Float3::Dot(level.LastRedrawSunDir, SunDir),
                    level.LastRedrawSunDir.X, level.LastRedrawSunDir.Y, level.LastRedrawSunDir.Z,
                    SunDir.X, SunDir.Y, SunDir.Z);
                level.NeedsFullRedraw = true;
            }

            if (!level.DepthTexture)
            {
                level.DepthTexture = GPUDevice::Instance->CreateTexture(TEXT("Shadow Clipmap Beyond"));
                auto desc = GPUTextureDescription::New2D(cascadeResolution, cascadeResolution, format, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil);
                if (level.DepthTexture->Init(desc))
                {
                    LOG(Warning, "Failed to create shadow clipmap beyond texture");
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
                    LOG(Warning, "Failed to resize shadow clipmap beyond texture");
                    Enabled = false;
                    return;
                }
                level.NeedsFullRedraw = true;
            }
        }

        Enabled = true;
    }

    void ComputeScroll(const Float3& cameraPos)
    {
        for (int32 i = 0; i < LevelCount; i++)
        {
            auto& level = Levels[i];
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
                LOG(Info, "[ClipmapTrigger] L{0} scroll-overflow: delta=({1},{2}) R={3}",
                    i, level.DirtyStrip.X, level.DirtyStrip.Y, level.Resolution);
                level.NeedsFullRedraw = true;
                level.DirtyStrip = Int2::Zero;
            }
        }
    }

    void MarkFullRedraw()
    {
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
    int32 ShadowsResolution;

    void Set(const RenderView& view, const RenderLightData& light, const Float4& cascadeSplits = Float4::Zero)
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

    void ValidateCache(const RenderView& view, const RenderLightData& light)
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
            // Sun
            if (!Float3::NearEqual(Cache.Position, view.Position, SHADOWS_POSITION_ERROR) ||
                !Float4::NearEqual(Cache.CascadeSplits, CascadeSplits) ||
                Float3::Dot(Cache.ViewDirection, view.Direction) < SHADOWS_ROTATION_ERROR)
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
            for (int32 i = 0; i < atlasLight.TilesCount; i++)
                atlasLight.Tiles[i].ClearDynamic();
        }
        StaticAtlas.Clear();
        StaticAtlasPixelsUsed = 0;
    }

    void Reset()
    {
        Lights.Clear();
        ClearDynamic();
        ClearStatic();
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
            DirtyStaticBounds(a->GetSphere());
    }

    void OnSceneRenderingUpdateActor(Actor* a, const BoundingSphere& prevBounds, UpdateFlags flags) override
    {
        // Dirty static objects to redraw when changed (eg. material modification)
        if (IsEffectivelyShadowStatic(a))
        {
            const BoundingSphere curBounds = a->GetSphere();
            if (Clipmap.Enabled)
            {
                String chain = BuildActorChain(a);
                // moved = world-space center travel this update; recurring nonzero values are the
                // real offenders (a Shadow-static asset being moved every frame). A near-zero moved
                // with a flag/material flags is a one-off and harmless.
                const float moved = (float)Float3::Distance((Float3)curBounds.Center, (Float3)prevBounds.Center);
                LOG(Info, "[ClipmapDirty] moved={0} path='{1}' type='{2}' flags={3} prev=({4},{5},{6})r={7} cur=({8},{9},{10})r={11}",
                    moved, chain, String(a->GetType().GetName()), (int32)flags,
                    prevBounds.Center.X, prevBounds.Center.Y, prevBounds.Center.Z, prevBounds.Radius,
                    curBounds.Center.X, curBounds.Center.Y, curBounds.Center.Z, curBounds.Radius);
            }
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
    if (_psDepthVisualize == nullptr)
    {
        // HACK clipmap debug overlay: color-write fullscreen triangle, no depth.
        psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = GPUDevice::Instance->QuadShader->GetPS("PS_DepthVisualize");
        _psDepthVisualize = GPUDevice::Instance->CreatePipelineState();
        if (_psDepthVisualize->Init(psDesc))
            return true;
    }

    return false;
}

void ShadowsPass::SetupRenderContext(RenderContext& renderContext, RenderContext& shadowContext, ShadowAtlasLight* atlasLight, RenderContext* dynamicContext)
{
    const auto& view = renderContext.View;

    // Use the current render view to sync model LODs with the shadow maps rendering stage
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

void ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderLightData& light, ShadowAtlasLight& atlasLight)
{
    // Copy light properties
    atlasLight.Sharpness = light.ShadowsSharpness;
    atlasLight.Fade = light.ShadowsStrength;
    atlasLight.NormalOffsetScale = light.ShadowsNormalOffsetScale * NormalOffsetScaleTweak * (1.0f / (float)atlasLight.Resolution);
    atlasLight.Bias = light.ShadowsDepthBias;
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

    // Adjust bias to account for lower shadow quality
    if (shadows.MaxShadowsQuality == 0)
        atlasLight.Bias *= 1.5f;
}

bool ShadowsPass::SetupLight(ShadowsCustomBuffer& shadows, RenderContext& renderContext, RenderContextBatch& renderContextBatch, RenderLocalLightData& light, ShadowAtlasLight& atlasLight)
{
    SetupLight(shadows, renderContext, renderContextBatch, (RenderLightData&)light, atlasLight);
    atlasLight.Bounds.Radius = light.Radius;

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

    const auto& view = renderContext.View;
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
            const auto logRatio = Math::Clamp(1.0f - lambda, 0.0f, 1.0f);
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

    // Update cached state (invalidate it if the light changed)
    atlasLight.ValidateCache(renderContext.View, light);

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

    const bool useClipmapForLight = light.StaticShadows && shadows.EnableStaticShadows && EnumHasAllFlags(light.StaticFlags, StaticFlags::Shadow);

    // Init shadow data. Allow non-dynamic paths (clipmap composite, static atlas copy) to still
    // process even when no dynamic cascade contexts will be added.
    if (atlasLight.ContextCount == 0 && !useClipmapForLight && !atlasLight.HasStaticShadowContext)
        return;
    if (atlasLight.ContextCount > 0)
        renderContextBatch.Contexts.AddDefault(atlasLight.ContextCount);
    atlasLight.Cache.Set(renderContext.View, light, atlasLight.CascadeSplits);

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
        Matrix invProjectionMatrix;
        Matrix::Invert(renderContext.View.NonJitteredProjection, invProjectionMatrix);
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
        ComputeCascadeSphere(frustumCornersVs, renderContext.View.IV, lightRight, lightUp, light.Direction, atlasLight.Resolution, splitMinRatio, splitMaxRatio, oldSplitMinRatio, csmOverlap, frustumCenter, frustumRadius);

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
        shadowContext.View.CascadeIndex = (int8)cascadeIndex;
        shadowContext.View.ModelLODBias += cascadeIndex;
        shadowContext.View.PrepareCache(shadowContext, shadowMapsSize, shadowMapsSize, Float2::Zero, &renderContext.View);
    }

    // Setup shadow clipmap for static geometry caching
    auto& clipmap = shadows.Clipmap;
    const bool useClipmap = light.StaticShadows && shadows.EnableStaticShadows && EnumHasAllFlags(light.StaticFlags, StaticFlags::Shadow);

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
                ComputeCascadeSphere(frustumCornersVs, renderContext.View.IV, lightRightClip, lightUpClip, light.Direction, atlasLight.Resolution, splitMinRatio2, splitMaxRatio2, oldSplitMin2, csmOverlap, cascadeFrustumCenters[ci], cascadeRadii[ci]);
            }
        }

        const PixelFormat clipmapFormat = shadows.ShadowMapAtlas->GetDescription().Format;
        float splitDistances[MAX_CSM_CASCADES];
        for (int32 i = 0; i < csmCount; i++)
            splitDistances[i] = atlasLight.CascadeSplits.Raw[i];
        clipmap.Init(clipmapFormat, csmCount, cascadeRadii, splitDistances, atlasLight.Resolution, light.Direction, light.StaticShadowBeyondCSMExtent);
        clipmap.LightId = light.ID;

        if (clipmap.Enabled)
        {
            clipmap.ComputeScroll(view.Position);

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
                // Clipmap now covers [-(DepthRange + NEAR_PAD), +DepthRange] along SunDir axis
                // (eye pancaked by SHADOW_CLIPMAP_NEAR_PAD toward sun; ortho near=0, far=2*DepthRange + NEAR_PAD).
                // Cascade covers [frustumCenter - R, frustumCenter + R] along SunDir axis (near=0, far=2R).
                // General formula: A = clipmapWorldRange / cascadeWorldRange,
                //                  B = (clipmapNearWorldZ - cascadeNearWorldZ) / cascadeWorldRange.
                const float cascadeRadius = cascadeRadii[ci];
                const float clipmapWorldRange = 2.0f * level.DepthRange + SHADOW_CLIPMAP_NEAR_PAD;
                const float cascadeWorldRange = 2.0f * cascadeRadius;
                const float clipmapNearWorldZ = -(level.DepthRange + SHADOW_CLIPMAP_NEAR_PAD);
                const float frustumCenterLightZ = Float3::Dot(cascadeFrustumCenters[ci], clipmap.SunDir);
                const float cascadeNearWorldZ = frustumCenterLightZ - cascadeRadius;
                const float depthA = clipmapWorldRange / cascadeWorldRange;
                const float depthB = (clipmapNearWorldZ - cascadeNearWorldZ) / cascadeWorldRange;
                level.DepthRemap = Float2(depthA, depthB);
            }

        }
    }
    else
    {
        clipmap.Enabled = false;
    }

    // Cascade static-exclusion filter. Applied when:
    //   - clipmap is providing static shadows (don't double-render in cascade), OR
    //   - StaticShadows is off (user explicitly wants no static contribution from any path).
    // NOT applied when StaticShadows is on but clipmap failed to enable (e.g. light not flagged
    // Static, atlas alloc failed) - in that case let the cascade render static the old way so we
    // don't silently drop shadows. Skipped when DynamicShadows is off (no contexts allocated).
    if (atlasLight.RenderDynamic && (clipmap.Enabled || !light.StaticShadows))
    {
        int32 ctxIdx = 0;
        for (int32 ci = 0; ci < csmCount; ci++)
        {
            auto& tile = atlasLight.Tiles[ci];
            if (tile.SkipUpdate)
                continue;
            auto& shadowCtx = renderContextBatch.Contexts[atlasLight.ContextIndex + ctxIdx++];
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
    SAFE_DELETE_GPU_RESOURCE(_psDepthVisualize);
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
            atlasLight.TilesNeeded = Math::Clamp(((const RenderDirectionalLightData*)light)->CascadeCount, 1, MAX_CSM_CASCADES);

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
    if (shadows.StaticAtlas.IsInitialized())
    {
        // Register for active scenes changes to invalidate static shadows
        for (SceneRendering* scene : renderContext.List->Scenes)
            shadows.ListenSceneRendering(scene);
    }

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
static void RenderClipmapStrip(
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
        return;
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
    // Pancake the near plane SHADOW_CLIPMAP_NEAR_PAD further toward the sun so tall geometry
    // above the cascade center isn't clipped (matches cascade view's `cullRangeExtent`).
    const float armEyeOffset = level.DepthRange + SHADOW_CLIPMAP_NEAR_PAD;
    const Float3 armEye = armCenterW - clipmap.SunDir * armEyeOffset;

    // Use the SAME LightUp the clipmap stored at Init time (single source of truth via
    // ComputeLightBasis). Recomputing it here from a world-axis "up hint" with a threshold
    // check would risk disagreeing with the compositor in the threshold band, leaving the
    // rasterized texture content rotated 90deg relative to the sampling math.
    Matrix armView, armProj;
    Matrix::LookAt(armEye, armCenterW, clipmap.LightUp, armView);
    // Far plane extended by the same pad so the full far-side reach (armCenter + SunDir * DepthRange)
    // remains captured. Total ortho range: 2*DepthRange + NEAR_PAD.
    Matrix::OrthoOffCenter(-armHx, armHx, -armHy, armHy, 0.0f, level.DepthRange * 2.0f + SHADOW_CLIPMAP_NEAR_PAD, armProj);
    Matrix armVP;
    Matrix::Multiply(armView, armProj, armVP);

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
    if (!mainRenderContext.Task)
        return;
    mainRenderContext.Task->OnCollectDrawCalls(armBatch, SceneRendering::DrawCategory::SceneDraw);
    mainRenderContext.Task->OnCollectDrawCalls(armBatch, SceneRendering::DrawCategory::SceneDrawAsync);
    for (const uint64 label : armBatch.WaitLabels)
        JobSystem::Wait(label);
    armBatch.WaitLabels.Clear();
    auto& ctx = armBatch.Contexts[0];
    if (!ctx.List)
    {
        return;
    }

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
            // Same pancaking as the parent arm - eye pushed SHADOW_CLIPMAP_NEAR_PAD further toward
            // the sun, ortho far extended by the same pad. Total range matches.
            const Float3 subEye = subCenterW - clipmap.SunDir * (level.DepthRange + SHADOW_CLIPMAP_NEAR_PAD);

            Matrix subView, subProj;
            Matrix::LookAt(subEye, subCenterW, clipmap.LightUp, subView);
            Matrix::OrthoOffCenter(-subHx, subHx, -subHy, subHy, 0.0f, level.DepthRange * 2.0f + SHADOW_CLIPMAP_NEAR_PAD, subProj);

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
}

// One-shot debug dump trigger. Set via ShadowsPass::RequestDump(); consumed once per request
// at the end of RenderShadowMaps. Volatile (rather than atomic) because the read and the
// reset both happen on the main render thread; writers (key handler, console) just set it.
static volatile bool ShadowsDumpRequested = false;

void ShadowsPass::RequestDump()
{
    ShadowsDumpRequested = true;
}

// Worker-thread routine: pulls texture bytes from GPU and writes the raw blob to disk.
// Lives on a thread-pool task because GPUTexture::DownloadData asserts off-main-thread
// (it queues a copy-to-staging on the GPU side then blocks on a CPU fence).
static void DumpTextureRawWorker(GPUTexture* tex, String path)
{
    if (!tex)
        return;
    TextureData data;
    if (tex->DownloadData(data))
    {
        LOG(Warning, "[ShadowDump] DownloadData failed for {0}", path);
        return;
    }
    if (data.Items.IsEmpty() || data.Items[0].Mips.IsEmpty())
    {
        LOG(Warning, "[ShadowDump] no mip data for {0}", path);
        return;
    }
    const TextureMipData& mip = data.Items[0].Mips[0];
    if (mip.Data.Length() == 0)
    {
        LOG(Warning, "[ShadowDump] empty mip data for {0}", path);
        return;
    }
    auto* file = FileWriteStream::Open(path);
    if (!file)
    {
        LOG(Warning, "[ShadowDump] failed to open {0}", path);
        return;
    }
    // 32-byte header: magic, width, height, format, rowPitch, dataBytes, reserved, reserved
    const uint32 magic = 0x4D444853u; // 'SHDM'
    const uint32 width = (uint32)data.Width;
    const uint32 height = (uint32)data.Height;
    const uint32 format = (uint32)data.Format;
    const uint32 rowPitch = mip.RowPitch;
    const uint32 dataBytes = (uint32)mip.Data.Length();
    const uint32 reserved0 = 0, reserved1 = 0;
    file->WriteBytes(&magic, sizeof(uint32));
    file->WriteBytes(&width, sizeof(uint32));
    file->WriteBytes(&height, sizeof(uint32));
    file->WriteBytes(&format, sizeof(uint32));
    file->WriteBytes(&rowPitch, sizeof(uint32));
    file->WriteBytes(&dataBytes, sizeof(uint32));
    file->WriteBytes(&reserved0, sizeof(uint32));
    file->WriteBytes(&reserved1, sizeof(uint32));
    file->WriteBytes(mip.Data.Get(), dataBytes);
    file->Close();
    Delete(file);
}

static void DumpTextureRaw(GPUTexture* tex, const String& path)
{
    if (!tex)
        return;
    // Capture pointer + path by value; the texture lives as long as the renderer does.
    Task::StartNew(Function<void()>([tex, path]() { DumpTextureRawWorker(tex, path); }));
}

static String JsonFloat(float v)
{
    // Avoid "inf"/"nan" tripping strict JSON parsers (no Math::IsFinite in Flax - inline check)
    if (v != v || v > 1e30f || v < -1e30f)
        return TEXT("null");
    return String::Format(TEXT("{0}"), v);
}

static String JsonFloat3(const Float3& v)
{
    return String::Format(TEXT("[{0}, {1}, {2}]"), JsonFloat(v.X), JsonFloat(v.Y), JsonFloat(v.Z));
}

static String JsonFloat4(const Float4& v)
{
    return String::Format(TEXT("[{0}, {1}, {2}, {3}]"), JsonFloat(v.X), JsonFloat(v.Y), JsonFloat(v.Z), JsonFloat(v.W));
}

static String JsonMatrix(const Matrix& m)
{
    String s = TEXT("[");
    for (int32 r = 0; r < 4; r++)
    {
        if (r) s += TEXT(", ");
        s += String::Format(TEXT("[{0}, {1}, {2}, {3}]"),
            JsonFloat(m.Values[r][0]), JsonFloat(m.Values[r][1]), JsonFloat(m.Values[r][2]), JsonFloat(m.Values[r][3]));
    }
    return s + TEXT("]");
}

static void DumpShadowsToDisk(const ShadowsCustomBuffer& shadows, const RenderContext& renderContext)
{
    PROFILE_CPU_NAMED("Shadow Dump");
    const uint64 frame = Engine::FrameCount;
    const String folder = Globals::ProjectFolder / TEXT("ShadowDumps") / String::Format(TEXT("dump_{0}"), frame);
    if (!FileSystem::DirectoryExists(folder))
    {
        if (FileSystem::CreateDirectory(folder))
        {
            LOG(Warning, "[ShadowDump] failed to create folder {0}", folder);
            return;
        }
    }

    // Textures: dynamic atlas, static atlas, weapon atlas, per-clipmap-level depth
    DumpTextureRaw(shadows.ShadowMapAtlas, folder / TEXT("atlas_dynamic.bin"));
    if (shadows.StaticShadowMapAtlas)
        DumpTextureRaw(shadows.StaticShadowMapAtlas, folder / TEXT("atlas_static.bin"));
    if (shadows.WeaponShadowMapAtlas)
        DumpTextureRaw(shadows.WeaponShadowMapAtlas, folder / TEXT("atlas_weapon.bin"));
    for (int32 i = 0; i < shadows.Clipmap.LevelCount; i++)
    {
        auto* tex = shadows.Clipmap.Levels[i].DepthTexture;
        if (tex)
            DumpTextureRaw(tex, folder / String::Format(TEXT("clipmap_L{0}.bin"), i));
    }

    // Metadata sidecar. Hand-rolled JSON because we only need write here and the engine's
    // JSON writer wants a full document object. Format is plain ASCII so any parser works.
    String j = TEXT("{\n");
    j += String::Format(TEXT("  \"frame\": {0},\n"), frame);
    j += String::Format(TEXT("  \"atlasResolution\": {0},\n"), shadows.Resolution);
    j += String::Format(TEXT("  \"viewOrigin\": {0},\n"), JsonFloat3((Float3)shadows.ViewOrigin));
    j += String::Format(TEXT("  \"viewPosition\": {0},\n"), JsonFloat3((Float3)renderContext.View.Position));
    j += String::Format(TEXT("  \"viewDirection\": {0},\n"), JsonFloat3(renderContext.View.Direction));
    j += String::Format(TEXT("  \"viewNear\": {0},\n"), JsonFloat(renderContext.View.Near));
    j += String::Format(TEXT("  \"viewFar\": {0},\n"), JsonFloat(renderContext.View.Far));
    j += String::Format(TEXT("  \"viewMatrix\": {0},\n"), JsonMatrix(renderContext.View.View));
    j += String::Format(TEXT("  \"projection\": {0},\n"), JsonMatrix(renderContext.View.NonJitteredProjection));

    // Clipmap-level math
    j += TEXT("  \"clipmap\": {\n");
    const auto& cm = shadows.Clipmap;
    j += String::Format(TEXT("    \"enabled\": {0},\n"), cm.Enabled ? TEXT("true") : TEXT("false"));
    j += String::Format(TEXT("    \"levelCount\": {0},\n"), cm.LevelCount);
    j += String::Format(TEXT("    \"sunDir\": {0},\n"), JsonFloat3(cm.SunDir));
    j += String::Format(TEXT("    \"lightRight\": {0},\n"), JsonFloat3(cm.LightRight));
    j += String::Format(TEXT("    \"lightUp\": {0},\n"), JsonFloat3(cm.LightUp));
    j += String::Format(TEXT("    \"beyondCSMExtent\": {0},\n"), JsonFloat(cm.BeyondCSMExtent));
    j += TEXT("    \"levels\": [\n");
    for (int32 i = 0; i < cm.LevelCount; i++)
    {
        const auto& L = cm.Levels[i];
        j += TEXT("      {");
        j += String::Format(TEXT("\"index\": {0}, "), i);
        j += String::Format(TEXT("\"resolution\": {0}, "), L.Resolution);
        j += String::Format(TEXT("\"worldExtent\": {0}, "), JsonFloat(L.WorldExtent));
        j += String::Format(TEXT("\"texelSize\": {0}, "), JsonFloat(L.TexelSize));
        j += String::Format(TEXT("\"lastRedrawTexelSize\": {0}, "), JsonFloat(L.LastRedrawTexelSize));
        j += String::Format(TEXT("\"depthRange\": {0}, "), JsonFloat(L.DepthRange));
        j += String::Format(TEXT("\"lastRedrawDepthRange\": {0}, "), JsonFloat(L.LastRedrawDepthRange));
        j += String::Format(TEXT("\"lastRedrawWorldExtent\": {0}, "), JsonFloat(L.LastRedrawWorldExtent));
        j += String::Format(TEXT("\"scrollTexels\": [{0}, {1}], "), L.ScrollTexels.X, L.ScrollTexels.Y);
        j += String::Format(TEXT("\"prevScrollTexels\": [{0}, {1}], "), L.PrevScrollTexels.X, L.PrevScrollTexels.Y);
        j += String::Format(TEXT("\"originTexels\": [{0}, {1}], "), L.TextureOriginTexels.X, L.TextureOriginTexels.Y);
        j += String::Format(TEXT("\"dirtyStrip\": [{0}, {1}], "), L.DirtyStrip.X, L.DirtyStrip.Y);
        j += String::Format(TEXT("\"compositingColor\": {0}, "), JsonFloat4(L.CompositingColor));
        j += String::Format(TEXT("\"depthRemap\": [{0}, {1}], "), JsonFloat(L.DepthRemap.X), JsonFloat(L.DepthRemap.Y));
        j += String::Format(TEXT("\"wrapOffsetUV\": [{0}, {1}], "), JsonFloat(L.WrapOffsetUV.X), JsonFloat(L.WrapOffsetUV.Y));
        j += String::Format(TEXT("\"needsFullRedraw\": {0}"), L.NeedsFullRedraw ? TEXT("true") : TEXT("false"));
        j += (i + 1 < cm.LevelCount) ? TEXT("},\n") : TEXT("}\n");
    }
    j += TEXT("    ]\n");
    j += TEXT("  },\n");

    // Per-light cascade tile data
    j += TEXT("  \"lights\": [\n");
    int32 lightIdx = 0;
    const int32 lightCount = shadows.Lights.Count();
    for (auto it = shadows.Lights.Begin(); it.IsNotEnd(); ++it)
    {
        const auto& al = it->Value;
        j += TEXT("    {");
        j += String::Format(TEXT("\"id\": \"{0}\", "), it->Key.ToString());
        j += String::Format(TEXT("\"resolution\": {0}, "), al.Resolution);
        j += String::Format(TEXT("\"tilesCount\": {0}, "), al.TilesCount);
        j += String::Format(TEXT("\"renderDynamic\": {0}, "), al.RenderDynamic ? TEXT("true") : TEXT("false"));
        j += String::Format(TEXT("\"hasStaticShadowContext\": {0}, "), al.HasStaticShadowContext ? TEXT("true") : TEXT("false"));
        j += String::Format(TEXT("\"cascadeSplits\": {0}, "), JsonFloat4(al.CascadeSplits));
        j += String::Format(TEXT("\"bias\": {0}, "), JsonFloat(al.Bias));
        j += String::Format(TEXT("\"softness\": {0}, "), JsonFloat(al.Softness));
        j += TEXT("\"tiles\": [");
        for (int32 t = 0; t < al.TilesCount; t++)
        {
            const auto& tile = al.Tiles[t];
            j += TEXT("{");
            if (tile.RectTile)
            {
                j += String::Format(TEXT("\"rect\": [{0}, {1}, {2}, {3}], "),
                    tile.RectTile->X, tile.RectTile->Y, tile.RectTile->Width, tile.RectTile->Height);
            }
            else
            {
                j += TEXT("\"rect\": null, ");
            }
            j += String::Format(TEXT("\"skipUpdate\": {0}, "), tile.SkipUpdate ? TEXT("true") : TEXT("false"));
            j += String::Format(TEXT("\"hasStaticGeometry\": {0}, "), tile.HasStaticGeometry ? TEXT("true") : TEXT("false"));
            j += String::Format(TEXT("\"worldToShadow\": {0}"), JsonMatrix(tile.WorldToShadow));
            j += (t + 1 < al.TilesCount) ? TEXT("}, ") : TEXT("}");
        }
        j += TEXT("]");
        j += (++lightIdx < lightCount) ? TEXT("},\n") : TEXT("}\n");
    }
    j += TEXT("  ]\n");
    j += TEXT("}\n");

    const String metaPath = folder / TEXT("meta.json");
    auto* metaFile = FileWriteStream::Open(metaPath);
    if (metaFile)
    {
        const StringAsANSI<2048> ansi(j.Get(), j.Length());
        metaFile->WriteBytes(ansi.Get(), j.Length());
        metaFile->Close();
        Delete(metaFile);
    }

    LOG(Info, "[ShadowDump] frame {0} -> {1} (textures download async on worker threads)", frame, folder);
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

            // Skip if nothing to update
            if (!level.NeedsFullRedraw && level.DirtyStrip.X == 0 && level.DirtyStrip.Y == 0)
                continue;

            const int32 R = level.Resolution;
            const Int2 newScroll = level.ScrollTexels;

            if (level.NeedsFullRedraw)
            {
                LOG(Info, "[ClipmapRedraw] L{0} FULL redraw extent={1} R={2} ts={3} dr={4} scroll=({5},{6})",
                    levelIdx, level.WorldExtent, R, level.TexelSize, level.DepthRange,
                    newScroll.X, newScroll.Y);
                // Anchor the toroidal mapping so the full rect renders into the texture without wrap
                // (texture pixel (0,0) holds the rect's top-left world-texel; matches the existing
                // non-flipped + Y-flipped ortho convention used by RenderClipmapStrip).
                level.TextureOriginTexels = Int2(newScroll.X - R / 2, newScroll.Y + R / 2);

                context->SetRenderTarget(level.DepthTexture->View(), (GPUTextureView*)nullptr);
                context->ClearDepth(level.DepthTexture->View(), 1.0f);
                RenderClipmapStrip(context, renderContext, clipmap, level,
                                   Int2(newScroll.X - R / 2, newScroll.Y - R / 2),
                                   Int2(newScroll.X + R / 2, newScroll.Y + R / 2),
                                   _psDepthClear, quadShaderCB);
                context->ResetRenderTarget();
                level.NeedsFullRedraw = false;
                // Stamp the basis the cache content was rendered against. Per-frame Init compares
                // current SunDir against this to detect any drift (catches sub-threshold cumulative
                // rotation that would otherwise slip past the coarser sunChanged heuristic).
                level.LastRedrawSunDir = clipmap.SunDir;
                // Stamp the math state the cache content was actually rendered with. Subsequent
                // frames compare current TexelSize/DepthRange against these to detect cumulative
                // drift (gradual FOV/near changes that slide under per-frame thresholds).
                level.LastRedrawTexelSize = level.TexelSize;
                level.LastRedrawDepthRange = level.DepthRange;
                level.LastRedrawWorldExtent = level.WorldExtent;
            }
            else
            {
                // Strip update - render only the L-shaped strip of new texels at the leading edges.
                // X-strip: full Y extent, |dx| wide. Y-strip: |dy| tall, with width = R - |dx| so the
                // corner (already covered by X-strip) isn't double-rendered.
                context->SetRenderTarget(level.DepthTexture->View(), (GPUTextureView*)nullptr);

                const int32 dx = level.DirtyStrip.X;
                const int32 dy = level.DirtyStrip.Y;
                // Strip path is otherwise silent - log delta so a moving capture can be told apart
                // from a full-redraw spam (invalidation) and fat-strip costs surface.
                if (g_ClipmapDebugDraw)
                    LOG(Info, "[ClipmapStrip] L{0} dx={1} dy={2} R={3} scroll=({4},{5})",
                        levelIdx, dx, dy, R, newScroll.X, newScroll.Y);
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

            if (useClipmapForLight && tileIndex < clipmap.LevelCount && clipmap.Levels[tileIndex].DepthTexture)
            {
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
            // HACK g_ClipmapIsolateStatic: skip dynamic draws on clipmap-active light so the
            // cascade tile shows ONLY composited clipmap content (still need to advance the
            // context-index counter so the static-shadow context lookup remains correct).
            const bool skipDynamicForIsolation = g_ClipmapIsolateStatic && useClipmapForLight;
            if (atlasLight.RenderDynamic && atlasLight.ContextCount > 0)
            {
                auto& shadowContext = renderContextBatch.Contexts[atlasLight.ContextIndex + contextIndex++];
                if (!skipDynamicForIsolation)
                {
                    shadowContext.List->ExecuteDrawCalls(shadowContext, DrawCallsListType::Depth);
                    shadowContext.List->ExecuteDrawCalls(shadowContext, shadowContext.List->ShadowDepthDrawCallsList, renderContext.List, nullptr);
                }
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
            // Calculate the actual visible area at the weapon distance using the configured weapon FOV
            const float weaponFovDegrees = renderContext.View.WeaponFOV > 0.0f ? renderContext.View.WeaponFOV : 54.0f;
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

    // One-shot debug dump (RequestDump() set the flag). Runs after all shadow textures are
    // populated for this frame so the downloaded data matches the metadata we serialise.
    if (ShadowsDumpRequested)
    {
        ShadowsDumpRequested = false;
        DumpShadowsToDisk(shadows, renderContext);
    }
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

// HACK debug overlay: paint each shadow clipmap level's depth texture as a grayscale thumbnail
// down the right edge of the output. Order is top-to-bottom = level 0 (nearest cascade) ...
// level N (farthest / beyond-CSM). Sized so all levels fit even with 5 levels (4 CSM + 1 beyond).
void ShadowsPass::DrawClipmapDebugOverlay(GPUContext* context, RenderContext& renderContext, GPUTextureView* output, const Viewport& outputViewport)
{
    if (!g_ClipmapDebugDraw)
        return;
    if (!renderContext.Buffers)
        return;
    const ShadowsCustomBuffer* shadowsPtr = renderContext.Buffers->FindCustomBuffer<ShadowsCustomBuffer>(TEXT("Shadows"), false);
    if (!shadowsPtr)
        return;
    const ShadowsCustomBuffer& shadows = *shadowsPtr;
    if (!shadows.Clipmap.Enabled || shadows.Clipmap.LevelCount <= 0)
        return;
    auto* instance = ShadowsPass::Instance();
    if (!instance || !instance->_psDepthVisualize || !instance->_psDepthVisualize->IsValid())
        return;

    PROFILE_GPU("Clipmap Debug Overlay");

    // Layout: right-edge strip, square thumbnails stacked vertically.
    const int32 levelCount = shadows.Clipmap.LevelCount;
    const float vpW = outputViewport.Width;
    const float vpH = outputViewport.Height;
    const float vpX = outputViewport.X;
    const float vpY = outputViewport.Y;
    // Thumbnail size: 1/levelCount of viewport height, but capped to ~1/5 of viewport width
    // so on wide viewports they don't dominate the screen.
    float thumb = vpH / (float)Math::Max(levelCount, 1);
    const float maxThumb = vpW * 0.2f;
    if (thumb > maxThumb)
        thumb = maxThumb;
    const float stripX = vpX + vpW - thumb;

    GPUConstantBuffer* quadShaderCB = GPUDevice::Instance->QuadShader->GetCB(0);
    QuadShaderData quadShaderData;
    quadShaderData.Color = Float4(1.0f, 1.0f, 0, 0); // x=invert (1 for far=white->near=dark hmm), y=contrast power
    quadShaderData.Params = Float4::Zero;

    context->ResetRenderTarget();
    context->SetRenderTarget(output);
    context->SetState(instance->_psDepthVisualize);

    for (int32 i = 0; i < levelCount; i++)
    {
        const auto& level = shadows.Clipmap.Levels[i];
        if (!level.DepthTexture)
            continue;

        const float yTop = vpY + (float)i * thumb;
        context->SetViewportAndScissors(Viewport(stripX, yTop, thumb, thumb));

        context->UpdateCB(quadShaderCB, &quadShaderData);
        context->BindCB(0, quadShaderCB);
        context->BindSR(0, level.DepthTexture->View());
        context->DrawFullscreenTriangle();
        context->UnBindSR(0);
    }

    context->ResetRenderTarget();
    context->SetViewportAndScissors(outputViewport);
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
