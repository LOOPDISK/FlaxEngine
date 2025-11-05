// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __EXPONENTIAL_HEIGHT_FOG__
#define __EXPONENTIAL_HEIGHT_FOG__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"

// 2D gradient texture for fog coloring (U = sun angle, V = height)
Texture2D FogGradientTexture : register(t15);
SamplerState FogGradientSampler : register(s15);

// Structure that contains information about exponential height fog
struct ExponentialHeightFogData
{
    float3 FogInscatteringColor;
    float FogMinOpacity;

    float FogDensity;
    float FogHeight;
    float FogHeightFalloff;
    float FogAtViewPosition;

    float3 InscatteringLightDirection;
    float ApplyDirectionalInscattering;

    float3 DirectionalInscatteringColor;
    float DirectionalInscatteringExponent;

    float FogCutoffDistance;
    float VolumetricFogMaxDistance;
    float DirectionalInscatteringStartDistance;
    float StartDistance;

    float GradientInfluence;
    float GradientHeightRange;
    float2 Padding;
};

float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance, float sceneDistance)
{
    float3 cameraToPos = posWS - camWS;
    float cameraToPosSqr = dot(cameraToPos, cameraToPos);
    float cameraToPosLenInv = rsqrt(cameraToPosSqr);
    float cameraToPosLen = cameraToPosSqr * cameraToPosLenInv;
    float3 cameraToReceiverNorm = cameraToPos * cameraToPosLenInv;

    float rayOriginTerms = exponentialHeightFog.FogAtViewPosition;
    float rayLength = cameraToPosLen;
    float rayDirectionY = cameraToPos.y;

    // Apply volumetric fog skip distance (hard cutoff for performance only)
    if (skipDistance > 0)
    {
        float excludeIntersectionTime = skipDistance * cameraToPosLenInv;
        float cameraToExclusionIntersectionY = excludeIntersectionTime * cameraToPos.y;
        float exclusionIntersectionY = camWS.y + cameraToExclusionIntersectionY;
        rayLength = (1.0f - excludeIntersectionTime) * cameraToPosLen;
        rayDirectionY = cameraToPos.y - cameraToExclusionIntersectionY;
        float exponent = exponentialHeightFog.FogHeightFalloff * (exclusionIntersectionY - exponentialHeightFog.FogHeight);
        rayOriginTerms = exponentialHeightFog.FogDensity * exp2(-exponent);
    }

    // Calculate the integral of the ray starting from the view to the object position with the fog density function
    float falloff = max(-127.0f, exponentialHeightFog.FogHeightFalloff * rayDirectionY);
    float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
    float lineIntegralTaylor = log(2.0f) - (0.5f * Pow2(log(2.0f))) * falloff;
    float exponentialHeightLineIntegralCalc = rayOriginTerms * (abs(falloff) > 0.01f ? lineIntegral : lineIntegralTaylor);

    // Apply gradual buildup to height falloff (matching depth math)
    float heightBuildupScale = 0.1;
    float heightBuildupPower = 0.2;
    float heightT = saturate(exponentialHeightLineIntegralCalc * rayLength * heightBuildupScale);
    float heightGradualCurve = pow(heightT, heightBuildupPower);

    float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength * heightGradualCurve;

    // Apply distance-based fog density ramping (core physics integration)
    float startDist = max(exponentialHeightFog.StartDistance, 0.1f);
    if (cameraToPosLen <= startDist)
    {
        // Ray is entirely within ramp-up zone: average density = (distance/startDist) * 0.5
        float avgDensityFactor = (cameraToPosLen / startDist) * 0.5f;
        exponentialHeightLineIntegral *= avgDensityFactor;
    }
    else
    {
        // Ray extends beyond ramp zone: weighted average of ramp and full density regions
        float rampZoneContribution = startDist * 0.5f; // ramp zone average = 0.5
        float fullZoneContribution = cameraToPosLen - startDist; // full density = 1.0
        float totalContribution = rampZoneContribution + fullZoneContribution;
        float avgDensityFactor = totalContribution / cameraToPosLen;
        exponentialHeightLineIntegral *= avgDensityFactor;
    }

    // Calculate the light that went through the fog using gradual buildup
    float buildupScale = 0.1;      // TWEAK: Lower = more gradual buildup, Higher = faster buildup
    float buildupPower = 0.2;      // TWEAK: Higher = more gradual start (try 3.0-6.0)
    float t = saturate(exponentialHeightLineIntegral * buildupScale);
    // Cubic/quartic curve for very gradual start: t^n
    float gradualCurve = pow(t, buildupPower);
    float expFogFactor = max(1.0 - gradualCurve, exponentialHeightFog.FogMinOpacity);

    // Calculate the fog inscattering color
    float3 inscatteringColor = exponentialHeightFog.FogInscatteringColor;

    // Sample 2D gradient texture if influence is enabled
    BRANCH
    if (exponentialHeightFog.GradientInfluence > 0.0f)
    {
        // Calculate height coordinate (V axis)
        float heightRange = max(1.0f, exponentialHeightFog.GradientHeightRange);
        float heightNormalized = saturate((posWS.y - exponentialHeightFog.FogHeight) / heightRange);

        // Calculate sun angle coordinate (U axis)
        // dot(viewDir, sunDir) gives -1 (away from sun) to +1 (toward sun)
        float sunAngle = dot(cameraToReceiverNorm, exponentialHeightFog.InscatteringLightDirection);
        float sunAngleNormalized = sunAngle * 0.5f + 0.5f; // Remap to 0-1

        // Sample gradient texture with bicubic filtering
        float2 gradientUV = float2(sunAngleNormalized, heightNormalized);
        float3 gradientColor = FogGradientTexture.SampleLevel(FogGradientSampler, gradientUV, 0).rgb;

        // Blend gradient with base fog color
        inscatteringColor = lerp(inscatteringColor, gradientColor, exponentialHeightFog.GradientInfluence);
    }
    
    float3 directionalInscattering = 0;
    BRANCH
    if (exponentialHeightFog.ApplyDirectionalInscattering > 0)
    {
        float3 directionalLightInscattering = exponentialHeightFog.DirectionalInscatteringColor * pow(saturate(dot(cameraToReceiverNorm, exponentialHeightFog.InscatteringLightDirection)), exponentialHeightFog.DirectionalInscatteringExponent);
        float dirExponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * max(rayLength - exponentialHeightFog.DirectionalInscatteringStartDistance, 0.0f);
        float directionalInscatteringFogFactor = saturate(exp2(-dirExponentialHeightLineIntegral));
        directionalInscattering = directionalLightInscattering * (1.0f - directionalInscatteringFogFactor);
    }

    // Disable fog after a certain distance
    FLATTEN
    if (exponentialHeightFog.FogCutoffDistance > 0 && sceneDistance > exponentialHeightFog.FogCutoffDistance)
    {
        expFogFactor = 1;
        directionalInscattering = 0;
    }

    return float4(inscatteringColor * (1.0f - expFogFactor) + directionalInscattering, expFogFactor);
}

float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance)
{
    return GetExponentialHeightFog(exponentialHeightFog, posWS, camWS, skipDistance, distance(posWS, camWS));
}

#endif
