// Copyright (c) Wojciech Figat. All rights reserved.

#define NO_GBUFFER_SAMPLING

#include "./Flax/Common.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/StylizedCloud.hlsl"

META_CB_BEGIN(0, Data)
GBufferData GBuffer;
float2 TexelSize;
float2 OutputSize;
float BlurSigmaBase;
float BlurDepthScale;
float DistortionStrength;
float AlphaThreshold;
float SoftIntersectionDistance;
float3 SunDirection;
float SunIntensity;
float3 SunColor;
float SkyIntensity;
float3 SkyColor;
float Time;
float2 DepthRange;
float DistanceSharpenStart;
float DistanceSharpenEnd;
float3 Padding0;
float4x4 ViewProjection;
float4x4 InvViewProjection;
META_CB_END

META_CB_BEGIN(1, PerCloud)
float4x4 WorldMatrix;
float CloudSunIntensity;
float CloudSkyIntensity;
float CloudDistortionScale;
float CloudAlphaThreshold;
float CloudDensity;
float3 CloudLightningColor;
float CloudLightningIntensity;
META_CB_END

Texture2D CloudColor : register(t0);
Texture2D CloudDepth : register(t1);
Texture2D SceneDepth : register(t2);
TextureCube DistortionCube : register(t3);

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

struct Cloud_VS2PS
{
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
};

struct CloudPrePassOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Target1;
};

float CloudGaussian(float x, float sigma)
{
    float s = max(sigma, 0.001f);
    return exp(-0.5f * (x * x) / (s * s));
}

// Pre-pass mesh draw
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT,   0, 0,     PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R16G16_FLOAT,      1, 0,     PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(NORMAL,   0, R10G10B10A2_UNORM, 1, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TANGENT,  0, R10G10B10A2_UNORM, 1, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 1, R16G16_FLOAT,      1, ALIGN, PER_VERTEX, 0, true)
Cloud_VS2PS VS_CloudPrePass(ModelInput input)
{
    Cloud_VS2PS output;
    float4 worldPos = mul(float4(input.Position.xyz, 1), WorldMatrix);
    output.Position = mul(worldPos, ViewProjection);
    output.WorldPos = worldPos.xyz;
    float3 normal = normalize(input.Normal.xyz * 2.0 - 1.0);
    output.WorldNormal = normalize(mul(normal, (float3x3)WorldMatrix));
    return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
CloudPrePassOutput PS_CloudPrePass(Cloud_VS2PS input)
{
    CloudPrePassOutput output;
    float3 n = normalize(input.WorldNormal);
    float3 l = -normalize(SunDirection);
    float NoL = dot(n, l);
    float wrappedNoL = saturate((NoL + 0.25f) / 1.25f);
    float hemi = saturate(n.y * 0.5f + 0.5f);
    float3 v = normalize(GetGBufferData().ViewPos - input.WorldPos);
    float rim = pow(1.0f - saturate(dot(n, v)), 2.0f);
    float3 sun = SunColor * (SunIntensity * CloudSunIntensity);
    float3 sky = SkyColor * (SkyIntensity * CloudSkyIntensity);
    float3 lightColor = sun * (0.1f + 0.9f * wrappedNoL)
                      + sky * (0.15f + 0.85f * hemi);
    lightColor += sun * (rim * 0.2f);
    lightColor += CloudLightningColor * CloudLightningIntensity;
    output.Color = float4(lightColor, max(CloudDensity, 0.0f));
    // Store view-ray depth in world units to match intersection distances.
    output.Depth = LinearizeZ(GetGBufferData(), input.Position.z) * max(GBuffer.ViewFar, 1.0f);
    return output;
}

// Depth-aware separable gaussian blur for cloud color
META_PS(true, FEATURE_LEVEL_ES2)
META_PERMUTATION_1(HORIZONTAL=0)
META_PERMUTATION_1(HORIZONTAL=1)
float4 PS_GaussianBlur(Quad_VS2PS input) : SV_Target0
{
    float centerDepth = SAMPLE_RT(CloudDepth, input.TexCoord).r;
    float depth01 = saturate(centerDepth / max(GBuffer.ViewFar, 1.0f));
    float sigma = max(0.75f, BlurSigmaBase + depth01 * BlurDepthScale);

#if HORIZONTAL
    float2 dir = float2(TexelSize.x, 0);
#else
    float2 dir = float2(0, TexelSize.y);
#endif

    float4 accum = 0;
    float accumWeight = 0;
    UNROLL
    for (int i = -6; i <= 6; i++)
    {
        float fi = (float)i;
        float2 uv = saturate(input.TexCoord + dir * fi);
        float4 c = SAMPLE_RT_LINEAR(CloudColor, uv);
        float d = SAMPLE_RT(CloudDepth, uv).r;
        float depthScale = max(centerDepth * 0.02f, 150.0f);
        float depthWeightRaw = exp(-abs(d - centerDepth) / depthScale);
        float depthIsValid = d < (DepthRange.y - 1.0f) ? 1.0f : 0.0f;
        float depthWeight = lerp(1.0f, depthWeightRaw, depthIsValid);
        float weight = CloudGaussian(fi, sigma) * depthWeight;
        accum += c * weight;
        accumWeight += weight;
    }
    return accum / max(accumWeight, 1e-6f);
}

// Separable box blur for cloud depth
META_PS(true, FEATURE_LEVEL_ES2)
META_PERMUTATION_1(HORIZONTAL=0)
META_PERMUTATION_1(HORIZONTAL=1)
float PS_BoxBlur(Quad_VS2PS input) : SV_Target0
{
#if HORIZONTAL
    float2 dir = float2(TexelSize.x, 0);
#else
    float2 dir = float2(0, TexelSize.y);
#endif

    float sum = 0;
    float wsum = 0;
    UNROLL
    for (int i = -3; i <= 3; i++)
    {
        float2 uv = saturate(input.TexCoord + dir * (float)i);
        float d = SAMPLE_RT(CloudDepth, uv).r;
        float a = SAMPLE_RT_LINEAR(CloudColor, uv).a;
        float w = saturate(a * 2.0f);
        sum += d * w;
        wsum += w;
    }
    if (wsum < 1e-4f)
        return DepthRange.y;
    return sum / wsum;
}

// Fullscreen cloud composition
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Composite(Quad_VS2PS input) : SV_Target0
{
    float4 cloud = SAMPLE_RT_LINEAR(CloudColor, input.TexCoord);
    if (cloud.a <= 0.0001f)
        return 0;
    float cloudAlphaRaw = max(cloud.a, 0.0f);
    float cloudCoverage = saturate(1.0f - exp(-cloudAlphaRaw * 2.0f));
    if (cloudCoverage <= 0.0001f)
        return 0;

    GBufferData gBufferData = GetGBufferData();
    float cloudLinearDepth = SAMPLE_RT(CloudDepth, input.TexCoord).r;
    if (cloudLinearDepth >= (DepthRange.y - 1.0f))
        return 0;
    float sceneDepthRaw = SAMPLE_RT(SceneDepth, input.TexCoord).r;
    float sceneLinearDepth = LinearizeZ(gBufferData, sceneDepthRaw) * max(GBuffer.ViewFar, 1.0f);

    // Reject cloud pixels that are behind opaque scene geometry.
    if (cloudLinearDepth >= sceneLinearDepth)
        return 0;

    float alpha = saturate((cloudCoverage - AlphaThreshold) / max(1.0f - AlphaThreshold, 1e-4f));
    // Fade cloud near geometry intersections to avoid hard clipping seams.
    float intersectionFade = saturate((sceneLinearDepth - cloudLinearDepth) / max(SoftIntersectionDistance, 1.0f));
    alpha *= intersectionFade;

    // Optional distance-based alpha sharpening.
    float distanceApprox = cloudLinearDepth;
    float sharpen = StylizedCloudSafeInvLerp(DistanceSharpenStart, DistanceSharpenEnd, distanceApprox);
    alpha = saturate(alpha + sharpen * 0.15f);

    if (DistortionStrength > 0.0001f)
    {
        float cloudDepthRaw = LinearZ2DeviceDepth(gBufferData, cloudLinearDepth / max(GBuffer.ViewFar, 1.0f));
        float3 worldPos = GetWorldPos(gBufferData, input.TexCoord, cloudDepthRaw);
        float3 dir = normalize(worldPos - GBuffer.ViewPos);
        float2 noise = DistortionCube.SampleLevel(SamplerLinearWrap, dir + float3(Time * 0.01f, 0, 0), 0).rg * 2.0f - 1.0f;
        alpha = saturate(alpha + noise.x * DistortionStrength * 0.15f);
    }

    return float4(cloud.rgb, alpha);
}
