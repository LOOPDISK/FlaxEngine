// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#include "PostProcessingPass.h"
#include "RenderList.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Engine/Time.h"

PostProcessingPass::PostProcessingPass()
    : _shader(nullptr)
    , _psThreshold(nullptr)
    , _psScale(nullptr)
    , _psBlendBloom(nullptr)
    , _psKawaseBlur(nullptr)
    , _psBlurH(nullptr)
    , _psBlurV(nullptr)
    , _psGenGhosts(nullptr)
    , _defaultLensColor(nullptr)
    , _defaultLensStar(nullptr)
    , _defaultLensDirt(nullptr)
{
}

String PostProcessingPass::ToString() const
{
    return TEXT("PostProcessingPass");
}

bool PostProcessingPass::Init()
{
    // Create pipeline states
    _psThreshold = GPUDevice::Instance->CreatePipelineState();
    _psScale = GPUDevice::Instance->CreatePipelineState();
    _psKawaseBlur = GPUDevice::Instance->CreatePipelineState();
    _psBlendBloom = GPUDevice::Instance->CreatePipelineState();
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

    // Validate shader constant buffer size
    if (shader->GetCB(0)->GetSize() != sizeof(Data))
    {
        REPORT_INVALID_SHADER_PASS_CB_SIZE(shader, 0, Data);
        return true;
    }
    if (shader->GetCB(1)->GetSize() != sizeof(GaussianBlurData))
    {
        REPORT_INVALID_SHADER_PASS_CB_SIZE(shader, 1, GaussianBlurData);
        return true;
    }

    // Create pipeline stages
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    if (!_psThreshold->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_Threshold");
        if (_psThreshold->Init(psDesc))
            return true;
    }
    if (!_psScale->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_Scale");
        if (_psScale->Init(psDesc))
            return true;
    }
    if (!_psBlendBloom->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_BlendBloom");
        if (_psBlendBloom->Init(psDesc))
            return true;
    }
    if (!_psKawaseBlur->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_KawaseBlur");
        if (_psKawaseBlur->Init(psDesc))
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

GPUTexture* PostProcessingPass::getCustomOrDefault(Texture* customTexture, AssetReference<Texture>& defaultTexture, const Char* defaultName)
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

void PostProcessingPass::GB_ComputeKernel(float sigma, float width, float height)
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

        GaussianBlurCacheH[index] = Float4(weight, i * xOffset, 0, 0);
        GaussianBlurCacheV[index] = Float4(weight, i * yOffset, 0, 0);
    }

    // Normalize weights
    for (int32 i = 0; i < GB_KERNEL_SIZE; i++)
    {
        GaussianBlurCacheH[i].X /= total;
        GaussianBlurCacheV[i].X /= total;
    }

    // Assign size
    _gbData.Size = Float2(width, height);
}

void PostProcessingPass::Dispose()
{
    // Base
    RendererPass::Dispose();

    // Cleanup
    SAFE_DELETE_GPU_RESOURCE(_psThreshold);
    SAFE_DELETE_GPU_RESOURCE(_psScale);
    SAFE_DELETE_GPU_RESOURCE(_psKawaseBlur);
    SAFE_DELETE_GPU_RESOURCE(_psBlendBloom);
    SAFE_DELETE_GPU_RESOURCE(_psBlurH);
    SAFE_DELETE_GPU_RESOURCE(_psBlurV);
    SAFE_DELETE_GPU_RESOURCE(_psGenGhosts);
    _psComposite.Delete();
    _shader = nullptr;
    _defaultLensColor = nullptr;
    _defaultLensDirt = nullptr;
    _defaultLensStar = nullptr;
}

void PostProcessingPass::Render(RenderContext& renderContext, GPUTexture* input, GPUTexture* output, GPUTexture* colorGradingLUT)
{
    PROFILE_GPU_CPU("Post Processing");
    auto device = GPUDevice::Instance;
    auto context = device->GetMainContext();
    auto& view = renderContext.View;
    
    context->ResetRenderTarget();

    PostProcessSettings& settings = renderContext.List->Settings;
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

    const int32 BLOOM_MIP_COUNT = 6; // Controls number of mip levels for bloom

    // Ensure to have valid data and if at least one effect should be applied
    if (!(useBloom || useToneMapping || useCameraArtifacts) || checkIfSkipPass() || w8 == 0 || h8 ==0)
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
        data.ChromaticDistortion = Math::Saturate(settings.CameraArtifacts.ChromaticDistortion);
        data.ScreenFadeColor = settings.CameraArtifacts.ScreenFadeColor;
    }
    else
    {
        data.VignetteIntensity = 0;
        data.GrainAmount = 0;
        data.ChromaticDistortion = 0;
        data.ScreenFadeColor = Color::Transparent;
    }

    if (useBloom)
    {
        data.BloomIntensity = settings.Bloom.Intensity;
        data.BloomThresholdStart = settings.Bloom.ThresholdStart;
        data.BloomThresholdSoftness = settings.Bloom.ThresholdSoftness;
        data.BloomScatter = Math::Max(settings.Bloom.Scatter, 0.0001f);
        data.BloomTintColor = (Float3)settings.Bloom.TintColor;
        data.BloomClampIntensity = settings.Bloom.ClampIntensity;
        data.BloomMipCount = BLOOM_MIP_COUNT; 
        data.BloomPadding = Float3(0.0f,0.0f,0.0f);
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
        Float3 camZ = renderContext.View.View.GetForward();
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
    data.PostExposure = Math::Exp2(settings.EyeAdaptation.PostExposure);
    data.InputSize = Float2(static_cast<float>(w1), static_cast<float>(h1));
    data.InvInputSize = Float2(1.0f / static_cast<float>(w1), 1.0f / static_cast<float>(h1));
    data.InputAspect = static_cast<float>(w1) / h1;
    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);

    ////////////////////////////////////////////////////////////////////////////////////
    // Bloom

    auto tempDesc = GPUTextureDescription::New2D(w2, h2, 0, output->Format(),
        GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget | GPUTextureFlags::PerMipViews);
    auto bloomTmp1 = RenderTargetPool::Get(tempDesc);
    RENDER_TARGET_POOL_SET_NAME(bloomTmp1, "PostProcessing.Bloom");
    auto bloomTmp2 = RenderTargetPool::Get(tempDesc);
    RENDER_TARGET_POOL_SET_NAME(bloomTmp2, "PostProcessing.Bloom");

    if (useBloom)
    {
        // Validate minimum dimensions
        const int32 MIN_DIMENSION = 4;
        if (w2 < MIN_DIMENSION || h2 < MIN_DIMENSION)
        {
            context->UnBindSR(2);
            return;
        }

        // Pre-calculate mip sizes
        struct MipSize {
            int32 width;
            int32 height;
        };
        MipSize mipSizes[6];  // Support up to 6 mips
        for (int32 i = 0; i < BLOOM_MIP_COUNT; i++)
        {
            mipSizes[i].width = w2 >> i;
            mipSizes[i].height = h2 >> i;
        }

        // Combined threshold and source treatment pass
        context->SetRenderTarget(bloomTmp1->View(0, 0));
        context->SetViewportAndScissors((float)mipSizes[0].width, (float)mipSizes[0].height);
        context->BindSR(0, input->View());
        context->SetState(_psThreshold);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Kawase downscale chain
        for (int32 mip = 1; mip < BLOOM_MIP_COUNT; mip++)
        {
            context->SetRenderTarget(bloomTmp1->View(0, mip));
            context->SetViewportAndScissors((float)mipSizes[mip].width, (float)mipSizes[mip].height);
            context->BindSR(0, bloomTmp1->View(0, mip - 1));
            context->SetState(_psScale);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        // Kawase upscale chain
        for (int32 mip = BLOOM_MIP_COUNT - 2; mip >= 0; mip--)
        {
            GPUTexture* target = (mip % 2 == 0) ? bloomTmp2 : bloomTmp1;
            context->SetRenderTarget(target->View(0, mip));
            context->SetViewportAndScissors((float)mipSizes[mip].width, (float)mipSizes[mip].height);
            context->BindSR(0, bloomTmp1->View(0, mip + 1));  // Higher mip
            context->BindSR(1, bloomTmp1->View(0, mip));      // Current mip
            context->SetState(_psKawaseBlur);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
        }

        // Final result will be in bloomTmp2 since we end on mip 0 (even)
        context->BindSR(2, bloomTmp2);
    }

    ////////////////////////////////////////////////////////////////////////////////////
    // Lens Flares

    // Check if use lens flares
    if (useLensFlares)
    {
        // Prepare lens flares helper textures
        context->BindSR(5, getCustomOrDefault(settings.LensFlares.LensStar, _defaultLensStar, TEXT("Engine/Textures/DefaultLensStarburst")));
        context->BindSR(6, getCustomOrDefault(settings.LensFlares.LensColor, _defaultLensColor, TEXT("Engine/Textures/DefaultLensColor")));

        // Render lens flares
        context->SetRenderTarget(bloomTmp2->View(0, 1));
        context->SetViewportAndScissors((float)w4, (float)h4);
        context->BindSR(3, bloomTmp1->View(0, 1));
        context->SetState(_psGenGhosts);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->UnBindSR(3);

        // Gaussian blur kernel
        GB_ComputeKernel(2.0f, static_cast<float>(w4), static_cast<float>(h4));

        // Gaussian blur H
        Platform::MemoryCopy(_gbData.GaussianBlurCache, GaussianBlurCacheH, sizeof(GaussianBlurCacheH));
        context->UpdateCB(cb1, &_gbData);
        context->BindCB(1, cb1);
        context->SetRenderTarget(bloomTmp1->View(0, 1));
        context->BindSR(0, bloomTmp2->View(0, 1));
        context->SetState(_psBlurH);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Gaussian blur V
        Platform::MemoryCopy(_gbData.GaussianBlurCache, GaussianBlurCacheV, sizeof(GaussianBlurCacheV));
        context->UpdateCB(cb1, &_gbData);
        context->BindCB(1, cb1);
        context->SetRenderTarget(bloomTmp2->View(0, 1));
        context->BindSR(0, bloomTmp1->View(0, 1));
        context->SetState(_psBlurV);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();

        // Set lens flares output
        context->BindSR(3, bloomTmp2->View(0, 1));
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
    // - 0 - Input0   - scene color
    // - 1 - Input1   - <unused>
    // - 2 - Input2   - bloom
    // - 3 - Input3   - lens flare color
    // - 4 - LensDirt - lens dirt texture
    // - 5 - LensStar - lens star texture
    // - 7 - ColorGradingLUT
    context->BindSR(0, input->View());
    context->BindSR(4, getCustomOrDefault(settings.LensFlares.LensDirt, _defaultLensDirt, TEXT("Engine/Textures/DefaultLensDirt")));
    context->BindSR(7, colorGradingLutView);

    // Composite final frame during single pass (done in full resolution)
    context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
    context->SetRenderTarget(*output);
    context->SetState(_psComposite.Get(compositePermutationIndex));
    context->DrawFullscreenTriangle();

    // Cleanup
    RenderTargetPool::Release(bloomTmp1);
    RenderTargetPool::Release(bloomTmp2);
}
