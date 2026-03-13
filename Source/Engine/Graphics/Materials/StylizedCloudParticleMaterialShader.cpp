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
    // Ribbon data (aligned to 16-byte HLSL register boundaries)
    Float2 RibbonUVScale;
    Float2 RibbonUVOffset;
    float RibbonUVTilingDistance;
    int32 RibbonWidthOffset;
    int32 RibbonTwistOffset;
    int32 RibbonFacingVectorOffset;
    uint32 RibbonSegmentCount;
    float RibbonPadding0;
    float RibbonPadding1;
    float RibbonPadding2;
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
    StylizedCloudLocalLight LocalLights[STYLIZED_CLOUD_MAX_LOCAL_LIGHTS];
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

    // Setup material constants - particle data (common)
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

    // Setup ribbon-specific material constants
    bool isRibbon = false;
    switch (drawCall.Particle.Module->TypeID)
    {
    // Ribbon Rendering
    case 404:
    {
        isRibbon = true;

        static StringView ParticleRibbonWidth(TEXT("RibbonWidth"));
        static StringView ParticleRibbonTwist(TEXT("RibbonTwist"));
        static StringView ParticleRibbonFacingVector(TEXT("RibbonFacingVector"));

        materialData->RibbonWidthOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleRibbonWidth, ParticleAttribute::ValueTypes::Float, -1);
        materialData->RibbonTwistOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleRibbonTwist, ParticleAttribute::ValueTypes::Float, -1);
        materialData->RibbonFacingVectorOffset = drawCall.Particle.Particles->Layout->FindAttributeOffset(ParticleRibbonFacingVector, ParticleAttribute::ValueTypes::Float3, -1);

        materialData->RibbonUVTilingDistance = drawCall.Particle.Ribbon.UVTilingDistance;
        materialData->RibbonUVScale.X = drawCall.Particle.Ribbon.UVScaleX;
        materialData->RibbonUVScale.Y = drawCall.Particle.Ribbon.UVScaleY;
        materialData->RibbonUVOffset.X = drawCall.Particle.Ribbon.UVOffsetX;
        materialData->RibbonUVOffset.Y = drawCall.Particle.Ribbon.UVOffsetY;
        materialData->RibbonSegmentCount = drawCall.Particle.Ribbon.SegmentCount;
        break;
    }
    default:
        break;
    }

    // Setup material constants - cloud lighting data
    {
        Matrix::Transpose(view.ViewProjection(), materialData->ViewProjection);
        materialData->Time = params.Time;

        BindStylizedCloudLightingData(materialData, (StylizedCloudCustomData*)params.CustomData);
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
    if (isRibbon)
    {
        if (_psCloudPrePassRibbon == nullptr)
        {
            GPUPipelineState::Description psDesc = GPUPipelineState::Description::Default;
            psDesc.VS = _shader->GetVS("VS_Ribbon");
            psDesc.PS = _shader->GetPS("PS_CloudPrePass");
            psDesc.DepthEnable = true;
            psDesc.DepthWriteEnable = true;
            psDesc.DepthFunc = ComparisonFunc::Less;
            psDesc.CullMode = CullMode::TwoSided;
            _psCloudPrePassRibbon = GPUDevice::Instance->CreatePipelineState();
            _psCloudPrePassRibbon->Init(psDesc);
        }
        context->SetState(_psCloudPrePassRibbon);
    }
    else
    {
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
}

void StylizedCloudParticleMaterialShader::Unload()
{
    // Base
    MaterialShader::Unload();

    SAFE_DELETE_GPU_RESOURCE(_psCloudPrePass);
    SAFE_DELETE_GPU_RESOURCE(_psCloudPrePassRibbon);
}

bool StylizedCloudParticleMaterialShader::Load()
{
    return false;
}
