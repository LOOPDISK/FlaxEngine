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
    uint32 ShadowsBufferAddress;
    bool HasShadow;

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
/// GPU-layout local light struct shared by StylizedCloud material shaders.
/// Must match CloudLocalLight in StylizedCloud.hlsl.
/// </summary>
PACK_STRUCT(struct StylizedCloudLocalLight {
    Float3 Position;
    float Radius;
    Float3 Color;
    float FalloffExponent;
    Float3 Direction;
    float SpotCosOuterCone;
    float SpotInvCosConeDiff;
    Float3 LightPadding;
    });

/// <summary>
/// Copies lighting data from StylizedCloudCustomData into a material shader constant buffer struct.
/// Works with any struct that has the matching lighting field names.
/// </summary>
template<typename T>
inline void BindStylizedCloudLightingData(T* materialData, const StylizedCloudCustomData* customData)
{
    if (customData)
    {
        materialData->SunDirection = customData->SunDirection;
        materialData->SunIntensity = customData->SunIntensity;
        materialData->SkyIntensity = customData->SkyIntensity;
        materialData->SunColor = customData->SunColor;
        materialData->SkyColor = customData->SkyColor;
        materialData->LocalLightCount = customData->LocalLightCount;
        materialData->ShadowsBufferAddress = customData->ShadowsBufferAddress;
        materialData->HasShadow = customData->HasShadow ? 1 : 0;
        for (int32 i = 0; i < customData->LocalLightCount; i++)
        {
            auto& src = customData->LocalLights[i];
            auto& dst = materialData->LocalLights[i];
            dst.Position = src.Position;
            dst.Radius = src.Radius;
            dst.Color = src.Color;
            dst.FalloffExponent = src.FalloffExponent;
            dst.Direction = src.Direction;
            dst.SpotCosOuterCone = src.SpotCosOuterCone;
            dst.SpotInvCosConeDiff = src.SpotInvCosConeDiff;
            dst.LightPadding = Float3::Zero;
        }
    }
    else
    {
        materialData->SunDirection = Float3::UnitY;
        materialData->SunIntensity = 1.0f;
        materialData->SunColor = Float3::One;
        materialData->SkyIntensity = 0.5f;
        materialData->SkyColor = Float3(0.4f, 0.5f, 0.7f);
        materialData->LocalLightCount = 0;
        materialData->ShadowsBufferAddress = 0;
        materialData->HasShadow = 0;
    }
}

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
