// Copyright (c) Wojciech Figat. All rights reserved.

#include "PostProcessingPass.h"
#include "RenderList.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/RenderTools.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Engine/Time.h"

#define GB_RADIUS 6
#define GB_KERNEL_SIZE (GB_RADIUS * 2 + 1)
#define OUTPUT_LINEAR 0 // Copies scene color directly to the output
#define OUTPUT_SRGB 1 // Converts scene color from linear to sRGB

GPU_CB_STRUCT(Data{
    float DepthHazeIntensity; // Overall depth haze strength multiplier
    float DepthHazeNearDistance; // Distance where fog starts
    float DepthHazeFarDistance;  // Distance where fog is maximum
    float DepthHazePower;        // Power curve for depth falloff

    float DepthHazeMaxMipLevel;  // Maximum mip level for depth haze blur
    float DepthHazeChromaticDispersion; // Chromatic dispersion strength
    float DepthHazeMipCount;
    float DepthHazePadding0;

    float BloomIntensity; // Overall bloom strength multiplier
    float BloomClamp; // Maximum brightness limit for bloom
    float BloomThreshold; // Luminance threshold where bloom begins
    float BloomThresholdKnee; // Controls the threshold rolloff curve

    float BloomBaseMix; // Base mip contribution
    float BloomHighMix; // High mip contribution
    float BloomMipCount;
    float BloomLayer;

    Float3 VignetteColor;
    float VignetteShapeFactor;

    Float2 InputSize;
    float InputAspect;
    float GrainAmount;

    float GrainTime;
    float GrainParticleSize;
    int32 Ghosts;
    float HaloWidth;

    float HaloIntensity;
    float Distortion;
    float GhostDispersal;
    float LensFlareIntensity;

    Float2 LensInputDistortion;
    float LensScale;
    float LensBias;

    Float2 InvInputSize;
    float ChromaticDistortion;
    float Time;

    uint32 OutputColorSpace;
    float PostExposure;
    float VignetteIntensity;
    float LensDirtIntensity;

    Color ScreenFadeColor;

    Float3 QuantizationError;
    float Dummy2;

    Matrix LensFlareStarMat;
    Float4 ViewInfo;
    float ViewFar;
    float DummyPadding1;
    float DummyPadding2;
    float DummyPadding3;
    });

GPU_CB_STRUCT(GaussianBlurData{
    Float2 Size;
    float Dummy3;
    float Dummy4;
    Float4 GaussianBlurCache[GB_KERNEL_SIZE]; // x-weight, y-offset
    });

String PostProcessingPass::ToString() const
{
    return TEXT("PostProcessingPass");
}

bool PostProcessingPass::Init()
{
    // Create pipeline states
    _psDepthHazeDualFilterUpsample = GPUDevice::Instance->CreatePipelineState();
    _psDepthHazePremultCopy = GPUDevice::Instance->CreatePipelineState();
    _psDepthHazeMarchingDownsample = GPUDevice::Instance->CreatePipelineState();
    _psDepthHazeComposite = GPUDevice::Instance->CreatePipelineState();
    _psBloomBrightPass = GPUDevice::Instance->CreatePipelineState();
    _psBloomDownsample = GPUDevice::Instance->CreatePipelineState();
    _psBloomDualFilterUpsample = GPUDevice::Instance->CreatePipelineState();
    _psBlurH = GPUDevice::Instance->CreatePipelineState();
    _psBlurV = GPUDevice::Instance->CreatePipelineState();
    _psGenGhosts = GPUDevice::Instance->CreatePipelineState();
    _psComposite.CreatePipelineStates();

    // Load shader
    _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/PostProcessing"));
    if (_shader == nullptr)
        return true;
#if COMPILE_WITH_DEV_ENV
    _shader.Get()->OnReloading.Bind<PostProcessingPass, &PostProcessingPass::OnShaderReloading>(this);
#endif

    return false;
}

bool PostProcessingPass::setupResources()
{
    // Wait for shader
    if (!_shader->IsLoaded())
        return true;
    auto shader = _shader->GetShader();
    CHECK_INVALID_SHADER_PASS_CB_SIZE(shader, 0, Data);
    CHECK_INVALID_SHADER_PASS_CB_SIZE(shader, 1, GaussianBlurData);

    // Create pipeline stages
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    if (!_psDepthHazeDualFilterUpsample->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_DepthHazeDualFilterUpsample");
        if (_psDepthHazeDualFilterUpsample->Init(psDesc))
            return true;
    }
    if (!_psDepthHazePremultCopy->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_DepthHazePremultCopy");
        if (_psDepthHazePremultCopy->Init(psDesc))
            return true;
    }
    if (!_psDepthHazeMarchingDownsample->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_DepthHazeMarchingDownsample");
        if (_psDepthHazeMarchingDownsample->Init(psDesc))
            return true;
    }
    if (!_psDepthHazeComposite->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_DepthHazeComposite");
        if (_psDepthHazeComposite->Init(psDesc))
            return true;
    }
    if (!_psBloomBrightPass->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_BloomBrightPass");
        if (_psBloomBrightPass->Init(psDesc))
            return true;
    }
    if (!_psBloomDownsample->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_BloomDownsample");
        if (_psBloomDownsample->Init(psDesc))
            return true;
    }
    if (!_psBloomDualFilterUpsample->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_BloomDualFilterUpsample");
        if (_psBloomDualFilterUpsample->Init(psDesc))
            return true;
    }
    if (!_psBlurH->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_GaussainBlurH");
        if (_psBlurH->Init(psDesc))
            return true;
    }
    if (!_psBlurV->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_GaussainBlurV");
        if (_psBlurV->Init(psDesc))
            return true;
    }
    if (!_psGenGhosts->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_Ghosts");
        if (_psGenGhosts->Init(psDesc))
            return true;
    }
    if (!_psComposite.IsValid())
    {
        if (_psComposite.Create(psDesc, shader, "PS_Composite"))
            return true;
    }

    return false;
}

GPUTexture* GetCustomOrDefault(Texture* customTexture, AssetReference<Texture>& defaultTexture, const Char* defaultName)
{
    // Check if use custom texture
    if (customTexture)
        return customTexture->GetTexture();

    // Check if need to load default texture
    if (defaultTexture == nullptr)
    {
        // Load default
        defaultTexture = Content::LoadAsyncInternal<Texture>(defaultName);
    }

    // Use default texture or nothing
    return defaultTexture ? defaultTexture->GetTexture() : nullptr;
}

/// <summary>
/// Calculates the Gaussian blur filter kernel. This implementation is
/// ported from the original Java code appearing in chapter 16 of
/// "Filthy Rich Clients: Developing Animated and Graphical Effects for Desktop Java".
/// </summary>
/// <param name="sigma">Gaussian Blur sigma parameter</param>
/// <param name="width">Texture to blur width in pixels</param>
/// <param name="height">Texture to blur height in pixels</param>
void GB_ComputeKernel(float sigma, float width, float height, Float4 gaussianBlurCacheH[GB_KERNEL_SIZE], Float4 gaussianBlurCacheV[GB_KERNEL_SIZE])
{
    float total = 0.0f;
    float twoSigmaSquare = 2.0f * sigma * sigma;
    float sigmaRoot = Math::Sqrt(twoSigmaSquare * PI);
    float xOffset = 1.0f / width;
    float yOffset = 1.0f / height;

    // Calculate weights and offsets
    for (int32 i = -GB_RADIUS; i <= GB_RADIUS; i++)
    {
        // Calculate pixel distance and index
        const float distance = static_cast<float>(i * i);
        const int32 index = i + GB_RADIUS;

        // Calculate pixel weight
        const float weight = Math::Exp(-distance / twoSigmaSquare) / sigmaRoot;

        // Calculate total weights sum
        total += weight;

        gaussianBlurCacheH[index] = Float4(weight, i * xOffset, 0, 0);
        gaussianBlurCacheV[index] = Float4(weight, i * yOffset, 0, 0);
    }

    // Normalize weights
    for (int32 i = 0; i < GB_KERNEL_SIZE; i++)
    {
        gaussianBlurCacheH[i].X /= total;
        gaussianBlurCacheV[i].X /= total;
    }
}

void PostProcessingPass::Dispose()
{
    // Base
    RendererPass::Dispose();

    // Cleanup
    SAFE_DELETE_GPU_RESOURCE(_psDepthHazeDualFilterUpsample);
    SAFE_DELETE_GPU_RESOURCE(_psDepthHazePremultCopy);
    SAFE_DELETE_GPU_RESOURCE(_psDepthHazeMarchingDownsample);
    SAFE_DELETE_GPU_RESOURCE(_psDepthHazeComposite);
    SAFE_DELETE_GPU_RESOURCE(_psBloomBrightPass);
    SAFE_DELETE_GPU_RESOURCE(_psBloomDownsample);
    SAFE_DELETE_GPU_RESOURCE(_psBloomDualFilterUpsample);
    SAFE_DELETE_GPU_RESOURCE(_psBlurH);
    SAFE_DELETE_GPU_RESOURCE(_psBlurV);
    SAFE_DELETE_GPU_RESOURCE(_psGenGhosts);
    _psComposite.Delete();
    _shader = nullptr;
    _defaultLensColor = nullptr;
    _defaultLensDirt = nullptr;
    _defaultLensStar = nullptr;
}

int32 CalculateBloomMipCount(int32 width, int32 height)
{
    // Calculate the smallest dimension
    int32 minDimension = Math::Min(width, height);

    // Calculate how many times we can half the dimension until we hit a minimum size
    // (e.g., 16x16 pixels as the smallest mip)
    const int32 MIN_MIP_SIZE = 16;
    int32 mipCount = 1;
    while (minDimension > MIN_MIP_SIZE)
    {
        minDimension /= 2;
        mipCount++;
    }
    return mipCount;
}

static int32 mipSize(const int32 baseSize, int32 mip)
{
    return Math::Max(baseSize >> mip, 1);
}

void PostProcessingPass::Render(RenderContext& renderContext, GPUTexture* input, GPUTexture* output, GPUTexture* colorGradingLUT)
{
    PROFILE_GPU_CPU("Post Processing");
    auto device = GPUDevice::Instance;
    auto context = device->GetMainContext();
    auto& view = renderContext.View;

    context->ResetRenderTarget();

    PostProcessSettings& settings = renderContext.List->Settings;

    bool useDepthHaze = EnumHasAnyFlags(view.Flags, ViewFlags::DepthHaze) && settings.DepthHaze.Enabled && settings.DepthHaze.Intensity > 0.0f;
    bool useBloom = EnumHasAnyFlags(view.Flags, ViewFlags::Bloom) && settings.Bloom.Enabled && settings.Bloom.Intensity > 0.0f;
    bool useToneMapping = EnumHasAnyFlags(view.Flags, ViewFlags::ToneMapping) && settings.ToneMapping.Mode != ToneMappingMode::None;
    bool useCameraArtifacts = EnumHasAnyFlags(view.Flags, ViewFlags::CameraArtifacts) && (settings.CameraArtifacts.VignetteIntensity > 0.0f || settings.CameraArtifacts.GrainAmount > 0.0f || settings.CameraArtifacts.ChromaticDistortion > 0.0f || settings.CameraArtifacts.ScreenFadeColor.A > 0.0f);
    bool useLensFlares = EnumHasAnyFlags(view.Flags, ViewFlags::LensFlares) && settings.LensFlares.Intensity > 0.0f && useBloom;

    // Cache viewport sizes
    int32 w1 = input->Width();
    int32 w2 = w1 >> 1;
    int32 w4 = w2 >> 1;
    int32 w8 = w4 >> 1;
    int32 h1 = input->Height();
    int32 h2 = h1 >> 1;
    int32 h4 = h2 >> 1;
    int32 h8 = h4 >> 1;
    int32 bloomMipCount = CalculateBloomMipCount(w1, h1);

    // Ensure to have valid data and if at least one effect should be applied
    if (!(useDepthHaze || useBloom || useToneMapping || useCameraArtifacts || colorGradingLUT) || checkIfSkipPass() || w8 <= 1 || h8 <= 1)
    {
        // Resources are missing. Do not perform rendering. Just copy raw frame
        context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
        context->SetRenderTarget(*output);
        context->Draw(input);
        return;
    }

    // Cache data
    auto shader = _shader->GetShader();
    auto cb0 = shader->GetCB(0);
    auto cb1 = shader->GetCB(1);

    ////////////////////////////////////////////////////////////////////////////////////
    // Setup shader

    Data data;
    float time = Time::Draw.UnscaledTime.GetTotalSeconds();
    data.Time = Math::Fractional(time);
    if (useCameraArtifacts)
    {
        data.VignetteColor = settings.CameraArtifacts.VignetteColor;
        data.VignetteIntensity = settings.CameraArtifacts.VignetteIntensity;
        data.VignetteShapeFactor = settings.CameraArtifacts.VignetteShapeFactor;
        data.GrainAmount = settings.CameraArtifacts.GrainAmount;
        data.GrainParticleSize = Math::Max(0.0001f, settings.CameraArtifacts.GrainParticleSize);
        data.GrainTime = time * 0.5f * settings.CameraArtifacts.GrainSpeed;
        data.ChromaticDistortion = Math::Saturate(settings.CameraArtifacts.ChromaticDistortion * (float)output->Width() / 1080.0f); // Rescale based on reference 1080p resolution
        data.ScreenFadeColor = settings.CameraArtifacts.ScreenFadeColor;
    }
    else
    {
        data.VignetteIntensity = 0;
        data.GrainAmount = 0;
        data.ChromaticDistortion = 0;
        data.ScreenFadeColor = Color::Transparent;
    }
    if (useDepthHaze)
    {
        data.DepthHazeIntensity = 0.0f; // Disable in composite pass as it's already rendered in RenderDepthHaze
        data.DepthHazeNearDistance = settings.DepthHaze.NearDistance;
        data.DepthHazeFarDistance = settings.DepthHaze.FarDistance;
        data.DepthHazePower = settings.DepthHaze.Power;
        data.DepthHazeMaxMipLevel = settings.DepthHaze.MaxMipLevel;
        data.DepthHazeChromaticDispersion = settings.DepthHaze.ChromaticDispersion;
        data.DepthHazeMipCount = (float)bloomMipCount;
    }
    else
    {
        data.DepthHazeIntensity = 0;
        data.DepthHazeNearDistance = 0;
        data.DepthHazeFarDistance = 0;
        data.DepthHazeMaxMipLevel = 0;
        data.DepthHazeChromaticDispersion = 0;
    }
    if (useBloom)
    {
        data.BloomIntensity = settings.Bloom.Intensity;
        data.BloomClamp = settings.Bloom.Clamp;
        data.BloomThreshold = settings.Bloom.Threshold;
        data.BloomThresholdKnee = settings.Bloom.ThresholdKnee;
        data.BloomBaseMix = settings.Bloom.BaseMix;
        data.BloomHighMix = settings.Bloom.HighMix;
        data.BloomMipCount = (float)bloomMipCount;
        data.BloomLayer = 0.0f;
    }
    else
    {
        data.BloomIntensity = 0;
    }
    if (useLensFlares)
    {
        data.LensFlareIntensity = settings.LensFlares.Intensity;
        data.LensDirtIntensity = settings.LensFlares.LensDirtIntensity;
        data.Ghosts = settings.LensFlares.Ghosts;

        data.HaloWidth = settings.LensFlares.HaloWidth;
        data.HaloIntensity = settings.LensFlares.HaloIntensity;
        data.Distortion = settings.LensFlares.Distortion;
        data.GhostDispersal = settings.LensFlares.GhostDispersal;

        data.LensBias = settings.LensFlares.ThresholdBias;
        data.LensScale = settings.LensFlares.ThresholdScale;
        data.LensInputDistortion = Float2(-(1.0f / w4) * settings.LensFlares.Distortion, (1.0f / w4) * settings.LensFlares.Distortion);

        // Calculate star texture rotation matrix
        Float3 camX = renderContext.View.View.GetRight();
        Float3 camZ = renderContext.View.View.GetBackward();
        float camRot = Float3::Dot(camX, Float3::Forward) + Float3::Dot(camZ, Float3::Up);
        float camRotCos = Math::Cos(camRot) * 0.8f;
        float camRotSin = Math::Sin(camRot) * 0.8f;
        Matrix rotation(
            camRotCos, -camRotSin, 0.0f, 0.0f,
            camRotSin, camRotCos, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.01f, 1.0f
        );
        data.LensFlareStarMat = rotation;
    }
    else
    {
        data.LensFlareIntensity = 0;
        data.LensDirtIntensity = 0;
    }
    data.QuantizationError = RenderTools::GetColorQuantizationError(output->Format());
    data.PostExposure = Math::Exp2(settings.EyeAdaptation.PostExposure);
    data.InputSize = Float2(static_cast<float>(w1), static_cast<float>(h1));
    data.InvInputSize = Float2(1.0f / static_cast<float>(w1), 1.0f / static_cast<float>(h1));
    data.InputAspect = static_cast<float>(w1) / h1;
    data.ViewInfo = view.ViewInfo;
    data.ViewFar = view.Far;
    if (Graphics::GammaColorSpace)
    {
        // Gamma-space colors always present image 'as-is'
        data.OutputColorSpace = OUTPUT_LINEAR;
    }
    else
    {
        // Convert linear scene color into the display's sRGB curve
        data.OutputColorSpace = OUTPUT_SRGB;
    }
    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);

    ////////////////////////////////////////////////////////////////////////////////////
    // Atmospheric Scattering Mip Chains
    // NOTE: Depth haze is now rendered in a separate pass (RenderDepthHaze) after the forward pass
    // This section is kept only for reference and is no longer executed

    ////////////////////////////////////////////////////////////////////////////////////
    // Bloom

    auto tempDesc = GPUTextureDescription::New2D(w2, h2, bloomMipCount, output->Format(), GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget | GPUTextureFlags::PerMipViews);
    GPUTexture* bloomBuffer1 = nullptr, *bloomBuffer2 = nullptr;
    if (useBloom || useLensFlares)
    {
        bloomBuffer1 = RenderTargetPool::Get(tempDesc);
        bloomBuffer2 = RenderTargetPool::Get(tempDesc);
        RENDER_TARGET_POOL_SET_NAME(bloomBuffer1, "PostProcessing.Bloom");
        RENDER_TARGET_POOL_SET_NAME(bloomBuffer2, "PostProcessing.Bloom");

        // TODO: skip this clear? or do it at once for the whole textures (2 calls instead of per-mip)
        for (int32 mip = 0; mip < bloomMipCount; mip++)
        {
            context->Clear(bloomBuffer1->View(0, mip), Color::Transparent);
            context->Clear(bloomBuffer2->View(0, mip), Color::Transparent);
        }
    }

    if (useBloom)
    {
        PROFILE_GPU("Bloom");
        context->SetRenderTarget(bloomBuffer1->View(0, 0));
        context->SetViewportAndScissors((float)w2, (float)h2);
        context->BindSR(0, input->View());
        context->SetState(_psBloomBrightPass);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Progressive downsamples
        for (int32 mip = 1; mip < bloomMipCount; mip++)
        {
            const int32 mipWidth = mipSize(w2, mip);
            const int32 mipHeight = mipSize(h2, mip);

            context->SetRenderTarget(bloomBuffer1->View(0, mip));
            context->SetViewportAndScissors((float)mipWidth, (float)mipHeight);
            context->BindSR(0, bloomBuffer1->View(0, mip - 1));
            context->SetState(_psBloomDownsample);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        // Progressive upsamples
        for (int32 mip = bloomMipCount - 2; mip >= 0; mip--)
        {
            auto upscaleBuffer = bloomBuffer2;
            if (mip == bloomMipCount - 2)
            {
                // If it's the first, copy the chain over
                upscaleBuffer = bloomBuffer1;
            }
            const int32 mipWidth = mipSize(w2, mip);
            const int32 mipHeight = mipSize(h2, mip);

            data.BloomLayer = static_cast<float>(mip);
            context->UpdateCB(cb0, &data);
            context->SetRenderTarget(bloomBuffer2->View(0, mip));
            context->SetViewportAndScissors((float)mipWidth, (float)mipHeight);
            context->BindSR(0, upscaleBuffer->View(0, mip + 1));
            context->BindSR(1, bloomBuffer1->View(0, mip + 1));
            context->SetState(_psBloomDualFilterUpsample);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        // Set bloom output
        context->UnBindSR(0);
        context->UnBindSR(1);
        context->BindSR(2, bloomBuffer2->View(0, 0));
    }
    else
    {
        context->UnBindSR(2);
    }

    ////////////////////////////////////////////////////////////////////////////////////
    // Lens Flares

    // Check if use lens flares
    if (useLensFlares)
    {
        PROFILE_GPU("Lens Flares");

        // Prepare lens flares helper textures
        context->BindSR(5, GetCustomOrDefault(settings.LensFlares.LensStar, _defaultLensStar, TEXT("Engine/Textures/DefaultLensStarburst")));
        context->BindSR(6, GetCustomOrDefault(settings.LensFlares.LensColor, _defaultLensColor, TEXT("Engine/Textures/DefaultLensColor")));

        // Render lens flares
        context->SetRenderTarget(bloomBuffer2->View(0, 1));
        context->SetViewportAndScissors((float)w4, (float)h4);
        context->BindSR(3, bloomBuffer1->View(0, 1)); // Use mip 1 of bloomBuffer1 as source
        context->SetState(_psGenGhosts);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->UnBindSR(3);

        // Gaussian blur kernel
        GaussianBlurData gbData;
        Float4 GaussianBlurCacheH[GB_KERNEL_SIZE];
        Float4 GaussianBlurCacheV[GB_KERNEL_SIZE];
        gbData.Size = Float2(static_cast<float>(w4), static_cast<float>(h4));
        GB_ComputeKernel(2.0f, gbData.Size.X, gbData.Size.Y, GaussianBlurCacheH, GaussianBlurCacheV);

        // Gaussian blur H
        Platform::MemoryCopy(gbData.GaussianBlurCache, GaussianBlurCacheH, sizeof(GaussianBlurCacheH));
        context->UpdateCB(cb1, &gbData);
        context->BindCB(1, cb1);
        context->SetRenderTarget(bloomBuffer1->View(0, 1));
        context->BindSR(0, bloomBuffer2->View(0, 1));
        context->SetState(_psBlurH);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Gaussian blur V
        Platform::MemoryCopy(gbData.GaussianBlurCache, GaussianBlurCacheV, sizeof(GaussianBlurCacheV));
        context->UpdateCB(cb1, &gbData);
        context->BindCB(1, cb1);
        context->SetRenderTarget(bloomBuffer2->View(0, 1));
        context->BindSR(0, bloomBuffer1->View(0, 1));
        context->SetState(_psBlurV);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Set lens flares output
        context->BindSR(3, bloomBuffer2->View(0, 1));
    }
    else
    {
        context->BindSR(3, (GPUResourceView*)nullptr);
    }

    ////////////////////////////////////////////////////////////////////////////////////
    // Final composite

    // TODO: consider to use more compute shader for post processing

    // TODO: maybe don't use this rt swap and start using GetTempRt to make this design easier

    // Check if use Tone Mapping + Color Grading LUT
    int32 compositePermutationIndex = 0;
    GPUTextureView* colorGradingLutView = nullptr;
    if (colorGradingLUT)
    {
        if (colorGradingLUT->IsVolume())
        {
            compositePermutationIndex = 1;
            colorGradingLutView = colorGradingLUT->ViewVolume();
        }
        else
        {
            compositePermutationIndex = 2;
            colorGradingLutView = colorGradingLUT->View();
        }
    }

    // Composite pass inputs mapping:
    // - 0 - Input0      - scene color
    // - 1 - Input1      - (unused now - we sample mips directly)
    // - 2 - Input2      - bloom
    // - 3 - Input3      - lens flare color
    // - 4 - LensDirt    - lens dirt texture
    // - 5 - LensStar    - lens star texture
    // - 7 - ColorGradingLUT
    // - 8 - DepthHaze   - depth haze color mip chain (for depth-selective sampling)
    // - 9 - DepthBuffer - raw depth buffer (for reference depth)
    // - 10 - DepthMips  - depth mip chain (for smooth transitions)
    context->BindSR(0, input->View());
    if ((useLensFlares || useBloom) && settings.LensFlares.LensDirtIntensity > 0)
        context->BindSR(4, GetCustomOrDefault(settings.LensFlares.LensDirt, _defaultLensDirt, TEXT("Engine/Textures/DefaultLensDirt")));
    else
        context->BindSR(4, device->GetDefaultBlackTexture());
    context->BindSR(7, colorGradingLutView);

    // Depth haze is now rendered separately before this pass, so don't bind it here
    context->BindSR(8, (GPUResourceView*)nullptr);
    context->BindSR(10, (GPUResourceView*)nullptr);

    // Composite final frame during single pass (done in full resolution)
    context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
    context->SetRenderTarget(*output);
    context->SetState(_psComposite.Get(compositePermutationIndex));
    context->DrawFullscreenTriangle();

    // Cleanup
    RenderTargetPool::Release(bloomBuffer1);
    RenderTargetPool::Release(bloomBuffer2);
}

void PostProcessingPass::RenderDepthHaze(RenderContext& renderContext, GPUTexture* input, GPUTexture* output)
{
    PROFILE_GPU_CPU("Depth Haze");
    auto device = GPUDevice::Instance;
    auto context = device->GetMainContext();
    auto& view = renderContext.View;

    context->ResetRenderTarget();

    PostProcessSettings& settings = renderContext.List->Settings;
    bool useDepthHaze = EnumHasAnyFlags(view.Flags, ViewFlags::DepthHaze) && settings.DepthHaze.Enabled && settings.DepthHaze.Intensity > 0.0f;

    // Cache viewport sizes
    int32 w1 = input->Width();
    int32 w2 = w1 >> 1;
    int32 h1 = input->Height();
    int32 h2 = h1 >> 1;
    int32 bloomMipCount = CalculateBloomMipCount(w1, h1);

    // Check if shader and pipeline states are ready
    if (checkIfSkipPass())
    {
        context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
        context->SetRenderTarget(output->View());
        context->Draw(input);
        return;
    }

    // Cache data
    auto shader = _shader->GetShader();
    auto cb0 = shader->GetCB(0);

    // If depth haze is not enabled or resources are not ready, just copy the input to output
    if (!useDepthHaze || w2 <= 1 || h2 <= 1 || bloomMipCount < 2 || !_psDepthHazeComposite || !_psDepthHazeComposite->IsValid())
    {
        context->ResetRenderTarget();
        context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
        context->SetRenderTarget(output->View());
        context->Draw(input);
        context->ResetRenderTarget();
        return;
    }

    // Setup shader data
    Data data;
    Platform::MemoryClear(&data, sizeof(Data));

    // Depth haze settings
    data.DepthHazeIntensity = Math::Saturate(settings.DepthHaze.Intensity);
    data.DepthHazeNearDistance = settings.DepthHaze.NearDistance;
    data.DepthHazeFarDistance = settings.DepthHaze.FarDistance;
    data.DepthHazePower = settings.DepthHaze.Power;
    data.DepthHazeMaxMipLevel = settings.DepthHaze.MaxMipLevel;
    data.DepthHazeChromaticDispersion = settings.DepthHaze.ChromaticDispersion;
    data.DepthHazeMipCount = (float)bloomMipCount;

    // BloomLayer doubles as the target mip index for the marching-mask downsample
    data.BloomMipCount = (float)bloomMipCount;
    data.BloomLayer = 0.0f;

    // View settings
    data.ViewInfo = view.ViewInfo;
    data.ViewFar = view.Far;

    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);

    ////////////////////////////////////////////////////////////////////////////////////
    // Atmospheric Scattering Mip Chains

    // Get depth buffer for atmospheric scattering calculation
    GPUTexture* depthBuffer = renderContext.Buffers->DepthBuffer;

    // Premultiplied haze mip chain: rgb = sceneColor * participation, a = participation
    // (needs alpha and HDR range, so the scene color format can't be reused here)
    auto scatteringColorDesc = GPUTextureDescription::New2D(w2, h2, bloomMipCount, PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget | GPUTextureFlags::PerMipViews);
    auto scatteringColorBuffer = RenderTargetPool::Get(scatteringColorDesc);
    RENDER_TARGET_POOL_SET_NAME(scatteringColorBuffer, "DepthHaze.ScatteringColor");
    GPUTexture* scatteringColorBuffer2 = nullptr;

    // Average linear depth of the participating surfaces per mip
    auto depthMipDesc = GPUTextureDescription::New2D(w2, h2, bloomMipCount, PixelFormat::R32_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget | GPUTextureFlags::PerMipViews);
    auto depthMipBuffer = RenderTargetPool::Get(depthMipDesc);
    RENDER_TARGET_POOL_SET_NAME(depthMipBuffer, "DepthHaze.DepthMips");

    if (depthBuffer)
    {
        // Initial half-res premultiplied copy (MRT: color mip 0 + participating depth mip 0)
        GPUTextureView* mip0Targets[2] = { scatteringColorBuffer->View(0, 0), depthMipBuffer->View(0, 0) };
        context->SetRenderTarget(nullptr, Span<GPUTextureView*>(mip0Targets, 2));
        context->SetViewportAndScissors((float)w2, (float)h2);
        context->BindSR(0, input->View());
        context->BindSR(1, depthBuffer->View());
        context->SetState(_psDepthHazePremultCopy);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->UnBindSR(1);

        // Marching-mask downsample chain: each mip re-masks taps against its own depth
        // threshold so it only keeps light from surfaces far enough to select it
        for (int32 mip = 1; mip < bloomMipCount; mip++)
        {
            const int32 mipWidth = mipSize(w2, mip);
            const int32 mipHeight = mipSize(h2, mip);

            data.BloomLayer = static_cast<float>(mip);
            context->UpdateCB(cb0, &data);

            GPUTextureView* mipTargets[2] = { scatteringColorBuffer->View(0, mip), depthMipBuffer->View(0, mip) };
            context->SetRenderTarget(nullptr, Span<GPUTextureView*>(mipTargets, 2));
            context->SetViewportAndScissors((float)mipWidth, (float)mipHeight);
            context->BindSR(0, scatteringColorBuffer->View(0, mip - 1));
            context->BindSR(10, depthMipBuffer->View(0, mip - 1));
            context->SetState(_psDepthHazeMarchingDownsample);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        context->UnBindSR(10);

        // Create second buffer for upsampling
        scatteringColorBuffer2 = RenderTargetPool::Get(scatteringColorDesc);
        RENDER_TARGET_POOL_SET_NAME(scatteringColorBuffer2, "DepthHaze.ScatteringColorUpsample");

        // The upsample loop never writes the top mip - clear it so trilinear reads stay defined
        context->Clear(scatteringColorBuffer2->View(0, bloomMipCount - 1), Color::Transparent);

        // Progressive premultiplied upsamples
        for (int32 mip = bloomMipCount - 2; mip >= 0; mip--)
        {
            auto upscaleBuffer = scatteringColorBuffer2;
            if (mip == bloomMipCount - 2)
            {
                upscaleBuffer = scatteringColorBuffer;
            }
            const int32 mipWidth = mipSize(w2, mip);
            const int32 mipHeight = mipSize(h2, mip);

            context->SetRenderTarget(scatteringColorBuffer2->View(0, mip));
            context->SetViewportAndScissors((float)mipWidth, (float)mipHeight);
            context->BindSR(0, upscaleBuffer->View(0, mip + 1));
            // Cascade with the mip+1 downsample (like bloom): output level K then holds blur level
            // ~K+1 stored 2x oversampled, which the composite can magnify without its bilinear
            // lattice showing; it selects one level lower to compensate for the octave shift
            context->BindSR(1, scatteringColorBuffer->View(0, mip + 1));
            context->SetState(_psDepthHazeDualFilterUpsample);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        context->UnBindSR(0);
        context->UnBindSR(1);
    }

    ////////////////////////////////////////////////////////////////////////////////////
    // Composite depth haze with scene

    // Re-update constant buffer with correct values for composite pass
    // (the downsample loop modified BloomLayer, so we need to restore the correct settings)
    data.BloomLayer = 0.0f;
    context->UpdateCB(cb0, &data);

    context->ResetRenderTarget();
    context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
    context->SetRenderTarget(output->View());
    context->BindSR(0, input->View());
    if (scatteringColorBuffer2)
    {
        context->BindSR(8, scatteringColorBuffer2->View()); // Premultiplied haze mip chain
        context->BindSR(10, depthBuffer->View());           // Full-res depth for pixel-exact masking and mip selection
    }
    else
    {
        context->BindSR(8, (GPUResourceView*)nullptr);
        context->BindSR(10, (GPUResourceView*)nullptr);
    }
    context->SetState(_psDepthHazeComposite);
    context->DrawFullscreenTriangle();

    // Cleanup
    context->ResetRenderTarget();
    context->UnBindSR(0);
    context->UnBindSR(8);
    context->UnBindSR(10);
    RenderTargetPool::Release(scatteringColorBuffer);
    if (scatteringColorBuffer2)
        RenderTargetPool::Release(scatteringColorBuffer2);
    RenderTargetPool::Release(depthMipBuffer);
}
