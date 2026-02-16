// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RendererPass.h"
#include "Engine/Graphics/GPUPipelineStatePermutations.h"
#include "Engine/Content/Assets/CubeTexture.h"

/// <summary>
/// Stylized cloud rendering service.
/// </summary>
class StylizedCloudPass : public RendererPass<StylizedCloudPass>
{
private:
    GPU_CB_STRUCT(Data {
        ShaderGBufferData GBuffer;
        Float2 TexelSize;
        Float2 OutputSize;
        float BlurSigmaBase;
        float BlurDepthScale;
        float DistortionStrength;
        float AlphaThreshold;
        float SoftIntersectionDistance;
        Float3 SunDirection;
        float SunIntensity;
        Float3 SunColor;
        float SkyIntensity;
        Float3 SkyColor;
        float Time;
        Float2 DepthRange;
        float DistanceSharpenStart;
        float DistanceSharpenEnd;
        Float3 Padding0;
        Matrix ViewProjection;
        Matrix InvViewProjection;
        });

    GPU_CB_STRUCT(PerCloud {
        Matrix WorldMatrix;
        float SunIntensity;
        float SkyIntensity;
        float DistortionScale;
        float AlphaThreshold;
        float Density;
        Float3 LightningColor;
        float LightningIntensity;
        });

    AssetReference<Shader> _shader;
    GPUPipelineState* _psCloudPrePass = nullptr;
    GPUPipelineStatePermutationsPs<2> _psGaussianBlur;
    GPUPipelineStatePermutationsPs<2> _psBoxBlur;
    GPUPipelineState* _psComposite = nullptr;
    AssetReference<CubeTexture> _distortionCubeMap;

private:
#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj)
    {
        if (_psCloudPrePass)
            _psCloudPrePass->ReleaseGPU();
        _psGaussianBlur.Release();
        _psBoxBlur.Release();
        if (_psComposite)
            _psComposite->ReleaseGPU();
        invalidateResources();
    }
#endif

public:
    /// <summary>
    /// Renders stylized clouds and composites them into frame buffer.
    /// </summary>
    void Render(RenderContext& renderContext, GPUTexture*& frameBuffer);

public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

protected:
    // [RendererPass]
    bool setupResources() override;
};
