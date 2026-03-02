// Copyright (c) Wojciech Figat. All rights reserved.

#include "StylizedCloudPass.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Content/Content.h"
#include "Engine/Core/Config/GraphicsSettings.h"
#include "Engine/Engine/Time.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/Materials/IMaterial.h"
#include "Engine/Graphics/Materials/StylizedCloudMaterialShader.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Renderer/RenderList.h"

String StylizedCloudPass::ToString() const
{
    return TEXT("StylizedCloudPass");
}

bool StylizedCloudPass::Init()
{
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

    _psGaussianBlur.Delete();
    _psBoxBlur.Delete();
    SAFE_DELETE_GPU_RESOURCE(_psComposite);
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
    auto& cloudDrawList = renderContext.List->DrawCallsLists[(int32)DrawCallsListType::StylizedCloud];
    if (frameBuffer == nullptr || cloudDrawList.IsEmpty() || checkIfSkipPass())
        return;
    if (_shader == nullptr || !_psComposite->IsValid())
        return;

    auto context = GPUDevice::Instance->GetMainContext();
    PROFILE_GPU_CPU("Stylized Clouds");

    // Allocate half-resolution buffers
    const int32 quarterWidth = Math::Max(1, renderContext.Buffers->GetWidth() / 2);
    const int32 quarterHeight = Math::Max(1, renderContext.Buffers->GetHeight() / 2);
    const auto colorDesc = GPUTextureDescription::New2D(quarterWidth, quarterHeight, PixelFormat::R16G16B16A16_Float);
    const auto depthDesc = GPUTextureDescription::New2D(quarterWidth, quarterHeight, PixelFormat::R32_Float);
    const auto hwDepthDesc = GPUTextureDescription::New2D(quarterWidth, quarterHeight, GPU_DEPTH_BUFFER_PIXEL_FORMAT, GPUTextureFlags::DepthStencil);
    auto cloudColor = RenderTargetPool::Get(colorDesc);
    auto cloudDepth = RenderTargetPool::Get(depthDesc);
    auto cloudHwDepth = RenderTargetPool::Get(hwDepthDesc);
    auto tempColor = RenderTargetPool::Get(colorDesc);
    auto tempDepth = RenderTargetPool::Get(depthDesc);
    RENDER_TARGET_POOL_SET_NAME(cloudColor, "StylizedCloud.CloudColor");
    RENDER_TARGET_POOL_SET_NAME(cloudDepth, "StylizedCloud.CloudDepth");
    RENDER_TARGET_POOL_SET_NAME(cloudHwDepth, "StylizedCloud.CloudHwDepth");
    RENDER_TARGET_POOL_SET_NAME(tempColor, "StylizedCloud.TempColor");
    RENDER_TARGET_POOL_SET_NAME(tempDepth, "StylizedCloud.TempDepth");

    const auto shader = _shader->GetShader();

    // Gather lighting data for material custom data
    Float3 sunDirection = Float3::UnitY;
    Float3 sunColor = Float3::One;
    Float3 skyColor = Float3(0.4f, 0.5f, 0.7f);
    float sunIntensity = 1.0f;
    float skyIntensity = 0.5f;

    if (renderContext.List->DirectionalLights.HasItems())
    {
        const auto& light = renderContext.List->DirectionalLights[0];
        sunDirection = light.Direction;
        sunColor = light.Color;
    }
    if (renderContext.List->SkyLights.HasItems())
    {
        const auto& skyLight = renderContext.List->SkyLights[0];
        skyColor = skyLight.Color + skyLight.AdditiveColor;
    }

    // Setup common constants for blur/composite
    auto* settings = GraphicsSettings::Get();
    Data data;
    GBufferPass::SetInputs(renderContext.View, data.GBuffer);
    data.TexelSize = Float2(1.0f / (float)quarterWidth, 1.0f / (float)quarterHeight);
    data.OutputSize = Float2((float)quarterWidth, (float)quarterHeight);
    data.BlurSigmaBase = settings->StylizedCloudBlurSigma;
    data.BlurDepthScale = settings->StylizedCloudBlurDepthScale;
    data.SunDirection = sunDirection;
    data.SunIntensity = sunIntensity;
    data.SunColor = sunColor;
    data.SkyIntensity = skyIntensity;
    data.SkyColor = skyColor;
    data.DistortionStrength = settings->StylizedCloudDistortionStrength;
    data.AlphaThreshold = settings->StylizedCloudAlphaThreshold;
    data.SoftIntersectionDistance = settings->StylizedCloudSoftIntersectionDistance;
    data.DepthRange = Float2(renderContext.View.Near, renderContext.View.Far);
    data.DistanceSharpenStart = settings->StylizedCloudDistanceSharpenStart;
    data.DistanceSharpenEnd = settings->StylizedCloudDistanceSharpenEnd;
    data.DistortionScrollSpeed = settings->StylizedCloudDistortionScrollSpeed;
    data.Padding0 = Float2::Zero;
    data.Time = Time::Draw.UnscaledTime.GetTotalSeconds();
    Matrix::Transpose(renderContext.View.ViewProjection(), data.ViewProjection);
    Matrix::Transpose(renderContext.View.IVP, data.InvViewProjection);

    // Pre-pass: render cloud meshes into color + depth buffers via material draw calls
    context->SetViewportAndScissors((float)quarterWidth, (float)quarterHeight);
    context->Clear(cloudColor->View(), Color::Transparent);
    context->Clear(cloudDepth->View(), Color(renderContext.View.Far));
    context->ClearDepth(cloudHwDepth->View());
    GPUTextureView* prePassTargets[] = { cloudColor->View(), cloudDepth->View() };
    context->SetRenderTarget(cloudHwDepth->View(), Span<GPUTextureView*>(prePassTargets, ARRAY_COUNT(prePassTargets)));

    // Prepare lighting custom data for material binding
    StylizedCloudCustomData customData;
    customData.SunDirection = sunDirection;
    customData.SunIntensity = sunIntensity;
    customData.SunColor = sunColor;
    customData.SkyIntensity = skyIntensity;
    customData.SkyColor = skyColor;

    // Gather local lights (point + spot)
    customData.LocalLightCount = 0;
    for (int32 i = 0; i < renderContext.List->PointLights.Count() && customData.LocalLightCount < STYLIZED_CLOUD_MAX_LOCAL_LIGHTS; i++)
    {
        const auto& light = renderContext.List->PointLights[i];
        auto& dst = customData.LocalLights[customData.LocalLightCount++];
        dst.Position = light.Position;
        dst.Radius = light.Radius;
        dst.Color = light.Color;
        dst.FalloffExponent = light.FallOffExponent;
        dst.Direction = Float3::Zero;
        dst.SpotCosOuterCone = -1.0f;
        dst.SpotInvCosConeDiff = 0.0f;
    }
    for (int32 i = 0; i < renderContext.List->SpotLights.Count() && customData.LocalLightCount < STYLIZED_CLOUD_MAX_LOCAL_LIGHTS; i++)
    {
        const auto& light = renderContext.List->SpotLights[i];
        auto& dst = customData.LocalLights[customData.LocalLightCount++];
        dst.Position = light.Position;
        dst.Radius = light.Radius;
        dst.Color = light.Color;
        dst.FalloffExponent = light.FallOffExponent;
        dst.Direction = light.Direction;
        dst.SpotCosOuterCone = light.CosOuterCone;
        dst.SpotInvCosConeDiff = light.InvCosConeDifference;
    }

    // Iterate draw calls from the StylizedCloud list
    const auto* drawCallsData = renderContext.List->DrawCalls.Get();
    const auto* listData = cloudDrawList.Indices.Get();
    constexpr int32 vbMax = ARRAY_COUNT(DrawCall::Geometry.VertexBuffers);
    IMaterial::BindParameters bindParams(context, renderContext);
    bindParams.CustomData = &customData;
    bindParams.BindViewData();

    for (int32 i = 0; i < cloudDrawList.Indices.Count(); i++)
    {
        const int32 index = listData[i];
        const DrawCall& drawCall = drawCallsData[index];
        if (!drawCall.Material || !drawCall.Material->IsReady())
            continue;

        bindParams.DrawCall = &drawCall;
        drawCall.Material->Bind(bindParams);

        // Bind geometry and draw
        context->BindIB(drawCall.Geometry.IndexBuffer);
        context->BindVB(ToSpan(drawCall.Geometry.VertexBuffers, vbMax), drawCall.Geometry.VertexBuffersOffsets);
        context->DrawIndexed(drawCall.Draw.IndicesCount, 0, drawCall.Draw.StartIndex);
    }

    context->ResetRenderTarget();

    // Update blur/composite constants
    auto cb0 = shader->GetCB(0);
    context->UpdateCB(cb0, &data);
    context->BindCB(0, cb0);

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
    auto distortionCubeMap = settings->StylizedCloudDistortionCubeMap.Get();
    context->BindSR(3, distortionCubeMap ? distortionCubeMap->GetTexture() : nullptr);
    context->SetViewportAndScissors((float)frameBuffer->Width(), (float)frameBuffer->Height());
    context->SetRenderTarget(frameBuffer->View());
    context->SetState(_psComposite);
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();

    // Cleanup
    context->ResetSR();
    context->UnBindCB(0);
    RenderTargetPool::Release(cloudColor);
    RenderTargetPool::Release(cloudDepth);
    RenderTargetPool::Release(cloudHwDepth);
    RenderTargetPool::Release(tempColor);
    RenderTargetPool::Release(tempDepth);
    context->SetViewportAndScissors(renderContext.Task->GetViewport());
}
