// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#ifndef __LIGHTING__
#define __LIGHTING__

#include "./Flax/LightingCommon.hlsl"

ShadowData GetShadow(LightData lightData, GBufferSample gBuffer, float4 shadowMask)
{
    ShadowData shadow;
    shadow.SurfaceShadow = gBuffer.AO * shadowMask.r;
    shadow.TransmissionShadow = shadowMask.g;
    return shadow;
}

// Update the core BRDF functions first:
float3 Diffuse_Disney(float3 diffuseColor, float roughness, float NoV, float NoL, float VoH)
{
    float FL = pow(1.0 - NoL, 5.0);
    float FV = pow(1.0 - NoV, 5.0);
    float Fretro = FL * FV;
    
    // Roughness-based retro-reflection
    float rough = roughness;
    float Dr = 1.0 + (0.5 + 0.5 * rough) * Fretro;
    
    return diffuseColor * Dr * (1.0 / PI);
}


// The main lighting calculation:
LightingData StandardShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    float3 diffuseColor = GetDiffuseColor(gBuffer);
    float3 H = normalize(V + L);
    float NoL = saturate(dot(N, L));
    float NoV = max(dot(N, V), 1e-5);
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    LightingData lighting;

    // Disney diffuse
    lighting.Diffuse = Diffuse_Disney(diffuseColor, gBuffer.Roughness, NoV, NoL, VoH);

#if LIGHTING_NO_SPECULAR
    lighting.Specular = 0;
#else
    float3 specularColor = GetSpecularColor(gBuffer);
    
    // Energy-conserving specular
    float3 F = F_Schlick(specularColor, VoH);
    float D = D_GGX(gBuffer.Roughness, NoH);
    float Vis = Vis_SmithJointApprox(gBuffer.Roughness, NoV, NoL);
    
    // Properly scaled specular term
    lighting.Specular = (D * Vis) * F * energy;
    
    // Energy conservation between diffuse and specular
    lighting.Diffuse *= (1.0 - F);
#endif

    lighting.Transmission = 0;
    return lighting;
}


LightingData SubsurfaceShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    LightingData lighting = StandardShading(gBuffer, energy, L, V, N);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    // Fake effect of the light going through the material
    float3 subsurfaceColor = gBuffer.CustomData.rgb;
    float opacity = gBuffer.CustomData.a;
    float3 H = normalize(V + L);
    float inscatter = pow(saturate(dot(L, -V)), 12.1f) * lerp(3, 0.1f, opacity);
    float normalContribution = saturate(dot(N, H) * opacity + 1.0f - opacity);
    float backScatter = gBuffer.AO * normalContribution / (PI * 2.0f);
    lighting.Transmission = lerp(backScatter, 1, inscatter) * subsurfaceColor;
#endif
    return lighting;
}

LightingData FoliageShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    LightingData lighting = StandardShading(gBuffer, energy, L, V, N);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    // Fake effect of the light going through the thin foliage
    float3 subsurfaceColor = gBuffer.CustomData.rgb;
    float wrapNoL = saturate((-dot(N, L) + 0.5f) / 2.25);
    float VoL = dot(V, L);
    float scatter = D_GGX(0.36, saturate(-VoL));
    lighting.Transmission = subsurfaceColor * (wrapNoL * scatter);
#endif
    return lighting;
}

LightingData SurfaceShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    switch (gBuffer.ShadingModel)
    {
        case SHADING_MODEL_UNLIT:
        case SHADING_MODEL_LIT:
            return StandardShading(gBuffer, energy, L, V, N);
        case SHADING_MODEL_SUBSURFACE:
            return SubsurfaceShading(gBuffer, energy, L, V, N);
        case SHADING_MODEL_FOLIAGE:
            return FoliageShading(gBuffer, energy, L, V, N);
        default:
            return (LightingData) 0;
    }
}

float4 GetSkyLightLighting(LightData lightData, GBufferSample gBuffer, TextureCube ibl)
{
    // Get material diffuse color
    float3 diffuseColor = GetDiffuseColor(gBuffer);

    // Compute the preconvolved incoming lighting with the normal direction (apply ambient color)
    // Some data is packed, see C++ RendererSkyLightData::SetupLightData
    float mip = lightData.SourceLength;
#if LIGHTING_NO_DIRECTIONAL
    float3 uvw = float3(0, 0, 0);
#else
    float3 uvw = gBuffer.Normal;
#endif
    float3 diffuseLookup = ibl.SampleLevel(SamplerLinearClamp, uvw, mip).rgb * lightData.Color.rgb;
    diffuseLookup += float3(lightData.SpotAngles.rg, lightData.SourceRadius);

    // Calculate specular reflection
    float3 V = normalize(gBuffer.WorldPos - lightData.Position);
    float3 R = reflect(-V, gBuffer.Normal);
    float roughness = max(gBuffer.Roughness, lightData.MinRoughness);
    float specularMip = roughness * 9.0; // Adjust the multiplier as needed
    float3 specularLookup = ibl.SampleLevel(SamplerLinearClamp, R, specularMip).rgb * lightData.Color.rgb;
    float3 specularColor = GetSpecularColor(gBuffer);
    float3 specularLight = specularLookup * specularColor;

    // Fade out based on distance to capture
    float3 captureVector = gBuffer.WorldPos - lightData.Position;
    float captureVectorLength = length(captureVector);
    float normalizedDistanceToCapture = saturate(captureVectorLength / lightData.Radius);
    float distanceAlpha = 1.0 - smoothstep(0.6, 1, normalizedDistanceToCapture);

    // Calculate final light
    float3 color = (diffuseLookup * diffuseColor) + specularLight;
    float luminance = Luminance(diffuseLookup + specularLookup);
    return float4(color, luminance) * (distanceAlpha * gBuffer.AO);
}




//float4 GetSkyLightLighting(LightData lightData, GBufferSample gBuffer, TextureCube ibl)
//{
//    // Get material diffuse color
//    float3 diffuseColor = GetDiffuseColor(gBuffer);

//    // Compute the preconvolved incoming lighting with the normal direction (apply ambient color)
//    float mip = lightData.SourceLength;
//    float3 uvw = (LIGHTING_NO_DIRECTIONAL ? float3(0, 0, 0) : gBuffer.Normal);
//    float3 diffuseLookup = ibl.SampleLevel(SamplerLinearClamp, uvw, mip).rgb * lightData.Color.rgb;
//    diffuseLookup += float3(lightData.SpotAngles.rg, lightData.SourceRadius);

//    // Calculate specular reflection
//    float3 V = normalize(gBuffer.WorldPos - lightData.Position);
//    float3 R = reflect(-V, gBuffer.Normal);
//    float roughness = max(gBuffer.Roughness, lightData.MinRoughness);
//    float specularMip = roughness * 6.0; // Adjust the multiplier as needed for texture sampling
//    float3 specularLookup = ibl.SampleLevel(SamplerLinearClamp, R, specularMip).rgb * lightData.Color.rgb;
//    float3 specularColor = GetSpecularColor(gBuffer);
//    float3 specularLight = specularLookup * specularColor;

//    // Combine specular and diffuse components
//    float3 color = (diffuseLookup * diffuseColor) + specularLight;

//    // Fade out based on distance to capture
//    float3 captureVector = gBuffer.WorldPos - lightData.Position;
//    float captureVectorLength = length(captureVector);
//    float normalizedDistanceToCapture = saturate(captureVectorLength / lightData.Radius);
//    float distanceAlpha = 1.0 - smoothstep(0.6, 1, normalizedDistanceToCapture);

//    // Calculate final light
//    float luminance = Luminance(diffuseLookup + specularLookup);
//    return float4(color, luminance) * (distanceAlpha * gBuffer.AO);
//}


float4 GetLighting(float3 viewPos, LightData lightData, GBufferSample gBuffer, float4 shadowMask, bool isRadial, bool isSpotLight)
{
    float4 result = 0;
    float3 V = normalize(viewPos - gBuffer.WorldPos);
    float3 N = gBuffer.Normal;
    float3 L = lightData.Direction;
    float NoL = saturate(dot(N, L));
    float3 toLight = lightData.Direction;

    // Get shadow with both surface and transmission components
    ShadowData shadow = GetShadow(lightData, gBuffer, shadowMask);

    // Handle radial lights
    if (isRadial)
    {
        toLight = lightData.Position - gBuffer.WorldPos;
        float distanceSqr = dot(toLight, toLight);
        L = toLight * rsqrt(distanceSqr);
        float attenuation = 1;
        GetRadialLightAttenuation(lightData, isSpotLight, N, distanceSqr, 1, toLight, L, NoL, attenuation);
        shadow.SurfaceShadow *= attenuation;
        shadow.TransmissionShadow *= attenuation;
    }

    BRANCH

    if (shadow.SurfaceShadow + shadow.TransmissionShadow > 0)
    {
        gBuffer.Roughness = max(gBuffer.Roughness, lightData.MinRoughness);
        float energy = AreaLightSpecular(lightData, gBuffer.Roughness, toLight, L, V, N);

        LightingData lighting;
        
        // Handle different shading models
        switch (gBuffer.ShadingModel)
        {
            case SHADING_MODEL_UNLIT:
            case SHADING_MODEL_LIT:
                lighting = StandardShading(gBuffer, energy, L, V, N);
                break;
            case SHADING_MODEL_SUBSURFACE:
                lighting = SubsurfaceShading(gBuffer, energy, L, V, N);
                break;
            case SHADING_MODEL_FOLIAGE:
                lighting = FoliageShading(gBuffer, energy, L, V, N);
                break;
            default:
                lighting = (LightingData) 0;
                break;
        }

        // Combine direct lighting (with improved energy conservation)
        float3 surfaceLight = (lighting.Diffuse + lighting.Specular) * NoL * shadow.SurfaceShadow;
        
        // Handle transmission separately since it doesn't use NoL the same way
        float3 subsurfaceLight = lighting.Transmission * shadow.TransmissionShadow;
        
        // Combine both lighting components
        float3 finalLight = surfaceLight + subsurfaceLight;
        
        result.rgb = lightData.Color * finalLight;
        result.a = Luminance(finalLight);
    }

    return result;
}

#endif
