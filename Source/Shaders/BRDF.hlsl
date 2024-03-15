// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#ifndef __BRDF__
#define __BRDF__

#include "./Flax/Math.hlsl"

float3 Diffuse_Lambert(float3 diffuseColor)
{
    return diffuseColor * (1 / PI);
}


float3 OrenNayarToon(
    float3 normal,      // Normalized surface normal
    float3 lightDir,    // Normalized direction to the light
    float3 viewDir,     // Normalized direction to the viewer (camera)
    float roughness,    // Surface roughness [0,1]
    float3 diffuseColor, // Diffuse color of the material
    int levels          // Number of quantization levels for toon shading
)
{
    // Basic Oren-Nayar calculations (simplified for demonstration)
    float LdotN = max(dot(lightDir, normal), 0.0);
    float roughnessSq = roughness * roughness;
    float A = 1.0f - 0.5f * (roughnessSq / (roughnessSq + 0.33f));
    float B = 0.45f * (roughnessSq / (roughnessSq + 0.09f));
    float theta_r = acos(dot(viewDir, normal));
    float theta_i = acos(LdotN);
    float alpha = max(theta_i, theta_r);
    float beta = min(theta_i, theta_r);
    float sinAlpha = sin(alpha);
    float tanBeta = tan(beta);

    float OrenNayar = LdotN * (A + B * max(dot(lightDir, viewDir) - LdotN * dot(viewDir, normal), 0.0) * sinAlpha * tanBeta);

    // Apply toon shading by quantizing the Oren-Nayar result
    float quantized = ceil(OrenNayar * levels) / levels;

    // Modulate the quantized result with the diffuse color
    return diffuseColor * quantized;
}

float OrenNayarApprox(float NoL, float NoV, float3 N, float3 L, float3 V, float roughness)
{
    float3 H = normalize(V + L);
    float VoH = saturate(dot(V, H));
    float LdotH = dot(L, H);
    float sigma2 = roughness * roughness;
    
    float s = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float t = 0.45 * sigma2 / (sigma2 + 0.09);
    
    float alpha = max(NoV, NoL);
    float beta = min(NoV, NoL);

    float OrenNayar = NoL * (s + t * max(dot(L - N * NoL, V - N * NoV), 0.0) * sin(alpha) * tan(beta));
    return OrenNayar / PI;
}

float OrenNayarDiffuse(
    float3 normal, // Normalized surface normal
    float3 lightDir, // Normalized direction to the light
    float3 viewDir, // Normalized direction to the viewer (camera)
    float roughness, // Surface roughness [0,1]
    float3 diffuseColor)   // Diffuse color of the material
{
    float sigma2 = roughness * roughness;
    float LdotN = max(dot(lightDir, normal), 0.0);
    float VdotN = max(dot(viewDir, normal), 0.0);

    float theta_i = acos(LdotN);
    float theta_r = acos(VdotN);

    float alpha = max(theta_i, theta_r);
    float beta = min(theta_i, theta_r);

    float gamma = dot(lightDir - normal * LdotN, viewDir - normal * VdotN);
    if (gamma < 0.0)
        gamma = 0.0; // Max with 0 to prevent artifacts in edge cases

    // Coefficients for the Oren-Nayar model
    float A = 1.0 - 0.5 * (sigma2 / (sigma2 + 0.33));
    float B = 0.45 * (sigma2 / (sigma2 + 0.09));

    float C = sin(alpha) * tan(beta);
    
    // Final Oren-Nayar reflectance
    float OrenNayar = LdotN * (A + B * max(0.0, gamma) * C);

    return diffuseColor * OrenNayar / PI;
}

// GGX / Trowbridge-Reitz
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float roughness, float NoH)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1;
    return a2 / (PI * d * d);
}

// Tuned to match behavior of Vis_Smith
// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float Vis_Schlick(float roughness, float NoV, float NoL)
{
    float k = Square(roughness) * 0.5;
    float visSchlickV = NoV * (1 - k) + k;
    float visSchlickL = NoL * (1 - k) + k;
    return 0.25 / (visSchlickV * visSchlickL);
}

// Smith term for GGX
// [Smith 1967, "Geometrical shadowing of a random rough surface"]
float Vis_Smith(float roughness, float NoV, float NoL)
{
    float a = Square(roughness);
    float a2 = a * a;
    float visSmithV = NoV + sqrt(NoV * (NoV - NoV * a2) + a2);
    float visSmithL = NoL + sqrt(NoL * (NoL - NoL * a2) + a2);
    return rcp(visSmithV * visSmithL);
}

// Appoximation of joint Smith term for GGX
// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float Vis_SmithJointApprox(float roughness, float NoV, float NoL)
{
    float a = Square(roughness);
    float visSmithV = NoL * (NoV * (1 - a) + a);
    float visSmithL = NoV * (NoL * (1 - a) + a);
    return 0.5 * rcp(visSmithV + visSmithL);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float3 F_Schlick(float3 specularColor, float VoH)
{
    float fc = Pow5(1 - VoH);
    return saturate(50.0 * specularColor.g) * fc + (1 - fc) * specularColor;
}

#define REFLECTION_CAPTURE_NUM_MIPS 7
#define REFLECTION_CAPTURE_ROUGHEST_MIP 1
#define REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE 1.2

half ProbeMipFromRoughness(half roughness)
{
    half mip1px = REFLECTION_CAPTURE_ROUGHEST_MIP - REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE * log2(roughness);
    return REFLECTION_CAPTURE_NUM_MIPS - 1 - mip1px;
}

half SSRMipFromRoughness(half roughness)
{
    half mip1px = 4 - REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE * log2(roughness);
    return max(1, 10 - mip1px);
}

float ProbeRoughnessFromMip(float mip)
{
    float mip1px = REFLECTION_CAPTURE_NUM_MIPS - 1 - mip;
    return exp2((REFLECTION_CAPTURE_ROUGHEST_MIP - mip1px) / REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE);
}

// [Lazarov 2013, "Getting More Physical in Call of Duty: Black Ops II"]
float3 EnvBRDFApprox(float3 specularColor, float roughness, float NoV)
{
    // Approximate version, base for pre integrated version
    const half4 c0 = { -1, -0.0275, -0.572, 0.022 };
    const half4 c1 = { 1, 0.0425, 1.04, -0.04 };
    half4 r = roughness * c0 + c1;
    half a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    half2 ab = half2(-1.04, 1.04) * a004 + r.zw;
    return specularColor * ab.x + saturate(50.0 * specularColor.g) * ab.y;
}

// Importance sampled preintegrated G * F
float3 EnvBRDF(Texture2D preIntegratedGF, float3 specularColor, float roughness, float NoV)
{
    float2 ab = preIntegratedGF.SampleLevel(SamplerLinearClamp, float2(NoV, roughness), 0).rg;
    return specularColor * ab.x + saturate(50.0 * specularColor.g) * ab.y;
}

float RoughnessToSpecularPower(float roughness)
{
    return pow(2, 13 * (1 - roughness));
}

#endif
