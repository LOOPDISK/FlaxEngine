// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/BRDF.hlsl"
#include "./Flax/Random.hlsl"
#include "./Flax/MonteCarlo.hlsl"
#include "./Flax/LightingCommon.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/ReflectionsCommon.hlsl"
#include "./Flax/BRDF.hlsl"

META_CB_BEGIN(0, Data)

ProbeData PData;
float4x4 WVP;
GBufferData GBuffer;

META_CB_END

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

TextureCube Probe : register(t4);
Texture2D Reflections : register(t5);
Texture2D PreIntegratedGF : register(t6);


// Vertex Shader for models rendering
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT, 0, ALIGN, PER_VERTEX, 0, true)
Model_VS2PS VS_Model(ModelInput_PosOnly input)
{
	Model_VS2PS output;
	output.Position = mul(float4(input.Position.xyz, 1), WVP);
	output.ScreenPos = output.Position;
	return output;
}

// Pixel Shader for enviroment probes rendering
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_EnvProbe(Model_VS2PS input) : SV_Target0
{
	// Obtain UVs corresponding to the current pixel
	float2 uv = (input.ScreenPos.xy / input.ScreenPos.w) * float2(0.5, -0.5) + float2(0.5, 0.5);

	// Sample GBuffer
	GBufferData gBufferData = GetGBufferData();
	GBufferSample gBuffer = SampleGBuffer(gBufferData, uv);

	// Check if cannot light a pixel
	BRANCH
	if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
	{
		discard;
		return 0;
	}

	// Sample probe
	return SampleReflectionProbe(gBufferData.ViewPos, Probe, PData, gBuffer.WorldPos, gBuffer.Normal, gBuffer.Roughness);
}

// Pixel Shader for reflections combine pass (additive rendering to the light buffer)
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_CombinePass(Quad_VS2PS input) : SV_Target0
{
	// Sample GBuffer
	GBufferData gBufferData = GetGBufferData();
	GBufferSample gBuffer = SampleGBuffer(gBufferData, input.TexCoord);

	// Check if cannot light pixel
	BRANCH
	if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
	{
		return 0;
	}

	// Sample reflections buffer
	float3 reflections = SAMPLE_RT(Reflections, input.TexCoord).rgb;

	// Calculate specular color
	float3 specularColor = GetSpecularColor(gBuffer);

	// Calculate reflecion color
	float3 V = normalize(gBufferData.ViewPos - gBuffer.WorldPos);
	float NoV = saturate(dot(gBuffer.Normal, V));
	
	// Combined approach: Working F0 calculation with proper metalness handling
	// Use the F0 range that worked for middle faces + additional specular boost for dielectrics only
	
	// Calculate F0 using the working formula (0.04 to 0.20 range)
	float dielectric_f0_scalar = 0.04 + 0.16 * gBuffer.Specular; // This worked for middle faces
	float3 metallic_f0 = gBuffer.Color.rgb; // Metals use full color as F0
	float3 F0 = lerp(float3(dielectric_f0_scalar, dielectric_f0_scalar, dielectric_f0_scalar), metallic_f0, gBuffer.Metalness);
	
	// Schlick Fresnel with roughness compensation  
	float3 F = F0 + (max(float3(1.0 - gBuffer.Roughness, 1.0 - gBuffer.Roughness, 1.0 - gBuffer.Roughness), F0) - F0) * pow(1.0 - NoV, 5.0);
	
	// Apply additional specular scaling only to dielectrics (avoid double-scaling for metals)
	float specular_boost = lerp(gBuffer.Specular, 1.0, gBuffer.Metalness);
	reflections *= F * specular_boost;

	// Apply specular occlusion
	float roughnessSq = gBuffer.Roughness * gBuffer.Roughness;
	float specularOcclusion = GetSpecularOcclusion(NoV, roughnessSq, gBuffer.AO);
	reflections *= specularOcclusion;

	return float4(reflections, 0);
}
