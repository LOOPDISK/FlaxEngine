#include "./Flax/Common.hlsl"

META_CB_BEGIN(0, HZBData)
float4 ViewInfo;
float3 ViewPos;
float ViewFar;
float4x4 InvViewMatrix;
float4x4 InvProjectionMatrix;
float2 Dimensions;
int Level;
int Offset;
//float Dummy0;
META_CB_END

Texture2D DepthTexture : register(t0);
RWTexture2D<float> HZBTexture : register(u1);

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_HZB(Quad_VS2PS input) : SV_Target
{
   float2 uv = input.TexCoord;

   if (Level == 0) // first level just samples depth texture
   {
        float2 texel = 1.0 / (Dimensions * 2);
        float bl = DepthTexture.SampleLevel(SamplerPointClamp, uv, 0).r;
        float tl = DepthTexture.SampleLevel(SamplerPointClamp, uv + float2(0, 1) * texel, 0).r;
        float br = DepthTexture.SampleLevel(SamplerPointClamp, uv + float2(1, 0) * texel, 0).r;
        float tr = DepthTexture.SampleLevel(SamplerPointClamp, uv + float2(1, 1) * texel, 0).r;
        float depth = max(bl, max(tl, max(br, tr)));
        int2 coords = int2((uv.x * Dimensions.x), (uv.y * Dimensions.y)); 
        HZBTexture[coords] = depth;
        return depth;
   }
   else // other levels sample from the previous level
   {
        int prevOffset = Offset - Dimensions.x * 2;
        int2 coordsIn = int2((uv.x * Dimensions.x * 2) + prevOffset, (uv.y * Dimensions.y * 2));
        int2 coordsOut = int2((uv.x * Dimensions.x) + Offset, (uv.y * Dimensions.y)); 

        float bl = HZBTexture[coordsIn + int2(0, 0)];
        float tl = HZBTexture[coordsIn + int2(0, 1)];
        float br = HZBTexture[coordsIn + int2(1, 0)];
        float tr = HZBTexture[coordsIn + int2(1, 1)];
        float depth = max(bl, max(tl, max(br, tr)));

        // odd dimensions need extra pixels
        bool oddX = (int)(Dimensions.x + 1) % 2 == 1;
        bool oddY = (int)(Dimensions.y + 1) % 2 == 1;
        if (oddX)
        {
            float x1 = HZBTexture[coordsIn + int2(2, 0)];
            float x2 = HZBTexture[coordsIn + int2(2, 1)];
            depth = max(depth, max(x1, x2));
        }
        if (oddY)
        {
            float y1 = HZBTexture[coordsIn + int2(0, 2)];
            float y2 = HZBTexture[coordsIn + int2(1, 2)];
            depth = max(depth, max(y1, y2));
            if (oddX)
            {
                float xy = HZBTexture[coordsIn + int2(2, 2)];
                depth = max(depth, xy);
            }
        }

        HZBTexture[coordsOut] = depth;
        return depth;
    }

	return float4(1,0,1,1);
}


// Get view space position at given pixel coordinate with given device depth
float3 GetViewPos(float2 uv, float deviceDepth, float4x4 invProj)
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

// Pixel shader for depth debug view
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DebugView(Quad_VS2PS input) : SV_Target
{
    float2 uv = input.TexCoord;
    float depth = DepthTexture.SampleLevel(SamplerPointClamp, uv, 0).r;
    
    int2 coords = int2((uv.x * Dimensions.x), (uv.y * Dimensions.y));
    float hzbDepth = HZBTexture[coords];
    if (hzbDepth != 0)
    { // draw hzb on top of base depth texture
        depth = hzbDepth;
    }

    float3 viewPos = GetViewPos(uv, depth, InvProjectionMatrix);
	float3 result = viewPos.z / ViewFar;
	return float4(result, 1);
}
