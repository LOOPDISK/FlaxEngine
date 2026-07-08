// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __GAMMA_CORRECTION_COMMON__
#define __GAMMA_CORRECTION_COMMON__

#include "./Flax/Math.hlsl"

// Shared configs for Color Grading LUT
#define COLOR_GRADING_LUT_LOG 1
#define COLOR_GRADING_LUT_SCALE 1.04f

// Fast reversible tonemapper
// http://gpuopen.com/optimized-reversible-tonemapper-for-resolve/
float3 FastTonemap(float3 c)
{
    return c * rcp(max(max(c.r, c.g), c.b) + 1.0);
}

float4 FastTonemap(float4 c)
{
    return float4(FastTonemap(c.rgb), c.a);
}

float3 FastTonemap(float3 c, float w)
{
    return c * (w * rcp(max(max(c.r, c.g), c.b) + 1.0));
}

float4 FastTonemap(float4 c, float w)
{
    return float4(FastTonemap(c.rgb, w), c.a);
}

float3 FastTonemapInvert(float3 c)
{
    return c * rcp(1.0 - max(max(c.r, c.g), c.b));
}

float4 FastTonemapInvert(float4 c)
{
    return float4(FastTonemapInvert(c.rgb), c.a);
}

float LinearToSrgb(float linearColor)
{
    if (linearColor < 0.00313067)
        return linearColor * 12.92;
    return pow(linearColor, (1.0 / 2.4)) * 1.055 - 0.055;
}

float3 LinearToSrgb(float3 linearColor)
{
    return float3(LinearToSrgb(linearColor.r), LinearToSrgb(linearColor.g), LinearToSrgb(linearColor.b));
}

float3 sRGBToLinear(float3 color)
{
    color = max(6.10352e-5, color);
    return select(color > 0.04045, pow(color * (1.0 / 1.055) + 0.0521327, 2.4), color * (1.0 / 12.92));
}

float3 LogToLinear(float3 logColor)
{
    const float linearRange = 14.0f;
    const float linearGrey = 0.18f;
    const float exposureGrey = 444.0f;
    return exp2((logColor - exposureGrey / 1023.0) * linearRange) * linearGrey;
}

float3 LinearToLog(float3 linearColor)
{
    const float linearRange = 14.0f;
    const float linearGrey = 0.18f;
    const float exposureGrey = 444.0f;
    return saturate(log2(linearColor) / linearRange - log2(linearGrey) / linearRange + exposureGrey / 1023.0f);
}

// MEDPOLE analytic tonemapper (Lu-substrate). Monotone rational tone curve acting on MAX(R,G,B),
// in-gamut by construction, branchless. MUST be evaluated per-pixel - it's ~15 flops, cheaper than a
// LUT fetch, and baking it into the 32-node grading LUT prints contour rings where the toe/shoulder
// steepen (a linear-interpolated node can't track the curve). SSOT shared by the composite (per-pixel)
// and the color-grading LUT bake. See medpole_operator design spec.
float3 MedpoleToneMap(float3 color, float toe, float shoulder, float saturation, float pathToWhite)
{
    color = max(color, 0.0);                                     // kill negative inputs (the one load-bearing max)
    float mx = max(color.r, max(color.g, color.b));
    float mn = min(color.r, min(color.g, color.b));
    float poly = (1.0 - toe) + mx * (toe + mx * shoulder);      // (1-toe) + toe*mx + shoulder*mx^2, coeffs >= 0
    float N = mx * poly;                                        // monotone up
    float den = 1.0 + N;                                        // rational pole, never degenerate
    float zp = N / den;                                         // tone curve in [0,1)
    float scale = poly / den;                                  // = zp/mx, finite at mx=0; compression proxy
    float roll = 1.0 - pathToWhite * (1.0 - scale);           // path-to-white: 1 (shadow) -> 1-pathToWhite (highlight)
    float w = scale * saturation * roll;                      // per-pixel desaturation weight
    float wcap = zp / max(mx - mn, 1e-6);                     // saturation ceiling: keeps darkest channel >= 0
    w = min(w, wcap);                                         // inert for sat<=1; pins sat>1 boost to gamut hull
    return max(zp - w * (mx - color), 0.0);                  // recompose; out in [0, zp], the max() an inert guard
}

#endif
