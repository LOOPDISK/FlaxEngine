// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/Common.hlsl"
#include "./Flax/AtmosphereFog.hlsl"

META_CB_BEGIN(0, Data)
float4x4 WVP;
float3 ViewOffset;
float Padding;
GBufferData GBuffer;
AtmosphericFogData AtmosphericFog;
float3 HorizonColor;
float Padding2;
float3 ZenithColor;
float Padding3;
META_CB_END


DECLARE_GBUFFERDATA_ACCESS(GBuffer)

struct MaterialInput
{
	float4 Position  : SV_Position;
	float4 ScreenPos : TEXCOORD0;
};

// Vertex Shader function for GBuffer Pass
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT, 0, 0, PER_VERTEX, 0, true)
MaterialInput VS(ModelInput_PosOnly input)
{
	MaterialInput output;

	// Compute vertex position
	output.Position = mul(float4(input.Position.xyz, 1), WVP);
	output.ScreenPos = output.Position;

	// Place pixels on the far plane
	output.Position = output.Position.xyzz;

	return output;
}

// Pixel Shader function for GBuffer Pass
META_PS(true, FEATURE_LEVEL_ES2)
GBufferOutput PS_Sky(MaterialInput input)
{
	GBufferOutput output;

	// Obtain UVs corresponding to the current pixel
	float2 uv = (input.ScreenPos.xy / input.ScreenPos.w) * float2(0.5, -0.5) + float2(0.5, 0.5);

	// Sample atmosphere color
	GBufferData gBufferData = GetGBufferData();
	float3 vsPos = GetViewPos(gBufferData, uv, LinearZ2DeviceDepth(gBufferData, 1));
	float3 wsPos = mul(float4(vsPos, 1), gBufferData.InvViewMatrix).xyz;
	float3 viewVector = wsPos - gBufferData.ViewPos;
	float4 color = GetAtmosphericFog(AtmosphericFog, gBufferData.ViewFar, wsPos + ViewOffset, gBufferData.ViewPos + ViewOffset);

	// TEMP: Always use fallback gradient until atmospheric fog is working properly
	float3 viewDir = normalize(viewVector);
	float skyGradient = saturate(viewDir.y * 0.5 + 0.5);

	// Use colors from Sky actor if set, otherwise use defaults
	float3 horizonColor = HorizonColor;
	float3 zenithColor = ZenithColor;

	// DEBUG: Always use defaults to see if shader is even running
	horizonColor = float3(0x2C / 255.0, 0x2C / 255.0, 0x2C / 255.0); // 0x2C2C2CFF
	zenithColor = float3(0xAE / 255.0, 0x9F / 255.0, 0x9A / 255.0);  // 0xAE9F9AFF

	color.rgb = lerp(horizonColor, zenithColor, skyGradient);
	color.a = 1.0;

	// Add sun disk (matching original GetSunColor implementation)
	float sunDiscScale = AtmosphericFog.AtmosphericFogSunDiscScale;
	if (sunDiscScale > 0.0)
	{
		float3 sunDir = AtmosphericFog.AtmosphericFogSunDirection;
		float sunIntensity = step(cos(PI * sunDiscScale / 180.0), dot(viewDir, sunDir));
		float3 sunColor = AtmosphericFog.AtmosphericFogSunColor * AtmosphericFog.AtmosphericFogSunPower;
		color.rgb += sunColor * sunIntensity;
	}

	// Pack GBuffer
	output.Light = color;
	output.RT0 = float4(0, 0, 0, 0);
	output.RT1 = float4(1, 0, 0, SHADING_MODEL_UNLIT);
	output.RT2 = float4(0, 0, 0, 0);
	output.RT3 = float4(0, 0, 0, 0);

	return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Fog(Quad_VS2PS input) : SV_Target0
{
	float4 result;
	/*
	// Sample GBuffer
	GBufferSample gBuffer = SampleGBuffer(GBuffer, input.TexCoord);
	
	// TODO: set valid scene color for better inscatter reflectance
	//float3 sceneColor = gBuffer.Color * AtmosphericFogDensityOffset;
	float3 sceneColor = float3(0, 0, 0);
	
	// Sample atmosphere color
	float3 viewVector = gBuffer.WorldPos - GBuffer.ViewPos;
	float SceneDepth = length(ViewVector);
	result = GetAtmosphericFog(AtmosphericFog, GBuffer.ViewFar, GBuffer.ViewPos, viewVector, SceneDepth, sceneColor);
	
	//result.rgb = normalize(ViewVector);
	//result.rgb = ViewVector;
	//result.rgb = SceneDepth.xxx / GBuffer.ViewFar * 0.5f;
	
	//result = float4(input.TexCoord, 0, 1);
	//result = AtmosphereTransmittanceTexture.Sample(SamplerLinearClamp, input.TexCoord);
	//result = float4(AtmosphereIrradianceTexture.Sample(SamplerLinearClamp, input.TexCoord).rgb*5.0, 1.0);
	//result = AtmosphereInscatterTexture.Sample(SamplerLinearClamp, float3(input.TexCoord.xy, (AtmosphericFogSunDirection.x+1.0)/2.0));
	*/

	// TODO: finish fog
	result = float4(1, 0, 0, 1);

	return result;
}
