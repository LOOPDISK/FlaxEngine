// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef STYLIZED_CLOUD_HLSL
#define STYLIZED_CLOUD_HLSL

float StylizedCloudSafeInvLerp(float a, float b, float value)
{
    const float d = max(abs(b - a), 1e-5f);
    return saturate((value - a) / d);
}

#endif

