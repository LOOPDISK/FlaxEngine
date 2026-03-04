// Copyright (c) Wojciech Figat. All rights reserved.

#include "StylizedCloudParticleMaterialShader.h"
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
#include "Engine/Particles/Graph/CPU/ParticleEmitterGraph.CPU.h"

PACK_STRUCT(struct StylizedCloudParticleLocalLight {
    Float3 Position;
    float Radius;
    Float3 Color;
    float FalloffExponent;
    Float3 Direction;
    float SpotCosOuterCone;
    float SpotInvCosConeDiff;
    Float3 LightPadding;
    });

PACK_STRUCT(struct StylizedCloudParticleMaterialShaderData {
    // Particle data
    Matrix3x4 WorldMatrix;
    uint32 SortedIndicesOffset;
    float PerInstanceRandom;
    int32 ParticleStride;
    int32 PositionOffset;
    int32 VelocityOffset;
    int32 RotationOffset;
    int32 ScaleOffset;
    int32 ModelFacingModeOffset;
    Matrix3x4 WorldMatrixInverseTransposed;
    // Cloud lighting data
    Matrix ViewProjection;
    Float3 SunDirection;
    float SunIntensity;
    Float3 SunColor;
    float SkyIntensity;
    Float3 SkyColor;
    float Time;
    int32 LocalLightCount;
    uint32 ShadowsBufferAddress;
    uint32 HasShadow;
    float CloudParticlePadding0;
    StylizedCloudParticleLocalLight LocalLights[STYLIZED_CLOUD_MAX_LOCAL_LIGHTS];
    });

DrawPass StylizedCloudParticleMaterialShader::GetDrawModes() const
{
    return DrawPass::StylizedCloud | DrawPass::Depth;
}

void StylizedCloudParticleMaterialShader::Bind(BindParameters& params)
{
    auto context = params.GPUContext;
    const RenderView& view = params.RenderContext.View;
    auto& drawCall = *params.DrawCall;
    const uint32 sortedIndicesOffset = drawCall.Particle.Module->SortedIndicesOffset;
    Span<byte> cb(_cbData.Get(), _cbData.Count());
    ASSERT_LOW_LAYER(cb.Length() >= sizeof(StylizedCloudParticleMaterialShaderData));
    auto materialData = reinterpret_cast<StylizedCloudParticleMaterialShaderData*>(cb.Get());
    cb = cb.Slice(sizeof(StylizedCloudParticleMaterialShaderData));
    int32 srv = 2;

    // Setup parameters
    MaterialParameter::BindMeta bindMeta;
    bindMeta.Context = context;
    bindMeta.Constants = cb;
    bindMeta.Input = nullptr;
    bindMeta.Buffers = params.RenderContext.Buffers;
    bindMeta.CanSampleDepth = false;
    bindMeta.CanSampleGBuffer = false;
    MaterialParams::Bind(params.ParamsLink, bindMeta);

    // Setup particles data
    context->BindSR(0, drawCall.Particle.Particles->GPU.Buffer->View());
    context->BindSR(1, drawCall.Particle.Particles->GPU.SortedIndices ? drawCall.Particle.Particles->GPU.SortedIndices->View() : nullptr);

    // Setup particles attributes binding info
    {
        const auto& p = *params.ParamsLink->This;
        for (int32 i = 0; i < p.Count(); i++)
        {
            const auto& param = p.At(i);
            if (param.GetParameterType() == MaterialParameterType::Integer && param.GetName().StartsWith(TEXT("Particle.")))
            {
                const StringView name(param.GetName().Get() + 9, param.GetName().Length() - 9);
                const int32 offset = drawCall.Particle.Particles->Layout->FindAttributeOffset(name);
                ASSERT_LOW_LAYER(bindMeta.Constants.Get() && bindMeta.Constants.Length() >= (int32)(param.GetBindOffset() + sizeof(int32)));
                *((int32*)(bindMeta.Constants.Get() + param.GetBindOffset())) = offset;
            }
        }
    }

    // Setup material constants - particle data
    {
        static StringView ParticlePosition(TEXT("Position"));
        static StringView ParticleVelocityOffset(TEXT("Velocity"));
        static StringView ParticleRotationOffset(TEXT("Rotation"));
        static StringView ParticleScaleOffset(TEXT("Scale"));
        static StringView ParticleModelFacingModeOffset(TEXT("ModelFacingMode"));

        materialData->WorldMatrix.SetMatrixTranspose(drawCall.World);
        const bool isDepthPass = params.RenderContext.View.Pass == DrawPass::Depth || params.RenderContext.View.Pass == DrawPass::WeaponDepth;
        materialData->SortedIndicesOffset = drawCall.Particle.Particles->GPU.SortedIndices && !isDepthPass ? sortedIndicesOffset : 0xFFFFFFFF;
        materialData->PerInstanceRandom = drawCall.PerInstanceRandom;
        materialData->ParticleStride = drawCall.Particle.Particles->Stride;
        materialData->PositionOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticlePosition, ParticleAttribute::ValueTypes::Float3);
        materialData->VelocityOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleVelocityOffset, ParticleAttribute::ValueTypes::Float3);
        materialData->RotationOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleRotationOffset, ParticleAttribute::ValueTypes::Float3, -1);
        materialData->ScaleOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleScaleOffset, ParticleAttribute::ValueTypes::Float3, -1);
        materialData->ModelFacingModeOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleModelFacingModeOffset, ParticleAttribute::ValueTypes::Int, -1);
        Matrix worldMatrixInverseTransposed;
        Matrix::Invert(drawCall.World, worldMatrixInverseTransposed);
        materialData->WorldMatrixInverseTransposed.SetMatrix(worldMatrixInverseTransposed);
    }

    // Setup material constants - cloud lighting data
    {
        Matrix::Transpose(view.ViewProjection(), materialData->ViewProjection);
        materialData->Time = params.Time;

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
        psDesc.VS = _shader->GetVS("VS_Model");
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

void StylizedCloudParticleMaterialShader::Unload()
{
    // Base
    MaterialShader::Unload();

    SAFE_DELETE_GPU_RESOURCE(_psCloudPrePass);
}

bool StylizedCloudParticleMaterialShader::Load()
{
    return false;
}
