// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "MaterialShader.h"

#define STYLIZED_CLOUD_MAX_LOCAL_LIGHTS 8

/// <summary>
/// Lighting data passed to StylizedCloudMaterialShader via BindParameters::CustomData.
/// </summary>
struct StylizedCloudCustomData
{
    Float3 SunDirection;
    float SunIntensity;
    Float3 SunColor;
    float SkyIntensity;
    Float3 SkyColor;
    int32 LocalLightCount;

    struct LocalLight
    {
        Float3 Position;
        float Radius;
        Float3 Color;
        float FalloffExponent;
        Float3 Direction;
        float SpotCosOuterCone; // cos outer cone for spots, -1 for point lights
        float SpotInvCosConeDiff;
    } LocalLights[STYLIZED_CLOUD_MAX_LOCAL_LIGHTS];
};

/// <summary>
/// Represents material that can be used to render stylized clouds on mesh geometry.
/// </summary>
class StylizedCloudMaterialShader : public MaterialShader
{
private:
    GPUPipelineState* _psCloudPrePass = nullptr;

public:
    /// <summary>
    /// Init
    /// </summary>
    /// <param name="name">Material resource name</param>
    StylizedCloudMaterialShader(const StringView& name)
        : MaterialShader(name)
    {
    }

public:
    // [MaterialShader]
    DrawPass GetDrawModes() const override;
    void Bind(BindParameters& params) override;
    void Unload() override;

protected:
    // [MaterialShader]
    bool Load() override;
};
