// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/Lighting/BRDF.hlsl"
#include "./Flax/Math/Noise.hlsl"
#include "./Flax/Reflections/ReflectionsCommon.hlsl"

// Enable/disable blurring SSR during sampling results and mixing with reflections buffer
#define SSR_MIX_BLUR (!defined(PLATFORM_WEB) && !defined(PLATFORM_ANDROID) && !defined(PLATFORM_IOS) && !defined(PLATFORM_SWITCH))

META_CB_BEGIN(0, Data)

EnvProbeData PData;
float4x4 WVP;
GBufferData GBuffer;
float2 SSRTexelSize;
float2 Dummy0;

META_CB_END

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

TextureCube Probe : register(t4);
Texture2D Reflections : register(t5);
Texture2D PreIntegratedGF : register(t6);
Texture2D SSR : register(t7);
Texture2D DiffuseReflections : register(t8);


// Vertex Shader for probe shape rendering
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT, 0, ALIGN, PER_VERTEX, 0, true)
Model_VS2PS VS_Model(ModelInput_PosOnly input)
{
	Model_VS2PS output;
	output.Position = PROJECT_POINT(float4(input.Position.xyz, 1), WVP);
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

	// Sample GBuffer
	float2 uv = ProjectClipToUV(input.ScreenPos.xy / input.ScreenPos.w);
	GBufferData gBufferData = GetGBufferData();
	GBufferSample gBuffer = SampleGBuffer(gBufferData, uv);

	// Check if cannot light a pixel
	BRANCH
	if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
	{
		discard;
		return output;
	}

	// Unpack env probe data (upstream packs fields into Data0/Data1/Data2; see ReflectionsCommon.hlsl)
	float3 probePos = PData.Data0.xyz;
	float probeBrightness = abs(PData.Data0.w); // negative w flags box projection
	float probeInvRadius = 1.0 / PData.Data1.x; // Data1.x = Radius (sphere) / extent.x (box)

	// Calculate distance from probe to the pixel
	float3 captureVector = gBuffer.WorldPos - probePos;
	float captureVectorLength = length(captureVector);
	float radius = 1.0 / probeInvRadius;
	
	// Check if outside probe radius
	BRANCH
	if (captureVectorLength >= radius)
	{
		discard;
		return output;
	}
	
	// Calculate probe weight with size-based blending priority
	// Smaller probes (higher InvRadius) get higher priority
	float normalizedDist = saturate(captureVectorLength * probeInvRadius);
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
	float sizeWeight = probeInvRadius * 0.1; // Smaller probes get higher weight
	weight = weight * probeBrightness * (1.0 + sizeWeight);
	
	if (weight <= 0.0001f)
	{
		discard;
		return output;
	}

	// Sample for specular reflections
	float3 V = normalize(gBuffer.WorldPos - gBufferData.ViewPos);
	float3 R = reflect(V, gBuffer.Normal);
	float3 specularDir = normalize(captureVector + R / probeInvRadius);
	float specularMip = ProbeMipFromRoughness(gBuffer.Roughness);
	float3 specularSample = Probe.SampleLevel(SamplerLinearClamp, specularDir, specularMip).rgb;

	// Sample for diffuse ambient lighting (use highest mip for diffuse)
	float3 diffuseDir = normalize(captureVector + gBuffer.Normal / probeInvRadius);
	float3 diffuseSample = Probe.SampleLevel(SamplerLinearClamp, diffuseDir, REFLECTION_CAPTURE_NUM_MIPS - 1).rgb;

	output.Specular = float4(specularSample, weight);
	output.Diffuse = float4(diffuseSample, weight);
	
	return output;
}

// Pixel Shader for reflections combine pass (additive rendering to the light buffer)
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_CombinePass(Quad_VS2PS input) : SV_Target0
{
	GBufferData gBufferData = GetGBufferData();
	GBufferSample gBuffer = SampleGBuffer(gBufferData, input.TexCoord);

	BRANCH
	if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
		return 0;

	float4 specularProbe = SAMPLE_RT(Reflections, input.TexCoord);
	float4 diffuseProbe = SAMPLE_RT(DiffuseReflections, input.TexCoord);

    // Blend with Screen Space Reflections
    float4 ssr = SSR.SampleLevel(SamplerLinearClamp, input.TexCoord, 0);
#if SSR_MIX_BLUR
    ssr += SSR.SampleLevel(SamplerLinearClamp, input.TexCoord + float2(0, SSRTexelSize.y), 0);
    ssr += SSR.SampleLevel(SamplerLinearClamp, input.TexCoord - float2(0, SSRTexelSize.y), 0);
    ssr += SSR.SampleLevel(SamplerLinearClamp, input.TexCoord + float2(SSRTexelSize.x, 0), 0);
    ssr += SSR.SampleLevel(SamplerLinearClamp, input.TexCoord - float2(SSRTexelSize.x, 0), 0);
    ssr *= (1.0f / 5.0f);
#endif

	if (specularProbe.a < 0.0001f && diffuseProbe.a < 0.0001f && ssr.a < 0.0001f)
		return 0;
		
	float3 normalizedSpecular = specularProbe.a > 0.0001f ? specularProbe.rgb / specularProbe.a : 0;
	normalizedSpecular = lerp(normalizedSpecular, ssr.rgb, saturate(ssr.a));
	float3 normalizedDiffuse = diffuseProbe.a > 0.0001f ? diffuseProbe.rgb / diffuseProbe.a : 0;

	float3 diffuseColor = GetDiffuseColor(gBuffer);
	float3 V = normalize(gBufferData.ViewPos - gBuffer.WorldPos);
	float NoV = saturate(dot(gBuffer.Normal, V));

	// 1. PBR F0 (Matches Lighting.hlsl)
	// We map 0.5 Specular to 0.04 F0 (Standard PBR)
	float dielectricF0 = 0.08 * gBuffer.Specular; 
	float3 F0 = lerp(float3(dielectricF0, dielectricF0, dielectricF0), gBuffer.Color.rgb, gBuffer.Metalness);

	// 2. Fresnel with Roughness Softening
	// At high roughness, strict Fresnel (pow5) can look like a sharp ring. 
	// We dampen the *power* of the Fresnel slightly based on roughness to soften the transition.
	float fresnelPower = 5.0 / (1.0 + gBuffer.Roughness * 4.0); 
	float3 F = F0 + (max(float3(1.0 - gBuffer.Roughness, 1.0 - gBuffer.Roughness, 1.0 - gBuffer.Roughness), F0) - F0) * pow(1.0 - NoV, fresnelPower);

	float3 finalSpecular = normalizedSpecular * F;

	// 3. Specular Occlusion (Relaxed)
	float roughnessSq = gBuffer.Roughness * gBuffer.Roughness;
	float specularOcclusion = saturate(pow(NoV + gBuffer.AO, roughnessSq * 0.5) - 1 + gBuffer.AO);
	
    // Fixed typo here: ensured 'finalSpecular' is used correctly
    finalSpecular *= specularOcclusion;
	
	float3 finalDiffuse = normalizedDiffuse * diffuseColor * gBuffer.AO;

	return float4(finalSpecular + finalDiffuse, 0);

	//// Sample reflections buffer
	//float3 reflections = SAMPLE_RT(Reflections, input.TexCoord).rgb;
	//
	//// Calculate reflection color
	//reflections *= GetReflectionSpecularLighting(PreIntegratedGF, gBufferData.ViewPos, gBuffer);
	//
	//return float4(reflections, 0);
}
