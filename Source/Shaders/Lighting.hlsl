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
    lighting.ClearcoatSpecular = 0;
    return lighting;
}
/*
LightingData ClearcoatShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    LightingData lighting = StandardShading(gBuffer, energy, L, V, N);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    float clearcoatIntensity = gBuffer.CustomData.r * 10.0;
    float spread = gBuffer.CustomData.g;
    float2 noise = gBuffer.CustomData.ba * 2.0 - 1.0;
    
    float3 clearcoatN = normalize(N + float3(noise.x, noise.y, 0) * 0.2);
    float3 R = reflect(-V, N);
    float RdotL = dot(R, L);
    
    // Convert RdotL from [-1,1] to [0,1]
    float alignmentFactor = (RdotL * 0.5 + 0.5);
    
    // Use spread to control falloff power
    // When spread is 0, power is very high (tight spot)
    // When spread is 1, power is 1 (full hemisphere)
    float power = lerp(50.0, 1.0, spread);
    float glintMask = pow(alignmentFactor, power);
    
    float3 H = normalize(V + L);
    float NoH = saturate(dot(clearcoatN, H));
    float NoV = max(dot(clearcoatN, V), 1e-5);
    float NoL = saturate(dot(clearcoatN, L));
    float VoH = saturate(dot(V, H));
    
    float3 clearcoatF0 = 0.04;
    float3 F = F_Schlick(clearcoatF0, VoH);
    
    const float fixedRoughness = 0.1;
    float D = D_GGX(fixedRoughness, NoH);
    float Vis = Vis_SmithJointApprox(fixedRoughness, NoV, NoL);
    
    float glintBoost = 1.0;
    lighting.ClearcoatSpecular = (D * Vis) * F * energy * glintBoost * clearcoatIntensity * glintMask;
#endif
    return lighting;
}
*/
/* first good colored
LightingData ClearcoatShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    LightingData lighting = StandardShading(gBuffer, energy, L, V, N);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    float clearcoatIntensity = gBuffer.CustomData.r * 10.0;
    float spread = gBuffer.CustomData.g;
    float2 noise = gBuffer.CustomData.ba * 2.0 - 1.0;
    
    float3 clearcoatN = normalize(N + float3(noise.x, noise.y, 0) * 0.2);
    float3 R = reflect(-V, N);
    float RdotL = dot(R, L);
    
    float alignmentFactor = (RdotL * 0.5 + 0.5);
    float power = lerp(50.0, 1.0, spread);
    float glintMask = pow(alignmentFactor, power);
    
    float3 H = normalize(V + L);
    float NoH = saturate(dot(clearcoatN, H));
    float NoV = max(dot(clearcoatN, V), 1e-5);
    float NoL = saturate(dot(clearcoatN, L));
    float VoH = saturate(dot(V, H));
    
    // Color variation based on viewing angle
    float3 warmColor = float3(1.0, 0.6, 0.3);  // Orange/gold
    float3 coolColor = float3(0.3, 0.6, 1.0);  // Blue
    float colorBlend = pow(1.0 - abs(dot(V, N)), 2.0); // Fresnel-like blend
    float3 glintColor = lerp(warmColor, coolColor, colorBlend);
    
    // Add some noise to the color
    float colorNoise = (noise.x * noise.y) * 0.5 + 0.5;
    glintColor = lerp(glintColor, glintColor * float3(1.2, 0.8, 0.6), colorNoise);
    
    float3 clearcoatF0 = 0.04;
    float3 F = F_Schlick(clearcoatF0, VoH);
    
    const float fixedRoughness = 0.3;
    float D = D_GGX(fixedRoughness, NoH);
    float Vis = Vis_SmithJointApprox(fixedRoughness, NoV, NoL);
    
    float glintBoost = 1.0;
    lighting.ClearcoatSpecular = (D * Vis) * F * energy * glintBoost * clearcoatIntensity * glintMask * glintColor;
#endif
    return lighting;
}
*/
LightingData ClearcoatShading(GBufferSample gBuffer, float energy, float3 L, float3 V, half3 N)
{
    LightingData lighting = StandardShading(gBuffer, energy, L, V, N);
#if defined(USE_GBUFFER_CUSTOM_DATA)
    float clearcoatIntensity = gBuffer.CustomData.r * 10.0;
    float spread = gBuffer.CustomData.g;
    float2 noise = gBuffer.CustomData.ba * 2.0 - 1.0;
    
    float3 clearcoatN = normalize(N + float3(noise.x, noise.y, 0) * 0.2);
    float3 R = reflect(-V, N);
    float RdotL = dot(R, L);
    
    float alignmentFactor = (RdotL * 0.5 + 0.5);
    float power = lerp(50.0, 1.0, spread);
    float glintMask = pow(alignmentFactor, power);
    
    float3 H = normalize(V + L);
    float NoH = saturate(dot(clearcoatN, H));
    float NoV = max(dot(clearcoatN, V), 1e-5);
    float NoL = saturate(dot(clearcoatN, L));
    float VoH = saturate(dot(V, H));
    
    // Use noise to create per-glint color variation
    float phase = frac(noise.x * noise.y * 8.0) * 6.28318; // Scale up noise variation
    
    // Create more dramatic spectral colors
    float3 color;
    color.r = saturate(sin(phase) + 0.5);
    color.g = saturate(sin(phase + 2.094) + 0.5); // 2pi/3
    color.b = saturate(sin(phase + 4.189) + 0.5); // 4pi/3
    
    color = pow(color, 0.4) * 3.0; // Boost intensity
    
    float3 clearcoatF0 = 0.04;
    float3 F = F_Schlick(clearcoatF0, VoH);
    
    const float fixedRoughness = 0.5;
    float D = D_GGX(fixedRoughness, NoH);
    float Vis = Vis_SmithJointApprox(fixedRoughness, NoV, NoL);
    
    float glintBoost = 2.0;
    lighting.ClearcoatSpecular = (D * Vis) * F * energy * glintBoost * clearcoatIntensity * glintMask * color;
#endif
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
    
    
    //lighting.Diffuse = 0.0;
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
        case SHADING_MODEL_CLEARCOAT:
            return ClearcoatShading(gBuffer, energy, L, V, N);
            
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
            case SHADING_MODEL_CLEARCOAT:
                lighting = ClearcoatShading(gBuffer, energy, L, V, N);
                break;
            default:
                lighting = (LightingData) 0;
                break;
        }

        // Combine direct lighting (with improved energy conservation)
        float3 surfaceLight = (lighting.Diffuse + lighting.Specular + lighting.ClearcoatSpecular) * NoL * shadow.SurfaceShadow;

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
