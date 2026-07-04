// Copyright (c) Wojciech Figat. All rights reserved.

// Film Grain post-process shader v1.1	
// Martins Upitis (martinsh) devlog-martinsh.blogspot.com
// 2013
// 
// --------------------------
// This work is licensed under a Creative Commons Attribution 3.0 Unported License.
// So you are free to share, modify and adapt it for your needs, and even use it for commercial use.
// I would also love to hear about a project you are using it.
// 
// Have fun,
// Martins
// --------------------------
// 
// Perlin noise shader by toneburst:
// http://machinesdontcare.wordpress.com/2009/06/25/3d-perlin-noise-sphere-vertex-shader-sourcecode/
// 
// Lens flares by John Chapman:
//https://john-chapman.github.io/2017/11/05/pseudo-lens-flare.html
// 

#include "./Flax/Common.hlsl"
#include "./Flax/Random.hlsl"
#include "./Flax/Noise.hlsl"
#include "./Flax/GammaCorrectionCommon.hlsl"

#define GB_RADIUS 6
#define GB_KERNEL_SIZE (GB_RADIUS * 2 + 1)
#define OUTPUT_LINEAR 0 // Copies scene color directly to the output
#define OUTPUT_SRGB 1 // Converts scene color from linear to sRGB

// Linearize raw device depth
float LinearizeZ(float depth, float4 viewInfo, float viewFar)
{
    return (viewInfo.w * viewFar) / (depth - viewInfo.z);
}

#ifndef NO_GRADING_LUT
#define NO_GRADING_LUT 0
#endif
#ifndef USE_VOLUME_LUT
#define USE_VOLUME_LUT 0
#endif

META_CB_BEGIN(0, Data)

float DepthHazeIntensity;
float DepthHazeNearDistance;
float DepthHazeFarDistance;
float DepthHazePower;

float DepthHazeMaxMipLevel;
float DepthHazeChromaticDispersion;
float DepthHazeMipCount;
float DepthHazePadding0;

float BloomIntensity;
float BloomClamp;
float BloomThreshold;
float BloomThresholdKnee;

float BloomBaseMix;
float BloomHighMix;
float BloomMipCount;
float BloomLayer;               

float3 VignetteColor;
float VignetteShapeFactor;

float2 InputSize;
float InputAspect;
float GrainAmount;

float GrainTime;
float GrainParticleSize;
int Ghosts;
float HaloWidth;

float HaloIntensity;
float Distortion;
float GhostDispersal;
float LensFlareIntensity;

float2 LensInputDistortion;
float LensScale;
float LensBias;

float2 InvInputSize;
float ChromaticDistortion;
float Time;

uint OutputColorSpace;
float PostExposure;
float VignetteIntensity;
float LensDirtIntensity;

float4 ScreenFadeColor;

float3 QuantizationError;
float Dummy2;

float4x4 LensFlareStarMat;

float4 ViewInfo;
float ViewFar;
float DummyPadding1;
float DummyPadding2;
float DummyPadding3;

META_CB_END

META_CB_BEGIN(1, GaussianBlurData)

float2 Size;
float Dummy3;
float Dummy4;
float4 GaussianBlurCache[GB_KERNEL_SIZE]; // x-weight, y-offset

META_CB_END

// Film Grain
static const float permTexUnit = 1.0 / 256.0;      // Perm texture texel-size
static const float permTexUnitHalf = 0.5 / 256.0;  // Half perm texture texel-size

// Input textures
Texture2D Input0 : register(t0);
Texture2D Input1 : register(t1);
Texture2D Input2 : register(t2);
Texture2D Input3 : register(t3);
Texture2D LensDirt : register(t4);
Texture2D LensStar : register(t5);
Texture2D LensColor : register(t6);
Texture2D DepthHaze : register(t8);
Texture2D DepthMips : register(t10);
#if USE_VOLUME_LUT
Texture3D ColorGradingLUT : register(t7);
#else
Texture2D ColorGradingLUT : register(t7);
#endif 
static const float LUTSize = 32;

half3 ColorLookupTable(half3 linearColor)
{
	// Move from linear color to encoded LUT color space
#if COLOR_GRADING_LUT_LOG
	float3 encodedColor = LinearToLog(linearColor + LogToLinear(0)); // Log
#else
	float3 encodedColor = linearColor; // Default
#endif

	float3 uvw = encodedColor * ((LUTSize - 1) / LUTSize) + (0.5f / LUTSize);
#if USE_VOLUME_LUT
	half3 color = ColorGradingLUT.Sample(SamplerLinearClamp, uvw).rgb;
#else
	half3 color = SampleUnwrappedTexture3D(ColorGradingLUT, SamplerLinearClamp, uvw, LUTSize).rgb;
#endif

	return color * COLOR_GRADING_LUT_SCALE;
}

// A random texture generator
float4 rnmRGBA(in float2 tc, in float time) 
{
    float noise =  sin(dot(tc + float2(time, time), float2(12.9898, 78.233))) * 43758.5453;
	float noiseR =  frac(noise) * 2.0 - 1.0;
	float noiseG =  frac(noise * 1.2154) * 2.0 - 1.0; 
	float noiseB =  frac(noise * 1.3453) * 2.0 - 1.0;
	float noiseA =  frac(noise * 1.3647) * 2.0 - 1.0;
	return float4(noiseR, noiseG, noiseB, noiseA);
}

float3 rnmRGB(in float2 tc, in float time) 
{
    float noise =  sin(dot(tc + float2(time, time), float2(12.9898, 78.233))) * 43758.5453;
	float noiseR =  frac(noise) * 2.0 - 1.0;
	float noiseG =  frac(noise * 1.2154) * 2.0 - 1.0; 
	float noiseB =  frac(noise * 1.3453) * 2.0 - 1.0;
	return float3(noiseR, noiseG, noiseB);
}

float2 rnmRG(in float2 tc, in float time) 
{
    float noise =  sin(dot(tc + float2(time, time), float2(12.9898, 78.233))) * 43758.5453;
	float noiseR =  frac(noise) * 2.0 - 1.0;
	float noiseG =  frac(noise * 1.2154) * 2.0 - 1.0;
	return float2(noiseR, noiseG);
}

float rnmA(in float2 tc, in float time) 
{
    float noise =  sin(dot(tc + float2(time, time), float2(12.9898, 78.233))) * 43758.5453;
	float noiseA =  frac(noise * 1.3647) * 2.0 - 1.0;
	return noiseA;
}

float pnoise3D(in float3 p, in float time)
{
	// Integer part, scaled so +1 moves permTexUnit texel
	float3 pi = permTexUnit * floor(p) + permTexUnitHalf;
	// and offset 1/2 texel to sample texel centers. Fractional part for interpolation
	float3 pf = frac(p);

	// Noise contributions from (x=0, y=0), z=0 and z=1
	float perm00 = rnmA(pi.xy, time);
	float3  grad000 = rnmRGB(float2(perm00, pi.z), time) * 4.0 - 1.0;
	float n000 = dot(grad000, pf);
	float3  grad001 = rnmRGB(float2(perm00, pi.z + permTexUnit), time) * 4.0 - 1.0;
	float n001 = dot(grad001, pf - float3(0.0, 0.0, 1.0));

	// Noise contributions from (x=0, y=1), z=0 and z=1
	float perm01 = rnmA(pi.xy + float2(0.0, permTexUnit), time);
	float3  grad010 = rnmRGB(float2(perm01, pi.z), time) * 4.0 - 1.0;
	float n010 = dot(grad010, pf - float3(0.0, 1.0, 0.0));
	float3  grad011 = rnmRGB(float2(perm01, pi.z + permTexUnit), time) * 4.0 - 1.0;
	float n011 = dot(grad011, pf - float3(0.0, 1.0, 1.0));

	// Noise contributions from (x=1, y=0), z=0 and z=1
	float perm10 = rnmA(pi.xy + float2(permTexUnit, 0.0), time);
	float3  grad100 = rnmRGB(float2(perm10, pi.z), time) * 4.0 - 1.0;
	float n100 = dot(grad100, pf - float3(1.0, 0.0, 0.0));
	float3  grad101 = rnmRGB(float2(perm10, pi.z + permTexUnit), time) * 4.0 - 1.0;
	float n101 = dot(grad101, pf - float3(1.0, 0.0, 1.0));

	// Noise contributions from (x=1, y=1), z=0 and z=1
	float perm11 = rnmA(pi.xy + float2(permTexUnit, permTexUnit), time);
	float3  grad110 = rnmRGB(float2(perm11, pi.z), time) * 4.0 - 1.0;
	float n110 = dot(grad110, pf - float3(1.0, 1.0, 0.0));
	float3  grad111 = rnmRGB(float2(perm11, pi.z + permTexUnit), time) * 4.0 - 1.0;
	float n111 = dot(grad111, pf - float3(1.0, 1.0, 1.0));

	// Blend contributions along x
	float4 n_x = lerp(float4(n000, n001, n010, n011), float4(n100, n101, n110, n111), PerlinRamp(pf.x));

	// Blend contributions along y
	float2 n_xy = lerp(n_x.xy, n_x.zw, PerlinRamp(pf.y));

	// Blend contributions along z
	float n_xyz = lerp(n_xy.x, n_xy.y, PerlinRamp(pf.z));

	// We're done, return the final noise value
	return n_xyz;
}

float pnoise2D(in float2 p, in float time)
{
	// Integer part, scaled so +1 moves permTexUnit texel
	float2 pi = permTexUnit * floor(p) + permTexUnitHalf;
	// and offset 1/2 texel to sample texel centers. Fractional part for interpolation
	float2 pf = frac(p);

	// Noise contributions from (x=0, y=0)
	float perm00 = rnmA(pi.xy, time);
	float2 grad000 = rnmRG(float2(perm00, 0), time) * 4.0 - 1.0;
	float n000 = dot(grad000, pf);

	// Noise contributions from (x=0, y=1)
	float perm01 = rnmA(pi.xy + float2(0.0, permTexUnit), time);
	float2 grad010 = rnmRG(float2(perm01, 0), time) * 4.0 - 1.0;
	float n010 = dot(grad010, pf - float2(0.0, 1.0));

	// Noise contributions from (x=1, y=0)
	float perm10 = rnmA(pi.xy + float2(permTexUnit, 0.0), time);
	float2 grad100 = rnmRG(float2(perm10, 0), time) * 4.0 - 1.0;
	float n100 = dot(grad100, pf - float2(1.0, 0.0));

	// Noise contributions from (x=1, y=1)
	float perm11 = rnmA(pi.xy + float2(permTexUnit, permTexUnit), time);
	float2 grad110 = rnmRG(float2(perm11, 0), time) * 4.0 - 1.0;
	float n110 = dot(grad110, pf - float2(1.0, 1.0));

	// Blend contributions along x
	float2 n_x = lerp(float2(n000, n010), float2(n100, n110), PerlinRamp(pf.x));

	// Blend contributions along y
	float n_xy = lerp(n_x.x, n_x.y, PerlinRamp(pf.y));

	// We're done, return the final noise value
	return n_xy;
}

// 2d coordinate orientation thing
float2 coordRot(in float2 tc, in float angle)
{
	float rotX = ((tc.x * 2.0 - 1.0) * InputAspect * cos(angle)) - ((tc.y * 2.0 - 1.0) * sin(angle));
	float rotY = ((tc.y * 2.0 - 1.0) * cos(angle)) + ((tc.x * 2.0 - 1.0) * InputAspect * sin(angle));
	rotX = ((rotX / InputAspect) * 0.5 + 0.5);
	rotY = rotY * 0.5 + 0.5;
	return float2(rotX, rotY);
}

// Uses a lower exposure to produce a value suitable for a bloom pass
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_BloomBrightPass(Quad_VS2PS input) : SV_Target
{
    // Get dimensions for precise texel calculations
    uint width, height;
    Input0.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);
    // Use fixed 13-tap sample pattern for initial bright pass
    float3 color = 0;
    float totalWeight = 0;

    // Center sample with high weight for energy preservation
    float3 center = Input0.Sample(SamplerLinearClamp, input.TexCoord).rgb;

    // Apply Karis average to prevent bright pixels from dominating
    float centerLuma = max(dot(center, float3(0.2126, 0.7152, 0.0722)), 0.0001);
    center = center / (1.0 + centerLuma);

    float centerWeight = 4.0;
    color += center * centerWeight;
    totalWeight += centerWeight;

    // Inner ring - fixed offset at 1.0 texel distance
    UNROLL
    for (int i = 0; i < 4; i++)
    {
        float angle = i * (PI / 2.0);
        float2 offset = float2(cos(angle), sin(angle)) * texelSize;
        float3 sampleColor = Input0.Sample(SamplerLinearClamp, input.TexCoord).rgb;

        // Apply Karis average
        float sampleLuma = max(dot(sampleColor, float3(0.2126, 0.7152, 0.0722)), 0.0001);
        sampleColor = sampleColor / (1.0 + sampleLuma);

        float weight = 2.0;
        color += sampleColor * weight;
        totalWeight += weight;
    }

    // Outer ring - fixed offset at 1.4142 texel distance (diagonal)
    UNROLL
    for (int j = 0; j < 8; j++)
    {
        float angle = j * (PI / 4.0);
        float2 offset = float2(cos(angle), sin(angle)) * texelSize * 1.4142;
        float3 sampleColor = Input0.Sample(SamplerLinearClamp, input.TexCoord + offset).rgb;

        // Apply Karis average
        float sampleLuma = max(dot(sampleColor, float3(0.2126, 0.7152, 0.0722)), 0.0001);
        sampleColor = sampleColor / (1.0 + sampleLuma);

        float weight = 1.0;
        color += sampleColor * weight;
        totalWeight += weight;
    }
    color /= totalWeight;

    // Un-apply Karis average to maintain energy
    float finalLuma = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0.0001);
    color = color * (1.0 + finalLuma);

    // Apply threshold with quadratic rolloff for smoother transition
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float threshold = max(BloomThreshold, 0.2);
    float knee = threshold * BloomThresholdKnee;
    float softMax = threshold + knee;

    float contribution = 0;
    if (luminance > threshold)
    {
        if (luminance < softMax)
        {
            // Quadratic softening between threshold and (threshold + knee)
            float x = (luminance - threshold) / knee;
            contribution = x * x * 0.5;
        }
        else
        {
            // Full contribution above softMax
            contribution = luminance - threshold;
        }
    }

    float testc = BloomClamp;
    float3 clamped = (color * contribution);
    clamped.r = min(clamped.r, testc);
    clamped.g = min(clamped.g, testc);
    clamped.b = min(clamped.b, testc);

    // Store threshold result in alpha for downsample chain
    return float4(clamped, luminance);
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_BloomDownsample(Quad_VS2PS input) : SV_Target
{
    uint width, height;
    Input0.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);

    // 9-tap tent filter with fixed weights
    float3 color = 0;
    float totalWeight = 0;

    // Sample offsets (fixed)
    const float2 offsets[9] =
    {
        float2( 0,  0),    // Center
        float2(-1, -1),    // Corners
        float2( 1, -1),
        float2(-1,  1),
        float2( 1,  1),
        float2( 0, -1),    // Cross
        float2(-1,  0),
        float2( 1,  0),
        float2( 0,  1)
    };

    // Sample weights (fixed)
    const float weights[9] =
    {
        4.0,    // Center
        1.0,    // Corners
        1.0,
        1.0,
        1.0,
        2.0,    // Cross
        2.0,
        2.0,
        2.0
    };

    UNROLL
    for (int i = 0; i < 9; i++)
    {
        float2 offset = offsets[i] * texelSize * 2.0; // Fixed scale factor for stability
        float4 sampleColor = Input0.Sample(SamplerLinearClamp, input.TexCoord + offset);
        color += sampleColor.rgb * weights[i];
        totalWeight += weights[i];
    }

    return float4(color / totalWeight, 1.0);
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_BloomDualFilterUpsample(Quad_VS2PS input) : SV_Target
{
    float anisotropy = 1.0; 
    uint width, height;
    Input0.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);

    // Maintain fixed scale through mip chain
    float baseOffset = 1.0;
    float offsetScale =  (1.0)  * baseOffset;
    float3 color = 0;
    float totalWeight = 0;

    // Center
    float4 center = Input0.Sample(SamplerLinearClamp, input.TexCoord);
    float centerWeight = 4.0;
    color += center.rgb * centerWeight;
    totalWeight += centerWeight;

    // Cross - fixed distance samples
    float2 crossOffsets[4] = {
        float2(offsetScale * anisotropy, 0),  
        float2(-offsetScale * anisotropy, 0), 
        float2(0, offsetScale),
        float2(0, -offsetScale)
    };

    UNROLL
    for (int i = 0; i < 4; i++)
    {
        float4 sampleColor = Input0.Sample(SamplerLinearClamp, input.TexCoord + crossOffsets[i] * texelSize);
        float weight = 2.0;
        color += sampleColor.rgb * weight;
        totalWeight += weight;
    }

    // Corners - fixed distance samples
    float2 cornerOffsets[4] =
    {
        float2(offsetScale * anisotropy, offsetScale), 
        float2(-offsetScale * anisotropy, offsetScale), 
        float2(offsetScale * anisotropy, -offsetScale), 
        float2(-offsetScale * anisotropy, -offsetScale) 
    };

    UNROLL
    for (int j = 0; j < 4; j++)
    {
        float4 sampleColor = Input0.Sample(SamplerLinearClamp, input.TexCoord + cornerOffsets[j] * texelSize);
        float weight = 1.0;
        color += sampleColor.rgb * weight;
        totalWeight += weight;
    }

    color /= totalWeight;

    uint width1, height1;
    Input1.GetDimensions(width1, height1);

    // Calculate mip fade factor (0 = smallest mip, 1 = largest mip)
    float mipFade = BloomLayer / (BloomMipCount - 1);

    // Muzz says: 
    // Lerp between your desired intensity values based on mip level
    // setting both to 0.6 is a decent default, but playing with these numbers will let you dial in the blending between the lowest and highest mips. 
    // you can make some really ugly bloom if you go too far. 
    // note this does change the intensity of the bloom. 
    // This was my own invention

    float mipIntensity = lerp(BloomBaseMix, BloomHighMix, mipFade);
    color *= mipIntensity;


    BRANCH
    if (width1 > 0)
    {
        float3 previousMip = Input1.Sample(SamplerLinearClamp, input.TexCoord).rgb;
        color += previousMip;
    }

    return float4(color, 1.0);
}

// ============================================================================
// Depth haze - premultiplied "marching mask" scheme
//
// The haze chain stores premultiplied data: rgb = sceneColor * participation,
// a = participation. Each mip K only keeps light from surfaces far enough to
// ever be composited at mip K's blurriness, so nearer surfaces can't halo into
// the blur - their contribution is zero and the composite renormalizes by alpha,
// which reconstructs pure background color right up to (and under) silhouettes.
// A second chain stores the average linear depth of the participating surfaces,
// used to evaluate the next mip's mask while downsampling.
// Everything is fixed-weight linear filtering (no data-dependent branches), so
// the result is deterministic and temporally stable without TAA.
// ============================================================================

// The nearest linear depth that ever selects this haze mip at composite time
// (inverse of the depth->mip curve in PS_DepthHazeComposite)
float HazeMipThresholdDepth(float mip)
{
    float t = saturate(mip / max(DepthHazeMipCount - 1.0, 1.0));
    float range = max(DepthHazeFarDistance - DepthHazeNearDistance, 1.0);
    return DepthHazeNearDistance + range * pow(t, 1.0 / max(DepthHazePower, 0.001));
}

// Haze participation of a surface at the given linear depth
float HazeParticipation(float linearDepth, float thresholdDepth)
{
    return smoothstep(thresholdDepth * 0.8, thresholdDepth * 1.2, linearDepth);
}

// Mirror kernel taps that land off-screen back onto interior content. With plain clamp
// addressing, off-screen taps clone the border pixel, so features at the screen edge get
// "blurred" with copies of themselves and stay sharp - visible when the camera rotates.
float2 MirrorScreenUV(float2 uv)
{
    return 1.0 - abs(1.0 - abs(uv));
}

// View-Z -> radial distance factor for this pixel's view ray (ViewInfo.xy = 1/Projection.M11/M22).
// Scattering depends on path length through the air, which is rotation-invariant; raw view-Z
// shrinks by cos(angle) toward the screen edges at high FOV, making off-center objects
// spuriously sharper as the camera rotates.
float HazeRadialScale(float2 uv)
{
    float2 ray = (uv * 2.0 - 1.0) * ViewInfo.xy;
    return sqrt(1.0 + dot(ray, ray));
}

// Initial full-res -> half-res premultiplied copy (MRT: color mip 0 + participating depth mip 0)
// Every tap point-samples matched color and depth so foreground never mixes into the haze
// before it gets masked (bilinear filtering would blend across silhouettes first).
// Input0: full-res scene color, Input1: full-res depth buffer
META_PS(true, FEATURE_LEVEL_ES2)
void PS_DepthHazePremultCopy(Quad_VS2PS input, out float4 outColor : SV_Target0, out float4 outDepth : SV_Target1)
{
    uint width, height;
    Input1.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);

    float4 color = 0;
    float depthSum = 0;
    float depthMass = 0;
    float totalWeight = 0;

    // 4x4 tent of point samples centered on this half-res texel's 2x2 source quad
    UNROLL
    for (int y = -1; y <= 2; y++)
    {
        UNROLL
        for (int x = -1; x <= 2; x++)
        {
            float2 uv = MirrorScreenUV(input.TexCoord + (float2(x, y) - 0.5) * texelSize);
            float wx = (x == -1 || x == 2) ? 0.25 : 0.75;
            float wy = (y == -1 || y == 2) ? 0.25 : 0.75;
            float w = wx * wy;

            float3 c = Input0.SampleLevel(SamplerPointClamp, uv, 0).rgb;
            float d = LinearizeZ(Input1.SampleLevel(SamplerPointClamp, uv, 0).r, ViewInfo, ViewFar) * HazeRadialScale(uv);
            float m = HazeParticipation(d, DepthHazeNearDistance);

            color += float4(c * m, m) * w;
            depthSum += d * (m * w);
            depthMass += m * w;
            totalWeight += w;
        }
    }

    outColor = color / totalWeight;
    outDepth = (depthMass > 0.0001 ? depthSum / depthMass : ViewFar).xxxx;
}

// Marching-mask premultiplied downsample: builds color/depth mip K (K = BloomLayer)
// from mip K-1. Taps are re-weighted by the ratio of this mip's participation to the
// previous mip's, so each mip only keeps light from surfaces at least as far as the
// depth that selects it - closer surfaces get composited sharper and must not bleed here.
// Input0: color mip K-1 (premultiplied), DepthMips: participating linear depth mip K-1 (per-mip view)
META_PS(true, FEATURE_LEVEL_ES2)
void PS_DepthHazeMarchingDownsample(Quad_VS2PS input, out float4 outColor : SV_Target0, out float4 outDepth : SV_Target1)
{
    uint width, height;
    Input0.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);

    float thisThreshold = HazeMipThresholdDepth(BloomLayer);
    float prevThreshold = HazeMipThresholdDepth(BloomLayer - 1.0);

    // 13-tap Jimenez downsample: 5 overlapping 2x2 bilinear quads spanning a 6x6 source
    // footprint. The narrow 9-tap tent this replaces let aliasing energy through at every
    // 2x decimation, compounding across the chain into deep-mip twinkle under sub-texel
    // camera motion.
    const float2 offsets[13] =
    {
        float2(-1, -1),    // Inner quad (half the total weight)
        float2( 1, -1),
        float2(-1,  1),
        float2( 1,  1),
        float2( 0,  0),    // Center (shared by all 4 outer quads)
        float2(-2,  0),    // Edge midpoints (each shared by 2 outer quads)
        float2( 2,  0),
        float2( 0, -2),
        float2( 0,  2),
        float2(-2, -2),    // Corners
        float2( 2, -2),
        float2(-2,  2),
        float2( 2,  2)
    };
    const float spatialWeights[13] =
    {
        0.125,   // Inner quad
        0.125,
        0.125,
        0.125,
        0.125,   // Center
        0.0625,  // Edge midpoints
        0.0625,
        0.0625,
        0.0625,
        0.03125, // Corners
        0.03125,
        0.03125,
        0.03125
    };

    float4 color = 0;
    float depthSum = 0;
    float depthMass = 0;
    float totalWeight = 0;

    UNROLL
    for (int i = 0; i < 13; i++)
    {
        float2 sampleUV = MirrorScreenUV(input.TexCoord + offsets[i] * texelSize);
        float4 c = Input0.Sample(SamplerLinearClamp, sampleUV);
        float d = DepthMips.SampleLevel(SamplerLinearClamp, sampleUV, 0).r;

        // March the mask outward: keep only the participation that survives this mip's threshold
        float ratio = saturate(HazeParticipation(d, thisThreshold) / max(HazeParticipation(d, prevThreshold), 0.001));
        float w = spatialWeights[i] * ratio;

        color += c * w;
        depthSum += d * (w * c.a);
        depthMass += w * c.a;
        totalWeight += spatialWeights[i];
    }

    outColor = color / totalWeight;
    outDepth = (depthMass > 0.0001 ? depthSum / depthMass : ViewFar).xxxx;
}

// Premultiplied dual-filter upsample: pure fixed-weight filtering with no depth taps
// or data-dependent branches. Foreground carries zero alpha so it can't leak in, which
// removes the need for edge rejection (the old sharp-scene fallback was the source of
// the sharp border around silhouettes).
// Input0: blurrier mip K+1 upsampled so far, Input1: downsampled chain at mip K+1.
// Output level K holds blur level ~K+1 at 2x oversampling: critically-sampled content shows
// a bilinear lattice when the composite magnifies it, oversampled content doesn't. The
// composite selects one level lower to compensate for the octave shift.
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DepthHazeDualFilterUpsample(Quad_VS2PS input) : SV_Target
{
    uint width, height;
    Input0.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);

    float4 color = Input0.Sample(SamplerLinearClamp, input.TexCoord) * 4.0;
    float totalWeight = 4.0;

    const float2 offsets[8] =
    {
        float2( 1,  0),    // Cross
        float2(-1,  0),
        float2( 0,  1),
        float2( 0, -1),
        float2( 1,  1),    // Corners
        float2(-1,  1),
        float2( 1, -1),
        float2(-1, -1)
    };
    const float weights[8] = { 2.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0 };

    UNROLL
    for (int i = 0; i < 8; i++)
    {
        color += Input0.Sample(SamplerLinearClamp, MirrorScreenUV(input.TexCoord + offsets[i] * texelSize)) * weights[i];
        totalWeight += weights[i];
    }
    color /= totalWeight;

    // Blend with the downsampled chain (energy-preserving cascade)
    uint width1, height1;
    Input1.GetDimensions(width1, height1);
    BRANCH
    if (width1 > 0)
    {
        float4 previousMip = Input1.Sample(SamplerLinearClamp, input.TexCoord);
        color = lerp(previousMip, color, 0.5);
    }

    return color;
}

// Horizontal gaussian blur
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_GaussainBlurH(Quad_VS2PS input) : SV_Target
{
	float4 color = 0;
	UNROLL
	for (int i = 0; i < GB_KERNEL_SIZE; i++)
	{
		color += Input0.Sample(SamplerLinearClamp, input.TexCoord + float2(GaussianBlurCache[i].y, 0.0)) * GaussianBlurCache[i].x;
	}
	return color;
}

// Vertical gaussian blur
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_GaussainBlurV(Quad_VS2PS input) : SV_Target
{
	float4 color = 0;
	UNROLL
	for (int i = 0; i < GB_KERNEL_SIZE; i++)
	{
		color += Input0.Sample(SamplerLinearClamp, input.TexCoord + float2(0.0, GaussianBlurCache[i].y)) * GaussianBlurCache[i].x;
	}
	return color;
}

// Generate 'ghosts' for lens flare
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Ghosts(Quad_VS2PS input) : SV_Target
{
	// Temporary data
	int i = 0;
	float weight;
	float2 offset;
	float2 haloFrac;
	float3 color;
	float3 result = 0;

	// Flip texcoordoords
	float2 texcoord = input.TexCoord * -1 + float2(1.0, 1.0);

	// Ahost vector to image centre
	float2 ghostVec = (float2(0.5, 0.5) - texcoord) * GhostDispersal;// TODO: optimize to MAD instruction
	float2 ghostVecnNorm = normalize(ghostVec);
	float2 haloVec = ghostVecnNorm * HaloWidth;

	// Calculate distortion vector
	float3 distortion = float3(LensInputDistortion.x, 0.0, LensInputDistortion.y);

	// Sample 'ghosts'
	// TODO: use uniform amount of ghosts and unroll loop
	LOOP
	for(; i < Ghosts; i++)
	{
		// Calculate ghost offset
		offset = frac(texcoord + ghostVec * (float)i);

		// Calculate ghost weight
		weight = pow(1.0 - length(float2(0.5, 0.5) - offset) / length(float2(0.71, 0.6)), 10.0);

		// Sample distored lens downsampled/threshold texture
		color = float3(
			Input3.Sample(SamplerLinearClamp, offset + ghostVecnNorm * distortion.r).r,
			Input3.Sample(SamplerLinearClamp, offset + ghostVecnNorm * distortion.g).g,
			Input3.Sample(SamplerLinearClamp, offset + ghostVecnNorm * distortion.b).b);
		color = clamp(color + LensBias, 0, 10) * (LensScale * weight);

		// Accumulate color
		result += color;
	}

	// Apply lens color
	result *= LensColor.Sample(SamplerLinearWrap, float2(length(float2(0.5, 0.5) - texcoord) / length(float2(0.5, 0.5)), 0)).rgb;

	// Add halo
	haloFrac = frac(texcoord + haloVec);
	weight = length(float2(0.5, 0.5) - haloFrac) / length(float2(0.5, 0.5));
	weight = pow(1.0 - weight, 5.0) * HaloIntensity;
	color = float3(
			Input3.Sample(SamplerLinearClamp, haloFrac + ghostVecnNorm * distortion.r).r,
			Input3.Sample(SamplerLinearClamp, haloFrac + ghostVecnNorm * distortion.g).g,
			Input3.Sample(SamplerLinearClamp, haloFrac + ghostVecnNorm * distortion.b).b);
	result += clamp((color + LensBias) * (LensScale * weight), 0, 8);

	return float4(result, 1);
}

float remap(float t, float a, float b)
{
	return clamp((t - a) / (b - a), 0.0, 1.0);
}

float2 remap(float2 t, float2 a, float2 b)
{
	return clamp((t - a) / (b - a), 0.0, 1.0);
}

float2 radialdistort(float2 coord, float2 amt)
{
	float2 cc = coord - 0.5;
	return coord + 2.0 * cc * amt;
}

float2 distort(float2 uv, float t, float2 min_distort, float2 max_distort)
{
    float2 dist = lerp(min_distort, max_distort, t);
    float2 cc = uv - 0.5;
	return uv + 4.0 * cc * dist;
}

float3 spectrum_offset(float t)
{
    float t0 = 3.0 * t - 1.5;
	return clamp(float3( -t0, 1.0 - abs(t0), t0), 0.0, 1.0);
}

float nrand(float2 n)
{
	return frac(sin(dot(n.xy, float2(12.9898, 78.233)))* 43758.5453);
}

// OFFICIAL FLAXENGINE VERSION
//// Applies exposure, color grading and tone mapping to the input.
//// Combines it with the results of the bloom pass and other postFx.
//META_PS(true, FEATURE_LEVEL_ES2)
//META_PERMUTATION_1(NO_GRADING_LUT=1)
//META_PERMUTATION_1(USE_VOLUME_LUT=1)
//META_PERMUTATION_1(USE_VOLUME_LUT=0)
//float4 PS_Composite(Quad_VS2PS input) : SV_Target
//{
//	float2 uv = input.TexCoord;
//	float3 lensLight = 0;
//	float4 color;
//
//	// Chromatic Abberation
//    BRANCH
//	if (ChromaticDistortion > 0)
//	{
//		const float MAX_DIST_PX = 24.0;
//		float max_distort_px = MAX_DIST_PX * ChromaticDistortion;
//		float2 max_distort = InvInputSize * max_distort_px;
//		float2 min_distort = 0.5 * max_distort;
//
//		float2 oversiz = distort(float2(1.0, 1.0), 1.0, min_distort, max_distort);
//		uv = remap(uv, 1.0 - oversiz, oversiz);
//
//		int iterations = (int)lerp(3, 10, ChromaticDistortion);
//		float stepsiz = 1.0 / (float(iterations) - 1.0);
//		float rnd = nrand(uv + Time);
//		float t = rnd * stepsiz;
//
//		float4 sumcol = 0;
//		float4 sumw = 0;
//		for (int i = 0; i < iterations; i++)
//		{
//			float4 w = float4(spectrum_offset(t), 1);
//			sumw += w;
//			float2 uvd = distort(uv, t, min_distort, max_distort);
//			sumcol += Input0.Sample(SamplerLinearClamp, uvd) * w;
//			t += stepsiz;
//		}
//		sumcol /= sumw;
//		color = sumcol + (rnd / 255.0);
//	}
//	else
//	{
//		color = Input0.Sample(SamplerPointClamp, uv);
//	}
//
//	// Lens Flares
//	BRANCH
//	if (LensFlareIntensity > 0)
//	{
//		// Get lens flare color
//		float3 lensFlares = Input3.Sample(SamplerLinearClamp, uv).rgb * LensFlareIntensity;
//
//		// Get lens star color and mix it with lens flares
//		float2 lensStarTexcoord = uv - 0.5;
//		lensStarTexcoord = mul(lensStarTexcoord, (float2x2)LensFlareStarMat).xy;
//		lensStarTexcoord += 0.5;
//		float3 lensStar = LensStar.Sample(SamplerLinearClamp, lensStarTexcoord).rgb;
//		lensFlares *= lensStar * 2 + 0.5;
//
//		// Accumulate final lens flares lght
//		lensLight += lensFlares * 1.5f;
//		color.rgb += lensFlares;
//	}
//
//    // Bloom
//    BRANCH
//    if (BloomIntensity > 0)
//    {
//        // Sample the final bloom result
//        float3 bloom = Input2.Sample(SamplerLinearClamp, input.TexCoord).rgb;
//        bloom = bloom * BloomIntensity;
//        lensLight += max(0, bloom * 3.0f + (-1.0f * 3.0f));
//        color.rgb += bloom;
//    }
//
//	// Lens Dirt
//	float3 lensDirt = LensDirt.SampleLevel(SamplerLinearClamp, uv, 0).rgb;
//	color.rgb += lensDirt * (lensLight * LensDirtIntensity);
//
//
//#if !NO_GRADING_LUT
//	color.rgb = ColorLookupTable(color.rgb);
//#endif
//
//	// Film Grain
//	BRANCH
//	if (GrainAmount > 0)
//	{
//		// Calculate noise
//		float2 rotCoordsR = coordRot(uv, GrainTime);
//		float noise = pnoise2D(rotCoordsR * (InputSize / GrainParticleSize), GrainTime);
//
//		// Noisiness response curve based on scene luminance
//		float luminance = Luminance(saturate(color.rgb));
//		luminance += smoothstep(0.2, 0.0, luminance);
//
//		// Add noise to the final color
//		noise = lerp(noise, 0, min(pow(luminance, 4.0), 100));
//		color.rgb += noise * GrainAmount;
//	}
//
//	// Vignette
//	BRANCH
//	if (VignetteIntensity > 0)
//	{
//		float2 uvCircle = uv * (1 - uv);
//		float uvCircleScale = uvCircle.x * uvCircle.y * 16.0f;
//		float mask = lerp(1, pow(uvCircleScale, VignetteShapeFactor), VignetteIntensity);
//		color.rgb = lerp(VignetteColor, color.rgb, mask);
//	}
//
//	// Screen fade
//	color.rgb = lerp(color.rgb, ScreenFadeColor.rgb, ScreenFadeColor.a);
//
//	// Saturate color since it will be rendered to the screen
//	color.rgb = saturate(color.rgb);
//
//	// Return final pixel color (preserve input alpha)
//	return color;
//}


// Applies exposure, color grading and tone mapping to the input.
// Combines it with the results of the bloom pass and other postFx.
META_PS(true, FEATURE_LEVEL_ES2)
META_PERMUTATION_1(NO_GRADING_LUT=1)
META_PERMUTATION_1(USE_VOLUME_LUT=1)
META_PERMUTATION_1(USE_VOLUME_LUT=0)
float4 PS_Composite(Quad_VS2PS input) : SV_Target
{
    float2 screenPos = input.TexCoord;

    // DEBUG STRIP: Top-middle area for debug visualization (20% width, top 15% height)
    float debugWidth = 0.2;
    float debugHeight = 0.15;
    float debugCenterX = 0.5;

    bool inDebugArea = (screenPos.y < debugHeight) &&
                      (screenPos.x > (debugCenterX - debugWidth * 0.5)) &&
                      (screenPos.x < (debugCenterX + debugWidth * 0.5));

    /*
    if (inDebugArea)
    {
        // Map screen position to debug area local coordinates
        float2 debugLocalPos = float2(
            (screenPos.x - (debugCenterX - debugWidth * 0.5)) / debugWidth,
            screenPos.y / debugHeight
        );

        float mipCount = DepthHazeMipCount;
        float mipHeight = 1.0 / mipCount;

        // Left half: Color mips, Right half: Depth mips
        if (debugLocalPos.x < 0.5)
        {
            // Color mips (left half of debug area)
            int mipLevel = (int)(debugLocalPos.y / mipHeight);
            mipLevel = min(mipLevel, (int)mipCount - 1);
            float2 mipUV = float2(debugLocalPos.x * 2.0, fmod(debugLocalPos.y, mipHeight) / mipHeight);
            float3 mipSample = DepthHaze.SampleLevel(SamplerLinearClamp, mipUV, (float)mipLevel).rgb;
            return float4(mipSample, 1.0);
        }
        else
        {
            // Depth mips (right half of debug area)
            int mipLevel = (int)(debugLocalPos.y / mipHeight);
            mipLevel = min(mipLevel, (int)mipCount - 1);
            float2 mipUV = float2((debugLocalPos.x - 0.5) * 2.0, fmod(debugLocalPos.y, mipHeight) / mipHeight);
            float depthValue = DepthMips.SampleLevel(SamplerLinearClamp, mipUV, (float)mipLevel).r;
            float linearDepth = LinearizeZ(depthValue, ViewInfo, ViewFar);
            float depthVisualized = saturate((linearDepth - DepthHazeNearDistance) / (DepthHazeFarDistance - DepthHazeNearDistance));
            return float4(depthVisualized, depthVisualized, depthVisualized, 1.0);
        }
    }
    */

    // Focal Plane Mask Visualization (commented out - use for advanced debugging)
    /*
    // 1. Define alternating colors for visualization
    float3 colors[8];
    colors[0] = float3(1, 0, 0); // Red
    colors[1] = float3(0, 1, 0); // Green
    colors[2] = float3(0, 0, 1); // Blue
    colors[3] = float3(1, 1, 0); // Yellow
    colors[4] = float3(0, 1, 1); // Cyan
    colors[5] = float3(1, 0, 1); // Magenta
    colors[6] = float3(1, 0.5, 0); // Orange
    colors[7] = float3(0.5, 0.2, 1); // Purple

    // 2. Define depth area boundaries
    int numAreas = (int)DepthHazeMipCount;
    float sliceWidth = (DepthHazeFarDistance - DepthHazeNearDistance) / numAreas;
    float transitionWidth = sliceWidth * 0.5; // Transition width is half a slice

    float3 finalColor = float3(0, 0, 0);
    float totalMask = 0;

    // 3. Calculate and blend masks
    for (int i = 0; i < numAreas; i++)
    {
        // A. Get the blurred depth from the CORRECT mip for this area
        float blurredDepthValue = DepthMips.SampleLevel(SamplerLinearClamp, screenPos, i).r;
        float linearDepth = LinearizeZ(blurredDepthValue, ViewInfo, ViewFar);

        // B. Define the boundaries for this specific area
        float areaStart = DepthHazeNearDistance + i * sliceWidth;
        float areaEnd = areaStart + sliceWidth;

        // C. Calculate the mask for this area (a smooth "box" filter)
        float mask = smoothstep(areaStart - transitionWidth, areaStart + transitionWidth, linearDepth) - smoothstep(areaEnd - transitionWidth, areaEnd + transitionWidth, linearDepth);

        // D. Add this area's color to the final output, weighted by its mask
        finalColor += mask * colors[i % 8];
        totalMask += mask;
    }

    // (Optional but good practice) Normalize the color in case masks overlap/underlap slightly
    if (totalMask > 0)
    {
        finalColor /= totalMask;
    }

    return float4(finalColor, 1.0);
    */

    // ====================================================================
    // ACTUAL SCENE COMPOSITION (not debug visualization)
    // ====================================================================

    // Sample base scene color (Input0)
    float3 sceneColor = Input0.Sample(SamplerLinearClamp, input.TexCoord).rgb;

    // Apply post-exposure
    sceneColor *= PostExposure;

    // NOTE: Depth haze is composited in the standalone PS_DepthHazeComposite pass
    // (right after the forward pass), not here - DepthHazeIntensity is always 0 in this pass

    // Add bloom effect (Input2)
    if (BloomIntensity > 0.0)
    {
        float3 bloomContribution = Input2.Sample(SamplerLinearClamp, input.TexCoord).rgb;
        sceneColor += bloomContribution * BloomIntensity;
    }

    // Add lens flares (Input3)
    if (LensFlareIntensity > 0.0)
    {
        float3 lensFlareContribution = Input3.Sample(SamplerLinearClamp, input.TexCoord).rgb;
        sceneColor += lensFlareContribution * LensFlareIntensity;
    }


#if !NO_GRADING_LUT
    // Apply color grading and tone mapping
    sceneColor = ColorLookupTable(sceneColor);
#endif

    if (OutputColorSpace == OUTPUT_SRGB)
    {
        // Convert scene color from linear into output display color space (sRGB)
        sceneColor = LinearToSrgb(sceneColor);
    }

    // Apply camera artifacts (vignette, grain, chromatic aberration)
    if (VignetteIntensity > 0.0)
    {
        float2 d = abs(input.TexCoord - 0.5) * VignetteShapeFactor;
        float vignette = pow(saturate(1.0 - dot(d, d)), VignetteIntensity);
        sceneColor *= lerp(VignetteColor, float3(1, 1, 1), vignette);
    }

    if (GrainAmount > 0.0)
    {
        float grain = pnoise3D(float3(input.TexCoord * InputSize / GrainParticleSize, 0), GrainTime) * GrainAmount;
        sceneColor += grain;
    }

    if (ChromaticDistortion > 0.0)
    {
        float2 coords = (input.TexCoord - 0.5) * 2.0;
        float2 end = input.TexCoord - coords * dot(coords, coords) * ChromaticDistortion;
        float2 delta = (end - input.TexCoord) / 3.0;

        sceneColor.r = Input0.Sample(SamplerLinearClamp, input.TexCoord).r;
        sceneColor.g = Input0.Sample(SamplerLinearClamp, input.TexCoord + delta).g;
        sceneColor.b = Input0.Sample(SamplerLinearClamp, input.TexCoord + delta * 2.0).b;
    }

    // Apply screen fade
    sceneColor = lerp(sceneColor, ScreenFadeColor.rgb, ScreenFadeColor.a);

    // Saturate color since it will be rendered to the screen
    sceneColor = saturate(sceneColor);

    // Apply quantization error to reduce banding artifacts due to R11G11B10 format
    float noise = rand2dTo1d(input.TexCoord);
    sceneColor = QuantizeColor(sceneColor, noise, QuantizationError);

    return float4(sceneColor, 1.0);
}

// Cubic B-spline sampling of one chain level using 4 bilinear fetches (weighted-offset trick).
// Bilinear magnification of coarse mips shows its C1 lattice as blocky patches that crawl when
// the camera moves (the mip grid is screen-fixed while content slides through it); B-spline
// reconstruction is C2-smooth, so the blur translates smoothly instead.
float4 SampleHazeBicubicLevel(float2 uv, float level)
{
    uint w, h, levels;
    DepthHaze.GetDimensions((uint)level, w, h, levels);
    float2 size = float2(w, h);
    float2 invSize = 1.0 / size;

    float2 tc = uv * size - 0.5;
    float2 f = frac(tc);
    float2 b = floor(tc) + 0.5; // Base texel center

    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 / 6.0) * (-f3 + 3.0 * f2 - 3.0 * f + 1.0);
    float2 w1 = (1.0 / 6.0) * (3.0 * f3 - 6.0 * f2 + 4.0);
    float2 w2 = (1.0 / 6.0) * (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0);
    float2 w3 = (1.0 / 6.0) * f3;

    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    float2 t0 = (b - 1.0 + w1 / g0) * invSize;
    float2 t1 = (b + 1.0 + w3 / g1) * invSize;

    return DepthHaze.SampleLevel(SamplerLinearClamp, MirrorScreenUV(float2(t0.x, t0.y)), level) * (g0.x * g0.y)
         + DepthHaze.SampleLevel(SamplerLinearClamp, MirrorScreenUV(float2(t1.x, t0.y)), level) * (g1.x * g0.y)
         + DepthHaze.SampleLevel(SamplerLinearClamp, MirrorScreenUV(float2(t0.x, t1.y)), level) * (g0.x * g1.y)
         + DepthHaze.SampleLevel(SamplerLinearClamp, MirrorScreenUV(float2(t1.x, t1.y)), level) * (g1.x * g1.y);
}

// Bicubic fetch at a fractional mip (manual trilinear between two B-spline-filtered levels)
float4 SampleHazeSmooth(float2 uv, float mip)
{
    float level = floor(mip);
    float t = mip - level;
    float4 s = SampleHazeBicubicLevel(uv, level);
    BRANCH
    if (t > 0.001)
        s = lerp(s, SampleHazeBicubicLevel(uv, level + 1.0), t);
    return s;
}

// Sample the premultiplied haze chain at the given mip. Where support is thin (a far surface
// seen through a small gap between near occluders), fall back toward a lower mip which has
// denser local support.
float4 SampleHazePremult(float2 uv, float mip)
{
    const float ALPHA_FLOOR = 0.05;
    float4 s = SampleHazeSmooth(uv, mip);
    BRANCH
    if (s.a < ALPHA_FLOOR)
    {
        float4 lower = SampleHazeSmooth(uv, mip * 0.5);
        s = lerp(lower, s, saturate(s.a / ALPHA_FLOOR));
    }
    return s;
}

// Standalone depth haze composition pass (applied after forward pass, before AA/UI)
// Input0: scene color, DepthHaze: premultiplied haze mip chain, DepthMips: full-res depth buffer
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DepthHazeComposite(Quad_VS2PS input) : SV_Target
{
    float3 sceneColor = Input0.Sample(SamplerLinearClamp, input.TexCoord).rgb;

    BRANCH
    if (DepthHazeIntensity > 0.0)
    {
        // Full-res depth drives both the mask and the mip selection - pixel-exact at
        // silhouettes, and it crawls in lockstep with the scene's own geometric edges.
        // Radial distance (not view-Z) so the haze is invariant under camera rotation.
        float deviceDepth = DepthMips.SampleLevel(SamplerPointClamp, input.TexCoord, 0).r;
        float pixelDepth = LinearizeZ(deviceDepth, ViewInfo, ViewFar) * HazeRadialScale(input.TexCoord);

        float depthRange = DepthHazeFarDistance - DepthHazeNearDistance;
        float normalizedDepth = saturate((pixelDepth - DepthHazeNearDistance) / max(depthRange, 1.0));

        // Apply power curve for artistic control
        float targetBlurLevel = pow(normalizedDepth, DepthHazePower) * (DepthHazeMipCount - 1);
        targetBlurLevel = min(targetBlurLevel, min(DepthHazeMaxMipLevel, DepthHazeMipCount - 1));

        // Chain level K holds blur level ~K+1 at 2x oversampling (see the upsample cascade), so
        // select one level lower: magnifying critically-sampled mips would show their bilinear
        // lattice as blocky aliasing that crawls under camera motion
        float targetMipFloat = max(targetBlurLevel - 1.0, 0.0);
        float effectiveMaxMip = max(DepthHazeMipCount - 2.0, 0.0); // Top chain level written by the upsample loop
        targetMipFloat = min(targetMipFloat, effectiveMaxMip);

        float3 scatteredColor;
        BRANCH
        if (DepthHazeChromaticDispersion > 0.001)
        {
            // Wavelength-dependent chromatic dispersion for Mie scattering:
            // red light scatters less (sharper), blue light scatters more (blurrier)
            float redMipLevel = clamp(targetMipFloat - DepthHazeChromaticDispersion * 0.5, 0.0, effectiveMaxMip);
            float blueMipLevel = clamp(targetMipFloat + DepthHazeChromaticDispersion, 0.0, effectiveMaxMip);

            float4 r = SampleHazePremult(input.TexCoord, redMipLevel);
            float4 g = SampleHazePremult(input.TexCoord, targetMipFloat);
            float4 b = SampleHazePremult(input.TexCoord, blueMipLevel);
            scatteredColor = float3(r.r / max(r.a, 0.001), g.g / max(g.a, 0.001), b.b / max(b.a, 0.001));
        }
        else
        {
            float4 s = SampleHazePremult(input.TexCoord, targetMipFloat);
            scatteredColor = s.rgb / max(s.a, 0.001);
        }

        float scatteringMask = HazeParticipation(pixelDepth, DepthHazeNearDistance);
        sceneColor = lerp(sceneColor, scatteredColor, scatteringMask * DepthHazeIntensity);
    }

    return float4(sceneColor, 1.0);
}
