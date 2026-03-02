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
float DistortionScrollSpeed;
float2 Padding0;
float4x4 ViewProjection;
float4x4 InvViewProjection;
META_CB_END

Texture2D CloudColor : register(t0);
Texture2D CloudDepth : register(t1);
Texture2D SceneDepth : register(t2);
TextureCube DistortionCube : register(t3);

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

float CloudGaussian(float x, float sigma)
{
    float s = max(sigma, 0.001f);
    return exp(-0.5f * (x * x) / (s * s));
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
    GBufferData gBufferData = GetGBufferData();
    const float viewFar = max(GBuffer.ViewFar, 1.0f);

    // UV distortion: offset cloud sampling UVs using world-space cubemap noise
    float2 cloudUV = input.TexCoord;
    if (DistortionStrength > 0.0001f)
    {
        // Use undistorted depth to reconstruct world position for cubemap lookup
        float preDistortDepth = SAMPLE_RT(CloudDepth, input.TexCoord).r;
        if (preDistortDepth < (DepthRange.y - 1.0f))
        {
            float depthRaw = LinearZ2DeviceDepth(gBufferData, preDistortDepth / viewFar);
            float3 worldPos = GetWorldPos(gBufferData, input.TexCoord, depthRaw);
            // Use world-space direction from origin, not camera — noise stays pinned
            // to the cloud's world position regardless of camera translation.
            // Time offset provides subtle drift so clouds feel alive.
            float3 dir = normalize(worldPos);
            float angle = Time * DistortionScrollSpeed;
            float s, c;
            sincos(angle, s, c);
            float3 rotDir = float3(dir.x * c - dir.z * s, dir.y, dir.x * s + dir.z * c);
            float2 noise = DistortionCube.SampleLevel(SamplerLinearWrap, rotDir, 0).rg * 2.0f - 1.0f;
            cloudUV = saturate(cloudUV + noise * DistortionStrength * TexelSize * 8.0f);
        }
    }

    float4 cloud = SAMPLE_RT_LINEAR(CloudColor, cloudUV);
    if (cloud.a <= 0.0001f)
        return 0;

    float cloudLinearDepth = SAMPLE_RT(CloudDepth, cloudUV).r;
    if (cloudLinearDepth >= (DepthRange.y - 1.0f))
        return 0;
    float sceneDepthRaw = SAMPLE_RT(SceneDepth, input.TexCoord).r;
    float sceneLinearDepthCenter = LinearizeZ(gBufferData, sceneDepthRaw) * viewFar;

    // Cheap 5-tap scene depth blur for fluffy soft intersections.
    float2 sceneTexel = gBufferData.ScreenSize.zw;
    float sceneLinearDepthSoft = sceneLinearDepthCenter;
    sceneLinearDepthSoft += LinearizeZ(gBufferData, SAMPLE_RT(SceneDepth, saturate(input.TexCoord + float2(sceneTexel.x, 0))).r) * viewFar;
    sceneLinearDepthSoft += LinearizeZ(gBufferData, SAMPLE_RT(SceneDepth, saturate(input.TexCoord + float2(-sceneTexel.x, 0))).r) * viewFar;
    sceneLinearDepthSoft += LinearizeZ(gBufferData, SAMPLE_RT(SceneDepth, saturate(input.TexCoord + float2(0, sceneTexel.y))).r) * viewFar;
    sceneLinearDepthSoft += LinearizeZ(gBufferData, SAMPLE_RT(SceneDepth, saturate(input.TexCoord + float2(0, -sceneTexel.y))).r) * viewFar;
    sceneLinearDepthSoft *= 0.2f;

    // Hard reject only when cloud is significantly behind geometry.
    float softRejectDistance = max(SoftIntersectionDistance * 2.0f, 1.0f);
    if (cloudLinearDepth >= (sceneLinearDepthCenter + softRejectDistance))
        return 0;

    float alpha = saturate((cloud.a - AlphaThreshold) / max(1.0f - AlphaThreshold, 1e-4f));
    // Fade cloud near geometry intersections to avoid hard clipping seams.
    float intersectionFade = saturate((sceneLinearDepthSoft - cloudLinearDepth) / max(SoftIntersectionDistance, 1.0f));
    alpha *= intersectionFade;

    // Optional distance-based alpha sharpening.
    float distanceApprox = cloudLinearDepth;
    float sharpen = StylizedCloudSafeInvLerp(DistanceSharpenStart, DistanceSharpenEnd, distanceApprox);
    alpha = saturate(alpha + sharpen * 0.15f);

    return float4(cloud.rgb, alpha);
}
