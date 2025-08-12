// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __EXPONENTIAL_HEIGHT_FOG__
#define __EXPONENTIAL_HEIGHT_FOG__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/GBuffer.hlsl"

// Environment cube texture for fog coloring
TextureCube EnvironmentTexture : register(t10);
SamplerState EnvironmentSampler : register(s10);

// 2D cloud texture for sun masking
Texture2D CloudTexture : register(t11);
SamplerState CloudSampler : register(s11);

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

    float EnvironmentInfluence;
    float EnvironmentMipLevel;
    
    float EnableSunDisc;
    float SunDiscSize;
    float SunDiscBrightness;
    float SunFogPenetration;
    float SunDiscSoftness;
    float SunMaxDistance;
    float SunBrightnessThresholdMin;
    float SunBrightnessThresholdMax;
    float CloudTiling;
    float TimeParam;
    float2 CloudSpeed;
    float2 CloudUVOffset;
};

// Sun disc rendering functions
float CalculateSunDiscMask(float3 viewDir, float3 sunDir, ExponentialHeightFogData fogData)
{
    // Calculate angular distance from view ray to sun direction
    float cosAngle = dot(viewDir, sunDir);
    
    // Convert sun disc size from degrees to cosine space
    float sunDiscSizeRad = fogData.SunDiscSize * PI / 180.0f;
    float sunDiscCos = cos(sunDiscSizeRad);
    
    // Create soft-edged sun disc mask
    float softEdge = fogData.SunDiscSoftness * sunDiscSizeRad;
    float innerCos = cos(sunDiscSizeRad - softEdge);
    
    // Smooth falloff from center to edge
    float sunMask = smoothstep(sunDiscCos, innerCos, cosAngle);
    
    return sunMask;
}

float3 CalculateSunDiscColor(float3 viewDir, float3 sunDir, ExponentialHeightFogData fogData)
{
    float sunMask = CalculateSunDiscMask(viewDir, sunDir, fogData);
    
    // Apply cloud masking if cloud texture is available
    float cloudMask = 1.0f;
    if (fogData.CloudTiling > 0.0f)
    {
        // Convert view direction to spherical coordinates for texture mapping
        float2 cloudUV = float2(
            atan2(viewDir.z, viewDir.x) * (1.0f / (2.0f * PI)) + 0.5f,
            acos(viewDir.y) * (1.0f / PI)
        );
        
        // Apply tiling and animation
        cloudUV *= fogData.CloudTiling;
        // Apply manual UV offset (for debugging)
        cloudUV += fogData.CloudUVOffset;
        // Use time parameter for smooth animation
        cloudUV += fogData.CloudSpeed * fogData.TimeParam;
        
        // Sample cloud texture and apply threshold
        float cloudSample = CloudTexture.SampleLevel(CloudSampler, cloudUV, 0.0f).r;
        cloudMask = smoothstep(fogData.SunBrightnessThresholdMin, fogData.SunBrightnessThresholdMax, cloudSample);
    }
    
    // Use directional inscattering color for sun tinting
    float3 sunColor = fogData.DirectionalInscatteringColor * fogData.SunDiscBrightness;
    
    return sunColor * sunMask * cloudMask;
}

float CalculateFogPenetrationFactor(float3 viewDir, float3 sunDir, ExponentialHeightFogData fogData)
{
    // Create sun-influenced penetration zone
    float sunAlignment = dot(viewDir, sunDir);
    float penetrationSize = fogData.SunDiscSize * 4.0f * PI / 180.0f; // Larger area for natural cloud effect
    float penetrationCos = cos(penetrationSize);
    
    // Base penetration from sun direction
    float sunInfluence = smoothstep(penetrationCos, 1.0f, sunAlignment);
    
    // Sample cloud texture with animated tiling
    // Convert view direction to spherical coordinates for texture mapping
    float3 sphericalCoords = viewDir;
    float2 cloudUV = float2(
        atan2(sphericalCoords.z, sphericalCoords.x) * (1.0f / (2.0f * PI)) + 0.5f,
        acos(sphericalCoords.y) * (1.0f / PI)
    );
    
    // Apply tiling and animation
    cloudUV *= fogData.CloudTiling;
    // Apply manual UV offset (for debugging)
    cloudUV += fogData.CloudUVOffset;
    // Use time parameter for smooth animation
    cloudUV += fogData.CloudSpeed * fogData.TimeParam;
    
    // Sample cloud texture
    float cloudMask = CloudTexture.SampleLevel(CloudSampler, cloudUV, 0.0f).r;
    
    // Apply brightness threshold mapping to cloud mask
    float cloudInfluence = smoothstep(fogData.SunBrightnessThresholdMin, fogData.SunBrightnessThresholdMax, cloudMask);
    
    // Modulate penetration by cloud mask (only areas where clouds allow penetration)
    float cloudBasedPenetration = cloudInfluence * sunInfluence * fogData.SunFogPenetration;
    
    // Return factor to reduce fog opacity (1 = normal fog, 0 = no fog)
    return 1.0f - cloudBasedPenetration;
}


// Forward declaration
float4 GetExponentialHeightFogInternal(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance, float sceneDistance);

// Overloaded version with depth masking support
float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance, float sceneDistance, float2 screenUV, float sceneDepth)
{
    float4 fog = GetExponentialHeightFogInternal(exponentialHeightFog, posWS, camWS, skipDistance, sceneDistance);
    
    // Apply proper depth masking to sun disc if it was rendered
    BRANCH
    if (exponentialHeightFog.EnableSunDisc > 0.0f && exponentialHeightFog.ApplyDirectionalInscattering > 0.0f && sceneDepth > 0.0f)
    {
        // Simple depth masking: if scene geometry is closer than sun max distance, hide sun
        float fadeDistance = exponentialHeightFog.SunMaxDistance * 0.1f; // 10% of max distance for smooth fade
        float depthMask = smoothstep(exponentialHeightFog.SunMaxDistance - fadeDistance, exponentialHeightFog.SunMaxDistance + fadeDistance, sceneDepth);
        
        // Extract and recalculate sun contribution for depth masking
        float3 sunDir = exponentialHeightFog.InscatteringLightDirection;
        float3 cameraToReceiver = normalize(posWS - camWS);
        float3 sunContribution = CalculateSunDiscColor(cameraToReceiver, sunDir, exponentialHeightFog);
        
        // Apply fog density masking - sun is less visible through thicker fog
        float fogVisibilityMask = saturate((fog.a - exponentialHeightFog.FogMinOpacity) * 2.0f);
        sunContribution *= fogVisibilityMask;
        
        // Apply depth mask to sun contribution
        float maskedSunStrength = depthMask;
        fog.rgb = fog.rgb - sunContribution + sunContribution * maskedSunStrength;
    }
    
    return fog;
}

// 5-parameter version (backward compatibility)
float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance, float sceneDistance)
{
    return GetExponentialHeightFogInternal(exponentialHeightFog, posWS, camWS, skipDistance, sceneDistance);
}

float4 GetExponentialHeightFogInternal(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance, float sceneDistance)
{
    float3 cameraToPos = posWS - camWS;
    float cameraToPosSqr = dot(cameraToPos, cameraToPos);
    float cameraToPosLenInv = rsqrt(cameraToPosSqr);
    float cameraToPosLen = cameraToPosSqr * cameraToPosLenInv;
    float3 cameraToReceiverNorm = cameraToPos * cameraToPosLenInv;

    float rayOriginTerms = exponentialHeightFog.FogAtViewPosition;
    float rayLength = cameraToPosLen;
    float rayDirectionY = cameraToPos.y;

    // Apply start distance offset
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

    // Calculate the integral of the ray starting from the view to the object position with the fog density function
    float falloff = max(-127.0f, exponentialHeightFog.FogHeightFalloff * rayDirectionY);
    float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
    float lineIntegralTaylor = log(2.0f) - (0.5f * Pow2(log(2.0f))) * falloff;
    float exponentialHeightLineIntegralCalc = rayOriginTerms * (abs(falloff) > 0.01f ? lineIntegral : lineIntegralTaylor);
    float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength;

    // Calculate the light that went through the fog using gradual buildup
    float buildupScale = 0.1;      // TWEAK: Lower = more gradual buildup, Higher = faster buildup
    float buildupPower = 0.2;      // TWEAK: Higher = more gradual start (try 3.0-6.0)
    float t = saturate(exponentialHeightLineIntegral * buildupScale);
    // Cubic/quartic curve for very gradual start: t^n
    float gradualCurve = pow(t, buildupPower);
    float expFogFactor = max(1.0 - gradualCurve, exponentialHeightFog.FogMinOpacity);
    
    // Apply sun fog penetration if sun disc is enabled and cloud texture is available  
    BRANCH
    if (exponentialHeightFog.EnableSunDisc > 0.0f && exponentialHeightFog.ApplyDirectionalInscattering > 0.0f)
    {
        float3 sunDir = exponentialHeightFog.InscatteringLightDirection;
        float fogPenetration = CalculateFogPenetrationFactor(cameraToReceiverNorm, sunDir, exponentialHeightFog);
        expFogFactor = lerp(exponentialHeightFog.FogMinOpacity, expFogFactor, fogPenetration);
    }

    // Calculate the directional light inscattering
    float3 inscatteringColor = exponentialHeightFog.FogInscatteringColor;
    
    // Sample environment texture if influence is enabled
    BRANCH
    if (exponentialHeightFog.EnvironmentInfluence > 0.0f)
    {
        float3 environmentColor = EnvironmentTexture.SampleLevel(EnvironmentSampler, cameraToReceiverNorm, exponentialHeightFog.EnvironmentMipLevel).rgb;
        inscatteringColor = lerp(inscatteringColor, environmentColor * inscatteringColor, exponentialHeightFog.EnvironmentInfluence);
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

    // Add sun disc if enabled
    float3 sunDiscContribution = float3(0, 0, 0);
    BRANCH
    if (exponentialHeightFog.EnableSunDisc > 0.0f && exponentialHeightFog.ApplyDirectionalInscattering > 0.0f)
    {
        float3 sunDir = exponentialHeightFog.InscatteringLightDirection;
        sunDiscContribution = CalculateSunDiscColor(cameraToReceiverNorm, sunDir, exponentialHeightFog);
        
        // Modulate sun disc by fog density - less visible through thicker fog
        float fogVisibilityMask = saturate((expFogFactor - exponentialHeightFog.FogMinOpacity) * 2.0f);
        
        // Note: Cloud masking is now handled in CalculateSunDiscColor function
        // Note: Depth masking is now handled per-pixel in the overloaded version
        
        sunDiscContribution *= fogVisibilityMask;
    }

    float3 finalColor = inscatteringColor * (1.0f - expFogFactor) + directionalInscattering + sunDiscContribution;
    return float4(finalColor, expFogFactor);
}

float4 GetExponentialHeightFog(ExponentialHeightFogData exponentialHeightFog, float3 posWS, float3 camWS, float skipDistance)
{
    return GetExponentialHeightFogInternal(exponentialHeightFog, posWS, camWS, skipDistance, distance(posWS, camWS));
}

#endif
