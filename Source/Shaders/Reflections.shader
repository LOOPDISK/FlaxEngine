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
Texture2D DiffuseReflections : register(t7);


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

// Environment probe output structure for dual render targets
struct ProbeBufferOutput 
{
    float4 Specular : SV_Target0;  // RGB: Specular radiance, A: Probe weight
    float4 Diffuse : SV_Target1;   // RGB: Diffuse irradiance, A: Probe weight
};

// Pixel Shader for enviroment probes rendering
META_PS(true, FEATURE_LEVEL_ES2)
ProbeBufferOutput PS_EnvProbe(Model_VS2PS input)
{
	ProbeBufferOutput output = (ProbeBufferOutput)0;
	
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
		return output;
	}

	// Calculate distance from probe to the pixel
	float3 captureVector = gBuffer.WorldPos - PData.ProbePos;
	float captureVectorLength = length(captureVector);
	float radius = 1.0 / PData.ProbeInvRadius;
	
	// Check if outside probe radius
	BRANCH
	if (captureVectorLength >= radius)
	{
		discard;
		return output;
	}
	
	// Calculate probe weight with size-based blending priority
	// Smaller probes (higher InvRadius) get higher priority
	float normalizedDist = saturate(captureVectorLength * PData.ProbeInvRadius);
	float weight;
	if (normalizedDist < 0.5)
	{
		weight = 1.0;
	}
	else
	{
		float t = (normalizedDist - 0.5) / 0.5;
		weight = 1.0 - t * t;
	}
	
	// Scale by brightness and add size priority (smaller probes override larger)
	float sizeWeight = PData.ProbeInvRadius * 0.1; // Smaller probes get higher weight
	weight = weight * PData.ProbeBrightness * (1.0 + sizeWeight);
	
	if (weight <= 0.0001f)
	{
		discard;
		return output;
	}

	// Sample for specular reflections
	float3 V = normalize(gBuffer.WorldPos - gBufferData.ViewPos);
	float3 R = reflect(V, gBuffer.Normal);
	float3 specularDir = normalize(captureVector + R / PData.ProbeInvRadius);
	float specularMip = ProbeMipFromRoughness(gBuffer.Roughness);
	float3 specularSample = Probe.SampleLevel(SamplerLinearClamp, specularDir, specularMip).rgb;

	// Sample for diffuse ambient lighting (use highest mip for diffuse)
	float3 diffuseDir = normalize(captureVector + gBuffer.Normal / PData.ProbeInvRadius);
	float3 diffuseSample = Probe.SampleLevel(SamplerLinearClamp, diffuseDir, REFLECTION_CAPTURE_NUM_MIPS - 1).rgb;

	output.Specular = float4(specularSample, weight);
	output.Diffuse = float4(diffuseSample, weight);
	
	return output;
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

	// Sample both reflection buffers
	float4 specularProbe = SAMPLE_RT(Reflections, input.TexCoord);
	float4 diffuseProbe = SAMPLE_RT(DiffuseReflections, input.TexCoord);

	// Skip if no probe contribution
	if (specularProbe.a < 0.0001f && diffuseProbe.a < 0.0001f)
		return 0;
		
	// Normalize contributions by total weight to handle overlapping probes
	float3 normalizedSpecular = specularProbe.a > 0.0001f ? specularProbe.rgb / specularProbe.a : 0;
	float3 normalizedDiffuse = diffuseProbe.a > 0.0001f ? diffuseProbe.rgb / diffuseProbe.a : 0;

	// Calculate specular color
	float3 specularColor = GetSpecularColor(gBuffer);
	float3 diffuseColor = GetDiffuseColor(gBuffer);

	// Calculate reflection color with advanced dielectric handling
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
	float3 finalSpecular = normalizedSpecular * F * specular_boost;

	// Apply specular occlusion
	float roughnessSq = gBuffer.Roughness * gBuffer.Roughness;
	float specularOcclusion = GetSpecularOcclusion(NoV, roughnessSq, gBuffer.AO);
	finalSpecular *= specularOcclusion;
	
	// Apply diffuse contribution with ambient occlusion
	float3 finalDiffuse = normalizedDiffuse * diffuseColor * gBuffer.AO;

	return float4(finalSpecular + finalDiffuse, 0);
}
