// Copyright (c) Wojciech Figat. All rights reserved.

#include "StylizedCloudPass.h"
#include "Engine/Content/Content.h"
#include "Engine/Engine/Time.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Level/Actors/CloudVolume.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Renderer/RenderList.h"

String StylizedCloudPass::ToString() const
{
    return TEXT("StylizedCloudPass");
}

bool StylizedCloudPass::Init()
{
    _psCloudPrePass = GPUDevice::Instance->CreatePipelineState();
    _psComposite = GPUDevice::Instance->CreatePipelineState();
    _psGaussianBlur.CreatePipelineStates();
    _psBoxBlur.CreatePipelineStates();

    _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/StylizedCloud"));
    if (_shader == nullptr)
    {
        LOG(Warning, "Cannot load stylized cloud shader. Stylized clouds will be disabled.");
        return false;
    }
#if COMPILE_WITH_DEV_ENV
    _shader.Get()->OnReloading.Bind<StylizedCloudPass, &StylizedCloudPass::OnShaderReloading>(this);
#endif

    return false;
}

void StylizedCloudPass::Dispose()
{
    // Base
    RendererPass::Dispose();

    SAFE_DELETE_GPU_RESOURCE(_psCloudPrePass);
    _psGaussianBlur.Delete();
    _psBoxBlur.Delete();
    SAFE_DELETE_GPU_RESOURCE(_psComposite);
    _distortionCubeMap = nullptr;
    _shader = nullptr;
}

bool StylizedCloudPass::setupResources()
{
    // Keep pass valid even if shader is missing (feature disabled).
    if (_shader == nullptr)
        return false;
    if (!_shader->IsLoaded())
        return true;

    const auto shader = _shader->GetShader();
    if (shader->GetCB(0)->GetSize() != sizeof(Data))
    {
        REPORT_INVALID_SHADER_PASS_CB_SIZE(shader, 0, Data);
        return true;
    }
    if (shader->GetCB(1)->GetSize() != sizeof(PerCloud))
    {
        REPORT_INVALID_SHADER_PASS_CB_SIZE(shader, 1, PerCloud);
        return true;
    }
    if (!_psCloudPrePass->IsValid())
    {
        GPUPipelineState::Description psDesc = GPUPipelineState::Description::Default;
        psDesc.VS = shader->GetVS("VS_CloudPrePass");
        psDesc.PS = shader->GetPS("PS_CloudPrePass");
        psDesc.DepthEnable = false;
        psDesc.DepthWriteEnable = false;
        psDesc.CullMode = CullMode::Normal;
        if (_psCloudPrePass->Init(psDesc))
            return true;
    }
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    if (!_psGaussianBlur.IsValid())
    {
        if (_psGaussianBlur.Create(psDesc, shader, "PS_GaussianBlur"))
            return true;
    }
    if (!_psBoxBlur.IsValid())
    {
        if (_psBoxBlur.Create(psDesc, shader, "PS_BoxBlur"))
            return true;
    }
    if (!_psComposite->IsValid())
    {
        psDesc.PS = shader->GetPS("PS_Composite");
        psDesc.DepthWriteEnable = false;
        psDesc.BlendMode.BlendEnable = true;
        psDesc.BlendMode.SrcBlend = BlendingMode::Blend::SrcAlpha;
        psDesc.BlendMode.DestBlend = BlendingMode::Blend::InvSrcAlpha;
        psDesc.BlendMode.BlendOp = BlendingMode::Operation::Add;
        psDesc.BlendMode.SrcBlendAlpha = BlendingMode::Blend::One;
        psDesc.BlendMode.DestBlendAlpha = BlendingMode::Blend::Zero;
        psDesc.BlendMode.BlendOpAlpha = BlendingMode::Operation::Add;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RGBA;
        if (_psComposite->Init(psDesc))
            return true;
    }

    return false;
}

void StylizedCloudPass::Render(RenderContext& renderContext, GPUTexture*& frameBuffer)
{
    if (frameBuffer == nullptr || renderContext.List->CloudVolumes.IsEmpty() || checkIfSkipPass())
        return;
    if (_shader == nullptr || !_psComposite->IsValid() || !_psCloudPrePass->IsValid())
        return;

    auto context = GPUDevice::Instance->GetMainContext();
    PROFILE_GPU_CPU("Stylized Clouds");

    // Allocate quarter-resolution buffers
    const int32 quarterWidth = Math::Max(1, renderContext.Buffers->GetWidth() / 2);
    const int32 quarterHeight = Math::Max(1, renderContext.Buffers->GetHeight() / 2);
    const auto colorDesc = GPUTextureDescription::New2D(quarterWidth, quarterHeight, PixelFormat::R16G16B16A16_Float);
    const auto depthDesc = GPUTextureDescription::New2D(quarterWidth, quarterHeight, PixelFormat::R32_Float);
    auto cloudColor = RenderTargetPool::Get(colorDesc);
    auto cloudDepth = RenderTargetPool::Get(depthDesc);
    auto tempColor = RenderTargetPool::Get(colorDesc);
    auto tempDepth = RenderTargetPool::Get(depthDesc);
    RENDER_TARGET_POOL_SET_NAME(cloudColor, "StylizedCloud.CloudColor");
    RENDER_TARGET_POOL_SET_NAME(cloudDepth, "StylizedCloud.CloudDepth");
    RENDER_TARGET_POOL_SET_NAME(tempColor, "StylizedCloud.TempColor");
    RENDER_TARGET_POOL_SET_NAME(tempDepth, "StylizedCloud.TempDepth");

    const auto shader = _shader->GetShader();

    // Setup common constants
    Data data;
    GBufferPass::SetInputs(renderContext.View, data.GBuffer);
    data.TexelSize = Float2(1.0f / (float)quarterWidth, 1.0f / (float)quarterHeight);
    data.OutputSize = Float2((float)quarterWidth, (float)quarterHeight);
    data.BlurSigmaBase = 2.5f;
    data.BlurDepthScale = 4.0f;
    data.SunDirection = Float3::UnitY;
    data.SunIntensity = 1.0f;
    data.SunColor = Float3::One;
    data.SkyIntensity = 0.5f;
    data.SkyColor = Float3(0.4f, 0.5f, 0.7f);
    data.DistortionStrength = 0.0f;
    data.AlphaThreshold = 0.3f;
    data.SoftIntersectionDistance = 35.0f;
    data.DepthRange = Float2(renderContext.View.Near, renderContext.View.Far);
    data.DistanceSharpenStart = 50000.0f;
    data.DistanceSharpenEnd = 100000.0f;
    data.Padding0 = Float3::Zero;
    data.Time = Time::GetGameTime();
    Matrix::Transpose(renderContext.View.ViewProjection(), data.ViewProjection);
    Matrix::Transpose(renderContext.View.IVP, data.InvViewProjection);

    if (renderContext.List->DirectionalLights.HasItems())
    {
        // Directional lights are pre-sorted by brightness, so the first item is the strongest one.
        const auto& light = renderContext.List->DirectionalLights[0];
        data.SunDirection = light.Direction;
        data.SunColor = light.Color;
    }
    if (renderContext.List->SkyLights.HasItems())
    {
        const auto& skyLight = renderContext.List->SkyLights[0];
        data.SkyColor = skyLight.Color + skyLight.AdditiveColor;
    }

    auto cb0 = shader->GetCB(0);
    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);

    // Pre-pass: cloud meshes into color + depth buffers
    context->SetViewportAndScissors((float)quarterWidth, (float)quarterHeight);
    context->Clear(cloudColor->View(), Color::Transparent);
    context->Clear(cloudDepth->View(), Color(renderContext.View.Far));
    GPUTextureView* prePassTargets[] = { cloudColor->View(), cloudDepth->View() };
    context->SetRenderTarget(nullptr, Span<GPUTextureView*>(prePassTargets, ARRAY_COUNT(prePassTargets)));
    context->SetState(_psCloudPrePass);
    auto cb1 = shader->GetCB(1);
    bool hasCloudSettings = false;
    for (CloudVolume* cloud : renderContext.List->CloudVolumes)
    {
        if (!cloud || !cloud->CloudMesh || !cloud->CloudMesh->CanBeRendered() || cloud->CloudMesh->LODs.IsEmpty())
            continue;

        PerCloud perCloud;
        Real4x4 worldReal;
        cloud->GetLocalToWorldMatrix(worldReal);
        renderContext.View.GetWorldMatrix(worldReal);
        Matrix world = (Matrix)worldReal;
        Matrix::Transpose(world, perCloud.WorldMatrix);
        perCloud.SunIntensity = cloud->SunIntensity;
        perCloud.SkyIntensity = cloud->SkyIntensity;
        perCloud.DistortionScale = cloud->DistortionScale;
        perCloud.AlphaThreshold = cloud->AlphaThreshold;
        perCloud.Density = cloud->Density;
        perCloud.LightningColor = cloud->LightningColor.ToFloat3();
        perCloud.LightningIntensity = cloud->LightningIntensity;
        context->UpdateCB(cb1, &perCloud);
        context->BindCB(1, cb1);

        const auto& lod = cloud->CloudMesh->LODs[0];
        for (int32 meshIndex = 0; meshIndex < lod.Meshes.Count(); meshIndex++)
            lod.Meshes[meshIndex].Render(context);

        if (!hasCloudSettings)
        {
            hasCloudSettings = true;
            data.AlphaThreshold = cloud->AlphaThreshold;
            data.SoftIntersectionDistance = cloud->SoftIntersectionDistance;
            data.DistanceSharpenStart = cloud->DistanceSharpening ? cloud->SharpeningStart : data.DistanceSharpenStart;
            data.DistanceSharpenEnd = cloud->DistanceSharpening ? cloud->SharpeningEnd : data.DistanceSharpenEnd;
        }
        else
        {
            data.AlphaThreshold = Math::Min(data.AlphaThreshold, cloud->AlphaThreshold);
            data.SoftIntersectionDistance = Math::Min(data.SoftIntersectionDistance, cloud->SoftIntersectionDistance);
            if (cloud->DistanceSharpening)
            {
                data.DistanceSharpenStart = Math::Min(data.DistanceSharpenStart, cloud->SharpeningStart);
                data.DistanceSharpenEnd = Math::Max(data.DistanceSharpenEnd, cloud->SharpeningEnd);
            }
        }
        if (_distortionCubeMap && _distortionCubeMap->IsLoaded())
            data.DistortionStrength = Math::Max(data.DistortionStrength, cloud->DistortionScale);
    }
    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);
    context->ResetRenderTarget();

    // Gaussian blur for cloud color
    context->BindSR(0, cloudColor->View());
    context->BindSR(1, cloudDepth->View());
    context->SetRenderTarget(tempColor->View());
    context->SetState(_psGaussianBlur.Get(0));
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    context->BindSR(0, tempColor->View());
    context->BindSR(1, cloudDepth->View());
    context->SetRenderTarget(cloudColor->View());
    context->SetState(_psGaussianBlur.Get(1));
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    // Box blur for cloud depth
    context->BindSR(0, cloudColor->View());
    context->BindSR(1, cloudDepth->View());
    context->SetRenderTarget(tempDepth->View());
    context->SetState(_psBoxBlur.Get(0));
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    context->BindSR(0, cloudColor->View());
    context->BindSR(1, tempDepth->View());
    context->SetRenderTarget(cloudDepth->View());
    context->SetState(_psBoxBlur.Get(1));
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    // Composite over frame buffer
    context->BindSR(0, cloudColor->View());
    context->BindSR(1, cloudDepth->View());
    context->BindSR(2, renderContext.Buffers->DepthBuffer);
    context->BindSR(3, _distortionCubeMap ? _distortionCubeMap->GetTexture() : nullptr);
    context->SetViewportAndScissors((float)frameBuffer->Width(), (float)frameBuffer->Height());
    context->SetRenderTarget(frameBuffer->View());
    context->SetState(_psComposite);
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    // Cleanup
    context->ResetSR();
    context->UnBindCB(0);
    context->UnBindCB(1);
    RenderTargetPool::Release(cloudColor);
    RenderTargetPool::Release(cloudDepth);
    RenderTargetPool::Release(tempColor);
    RenderTargetPool::Release(tempDepth);
    context->SetViewportAndScissors(renderContext.Task->GetViewport());
}
