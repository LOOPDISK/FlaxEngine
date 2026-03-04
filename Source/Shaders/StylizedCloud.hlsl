#ifndef STYLIZED_CLOUD_HLSL
#define STYLIZED_CLOUD_HLSL

#define STYLIZED_CLOUD_MAX_LOCAL_LIGHTS 8

struct CloudLocalLight
{
    float3 Position;
    float Radius;
    float3 Color;
    float FalloffExponent;
    float3 Direction;
    float SpotCosOuterCone;
    float SpotInvCosConeDiff;
    float3 LightPadding;
};

float StylizedCloudSafeInvLerp(float a, float b, float value)
{
    const float d = max(abs(b - a), 1e-5f);
    return saturate((value - a) / d);
}

// Computes stylized cloud lighting from sun, sky, and local lights.
// sunShadow should be pre-computed from shadow sampling before calling.
float3 ComputeStylizedCloudLighting(
    float3 worldPos, float3 normal, float3 viewPos,
    float3 sunDirection, float sunIntensity, float3 sunColor,
    float skyIntensity, float3 skyColor,
    float sunShadow,
    int localLightCount, CloudLocalLight localLights[STYLIZED_CLOUD_MAX_LOCAL_LIGHTS])
{
    float3 l = -normalize(sunDirection);
    float NoL = dot(normal, l);
    float wrappedNoL = saturate((NoL + 0.25f) / 1.25f);
    float hemi = saturate(normal.y * 0.5f + 0.5f);
    float3 v = normalize(viewPos - worldPos);
    float rim = pow(1.0f - saturate(dot(normal, v)), 2.0f);
    float3 sun = sunColor * sunIntensity;
    float3 sky = skyColor * skyIntensity;

    float3 lightColor = sun * sunShadow * (0.1f + 0.9f * wrappedNoL)
                       + sky * (0.15f + 0.85f * hemi);
    lightColor += sun * sunShadow * (rim * 0.2f);

    // Local lights (point + spot)
    for (int li = 0; li < localLightCount; li++)
    {
        float3 lightVec = localLights[li].Position - worldPos;
        float dist = length(lightVec);
        float3 lDir = lightVec / max(dist, 0.001f);
        float atten = pow(saturate(1.0f - dist / max(localLights[li].Radius, 0.001f)), localLights[li].FalloffExponent);

        // Spot cone attenuation
        if (localLights[li].SpotCosOuterCone > -0.5f)
        {
            float cosAngle = dot(-lDir, localLights[li].Direction);
            atten *= saturate((cosAngle - localLights[li].SpotCosOuterCone) * localLights[li].SpotInvCosConeDiff);
        }

        // Clouds are translucent: omnidirectional scatter plus soft directional term
        float localNoL = dot(normal, lDir);
        float localWrapped = saturate((localNoL + 0.25f) / 1.25f);
        float translucent = 0.55f;
        float directional = 0.45f * localWrapped;
        // Forward scattering: bright halo when looking toward light through cloud
        float VdotL = saturate(dot(v, -lDir));
        float forwardScatter = pow(VdotL, 4.0f) * 0.35f;
        lightColor += localLights[li].Color * atten * (translucent + directional + forwardScatter);
    }

    return lightColor;
}

#endif
