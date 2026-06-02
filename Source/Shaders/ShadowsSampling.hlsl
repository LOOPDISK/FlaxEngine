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
// Compute shaders that sample shadows (e.g. volumetric fog) must define this 0 before including,
// else ddx/ddy fail to map to the compute instruction set (X4532). With it off, the slope-scaled
// constant bias still applies; only the per-tap receiver-plane refinement is dropped.
#ifndef SHADOWS_USE_RECEIVER_PLANE_BIAS
#define SHADOWS_USE_RECEIVER_PLANE_BIAS 1
#endif

#include "./Flax/ShadowsCommon.hlsl"
#include "./Flax/GBufferCommon.hlsl"
#include "./Flax/LightingCommon.hlsl"
#if SHADOWS_CSM_DITHERING
#include "./Flax/Random.hlsl"
#endif

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

float2 GetLightShadowAtlasUV(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, out float4 shadowPosition)
{
    // Project into shadow space (WorldToShadow is pre-multiplied to convert Clip Space to UV Space)
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    // Bias is now applied per-pixel via receiver plane bias in directional light sampling
    // For local lights, still apply constant bias as fallback
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

float2 GetLightShadowAtlasUVWithReceiverBias(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, float NoL, out float4 shadowPosition, out float2 receiverPlaneDepthBias)
{
    // Project into shadow space (WorldToShadow is pre-multiplied to convert Clip Space to UV Space)
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    shadowPosition.xyz /= shadowPosition.w;

    // Receiver plane depth bias from shadow-space gradient. The formula is approximate
    // (uses raw ddx/ddy(z) instead of the full inverse-Jacobian) and only contributes a
    // small adjustment to PCF taps; the tight clamp here is what keeps glancing-angle edge
    // pixels from poisoning the kernel. Derivatives are pixel-shader only - compute callers
    // (SHADOWS_USE_RECEIVER_PLANE_BIAS 0) get zero bias and rely on the slope-scaled term below.
#if SHADOWS_USE_RECEIVER_PLANE_BIAS
    float3 shadowPosDDX = ddx(shadowPosition.xyz);
    float3 shadowPosDDY = ddy(shadowPosition.xyz);
    receiverPlaneDepthBias = float2(shadowPosDDX.z, shadowPosDDY.z);
    receiverPlaneDepthBias = clamp(receiverPlaneDepthBias, -0.05, 0.05);
#else
    receiverPlaneDepthBias = float2(0.0, 0.0);
#endif

    // Constant slope-scaled depth bias for the comparison reference.
    shadowPosition.z -= ComputeSlopeScaledBias(shadow.Bias, NoL);

    // UV Space -> Atlas Tile UV Space
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

// 16-tap optimized Poisson disk in unit-radius. Used for PCSS blocker search and variable-radius PCF.
static const float2 PoissonDisk16[16] =
{
    float2(-0.94201624, -0.39906216),
    float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870),
    float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432),
    float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845),
    float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554),
    float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023),
    float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507),
    float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367),
    float2( 0.14383161, -0.14100790),
};

// PCSS blocker search. Returns average blocker depth in shadow-NDC, or -1 if no blockers found.
// searchRadiusAtlasUV is the search disk radius expressed in atlas-UV units (already tile-scaled).
float FindBlockerDepth_Directional(Texture2D<float> shadowMap, float2 atlasUV, float receiverDepth, float searchRadiusAtlasUV, float bias)
{
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    UNROLL
    for (int i = 0; i < 16; i++)
    {
        float2 uv = atlasUV + PoissonDisk16[i] * searchRadiusAtlasUV;
        float d = shadowMap.SampleLevel(SamplerLinearClamp, uv, 0).r;
        if (d < receiverDepth - bias)
        {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }
    return blockerCount > 0.0 ? (blockerSum / blockerCount) : -1.0;
}

// Variable-radius Poisson PCF in atlas UV. Radius is the disk radius (atlas-UV units).
float SamplePCF_Poisson16(Texture2D<float> shadowMap, float2 atlasUV, float sceneDepth, float radiusAtlasUV)
{
    float sum = 0.0;
    UNROLL
    for (int i = 0; i < 16; i++)
    {
        float2 uv = atlasUV + PoissonDisk16[i] * radiusAtlasUV;
        sum += SAMPLE_SHADOW_MAP(shadowMap, uv, sceneDepth);
    }
    return sum * (1.0 / 16.0);
}

// PCSS for directional light: blocker search -> penumbra estimate -> variable-radius PCF.
// lightSize is in cascade-UV space (a fraction of the cascade's coverage). Converted to atlas UV
// via the tile's UV scale so the search/filter disk stays the right physical size per cascade.
float SamplePCSS_Directional(Texture2D<float> shadowMap, ShadowTileData tile, float2 atlasUV, float receiverDepth, float lightSize, float bias)
{
    // Same scale on X and Y (tiles are square)
    float tileScale = tile.ShadowToAtlas.x;

    float searchRadiusAtlasUV = lightSize * tileScale;
    float avgBlocker = FindBlockerDepth_Directional(shadowMap, atlasUV, receiverDepth, searchRadiusAtlasUV, bias);
    if (avgBlocker < 0.0)
        return 1.0; // No blockers in search disk -> fully lit

    // Orthographic projection: penumbra width is linear in (receiver - blocker) depth.
    float penumbraCascadeUV = (receiverDepth - avgBlocker) * lightSize;

    // Clamp to at least ~one cascade texel so we always get a stable filter footprint.
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);
    float minRadiusCascadeUV = 1.0 / (shadowMapSize.x * tileScale);
    float filterRadiusCascadeUV = max(penumbraCascadeUV, minRadiusCascadeUV);
    float filterRadiusAtlasUV = filterRadiusCascadeUV * tileScale;

    return SamplePCF_Poisson16(shadowMap, atlasUV, receiverDepth - bias, filterRadiusAtlasUV);
}

// Samples the shadow cascade for the given directional light on the material surface (supports subsurface shadowing)
ShadowSample SampleDirectionalLightShadowCascade(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, ShadowData shadow, float3 samplePosition, uint cascadeIndex, float2 screenPos, float NoL)
{
    ShadowSample result;
    ShadowTileData shadowTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, cascadeIndex);

    // Project position into shadow atlas UV with slope-scaled bias + receiver plane bias
    float4 shadowPosition;
    float2 receiverPlaneBias;
    float2 shadowMapUV = GetLightShadowAtlasUVWithReceiverBias(shadow, shadowTile, samplePosition, NoL, shadowPosition, receiverPlaneBias);

    BRANCH
    if (shadow.Softness > 0.0)
    {
        // PCSS contact-hardening. Bias is folded in via the comparison reference.
        result.SurfaceShadow = SamplePCSS_Directional(shadowMap, shadowTile, shadowMapUV, shadowPosition.z, shadow.Softness, 0.0);
    }
    else
    {
        // Fixed-radius Witness PCF with receiver plane bias and blue noise
        result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z, screenPos, receiverPlaneBias);
    }

    // Increase the sharpness for higher cascades to match the filter radius
    const float SharpnessScale[MaxNumCascades] = { 1.0f, 1.5f, 3.0f, 3.5f };
    shadow.Sharpness *= SharpnessScale[cascadeIndex];

    result.TransmissionShadow = 1;
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
ShadowSample SampleDirectionalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, float dither = 0.0f)
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
    // Apply normal offset bias
    samplePosition += GetShadowPositionOffset(shadow.NormalOffsetScale, NoL, gBuffer.Normal);
#endif
    // Use view position for screen-space noise (available in all contexts)
    float2 screenPos = gBuffer.ViewPos.xy;
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

    // Project position into shadow atlas UV
    float4 shadowPosition;
    float2 shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, samplePosition, shadowPosition);

    // Sample shadow map
    result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z);

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
