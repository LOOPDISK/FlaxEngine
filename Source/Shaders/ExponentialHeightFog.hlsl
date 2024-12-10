// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#ifndef __EXPONENTIAL_HEIGHT_FOG__
#define __EXPONENTIAL_HEIGHT_FOG__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"

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
};




float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance)
{
    // Preserve original distance calculations for compatibility
    float3 cameraToPos = posWS - camWS;
    float cameraToPosSqr = dot(cameraToPos, cameraToPos);
    float cameraToPosLenInv = rsqrt(cameraToPosSqr);
    float cameraToPosLen = cameraToPosSqr * cameraToPosLenInv;
    float3 cameraToReceiverNorm = cameraToPos * cameraToPosLenInv;

    // Original ray calculations
    float rayOriginTerms = exponentialHeightFog.FogAtViewPosition;
    float rayLength = cameraToPosLen;
    float rayDirectionY = cameraToPos.y;

    // Maintain skip distance logic
    skipDistance = max(skipDistance, exponentialHeightFog.StartDistance);
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

    // Calculate rational falloff (new implementation)
    float normalizedDistance = rayLength * exponentialHeightFog.FogDensity;
    float distanceSquared = normalizedDistance * normalizedDistance;
    float rationalFalloff = (4.0f * distanceSquared) / (4.0f * distanceSquared + 1.0f);
    
    // Blend with height-based calculations
    float falloff = max(-127.0f, exponentialHeightFog.FogHeightFalloff * rayDirectionY);
    float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
    float lineIntegralTaylor = log(2.0f) - (0.5f * Pow2(log(2.0f))) * falloff;
    float exponentialHeightLineIntegralCalc = rayOriginTerms * (abs(falloff) > 0.01f ? lineIntegral : lineIntegralTaylor);
    
    // Combine rational and exponential components
    float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength * rationalFalloff;

    // Calculate fog factor with rational modification
    float expFogFactor = max(saturate(exp2(-exponentialHeightLineIntegral)), exponentialHeightFog.FogMinOpacity);

    // Preserve directional inscattering calculations
    float3 inscatteringColor = exponentialHeightFog.FogInscatteringColor;
    float3 directionalInscattering = 0;
    BRANCH

    if (exponentialHeightFog.ApplyDirectionalInscattering > 0)
    {
        float3 directionalLightInscattering = exponentialHeightFog.DirectionalInscatteringColor *
            pow(saturate(dot(cameraToReceiverNorm, exponentialHeightFog.InscatteringLightDirection)),
                exponentialHeightFog.DirectionalInscatteringExponent);
        float dirExponentialHeightLineIntegral = exponentialHeightLineIntegralCalc *
            max(rayLength - exponentialHeightFog.DirectionalInscatteringStartDistance, 0.0f);
        float directionalInscatteringFogFactor = saturate(exp2(-dirExponentialHeightLineIntegral));
        directionalInscattering = directionalLightInscattering * (1.0f - directionalInscatteringFogFactor);
    }

    // Maintain distance cutoff behavior
    FLATTEN

    if (exponentialHeightFog.FogCutoffDistance > 0 && cameraToPosLen > exponentialHeightFog.FogCutoffDistance)
    {
        expFogFactor = 1;
        directionalInscattering = 0;
    }

    return float4(inscatteringColor * (1.0f - expFogFactor) + directionalInscattering, expFogFactor);
}

#endif
