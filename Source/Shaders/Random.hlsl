// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __RANDOM__
#define __RANDOM__

float PseudoRandom(float2 xy)
{
    float2 p = frac(xy / 128.0f) * 128.0f + float2(-64.340622f, -72.465622f);
    return frac(dot(p.xyx * p.xyy, float3(20.390625f, 60.703125f, 2.4281209f)));
}

// Generic noise (1-component)
float RandN1(float n)
{
    return frac(sin(n) * 43758.5453123);
}

// Generic noise (2-components)
float2 RandN2(float2 n)
{
    return frac(sin(dot(n, float2(12.9898, 78.233))) * float2(43758.5453123, 28001.8384));
}

void FindBestAxisVectors(float3 input, out float3 axis1, out float3 axis2)
{
    const float3 a = abs(input);
    if (a.z > a.x && a.z > a.y)
        axis1 = float3(1, 0, 0);
    else
        axis1 = float3(0, 0, 1);
    axis1 = normalize(axis1 - input * dot(axis1, input));
    axis2 = cross(axis1, input);
}

float PerlinRamp(in float t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

float2 PerlinRamp(in float2 t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

float3 PerlinRamp(in float3 t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

float4 PerlinRamp(in float4 t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

// Interleaved Gradient Noise (screen-space stable dither/blue noise)
// Based on: http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float InterleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

#endif
