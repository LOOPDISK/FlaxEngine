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
int DistortionMode;
float NoiseScale;
float4x4 ViewProjection;
float4x4 InvViewProjection;
META_CB_END

Texture2D CloudColor : register(t0);
Texture2D CloudDepth : register(t1);
Texture2D SceneDepth : register(t2);
TextureCube DistortionCube : register(t3);
Texture2D CloudOrigin : register(t4);
Texture2D CloudNormal : register(t5);

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

float CloudHash(float3 p)
{
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float CloudNoise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(
        lerp(lerp(CloudHash(i + float3(0,0,0)), CloudHash(i + float3(1,0,0)), f.x),
             lerp(CloudHash(i + float3(0,1,0)), CloudHash(i + float3(1,1,0)), f.x), f.y),
        lerp(lerp(CloudHash(i + float3(0,0,1)), CloudHash(i + float3(1,0,1)), f.x),
             lerp(CloudHash(i + float3(0,1,1)), CloudHash(i + float3(1,1,1)), f.x), f.y),
        f.z);
}

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

    // Scale step size so 13 taps always span the full gaussian (3*sigma radius)
    float stepScale = max(sigma / 6.0f, 1.0f);

    float4 accum = 0;
    float accumWeight = 0;
    UNROLL
    for (int i = -6; i <= 6; i++)
    {
        float fi = (float)i * stepScale;
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

    // UV distortion: offset cloud sampling UVs using noise
    float2 cloudUV = input.TexCoord;
    if (DistortionStrength > 0.0001f)
    {
        float preDistortDepth = SAMPLE_RT(CloudDepth, input.TexCoord).r;
        if (preDistortDepth < (DepthRange.y - 1.0f))
        {
            float depthRaw = LinearZ2DeviceDepth(gBufferData, preDistortDepth / viewFar);
            float3 worldPos = GetWorldPos(gBufferData, input.TexCoord, depthRaw);
            float depthCompensation = 500.0f / max(preDistortDepth, 1.0f);
            float2 noise;
            if (DistortionMode == 4)
            {
                // Procedural3D: 3D value noise from world position, no cubemap needed
                float3 scroll = float3(Time * DistortionScrollSpeed, 0, Time * DistortionScrollSpeed * 0.7f);
                float3 noisePos = worldPos * NoiseScale + scroll;
                noise = float2(
                    CloudNoise3D(noisePos) * 2.0f - 1.0f,
                    CloudNoise3D(noisePos + 137.531f) * 2.0f - 1.0f
                );
            }
            else
            {
                // Cubemap-based modes
                float3 dir;
                if (DistortionMode == 3)
                {
                    float3 viewDir = normalize(worldPos - gBufferData.ViewPos);
                    float3 n = normalize(SAMPLE_RT(CloudNormal, input.TexCoord).rgb);
                    dir = reflect(viewDir, n);
                }
                else if (DistortionMode == 2)
                {
                    float3 origin = SAMPLE_RT(CloudOrigin, input.TexCoord).rgb;
                    dir = normalize(worldPos - origin);
                }
                else if (DistortionMode == 1)
                {
                    dir = normalize(worldPos);
                }
                else
                {
                    dir = normalize(worldPos - gBufferData.ViewPos);
                }
                float angle = Time * DistortionScrollSpeed;
                float s, c;
                sincos(angle, s, c);
                float3 rotDir = float3(dir.x * c - dir.z * s, dir.y, dir.x * s + dir.z * c);
                noise = DistortionCube.SampleLevel(SamplerLinearWrap, rotDir, 0).rg * 2.0f - 1.0f;
            }
            // Fade distortion by cloud alpha so edges don't offset into empty space
            float edgeAlpha = SAMPLE_RT_LINEAR(CloudColor, input.TexCoord).a;
            cloudUV = saturate(cloudUV + noise * DistortionStrength * TexelSize * 8.0f * depthCompensation * saturate(edgeAlpha * 3.0f));
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

    float alpha = smoothstep(AlphaThreshold, saturate(AlphaThreshold + 0.15f), cloud.a);
    // Fade cloud near geometry intersections to avoid hard clipping seams.
    float intersectionFade = saturate((sceneLinearDepthSoft - cloudLinearDepth) / max(SoftIntersectionDistance, 1.0f));
    alpha *= intersectionFade;

    // Optional distance-based alpha sharpening.
    float distanceApprox = cloudLinearDepth;
    float sharpen = StylizedCloudSafeInvLerp(DistanceSharpenStart, DistanceSharpenEnd, distanceApprox);
    alpha = saturate(alpha + sharpen * 0.15f);

    return float4(cloud.rgb, alpha);
}
