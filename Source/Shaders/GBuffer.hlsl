// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __GBUFFER__
#define __GBUFFER__

#include "./Flax/GBufferCommon.hlsl"

#if !defined(NO_GBUFFER_SAMPLING)

// GBuffer
Texture2D GBuffer0 : register(t0);
Texture2D GBuffer1 : register(t1);
Texture2D GBuffer2 : register(t2);
Texture2D Depth : register(t3);
#if defined(USE_GBUFFER_CUSTOM_DATA)
Texture2D GBuffer3 : register(t4);
#endif

// GBuffer Layout:
// GBuffer0 = [RGB] Color, [A] AO 
// GBuffer1 = [RGB] Normal, [A] ShadingModel 
// GBuffer2 = [R] Roughness, [G] Metalness, [B] Specular, [A] UNUSED
// GBuffer3 = [RGBA] Custom Data (per shading mode)

#endif

// Linearize raw device depth
float LinearizeZ(GBufferData gBuffer, float depth)
{
    return gBuffer.ViewInfo.w / (depth - gBuffer.ViewInfo.z);
}

// Convert linear depth to device depth
float LinearZ2DeviceDepth(GBufferData gBuffer, float linearDepth)
{
    return (gBuffer.ViewInfo.w / linearDepth) + gBuffer.ViewInfo.z;
}

// Get view space position at given pixel coordinate with given device depth
float3 GetViewPos(GBufferData gBuffer, float2 uv, float deviceDepth)
{
    float4 clipPos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), deviceDepth, 1.0);
    float4 viewPos = mul(clipPos, gBuffer.InvProjectionMatrix);
    return viewPos.xyz / viewPos.w;
}

// Get world space position at given pixel coordinate with given device depth
float3 GetWorldPos(GBufferData gBuffer, float2 uv, float deviceDepth)
{
    float3 viewPos = GetViewPos(gBuffer, uv, deviceDepth);
    return mul(float4(viewPos, 1), gBuffer.InvViewMatrix).xyz;
}

#if !defined(NO_GBUFFER_SAMPLING)

// Sample raw device depth buffer
float SampleZ(float2 uv)
{
    return SAMPLE_RT(Depth, uv).r;
}

// Sample linear depth
float SampleDepth(GBufferData gBuffer, float2 uv)
{
    float deviceDepth = SampleZ(uv);
    return LinearizeZ(gBuffer, deviceDepth);
}

// Get view space position at given pixel coordinate
float3 GetViewPos(GBufferData gBuffer, float2 uv)
{
    float deviceDepth = SampleZ(uv);
    return GetViewPos(gBuffer, uv, deviceDepth);
}

// Get world space position at given pixel coordinate
float3 GetWorldPos(GBufferData gBuffer, float2 uv)
{
    float deviceDepth = SampleZ(uv);
    return GetWorldPos(gBuffer, uv, deviceDepth);
}

// Sample normal vector with pixel shading model
float3 SampleNormal(float2 uv, out int shadingModel)
{
    // Sample GBuffer
    float4 gBuffer1 = SAMPLE_RT(GBuffer1, uv);

    // Decode normal and shading model
    shadingModel = (int)(gBuffer1.a * 4.999);
    return DecodeNormal(gBuffer1.rgb);
}

// Sample GBuffer
GBufferSample SampleGBuffer(GBufferData gBuffer, float2 uv)
{
    GBufferSample result;

    // Sample GBuffer
    float4 gBuffer0 = SAMPLE_RT(GBuffer0, uv);
    float4 gBuffer1 = SAMPLE_RT(GBuffer1, uv);
    float4 gBuffer2 = SAMPLE_RT(GBuffer2, uv);
#if defined(USE_GBUFFER_CUSTOM_DATA)
	float4 gBuffer3 = SAMPLE_RT(GBuffer3, uv);
#endif

    // Decode normal and shading model
    result.Normal = DecodeNormal(gBuffer1.rgb);
    result.ShadingModel = (int)(gBuffer1.a * 4.999);

    // Decode GBuffer data first
    result.Color = gBuffer0.rgb;
    result.AO = gBuffer0.a;
    result.Roughness = gBuffer2.r;
    result.Metalness = gBuffer2.g;
    result.Specular = gBuffer2.b;
    result.WeaponMask = gBuffer2.a;
#if defined(USE_GBUFFER_CUSTOM_DATA)
	result.CustomData = gBuffer3;
#endif

    // Calculate view space and world space positions
    float rawDepth = SampleZ(uv);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    if (result.ShadingModel == SHADING_MODEL_WEAPON)
    {
        // Weapon pixels: world position from GBuffer3, undo depth remapping (0.01x) for view position
        result.WorldPos = gBuffer3.rgb + gBuffer.ViewPos;
        result.ViewPos = GetViewPos(gBuffer, uv, rawDepth * 100.0);
    }
    else
    {
        result.ViewPos = GetViewPos(gBuffer, uv, rawDepth);
        result.WorldPos = mul(float4(result.ViewPos, 1), gBuffer.InvViewMatrix).xyz;
    }
#else
    result.ViewPos = GetViewPos(gBuffer, uv, rawDepth);
    result.WorldPos = mul(float4(result.ViewPos, 1), gBuffer.InvViewMatrix).xyz;
#endif

    return result;
}

// Sample GBuffer (fast - only few parameters are being sampled)
GBufferSample SampleGBufferFast(GBufferData gBuffer, float2 uv)
{
    GBufferSample result;

    // Sample GBuffer
    float4 gBuffer1 = SAMPLE_RT(GBuffer1, uv);

    // Decode normal and shading model
    result.Normal = DecodeNormal(gBuffer1.rgb);
    result.ShadingModel = (int)(gBuffer1.a * 4.999);

    // Calculate view space and world space positions
    float rawDepth = SampleZ(uv);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    if (result.ShadingModel == SHADING_MODEL_WEAPON)
    {
        // Weapon pixels: world position from GBuffer3, undo depth remapping (0.01x) for view position
        float4 gBuffer3 = SAMPLE_RT(GBuffer3, uv);
        result.WorldPos = gBuffer3.rgb + gBuffer.ViewPos;
        result.ViewPos = GetViewPos(gBuffer, uv, rawDepth * 100.0);
    }
    else
    {
        result.ViewPos = GetViewPos(gBuffer, uv, rawDepth);
        result.WorldPos = mul(float4(result.ViewPos, 1), gBuffer.InvViewMatrix).xyz;
    }
#else
    result.ViewPos = GetViewPos(gBuffer, uv, rawDepth);
    result.WorldPos = mul(float4(result.ViewPos, 1), gBuffer.InvViewMatrix).xyz;
#endif

    return result;
}

#if defined(USE_GBUFFER_CUSTOM_DATA)

// Sample GBuffer custom data only
float4 SampleGBufferCustomData(float2 uv)
{
	return SAMPLE_RT(GBuffer3, uv);
}

#endif

#endif

#endif
