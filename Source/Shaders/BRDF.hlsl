// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __BRDF__
#define __BRDF__

#include "./Flax/Math.hlsl"

float3 Diffuse_Lambert(float3 diffuseColor)
{
    return diffuseColor * (1 / PI);
}

// [Burley 2012, "Physically-Based Shading at Disney"]
float3 Diffuse_Burley(float3 diffuseColor, float roughness, float NoV, float NoL, float VoH)
{
    float FD90 = 0.5 + 2 * VoH * VoH * roughness;
    float FdV = 1 + (FD90 - 1) * Pow5(1 - NoV);
    float FdL = 1 + (FD90 - 1) * Pow5(1 - NoL);
    return diffuseColor * ((1 / PI) * FdV * FdL);
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
    float Vis_SmithV = NoL * (NoV * (1 - a) + a);
    float Vis_SmithL = NoV * (NoL * (1 - a) + a);
    // Add a tiny epsilon to prevent division by zero
    return 0.5 * rcp(max(Vis_SmithV + Vis_SmithL, 0.0001));
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float3 F_Schlick_Raw(float3 f0, float f90, float VoH)
{
    return f0 + (f90 - f0) * Pow5(1 - VoH);
}

float3 F_Schlick(float3 specularColor, float VoH)
{
    float f90 = saturate(50.0 * specularColor.g);
    return F_Schlick_Raw(specularColor, f90, VoH);
}



float3 F_Schlick(float3 specularColor, float VoH, float roughness)
{
    float f90 = saturate(50.0 * specularColor.g);
    // REMOVED: f90 *= saturate(1.0 - roughness); <--- The culprit for sharp cutoffs
    return F_Schlick_Raw(specularColor, f90, VoH);
}

float3 F_Schlick(float3 f0, float3 f90, float VoH)
{
	float fc = Pow5(1 - VoH);
	return f90 * fc + (1 - fc) * f0;
}

// Grazing-sharpness gate: how much a lobe is allowed to become a white mirror at grazing.
// Sharp/mirror lobes -> 1; rough/broad lobes -> 0 (no Fresnel edge-brightening). Ported from
// ratpole's per-lobe gate clamp(1 - 2*xn/beta), mapped to GGX via alpha^2 = 1/(1+k*beta):
//   gate = saturate(1 - K*a2/(1-a2)),  a2 = roughness^4.
// K = 0.8284271 (= 2(sqrt2-1)) is the physically-derived value; the gate then zeroes at
// roughness ~0.86. Raise K for a matter look -> the edge sheen dies at LOWER roughness.
// Zero-cross roughness = (1/(K+1))^(1/4):  K=0.83->0.86  K=3->0.71  K=6->0.62  K=12->0.53.
// Gentle scalpel value: the matte flatness is carried by the roughness remap (MATTE_AMOUNT in
// GBufferCommon.hlsl); the gate just cleans the residual edge sheen on whatever gloss remains.
#define SPECULAR_GRAZING_GATE_K 3.0
float SpecularGrazingGate(float roughness)
{
    float a2 = Square(Square(roughness));
    return saturate(1.0 - SPECULAR_GRAZING_GATE_K * a2 / max(1.0 - a2, 1e-4));
}

#define REFLECTION_CAPTURE_NUM_MIPS 7
// Allow going down to mip 0 (1x1 pixel) for fully diffuse/chalky look
#define REFLECTION_CAPTURE_ROUGHEST_MIP 0 
#define REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE 1.2

half ProbeMipFromRoughness(half roughness)
{
    // Original Log2 formula was too conservative. 
    // We use a linear-to-sqrt mapping to ensure Roughness 1.0 hits the bottom mip.
    float mipLevel = (REFLECTION_CAPTURE_NUM_MIPS - 1) * sqrt(roughness);
    return mipLevel;
}

half SSRMipFromRoughness(half roughness)
{
    // Matches the probe logic for consistency
    half mipLevel = 5.0 * sqrt(roughness);
    return mipLevel;
}

float ProbeRoughnessFromMip(float mip)
{
    // Inverse of the sqrt curve above
    float t = mip / (REFLECTION_CAPTURE_NUM_MIPS - 1);
    return t * t;
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
    // ab.y is the achromatic grazing (white-mirror) term; gate it so rough env reflections don't rim.
    return specularColor * ab.x + SpecularGrazingGate(roughness) * saturate(50.0 * specularColor.g) * ab.y;
}

// Importance sampled preintegrated G * F
float3 EnvBRDF(Texture2D preIntegratedGF, float3 specularColor, float roughness, float NoV)
{
    float2 ab = preIntegratedGF.SampleLevel(SamplerLinearClamp, float2(NoV, roughness), 0).rg;
    return specularColor * ab.x + SpecularGrazingGate(roughness) * saturate(50.0 * specularColor.g) * ab.y;
}

float RoughnessToSpecularPower(float roughness)
{
    return pow(2, 13 * (1 - roughness));
}

#endif
