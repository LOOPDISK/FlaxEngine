// Copyright (c) Wojciech Figat. All rights reserved.

#include "SkinnedMeshDrawData.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Animations/Config.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Math/Matrix.h"

SkinnedMeshDrawData::~SkinnedMeshDrawData()
{
    SAFE_DELETE_GPU_RESOURCE(BoneMatrices);
    SAFE_DELETE_GPU_RESOURCE(PrevBoneMatrices);
    for (GPUBuffer* b : OutputVB0)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputVB1)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputVB2)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputPreSkinVB)
        SAFE_DELETE_GPU_RESOURCE(b);
}

void SkinnedMeshDrawData::Setup(int32 bonesCount)
{
    if (BoneMatrices == nullptr)
    {
        BoneMatrices = GPUDevice::Instance->CreateBuffer(TEXT("BoneMatrices"));
    }

    const int32 elementsCount = bonesCount * 3; // 3 * float4 per bone
    if (BoneMatrices->Init(GPUBufferDescription::Typed(elementsCount, PixelFormat::R32G32B32A32_Float, false, GPUResourceUsage::Dynamic)))
    {
        LOG(Error, "Failed to initialize the skinned mesh bones buffer");
        return;
    }

    BonesCount = bonesCount;
    _hasValidData = false;
    _isDirty = true;
    _settled = false;
    _prevData.Resize(0);
    Data.Resize(BoneMatrices->GetSize());
    SAFE_DELETE_GPU_RESOURCE(PrevBoneMatrices);
}

void SkinnedMeshDrawData::ReleaseOutputVBs()
{
    for (GPUBuffer* b : OutputVB0)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputVB1)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputVB2)
        SAFE_DELETE_GPU_RESOURCE(b);
    for (GPUBuffer* b : OutputPreSkinVB)
        SAFE_DELETE_GPU_RESOURCE(b);
    OutputVB0.Clear();
    OutputVB1.Clear();
    OutputVB2.Clear();
    OutputPreSkinVB.Clear();
    OutputVersion.Clear();
}

bool SkinnedMeshDrawData::OnDataChanged(bool dropHistory)
{
    // Dormant skeleton: bone data byte-identical to last update.
    const bool unchanged = _hasValidData && _prevData.Count() == Data.Count() && Platform::MemoryCompare(_prevData.Get(), Data.Get(), Data.Count()) == 0;

    // Skip flush + dispatch on a dormant pose (reuse cached output, hold SkinningVersion).
    // No motion blur: skip any unchanged frame. With motion blur: run one settling frame (Prev==Bone) before skipping.
    if (unchanged && (dropHistory || _settled))
        return false;

    // Setup previous frame bone matrices if needed
    if (_hasValidData && !dropHistory)
    {
        ASSERT(BoneMatrices);
        if (PrevBoneMatrices == nullptr)
        {
            PrevBoneMatrices = GPUDevice::Instance->CreateBuffer(TEXT("BoneMatrices"));
            if (PrevBoneMatrices->Init(BoneMatrices->GetDescription()))
            {
                LOG(Fatal, "Failed to initialize the skinned mesh bones buffer");
            }
        }
        Swap(PrevBoneMatrices, BoneMatrices);
    }
    else
    {
        SAFE_DELETE_GPU_RESOURCE(PrevBoneMatrices);
    }

    _prevData.Set(Data.Get(), Data.Count());
    _settled = unchanged; // settling frame ran: Prev==Bone now, next identical frame may skip
    _isDirty = true;
    _hasValidData = true;

    // Bump version so the skinning pass re-dispatches; dormant skeletons (no change) keep the cached output.
    SkinningVersion++;
    return true;
}
