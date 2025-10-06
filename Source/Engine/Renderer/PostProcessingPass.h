// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RendererPass.h"
#include "Engine/Graphics/GPUPipelineStatePermutations.h"

/// <summary>
/// Post-processing rendering service.
/// </summary>
class PostProcessingPass : public RendererPass<PostProcessingPass>
{
private:
    AssetReference<Shader> _shader;
    GPUPipelineState* _psDepthHazeBrightPass = nullptr;
    GPUPipelineState* _psDepthHazeDownsample = nullptr;
    GPUPipelineState* _psDepthHazeDualFilterUpsample = nullptr;
    GPUPipelineState* _psDepthHazeInitial = nullptr;
    GPUPipelineState* _psDepthHazeUpsample = nullptr;
    GPUPipelineState* _psDepthHazeImprovedCopy = nullptr;
    GPUPipelineState* _psDepthCopy = nullptr;
    GPUPipelineState* _psDepthFrequencySeparation = nullptr;
    GPUPipelineState* _psDepthHazeAdaptiveBilateralDownsample = nullptr;
    GPUPipelineState* _psDepthHazeComposite = nullptr;
    GPUPipelineState* _psBloomBrightPass = nullptr;
    GPUPipelineState* _psBloomDownsample = nullptr;
    GPUPipelineState* _psBloomDualFilterUpsample = nullptr;
    GPUPipelineState* _psBlurH = nullptr;
    GPUPipelineState* _psBlurV = nullptr;
    GPUPipelineState* _psGenGhosts = nullptr;
    GPUPipelineStatePermutationsPs<3> _psComposite;
    AssetReference<Texture> _defaultLensColor;
    AssetReference<Texture> _defaultLensStar;
    AssetReference<Texture> _defaultLensDirt;

public:
    /// <summary>
    /// Perform postFx rendering for the input task
    /// </summary>
    /// <param name="renderContext">The rendering context.</param>
    /// <param name="input">Target with rendered HDR frame to post process</param>
    /// <param name="output">Output frame</param>
    /// <param name="colorGradingLUT">The prebaked LUT for color grading and tonemapping.</param>
    void Render(RenderContext& renderContext, GPUTexture* input, GPUTexture* output, GPUTexture* colorGradingLUT);

    /// <summary>
    /// Renders depth haze composition pass (should be called after forward pass, before UI)
    /// </summary>
    /// <param name="renderContext">The rendering context.</param>
    /// <param name="input">Target with rendered HDR frame</param>
    /// <param name="output">Output frame</param>
    void RenderDepthHaze(RenderContext& renderContext, GPUTexture* input, GPUTexture* output);

private:
#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj)
    {
        _psDepthHazeBrightPass->ReleaseGPU();
        _psDepthHazeDownsample->ReleaseGPU();
        _psDepthHazeDualFilterUpsample->ReleaseGPU();
        _psDepthHazeInitial->ReleaseGPU();
        _psDepthHazeUpsample->ReleaseGPU();
        _psDepthHazeImprovedCopy->ReleaseGPU();
        _psDepthCopy->ReleaseGPU();
        _psDepthFrequencySeparation->ReleaseGPU();
        _psDepthHazeAdaptiveBilateralDownsample->ReleaseGPU();
        _psDepthHazeComposite->ReleaseGPU();
        _psBloomBrightPass->ReleaseGPU();
        _psBloomDownsample->ReleaseGPU();
        _psBloomDualFilterUpsample->ReleaseGPU();
        _psBlurH->ReleaseGPU();
        _psBlurV->ReleaseGPU();
        _psGenGhosts->ReleaseGPU();
        _psComposite.Release();
        invalidateResources();
    }
#endif

public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

protected:
    // [RendererPass]
    bool setupResources() override;
};
