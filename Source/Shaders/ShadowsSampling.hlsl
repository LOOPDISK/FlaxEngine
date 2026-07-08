// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __SHADOWS_SAMPLING__
#define __SHADOWS_SAMPLING__

#ifndef SHADOWS_CSM_BLENDING
#define SHADOWS_CSM_BLENDING 0
#endif
#ifndef SHADOWS_CSM_DITHERING
#define SHADOWS_CSM_DITHERING 0
#endif
// Receiver plane depth bias needs screen-space derivatives (ddx/ddy) -> pixel-shader only.
// Compute shaders that sample shadows must not hit those ops (X4532 on cs_5_0). A consumer can
// set this 0 before including, but engine shaders bake a stale flattened root into their .flax
// (includes resolve live, the root does not), so a .shader-side #define won't apply until the
// engine shader is recompiled. To stay correct without that rebuild, auto-disable for the known
// compute entry points that reach shadow sampling - Flax defines _<EntryName> for the function
// being compiled (e.g. _CS_LightScattering for volumetric fog). With the bias off, the slope-
// scaled constant bias still applies; only the per-tap receiver-plane refinement is dropped.
#ifndef SHADOWS_USE_RECEIVER_PLANE_BIAS
    #if defined(_CS_LightScattering)
        #define SHADOWS_USE_RECEIVER_PLANE_BIAS 0
    #else
        #define SHADOWS_USE_RECEIVER_PLANE_BIAS 1
    #endif
#endif

#include "./Flax/ShadowsCommon.hlsl"
#include "./Flax/GBufferCommon.hlsl"
#include "./Flax/LightingCommon.hlsl"
// Always needed: Vogel PCSS (ShadowVogelRotation) uses InterleavedGradientNoise at file scope, not
// just the CSM dither path. Random.hlsl is pure ALU (guarded, no samplers/derivatives) so it's safe
// in every consumer including compute shaders.
#include "./Flax/Random.hlsl"

#if FEATURE_LEVEL >= FEATURE_LEVEL_SM5 || defined(WGSL)
#define SAMPLE_SHADOW_MAP(shadowMap, shadowUV, sceneDepth) shadowMap.SampleCmpLevelZero(ShadowSamplerLinear, shadowUV, sceneDepth)
#define SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowUV, texelOffset, sceneDepth) shadowMap.SampleCmpLevelZero(ShadowSamplerLinear, shadowUV, sceneDepth, texelOffset)
#else
#define SAMPLE_SHADOW_MAP(shadowMap, shadowUV, sceneDepth) (sceneDepth < shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0).r)
#define SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowUV, texelOffset, sceneDepth) (sceneDepth < shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0, texelOffset).r)
#endif
#if defined(WGSL)
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) SAMPLE_RT_DEPTH(shadowMap, shadowUV)
#elif VULKAN || FEATURE_LEVEL < FEATURE_LEVEL_SM5
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) shadowMap.SampleLevel(SamplerPointClamp, shadowUV, 0).r
#else
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0).r
#endif

float4 GetShadowMask(ShadowSample shadow)
{
    return float4(shadow.SurfaceShadow, shadow.TransmissionShadow, 1, 1);
}

// Gets the cube texture face index to use for shadow map sampling for the given view-to-light direction vector
// Where: direction = normalize(worldPosition - lightPosition)
uint GetCubeFaceIndex(float3 direction)
{
    uint cubeFaceIndex;
    float3 absDirection = abs(direction);
    float maxDirection = max(absDirection.x, max(absDirection.y, absDirection.z));
    if (maxDirection == absDirection.x)
        cubeFaceIndex = absDirection.x == direction.x ? 0 : 1;
    else if (maxDirection == absDirection.y)
        cubeFaceIndex = absDirection.y == direction.y ? 2 : 3;
    else
        cubeFaceIndex = absDirection.z == direction.z ? 4 : 5;
    return cubeFaceIndex;
}

// World-space units per one shadow-projection depth unit for the tile, recovered from the depth
// column magnitude of the world-to-shadow matrix. For directional cascades the ortho maps the full
// cascade depth range (2 * cascadeRadius) into [0,1], so this returns that range in world units.
float GetShadowTileWorldPerDepthUnit(ShadowTileData tile)
{
    float3 depthColumn = float3(tile.WorldToShadow[0].z, tile.WorldToShadow[1].z, tile.WorldToShadow[2].z);
    return 1.0f / max(length(depthColumn), 1e-9f);
}

// World-space lateral distance spanned by 1 unit of tile UV (i.e. the cascade's world extent). Used to
// convert a world-space penumbra into tile/atlas UV so soft shadows stay world-consistent across cascades.
float GetShadowTileWorldPerUV(ShadowTileData tile)
{
    float3 uvColumn = float3(tile.WorldToShadow[0].x, tile.WorldToShadow[1].x, tile.WorldToShadow[2].x);
    return 1.0f / max(length(uvColumn), 1e-9f);
}

// World-space size of one shadow map texel for the tile (directional cascades only - perspective
// tiles have no single texel size). The UV column magnitude is 1/(cascade world extent).
float GetShadowTileTexelWorldSize(ShadowTileData tile, float atlasResolution)
{
    float3 uvColumn = float3(tile.WorldToShadow[0].x, tile.WorldToShadow[1].x, tile.WorldToShadow[2].x);
    float worldPerUV = 1.0f / max(length(uvColumn), 1e-9f);
    float tileResolution = max(tile.ShadowToAtlas.x * atlasResolution, 1.0f);
    return worldPerUV / tileResolution;
}

float2 GetLightShadowAtlasUV(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, out float4 shadowPosition)
{
    // Project into shadow space (WorldToShadow is pre-multiplied to convert Clip Space to UV Space)
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    // Flat bias only (no gradient context here): tiny quantization epsilon for directional lights,
    // legacy authored constant for local lights (pre-divide, matching existing content tuning).
    shadowPosition.z -= shadow.Bias;
    shadowPosition.xyz /= shadowPosition.w;

    // UV Space -> Atlas Tile UV Space
    float2 shadowMapUV = saturate(shadowPosition.xy);
    shadowMapUV = shadowMapUV * shadowTile.ShadowToAtlas.xy + shadowTile.ShadowToAtlas.zw;
    return shadowMapUV;
}

// Slope-scaled depth bias. Bias scales with tan(theta) where theta = angle between surface
// normal and light, so grazing surfaces get progressively more bias (kills peter-panning) while
// flat-facing surfaces keep the authored value. Matches the prior 0.5x at NoL=1 to stay close
// to existing tuning; capped to avoid runaway at near-perpendicular angles.
float ComputeSlopeScaledBias(float authoredBias, float NoL)
{
    float tanTheta = sqrt(saturate(1.0 - NoL * NoL)) / max(NoL, 0.1);
    tanTheta = min(tanTheta, 4.0);
    return authoredBias * 0.5 * (1.0 + tanTheta);
}

// Exact receiver-plane depth gradient dz/d(uv) in shadow projection space (Isidoro, "Shadow Mapping:
// GPU-based Tips and Techniques"), from the full inverse-Jacobian of the shadow-space position's
// screen derivatives. Purely geometric: exact for planar receivers under any projective tile matrix
// (ortho cascade or perspective spot) and immune to normal maps. Returns 0 when the pixel quad
// straddles a silhouette (degenerate Jacobian) - callers then rely on the flat epsilon alone.
float2 ComputeReceiverPlaneDepthGradient(float3 shadowPosDDX, float3 shadowPosDDY)
{
    float det = shadowPosDDX.x * shadowPosDDY.y - shadowPosDDX.y * shadowPosDDY.x;
    if (abs(det) < 1e-12f)
        return float2(0, 0);
    float2 gradient;
    gradient.x = shadowPosDDY.y * shadowPosDDX.z - shadowPosDDX.y * shadowPosDDY.z;
    gradient.y = shadowPosDDX.x * shadowPosDDY.z - shadowPosDDY.x * shadowPosDDX.z;
    // Clamp the slope (units: depth per tile UV; 8 = receiver at ~83deg to the light) so silhouette
    // misestimates can't push tap references through nearby occluders (light leaks).
    return clamp(gradient / det, -8.0f, 8.0f);
}

float2 GetLightShadowAtlasUVWithReceiverBias(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, float3 ddxWorld, float3 ddyWorld, float NoL, out float4 shadowPosition, out float2 receiverPlaneDepthBias)
{
    // Project into shadow space (WorldToShadow is pre-multiplied to convert Clip Space to UV Space)
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    shadowPosition.xyz /= shadowPosition.w;

    // World-constant flat bias. shadow.Bias is normalized shadow-depth, so subtracting it directly
    // peter-pans by shadow.Bias * worldPerDepthUnit - and worldPerDepthUnit tracks the cascade's
    // depth range, which ~doubles per cascade. So far cascades (3+) over-biased by tens of cm and
    // shrank small shadows (worst at a vertical sun, where a sphere's true shadow is smallest).
    // Divide by worldPerDepthUnit and scale by a reference so the world peter-pan is the same on
    // every cascade: worldPeterPan = shadow.Bias * BiasWorldScale cm. Scale keeps the 0.005 default
    // at ~2.5 cm, matching a typical near cascade's prior look.
    const float BiasWorldScale = 500.0f; // cm of world peter-pan per unit of normalized Bias
    float biasNorm = shadow.Bias * (BiasWorldScale / GetShadowTileWorldPerDepthUnit(shadowTile));

#if SHADOWS_USE_RECEIVER_PLANE_BIAS && SHADOWS_QUALITY != 0
    // Exact plane gradient, converted from tile-UV to atlas-UV units (PCF taps offset in atlas UV):
    // atlasUV = tileUV * ShadowToAtlas.xy + zw, so dz/d(atlasUV) = dz/d(tileUV) / ShadowToAtlas.xy.
    // Derive the shadow-space screen gradient from the CONTINUOUS world-position derivatives, not from
    // ddx(shadowPosition): the CSM dither band mixes two cascades within one 2x2 quad, so a reprojected
    // shadowPosition jumps between cascades and its ddx explodes -> a thin dark seam at the split. The
    // world position doesn't jump between cascades. Directional cascades are orthographic (w==1), so
    // d(shadowPos)/d(screen) = WorldToShadow * d(worldPos) exactly (the /w above is a no-op here).
    float3 shadowPosDDX = mul(float4(ddxWorld, 0.0f), shadowTile.WorldToShadow).xyz;
    float3 shadowPosDDY = mul(float4(ddyWorld, 0.0f), shadowTile.WorldToShadow).xyz;
    receiverPlaneDepthBias = ComputeReceiverPlaneDepthGradient(shadowPosDDX, shadowPosDDY) / shadowTile.ShadowToAtlas.xy;

    // The plane gradient replaces the authored slope-scaled bias: the comparison reference follows
    // the receiver surface per tap, so only the flat epsilon remains (depth-format quantization,
    // folded into Bias on the CPU, plus any authored extra for LOD-mismatched or masked casters).
    shadowPosition.z -= biasNorm;
#else
    // No derivatives (compute shaders) or single-tap quality that ignores the gradient: flat
    // reference with the legacy slope-scaled authored bias.
    receiverPlaneDepthBias = float2(0.0, 0.0);
    shadowPosition.z -= ComputeSlopeScaledBias(biasNorm, NoL);
#endif

    // UV Space -> Atlas Tile UV Space
    float2 shadowMapUV = saturate(shadowPosition.xy);
    shadowMapUV = shadowMapUV * shadowTile.ShadowToAtlas.xy + shadowTile.ShadowToAtlas.zw;
    return shadowMapUV;
}

// Local-light variant: keeps the legacy constant bias applied before the perspective divide
// (existing content is tuned against that behavior) but adds the exact receiver-plane gradient
// so the PCF taps follow the receiver surface instead of a flat reference.
float2 GetLightShadowAtlasUVLocalWithReceiverBias(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, out float4 shadowPosition, out float2 receiverPlaneDepthBias)
{
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    shadowPosition.z -= shadow.Bias;
    shadowPosition.xyz /= shadowPosition.w;
#if SHADOWS_USE_RECEIVER_PLANE_BIAS && SHADOWS_QUALITY != 0
    float3 shadowPosDDX = ddx(shadowPosition.xyz);
    float3 shadowPosDDY = ddy(shadowPosition.xyz);
    receiverPlaneDepthBias = ComputeReceiverPlaneDepthGradient(shadowPosDDX, shadowPosDDY) / shadowTile.ShadowToAtlas.xy;
#else
    receiverPlaneDepthBias = float2(0.0, 0.0);
#endif
    float2 shadowMapUV = saturate(shadowPosition.xy);
    shadowMapUV = shadowMapUV * shadowTile.ShadowToAtlas.xy + shadowTile.ShadowToAtlas.zw;
    return shadowMapUV;
}

float SampleShadowMap(Texture2D<float> shadowMap, float2 shadowMapUV, float sceneDepth)
{
    float result = SAMPLE_SHADOW_MAP(shadowMap, shadowMapUV, sceneDepth);
#if SHADOWS_QUALITY == 1
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 0), sceneDepth);
	result = result * (1.0f / 4.0);
#elif SHADOWS_QUALITY == 2 || SHADOWS_QUALITY == 3
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 1), sceneDepth);
	result = result * (1.0f / 9.0);
#endif
    return result;
}

float SampleShadowMapOptimizedPCFHelper(Texture2D<float> shadowMap, float2 baseUV, float u, float v, float2 shadowMapSizeInv, float sceneDepth, float2 receiverPlaneBias = float2(0, 0))
{
    float2 offset = float2(u, v) * shadowMapSizeInv;
    float2 uv = baseUV + offset;
    float biasedDepth = sceneDepth + dot(offset, receiverPlaneBias);
    return SAMPLE_SHADOW_MAP(shadowMap, uv, biasedDepth);
}

// [Shadow map sampling method used in The Witness, https://github.com/TheRealMJP/Shadows]
float SampleShadowMapOptimizedPCF(Texture2D<float> shadowMap, float2 shadowMapUV, float sceneDepth, float2 screenPos = float2(0, 0), float2 receiverPlaneBias = float2(0, 0))
{
#if SHADOWS_QUALITY != 0
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);

    float2 uv = shadowMapUV.xy * shadowMapSize; // 1 unit - 1 texel
    float2 shadowMapSizeInv = 1.0f / shadowMapSize;

    // Add screen-space stable blue noise offset for temporal stability
    float noise = 0.0;
#if SHADOWS_CSM_DITHERING
    if (any(screenPos))
        noise = InterleavedGradientNoise(screenPos) - 0.5;
#endif

    float2 baseUV;
    baseUV.x = floor(uv.x + 0.5 + noise);
    baseUV.y = floor(uv.y + 0.5 + noise);
    float s = (uv.x + 0.5 - baseUV.x);
    float t = (uv.y + 0.5 - baseUV.y);
    baseUV -= float2(0.5, 0.5);
    baseUV *= shadowMapSizeInv;

    // Re-anchor the plane reference from the pixel's exact UV to the snapped (+ jittered) kernel
    // origin, then pad by the half-texel slope a shared bilinear comparison reference can't resolve
    // (hardware PCF compares 4 texels against one reference). No-op when the gradient is zero.
    sceneDepth += dot(baseUV - shadowMapUV, receiverPlaneBias);
    sceneDepth -= 0.5f * dot(abs(receiverPlaneBias), shadowMapSizeInv);

    float sum = 0;
#endif
#if SHADOWS_QUALITY == 0
    return SAMPLE_SHADOW_MAP(shadowMap, shadowMapUV, sceneDepth);
#elif SHADOWS_QUALITY == 1
	float uw0 = (3 - 2 * s);
	float uw1 = (1 + 2 * s);

	float u0 = (2 - s) / uw0 - 1;
	float u1 = s / uw1 + 1;

	float vw0 = (3 - 2 * t);
	float vw1 = (1 + 2 * t);

	float v0 = (2 - t) / vw0 - 1;
	float v1 = t / vw1 + 1;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	return sum * 1.0f / 16;
#elif SHADOWS_QUALITY == 2
	float uw0 = (4 - 3 * s);
	float uw1 = 7;
	float uw2 = (1 + 3 * s);

	float u0 = (3 - 2 * s) / uw0 - 2;
	float u1 = (3 + s) / uw1;
	float u2 = s / uw2 + 2;

	float vw0 = (4 - 3 * t);
	float vw1 = 7;
	float vw2 = (1 + 3 * t);

	float v0 = (3 - 2 * t) / vw0 - 2;
	float v1 = (3 + t) / vw1;
	float v2 = t / vw2 + 2;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	sum += uw0 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	return sum * 1.0f / 144;
#elif SHADOWS_QUALITY == 3
	float uw0 = (5 * s - 6);
	float uw1 = (11 * s - 28);
	float uw2 = -(11 * s + 17);
	float uw3 = -(5 * s + 1);

	float u0 = (4 * s - 5) / uw0 - 3;
	float u1 = (4 * s - 16) / uw1 - 1;
	float u2 = -(7 * s + 5) / uw2 + 1;
	float u3 = -s / uw3 + 3;

	float vw0 = (5 * t - 6);
	float vw1 = (11 * t - 28);
	float vw2 = -(11 * t + 17);
	float vw3 = -(5 * t + 1);

	float v0 = (4 * t - 5) / vw0 - 3;
	float v1 = (4 * t - 16) / vw1 - 1;
	float v2 = -(7 * t + 5) / vw2 + 1;
	float v3 = -t / vw3 + 3;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw3 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v0, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw3 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v1, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	sum += uw0 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw3 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v2, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	sum += uw0 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v3, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw1 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v3, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw2 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v3, shadowMapSizeInv, sceneDepth, receiverPlaneBias);
	sum += uw3 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v3, shadowMapSizeInv, sceneDepth, receiverPlaneBias);

	return sum * (1.0f / 2704);
#else
    return 0.0f;
#endif
}

// Golden-angle Vogel disk sampling for smooth PCSS. Deterministic low-discrepancy points (uniform
// area, works for any tap count) plus a per-pixel IGN rotation turn residual under-sampling into fine
// grain instead of the concentric banding a fixed Poisson set prints once the penumbra widens. Each
// tap is a hardware 2x2 comparison (ShadowSamplerLinear), so effective coverage is ~4x the tap count.
// Ported from the magi area-light MC-PCSS sampler.
static const float SHADOW_GOLDEN_ANGLE = 2.39996323; // 2*PI*(2 - phi)

// Tap counts: the grain<->cost dial for soft (Softness>0) directional shadows. The blocker search
// dominates residual grain - sparse blocker taps under-sample the footprint and leave salt-and-pepper
// leaks - so it is kept as dense as the PCF. Bump both for smoother/softer, drop for cheaper.
#define PCSS_BLOCKER_TAPS 32
#define PCSS_PCF_TAPS 32

// Per-pixel rotation from interleaved-gradient noise (Jimenez): decorrelates the Vogel disk between
// neighbouring pixels so leftover error reads as high-frequency grain (TAA-friendly), not banding.
float ShadowVogelRotation(float2 screenPos)
{
    return InterleavedGradientNoise(screenPos) * 6.2831853;
}

// Vogel (golden-angle) disk: uniform-area low-discrepancy point i of n on the unit disk, rotated by rot.
float2 ShadowVogelDisk(int i, int n, float rot)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * SHADOW_GOLDEN_ANGLE + rot;
    return r * float2(cos(theta), sin(theta));
}

// PCSS blocker search over a rotated Vogel disk. Returns average blocker depth in shadow-NDC, or -1
// if no blockers found. searchRadiusAtlasUV is the disk radius in atlas-UV units (already tile-scaled).
// Each tap compares against the receiver PLANE (receiverDepth extrapolated along the gradient), so
// a sloped receiver's own surface doesn't register as a blocker across the disk. Depth is point-
// sampled: linear-filtering raw depth across discontinuities corrupts the blocker average.
float FindBlockerDepth_Directional(Texture2D<float> shadowMap, float2 atlasUV, float receiverDepth, float searchRadiusAtlasUV, float2 receiverPlaneBias, float rot)
{
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    UNROLL
    for (int i = 0; i < PCSS_BLOCKER_TAPS; i++)
    {
        float2 offset = ShadowVogelDisk(i, PCSS_BLOCKER_TAPS, rot) * searchRadiusAtlasUV;
        float d = shadowMap.SampleLevel(SamplerPointClamp, atlasUV + offset, 0).r;
        if (d < receiverDepth + dot(offset, receiverPlaneBias))
        {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }
    return blockerCount > 0.0 ? (blockerSum / blockerCount) : -1.0;
}

// Variable-radius Vogel PCF in atlas UV (rotated per pixel). Radius is the disk radius (atlas-UV units).
// Per-tap references follow the receiver plane along the gradient.
float SamplePCF_VogelDirectional(Texture2D<float> shadowMap, float2 atlasUV, float sceneDepth, float radiusAtlasUV, float2 receiverPlaneBias, float rot)
{
    float sum = 0.0;
    UNROLL
    for (int i = 0; i < PCSS_PCF_TAPS; i++)
    {
        float2 offset = ShadowVogelDisk(i, PCSS_PCF_TAPS, rot) * radiusAtlasUV;
        sum += SAMPLE_SHADOW_MAP(shadowMap, atlasUV + offset, sceneDepth + dot(offset, receiverPlaneBias));
    }
    return sum * (1.0 / float(PCSS_PCF_TAPS));
}

// PCSS for directional light: blocker search -> penumbra estimate -> variable-radius Vogel PCF.
// lightSize is in cascade-UV space (a fraction of the cascade's coverage). Converted to atlas UV
// via the tile's UV scale so the search/filter disk stays the right physical size per cascade.
// receiverDepth must already include the flat epsilon; slope handling comes from the plane gradient.
// screenPos drives the shared per-pixel Vogel rotation (same rotation for blocker + PCF).
float SamplePCSS_Directional(Texture2D<float> shadowMap, ShadowTileData tile, float2 atlasUV, float receiverDepth, float lightSize, float2 receiverPlaneBias, float2 screenPos)
{
    float rot = ShadowVogelRotation(screenPos);

    // Work in WORLD units so the penumbra matches across cascade boundaries. Doing this in cascade-UV
    // (the old way) made a fixed Softness mean a different world penumbra per cascade - each cascade
    // covers ~2x the extent of the previous, so the softness visibly jumped at every split. lightSize
    // (Softness) is treated as a world tangent: penumbra per unit world blocker->receiver gap. The
    // cascade's UV and depth scales cancel, so identical geometry gives an identical world penumbra
    // regardless of which cascade shades the pixel.
    float tileScale = tile.ShadowToAtlas.x;                       // atlas-UV per tile-UV (same for all cascades)
    float worldPerUV = GetShadowTileWorldPerUV(tile);            // cm per tile-UV (cascade lateral extent)
    float worldPerDepth = GetShadowTileWorldPerDepthUnit(tile);  // cm per normalized depth (cascade depth span)
    float worldToAtlasUV = tileScale / worldPerUV;              // convert world cm -> atlas UV

    // Blocker search over the cascade's depth span (the widest plausible penumbra), sized in world.
    float searchRadiusAtlasUV = lightSize * worldPerDepth * worldToAtlasUV;
    float avgBlocker = FindBlockerDepth_Directional(shadowMap, atlasUV, receiverDepth, searchRadiusAtlasUV, receiverPlaneBias, rot);
    if (avgBlocker < 0.0)
        return 1.0; // No blockers in search disk -> fully lit

    // Similar triangles for a directional source: penumbra grows with the WORLD gap to the blocker.
    float gapWorld = max(receiverDepth - avgBlocker, 0.0) * worldPerDepth;
    float penumbraWorld = lightSize * gapWorld;
    float filterRadiusAtlasUV = penumbraWorld * worldToAtlasUV;

    // Clamp to >= ~1 atlas texel for a stable footprint (resolution-relative, intentionally per-tile).
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);
    filterRadiusAtlasUV = max(filterRadiusAtlasUV, 1.0 / shadowMapSize.x);

    return SamplePCF_VogelDirectional(shadowMap, atlasUV, receiverDepth, filterRadiusAtlasUV, receiverPlaneBias, rot);
}

// Samples the shadow cascade for the given directional light on the material surface (supports subsurface shadowing)
ShadowSample SampleDirectionalLightShadowCascade(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, ShadowData shadow, float3 samplePosition, uint cascadeIndex, float2 screenPos, float NoL)
{
    ShadowSample result;
    ShadowTileData shadowTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, cascadeIndex);

    // Receiver-plane gradient is built from the raw surface world position's screen derivatives (not the
    // per-cascade biased samplePosition), so it stays continuous across cascade splits / the dither band.
    float3 ddxWorld = ddx(gBuffer.WorldPos);
    float3 ddyWorld = ddy(gBuffer.WorldPos);

    // Project position into shadow atlas UV with slope-scaled bias + receiver plane bias
    float4 shadowPosition;
    float2 receiverPlaneBias;
    float2 shadowMapUV = GetLightShadowAtlasUVWithReceiverBias(shadow, shadowTile, samplePosition, ddxWorld, ddyWorld, NoL, shadowPosition, receiverPlaneBias);

    BRANCH
    if (shadow.Softness > 0.0)
    {
        // PCSS contact-hardening. Flat epsilon is folded into the reference; taps follow the plane.
        result.SurfaceShadow = SamplePCSS_Directional(shadowMap, shadowTile, shadowMapUV, shadowPosition.z, shadow.Softness, receiverPlaneBias, screenPos);
    }
    else
    {
        // Fixed-radius Witness PCF with receiver plane bias and blue noise
        result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z, screenPos, receiverPlaneBias);
    }

    // Increase sharpness for higher cascades to match the fixed-radius PCF's growing texel footprint.
    // Skip it for PCSS: its penumbra is world-consistent across cascades, so a per-cascade sharpness
    // would re-introduce the very cascade seam that costs us here (contrast jumping at each split).
    if (shadow.Softness <= 0.0)
    {
        const float SharpnessScale[MaxNumCascades] = { 1.0f, 1.5f, 3.0f, 3.5f };
        shadow.Sharpness *= SharpnessScale[min(cascadeIndex, (uint)MaxNumCascades - 1)];
    }

    result.TransmissionShadow = 1;
#if defined(USE_GBUFFER_CUSTOM_DATA)
	// Subsurface shadowing
	BRANCH
	if (IsSubsurfaceMode(gBuffer.ShadingModel))
	{
		float opacity = gBuffer.CustomData.a;
        shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, gBuffer.WorldPos, shadowPosition);
		float shadowMapDepth = LOAD_SHADOW_MAP(shadowMap, shadowMapUV);
        // Depth delta is in cascade-normalized units (1 = the cascade's depth range, which tracks
        // camera FOV); convert to world units so transmission doesn't vary per cascade or with FOV.
        float thicknessWorld = max(shadowPosition.z - shadowMapDepth, 0.0f) * GetShadowTileWorldPerDepthUnit(shadowTile);
		result.TransmissionShadow = CalculateSubsurfaceOcclusionWorld(opacity, thicknessWorld, shadowMapDepth);
        result.TransmissionShadow = PostProcessShadow(shadow, result.TransmissionShadow);
	}
#else
    result.TransmissionShadow = 1;
#endif

    result.SurfaceShadow = PostProcessShadow(shadow, result.SurfaceShadow);

    return result;
}

// Samples weapon shadow for the given directional light (simple single shadow map, no cascades)
float SampleWeaponShadow(LightData light, Buffer<float4> weaponShadowsBuffer, Texture2D<float> weaponShadowMap, float3 worldPosition, float3 cameraPosition, int shadingModel)
{
    // Only apply weapon shadows to surfaces with weapon shading model (for self-shadowing)
    if (shadingModel != SHADING_MODEL_WEAPON)
        return 1.0; // Non-weapon surfaces don't receive weapon shadows

    // Check if weapon shadows are enabled
    if (light.WeaponShadowsBufferAddress == 0)
        return 1.0; // No weapon shadow

    // Load weapon shadow matrix from buffer (single 4x4 matrix)
    float4x4 weaponWorldToShadow;
    weaponWorldToShadow[0] = weaponShadowsBuffer[light.WeaponShadowsBufferAddress + 0];
    weaponWorldToShadow[1] = weaponShadowsBuffer[light.WeaponShadowsBufferAddress + 1];
    weaponWorldToShadow[2] = weaponShadowsBuffer[light.WeaponShadowsBufferAddress + 2];
    weaponWorldToShadow[3] = weaponShadowsBuffer[light.WeaponShadowsBufferAddress + 3];

    // Use standard shadow bias for weapons
    float distanceFromCamera = length(worldPosition - cameraPosition);
    float shadowBias = lerp(0.0001f, 0.001f, saturate(1.0f - distanceFromCamera / 200.0f));

    // Transform world position to shadow atlas space (EXACTLY like CSM)
    float4 shadowPos = mul(float4(worldPosition, 1.0), weaponWorldToShadow);

    // Apply bias BEFORE perspective divide (EXACTLY like CSM)
    shadowPos.z -= shadowBias;

    // Perspective divide (EXACTLY like CSM)
    shadowPos.xyz /= shadowPos.w;

    // Check bounds (atlas UVs should be [0,1])
    if (any(shadowPos.xy < 0.0) || any(shadowPos.xy > 1.0))
        return 1.0; // Outside weapon shadow coverage, fully lit

    // Sample with PCF (EXACTLY like CSM)
    float weaponShadow = SampleShadowMapOptimizedPCF(weaponShadowMap, shadowPos.xy, shadowPos.z);

    return weaponShadow;
}

// Samples the shadow for the given directional light on the material surface (supports subsurface shadowing)
// svPosition: viewport pixel coords (SV_Position.xy) for the sample-rotation noise seed. Pixel
// shaders pass it; compute/custom callers leave it negative to fall back to view-space position.
ShadowSample SampleDirectionalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, float dither = 0.0f, float2 svPosition = float2(-1, -1))
{
#if !LIGHTING_NO_DIRECTIONAL
    // Skip if surface is in a full shadow
    float NoL = dot(gBuffer.Normal, light.Direction);
    BRANCH
    if (NoL <= 0
#if defined(USE_GBUFFER_CUSTOM_DATA)
        && !IsSubsurfaceMode(gBuffer.ShadingModel)
#endif
        )
        return (ShadowSample)0;
#else
    float NoL = 1.0; // No directional lighting context: bias divisor stays at 1.
#endif

    ShadowSample result;
    result.SurfaceShadow = 1;
    result.TransmissionShadow = 1;

    // Load shadow data
    if (light.ShadowsBufferAddress == 0)
        return result; // No shadow assigned
    ShadowData shadow = LoadShadowsBuffer(shadowsBuffer, light.ShadowsBufferAddress);

    // Create a blend factor which is one before and at the fade plane
    float viewDepth = gBuffer.ViewPos.z;
    float fade = saturate((viewDepth - shadow.CascadeSplits[shadow.TilesCount - 1] + shadow.FadeDistance) / shadow.FadeDistance);
    BRANCH
    if (fade >= 1.0)
        return result;

    // Figure out which cascade to sample from
    uint cascadeIndex = 0;
    for (uint i = 0; i < shadow.TilesCount - 1; i++)
    {
        if (viewDepth > shadow.CascadeSplits[i])
            cascadeIndex = i + 1;
    }
#if SHADOWS_CSM_DITHERING || SHADOWS_CSM_BLENDING
	float nextSplit = shadow.CascadeSplits[cascadeIndex];
	float splitSize = cascadeIndex == 0 ? nextSplit : nextSplit - shadow.CascadeSplits[cascadeIndex - 1];
	float splitDist = (nextSplit - viewDepth) / splitSize;
#endif
#if SHADOWS_CSM_DITHERING && !SHADOWS_CSM_BLENDING
	const float BlendThreshold = 0.05f;
    if (splitDist <= BlendThreshold && cascadeIndex != shadow.TilesCount - 1)
    {
        // Dither with the next cascade but with screen-space dithering (gets cleaned out by TAA)
        float lerpAmount = 1 - splitDist / BlendThreshold;
        if (step(RandN2(gBuffer.ViewPos.xy + dither).x, lerpAmount))
            cascadeIndex++;
    }
#endif

    // Sample cascade
    float3 samplePosition = gBuffer.WorldPos;
#if !LIGHTING_NO_DIRECTIONAL
    // Apply normal offset bias. NormalOffsetScale is authored in texel units (10 = 1 texel) and
    // converted with this cascade's world texel size, so the offset tracks the cascade extent,
    // atlas resolution and camera FOV (a fixed world offset cannot suit more than one projection).
    float2 shadowAtlasSize;
    shadowMap.GetDimensions(shadowAtlasSize.x, shadowAtlasSize.y);
    ShadowTileData offsetTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, cascadeIndex);
    float texelWorldSize = GetShadowTileTexelWorldSize(offsetTile, shadowAtlasSize.x);
    samplePosition += GetShadowPositionOffset(shadow.NormalOffsetScale * texelWorldSize, NoL, gBuffer.Normal);
#endif
    // Seed sample-rotation noise (Vogel disk / PCF dither) with true viewport pixel coords when the
    // caller supplies them. InterleavedGradientNoise expects unit-spaced pixel indices; view-space
    // ViewPos.xy (cm, spacing ~depth) correlates neighbouring taps into screen-locked moire rings.
    // Fall back to ViewPos.xy for compute/custom callers that can't provide pixel coordinates.
    float2 screenPos = svPosition.x >= 0.0 ? svPosition : gBuffer.ViewPos.xy;
    // Slope-scaled bias uses saturated NoL; subsurface paths pass through with NoL=1 fallback.
    float NoLSat = saturate(NoL);
    result = SampleDirectionalLightShadowCascade(light, shadowsBuffer, shadowMap, gBuffer, shadow, samplePosition, cascadeIndex, screenPos, NoLSat);

#if SHADOWS_CSM_BLENDING
	const float BlendThreshold = 0.1f;
    if (splitDist <= BlendThreshold && cascadeIndex != shadow.TilesCount - 1)
    {
	    // Sample the next cascade, and blend between the two results to smooth the transition
        ShadowSample nextResult = SampleDirectionalLightShadowCascade(light, shadowsBuffer, shadowMap, gBuffer, shadow, samplePosition, cascadeIndex + 1, screenPos, NoLSat);
		float blendAmount = splitDist / BlendThreshold;
		result.SurfaceShadow = lerp(nextResult.SurfaceShadow, result.SurfaceShadow, blendAmount);
		result.TransmissionShadow = lerp(nextResult.TransmissionShadow, result.TransmissionShadow, blendAmount);
    }
#endif

    // Apply CSM fade to smoothly blend to no-shadow at distance
    result.SurfaceShadow = lerp(result.SurfaceShadow, 1.0, fade);
    result.TransmissionShadow = lerp(result.TransmissionShadow, 1.0, fade);

    return result;
}

// Samples the shadow for the given local light on the material surface (supports subsurface shadowing)
ShadowSample SampleLocalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, float3 L, float toLightLength, uint tileIndex)
{
#if !LIGHTING_NO_DIRECTIONAL
    // Skip if surface is in a full shadow
    float NoL = dot(gBuffer.Normal, L);
    BRANCH
    if (NoL <= 0
#if defined(USE_GBUFFER_CUSTOM_DATA)
        && !IsSubsurfaceMode(gBuffer.ShadingModel)
#endif
        )
        return (ShadowSample)0;
#endif

    ShadowSample result;
    result.SurfaceShadow = 1;
    result.TransmissionShadow = 1;

    // Skip pixels outside of the light influence
    BRANCH
    if (toLightLength > light.Radius)
        return result;

    // Load shadow data
    if (light.ShadowsBufferAddress == 0)
        return result; // No shadow assigned
    ShadowData shadow = LoadShadowsBuffer(shadowsBuffer, light.ShadowsBufferAddress);
    ShadowTileData shadowTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, tileIndex);

    float3 samplePosition = gBuffer.WorldPos;
#if !LIGHTING_NO_DIRECTIONAL
    // Apply normal offset bias
    samplePosition += GetShadowPositionOffset(shadow.NormalOffsetScale, NoL, gBuffer.Normal);
#endif

    // Project position into shadow atlas UV, with the exact receiver-plane gradient for PCF taps
    float4 shadowPosition;
    float2 receiverPlaneBias;
    float2 shadowMapUV = GetLightShadowAtlasUVLocalWithReceiverBias(shadow, shadowTile, samplePosition, shadowPosition, receiverPlaneBias);

    // Sample shadow map
    result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z, float2(0, 0), receiverPlaneBias);

#if defined(USE_GBUFFER_CUSTOM_DATA)
	// Subsurface shadowing
	BRANCH
	if (IsSubsurfaceMode(gBuffer.ShadingModel))
	{
		float opacity = gBuffer.CustomData.a;
        shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, gBuffer.WorldPos, shadowPosition);
		float shadowMapDepth = LOAD_SHADOW_MAP(shadowMap, shadowMapUV);
		result.TransmissionShadow = CalculateSubsurfaceOcclusion(opacity, shadowPosition.z, shadowMapDepth);
        result.TransmissionShadow = PostProcessShadow(shadow, result.TransmissionShadow);
	}
#endif

    result.SurfaceShadow = PostProcessShadow(shadow, result.SurfaceShadow);
    return result;
}

// Samples the shadow for the given spot light on the material surface (supports subsurface shadowing)
ShadowSample SampleSpotLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer)
{
    float3 toLight = light.Position - gBuffer.WorldPos;
    float toLightLength = length(toLight);
    float3 L = toLight / toLightLength;
    return SampleLocalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, L, toLightLength, 0);
}

// Samples the shadow for the given point light on the material surface (supports subsurface shadowing)
ShadowSample SamplePointLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer)
{
    float3 toLight = light.Position - gBuffer.WorldPos;
    float toLightLength = length(toLight);
    float3 L = toLight / toLightLength;

    // Figure out which cube face we're sampling from
    uint cubeFaceIndex = GetCubeFaceIndex(-L);

    return SampleLocalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, L, toLightLength, cubeFaceIndex);
}

GBufferSample GetDummyGBufferSample(float3 worldPosition)
{
    GBufferSample gBuffer = (GBufferSample)0;
    gBuffer.ShadingModel = SHADING_MODEL_LIT;
    gBuffer.WorldPos = worldPosition;
    return gBuffer;
}

// Samples the shadow for the given directional light at custom location
ShadowSample SampleDirectionalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition, float viewDepth, float dither = 0.0f)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    gBuffer.ViewPos.z = viewDepth;
    return SampleDirectionalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, dither);
}

// Samples the shadow for the given spot light at custom location
ShadowSample SampleSpotLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    return SampleSpotLightShadow(light, shadowsBuffer, shadowMap, gBuffer);
}

// Samples the shadow for the given point light at custom location
ShadowSample SamplePointLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    return SamplePointLightShadow(light, shadowsBuffer, shadowMap, gBuffer);
}

#endif
