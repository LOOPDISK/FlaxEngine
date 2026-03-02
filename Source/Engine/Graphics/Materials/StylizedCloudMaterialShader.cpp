// Copyright (c) Wojciech Figat. All rights reserved.

#include "StylizedCloudMaterialShader.h"
#include "MaterialParams.h"
#include "Engine/Core/Math/Matrix3x4.h"
#include "Engine/Renderer/DrawCall.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Renderer/ShadowsPass.h"

PACK_STRUCT(struct StylizedCloudLocalLight {
    Float3 Position;
    float Radius;
    Float3 Color;
    float FalloffExponent;
    Float3 Direction;
    float SpotCosOuterCone; // cos outer cone for spots, -1 for point lights
    float SpotInvCosConeDiff;
    Float3 LightPadding;
    });

PACK_STRUCT(struct StylizedCloudMaterialShaderData {
    Matrix WorldMatrix;
    Matrix ViewProjection;
    Float3 SunDirection;
    float SunIntensity;
    Float3 SunColor;
    float SkyIntensity;
    Float3 SkyColor;
    float Time;
    float PerInstanceRandom;
    int32 LocalLightCount;
    uint32 ShadowsBufferAddress;
    uint32 HasShadow;
    StylizedCloudLocalLight LocalLights[STYLIZED_CLOUD_MAX_LOCAL_LIGHTS];
    });

DrawPass StylizedCloudMaterialShader::GetDrawModes() const
{
    return DrawPass::StylizedCloud | DrawPass::Depth;
}

void StylizedCloudMaterialShader::Bind(BindParameters& params)
{
    auto context = params.GPUContext;
    const RenderView& view = params.RenderContext.View;
    auto& drawCall = *params.DrawCall;
    Span<byte> cb(_cbData.Get(), _cbData.Count());
    ASSERT_LOW_LAYER(cb.Length() >= sizeof(StylizedCloudMaterialShaderData));
    auto materialData = reinterpret_cast<StylizedCloudMaterialShaderData*>(cb.Get());
    cb = cb.Slice(sizeof(StylizedCloudMaterialShaderData));
    int32 srv = 0;

    // Setup parameters
    MaterialParameter::BindMeta bindMeta;
    bindMeta.Context = context;
    bindMeta.Constants = cb;
    bindMeta.Input = nullptr;
    bindMeta.Buffers = params.RenderContext.Buffers;
    bindMeta.CanSampleDepth = false;
    bindMeta.CanSampleGBuffer = false;
    MaterialParams::Bind(params.ParamsLink, bindMeta);

    // Setup material constants
    {
        Matrix::Transpose(drawCall.World, materialData->WorldMatrix);
        Matrix::Transpose(view.ViewProjection(), materialData->ViewProjection);
        materialData->PerInstanceRandom = drawCall.PerInstanceRandom;
        materialData->Time = params.Time;

        // Lighting data from custom data or defaults
        auto* customData = (StylizedCloudCustomData*)params.CustomData;
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

    // Bind constants
    if (_cb)
    {
        context->UpdateCB(_cb, _cbData.Get());
        context->BindCB(0, _cb);
    }

    // Bind shadow resources
    {
        GPUTexture* shadowMapAtlas = nullptr;
        GPUBufferView* shadowsBuffer = nullptr;
        ShadowsPass::GetShadowAtlas(params.RenderContext.Buffers, shadowMapAtlas, shadowsBuffer);
        context->BindSR(14, shadowsBuffer);
        context->BindSR(15, shadowMapAtlas);
    }

    // Bind pipeline
    if (_psCloudPrePass == nullptr)
    {
        GPUPipelineState::Description psDesc = GPUPipelineState::Description::Default;
        psDesc.VS = _shader->GetVS("VS_CloudPrePass");
        psDesc.PS = _shader->GetPS("PS_CloudPrePass");
        psDesc.DepthEnable = true;
        psDesc.DepthWriteEnable = true;
        psDesc.DepthFunc = ComparisonFunc::Less;
        psDesc.CullMode = _info.CullMode;
        _psCloudPrePass = GPUDevice::Instance->CreatePipelineState();
        _psCloudPrePass->Init(psDesc);
    }
    context->SetState(_psCloudPrePass);
}

void StylizedCloudMaterialShader::Unload()
{
    // Base
    MaterialShader::Unload();

    SAFE_DELETE_GPU_RESOURCE(_psCloudPrePass);
}

bool StylizedCloudMaterialShader::Load()
{
    return false;
}
