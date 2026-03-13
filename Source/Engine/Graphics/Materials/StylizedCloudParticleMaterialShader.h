#pragma once

#include "MaterialShader.h"

/// <summary>
/// Represents material that can be used to render stylized clouds on particle model instances.
/// </summary>
class StylizedCloudParticleMaterialShader : public MaterialShader
{
private:
    GPUPipelineState* _psCloudPrePass = nullptr;
    GPUPipelineState* _psCloudPrePassRibbon = nullptr;

public:
    /// <summary>
    /// Init
    /// </summary>
    /// <param name="name">Material resource name</param>
    StylizedCloudParticleMaterialShader(const StringView& name)
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
