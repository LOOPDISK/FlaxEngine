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
    Float3 Padding;
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
        materialData->Padding = Float3::Zero;

        // Lighting data from custom data or defaults
        auto* customData = (Float3*)params.CustomData;
        if (customData)
        {
            // CustomData layout: [0]=SunDirection, [1]=(SunIntensity, SkyIntensity, 0), [2]=SunColor, [3]=SkyColor
            materialData->SunDirection = customData[0];
            materialData->SunIntensity = customData[1].X;
            materialData->SkyIntensity = customData[1].Y;
            materialData->SunColor = customData[2];
            materialData->SkyColor = customData[3];
        }
        else
        {
            materialData->SunDirection = Float3::UnitY;
            materialData->SunIntensity = 1.0f;
            materialData->SunColor = Float3::One;
            materialData->SkyIntensity = 0.5f;
            materialData->SkyColor = Float3(0.4f, 0.5f, 0.7f);
        }
    }

    // Bind constants
    if (_cb)
    {
        context->UpdateCB(_cb, _cbData.Get());
        context->BindCB(0, _cb);
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
