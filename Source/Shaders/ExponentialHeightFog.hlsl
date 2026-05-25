// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __EXPONENTIAL_HEIGHT_FOG__
#define __EXPONENTIAL_HEIGHT_FOG__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"

// Henyey-Greenstein phase function for Mie scattering
// g = anisotropy parameter (-1 to 1, typically 0.6-0.9 for fog)
// cosTheta = dot(viewDir, lightDir)
float HenyeyGreensteinPhase(float g, float cosTheta)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

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
    float MieScatteringAnisotropy;

    float FogCutoffDistance;
    float VolumetricFogMaxDistance;
    float DirectionalInscatteringStartDistance;
    float StartDistance;

    float GlobalTime;
    float Padding;
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

    // [NOISE] Add subtle 3D noise to break up uniformity
    // Use low-frequency sine patterns for "wispy" cloud-like variation rather than high-freq grain
    float3 p = posWS * 0.0001;
    // Animate noise by offsetting the sampling position over time
    float timeFactor = exponentialHeightFog.GlobalTime * 0.5; // Adjust speed as needed
    float noise = sin(p.x + timeFactor) * sin(p.z * 0.8 + timeFactor * 0.7) + sin(p.x * 2.3 + p.y * 1.5 - timeFactor * 0.4) * 0.5;

    // Fade out noise at distance to prevent visible patterns in sky
    float noiseFadeDistance = 5000.0f; // Distance at which noise fully fades
    float noiseFade = saturate(1.0 - (rayLength / noiseFadeDistance));
    float noiseIntensity = 0.15 * noiseFade;

    float noiseMod = 1.0 + clamp(noise, -1.0, 1.0) * noiseIntensity;
    exponentialHeightLineIntegralCalc *= noiseMod;

    // Apply gradual buildup to height falloff (matching depth math)
    float heightBuildupScale = 0.1;
    float heightBuildupPower = 0.1;
    float heightT = saturate(exponentialHeightLineIntegralCalc * rayLength * heightBuildupScale);
    float heightGradualCurve = pow(heightT, heightBuildupPower);

    float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength * heightGradualCurve;

    // Apply distance-based fog density ramping (core physics integration)
    float startDist = max(exponentialHeightFog.StartDistance, 0.1f);
    
    // [FIX] Smoother start distance transition using smoothstep instead of hard branch
    // This eliminates the "sudden thick line" artifact when crossing the start distance threshold.
    float distanceFade = smoothstep(0.0, startDist, cameraToPosLen);
    // We also want a linear ramp up of density *within* the start zone to match physical "entering fog"
    // But the previous integration was creating a kink. 
    // Let's just dampen the total integral near the start.
    exponentialHeightLineIntegral *= distanceFade;

    // Calculate the light that went through the fog using gradual buildup
    float buildupScale = 0.07;      // TWEAK: Lower = more gradual buildup, Higher = faster buildup
    float buildupPower = 0.1;      // TWEAK: Higher = more gradual start (try 3.0-6.0)
    float t = saturate(exponentialHeightLineIntegral * buildupScale);
    // Cubic/quartic curve for very gradual start: t^n
    float gradualCurve = pow(t, buildupPower);
    float expFogFactor = max(1.0 - gradualCurve, exponentialHeightFog.FogMinOpacity);

    // Calculate base fog inscattering color
    float3 baseFogColor = exponentialHeightFog.FogInscatteringColor;

    // Physically-based Mie scattering for directional light
    float3 directionalInscattering = 0;
    BRANCH
    if (exponentialHeightFog.ApplyDirectionalInscattering > 0)
    {
        // Calculate phase function based on view-light angle
        float cosTheta = dot(cameraToReceiverNorm, exponentialHeightFog.InscatteringLightDirection);
        float phase = HenyeyGreensteinPhase(exponentialHeightFog.MieScatteringAnisotropy, cosTheta);

        // Integrate Mie scattering with fog density
        // Account for start distance before directional scattering takes effect
        float dirExponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * max(rayLength - exponentialHeightFog.DirectionalInscatteringStartDistance, 0.0f);
        float directionalInscatteringFogFactor = saturate(exp2(-dirExponentialHeightLineIntegral));

        // Apply phase function to modulate directional light scattering
        directionalInscattering = exponentialHeightFog.DirectionalInscatteringColor * phase * (1.0f - directionalInscatteringFogFactor);
    }

    // Combine base fog color with Mie-scattered directional light
    float3 inscatteringColor = baseFogColor;

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
