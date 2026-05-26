// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Graphics/GPUBuffer.h"

/// <summary>
/// Data storage for the skinned meshes rendering
/// </summary>
class FLAXENGINE_API SkinnedMeshDrawData
{
private:
    bool _hasValidData = false;
    bool _isDirty = false;

public:
    /// <summary>
    /// The bones count.
    /// </summary>
    int32 BonesCount = 0;

    /// <summary>
    /// The bone matrices buffer. Contains prepared skeletal bones transformations (stored as 4x3, 3 Vector4 behind each other).
    /// </summary>
    GPUBuffer* BoneMatrices = nullptr;

    /// <summary>
    /// The bone matrices buffer used during the previous update. Used by per-bone motion blur.
    /// </summary>
    GPUBuffer* PrevBoneMatrices = nullptr;

    /// <summary>
    /// The CPU data buffer with the bones transformations (ready to be flushed with the GPU).
    /// </summary>
    Array<byte> Data;

    /// <summary>
    /// Per-mesh compute-skinning output: pre-skinned Position VB (12 bytes/vert). Indexed by mesh slot.
    /// </summary>
    Array<GPUBuffer*> OutputVB0;

    /// <summary>
    /// Per-mesh compute-skinning output: TexCoord+Normal+Tangent+TexCoord1 VB (16 bytes/vert, TexCoord1 zero).
    /// </summary>
    Array<GPUBuffer*> OutputVB1;

    /// <summary>
    /// Per-mesh compute-skinning output: Color VB (4 bytes/vert), allocated only when the source has Color.
    /// </summary>
    Array<GPUBuffer*> OutputVB2;

    /// <summary>
    /// Bumped on each OnDataChanged. The compute-skinning pass skips dispatch when the output is up to date with it (dormant skeletons).
    /// </summary>
    uint64 SkinningVersion = 0;

    /// <summary>
    /// Per-mesh SkinningVersion at the last dispatch; equal to SkinningVersion means the cached output is valid.
    /// </summary>
    Array<uint64> OutputVersion;

public:
    /// <summary>
    /// Finalizes an instance of the <see cref="SkinnedMeshDrawData"/> class.
    /// </summary>
    ~SkinnedMeshDrawData();

public:
    /// <summary>
    /// Determines whether this instance is ready for rendering.
    /// </summary>
    FORCE_INLINE bool IsReady() const
    {
        return BoneMatrices != nullptr && BoneMatrices->IsAllocated();
    }

    /// <summary>
    /// Determines whether this instance has been modified and needs to be flushed with GPU buffer.
    /// </summary>
    FORCE_INLINE bool IsDirty() const
    {
        return _isDirty;
    }

    /// <summary>
    /// Setups the data container for the specified bones amount.
    /// </summary>
    /// <param name="bonesCount">The bones count.</param>
    void Setup(int32 bonesCount);

    /// <summary>
    /// After bones Data has been modified externally. Updates the bone matrices data for the GPU buffer. Ensure to call Flush before rendering.
    /// </summary>
    /// <param name="dropHistory">True if drop previous update bones used for motion blur, otherwise will keep them and do the update.</param>
    void OnDataChanged(bool dropHistory);

    /// <summary>
    /// After bones Data has been sent to the GPU buffer.
    /// </summary>
    void OnFlush()
    {
        _isDirty = false;
    }

    /// <summary>
    /// Releases the compute-skinning output VB cache. Call on SkinnedModel change: the cached buffers are
    /// sized for the old vertex counts and reusing them would truncate the dispatch and collapse vertices.
    /// </summary>
    void ReleaseOutputVBs();
};
