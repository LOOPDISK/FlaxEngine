#include "./Flax/Common.hlsl"

META_CB_BEGIN(0, HZBData)
float4 ViewInfo;
float3 ViewPos;
float ViewFar;
float4x4 InvViewMatrix;
float4x4 InvProjectionMatrix;
float3 Dummy0;
META_CB_END

Texture2D DepthTexture : register(t0);

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_HZB(Quad_VS2PS input) : SV_Target
{
	return float4(1,0,1,1);
}

// Get view space position at given pixel coordinate with given device depth
float3 GetViewPosV(float2 uv, float deviceDepth, float4x4 invProj)
{
    float4 clipPos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), deviceDepth, 1.0);
    float4 viewPos = mul(clipPos, invProj);
    return viewPos.xyz / viewPos.w;
}

// Linearize raw device depth
float LinearizeZ(float depth, float4 viewInfo)
{
    return viewInfo.w / (depth - viewInfo.z);
}

// Convert linear depth to device depth
float LinearZ2DeviceDepth(float linearDepth, float4 viewInfo)
{
    return (viewInfo.w / linearDepth) + viewInfo.z;
}

// Pixel shader for debug view
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DebugView(Quad_VS2PS input) : SV_Target
{
    float2 uv = input.TexCoord;
    float depth = DepthTexture.SampleLevel(SamplerPointClamp, uv, 0).r;
    float3 viewPos = GetViewPosV(uv, depth, InvProjectionMatrix);
	float3 result = viewPos.z / ViewFar;
	return float4(result, 1);
}
