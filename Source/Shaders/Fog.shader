// Copyright (c) Wojciech Figat. All rights reserved.

#define NO_GBUFFER_SAMPLING

#include "./Flax/Common.hlsl"
#include "./Flax/GBufferCommon.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/VolumetricFog.hlsl"
#include "./Flax/ExponentialHeightFog.hlsl"

// Disable Volumetric Fog if is not supported
#if VOLUMETRIC_FOG && !CAN_USE_COMPUTE_SHADER
#undef VOLUMETRIC_FOG
#define VOLUMETRIC_FOG 0
#endif

META_CB_BEGIN(0, Data)
GBufferData GBuffer;
ExponentialHeightFogData ExponentialHeightFog;
VolumetricFogData VolumetricFog;
float4 TemporalAAJitter;
float FogInjectStrength;
float3 FogInjectPadding;
META_CB_END

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

Texture2D Depth : register(t0);
#if VOLUMETRIC_FOG
Texture3D VolumetricFogTexture : register(t1);
#endif
Texture2D GBuffer1 : register(t2);
#if FOG_INJECT
Texture2D FogInjectTexture : register(t3); // Low-res particle-driven density (R = signed density push)
#endif

META_PS(true, FEATURE_LEVEL_ES2)
META_PERMUTATION_2(VOLUMETRIC_FOG=0, FOG_INJECT=0)
META_PERMUTATION_2(VOLUMETRIC_FOG=1, FOG_INJECT=0)
META_PERMUTATION_2(VOLUMETRIC_FOG=0, FOG_INJECT=1)
META_PERMUTATION_2(VOLUMETRIC_FOG=1, FOG_INJECT=1)
float4 PS_Fog(Quad_VS2PS input) : SV_Target0
{
    // Get world space position at given pixel coordinate
	float rawDepth = SAMPLE_RT_DEPTH(Depth, input.TexCoord);

	// Detect weapon pixels and undo depth remapping for correct fog distance
	int shadingModel = (int)(SAMPLE_RT(GBuffer1, input.TexCoord).a * 4.999);
	if (shadingModel == SHADING_MODEL_WEAPON)
		rawDepth *= 100.0; // Undo 0.01 depth remapping from WeaponFOVOverride

	GBufferData gBufferData = GetGBufferData();
	float3 viewPos = GetViewPos(gBufferData, input.TexCoord, rawDepth);
	float3 worldPos = mul(float4(viewPos, 1), gBufferData.InvViewMatrix).xyz;

    float skipDistance = 0;
#if VOLUMETRIC_FOG
	skipDistance = max(ExponentialHeightFog.VolumetricFogMaxDistance - 100, 0);
#endif


	// Calculate exponential fog color
	float4 fog = GetExponentialHeightFog(ExponentialHeightFog, worldPos, GBuffer.ViewPos, skipDistance, viewPos.z);

#if VOLUMETRIC_FOG
    // Sample volumetric fog and mix it in
	float4 volumetricFog = SampleVolumetricFog(VolumetricFogTexture, VolumetricFog, worldPos - GBuffer.ViewPos, input.TexCoord, TemporalAAJitter);
	fog = CombineVolumetricFog(fog, volumetricFog);
#endif

#if FOG_INJECT
	// Organic particle-driven density push, RELATIVE to the fog already here. fog.a is transmittance
	// (exp(-opticalDepth)); scaling optical depth by k gives pow(fog.a, k) -> k = 1 + push: k>1 denser,
	// k<1 thinner, k=0 clears. Where there's no fog (fog.a=1) pow(1,k)=1 so particles can't conjure
	// fog from nothing. Match the transmittance delta in inscattering so color stays consistent.
	// Push is clamped as the buffer accumulates unbounded (overlapping particles stack additively).
	float push = clamp(FogInjectTexture.SampleLevel(SamplerLinearClamp, input.TexCoord, 0).r * FogInjectStrength, -8.0, 8.0);
	float fogA = pow(fog.a, max(0.0, 1.0 + push));
	fog.rgb += ExponentialHeightFog.FogInscatteringColor * (fog.a - fogA);
	fog.a = fogA;
#endif

	return float4(fog.rgb, 1.0 - fog.a);
}

