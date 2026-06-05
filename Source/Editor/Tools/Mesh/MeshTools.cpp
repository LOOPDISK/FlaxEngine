// Copyright (c) Wojciech Figat. All rights reserved.

#include "MeshTools.h"
#include "Engine/Core/Math/Math.h"
#include <ThirdParty/meshoptimizer/meshoptimizer.h>

bool MeshTools::Simplify(const Span<Float3>& positions, const Span<uint32>& indices, MeshSimplifyMode mode, float targetRatio, float targetError, MeshSimplifyFlags flags, Array<Float3>& outPositions, Array<uint32>& outIndices, float& outError)
{
    outPositions.Clear();
    outIndices.Clear();
    outError = 0.0f;
    const int32 srcIndexCount = indices.Length();
    const int32 srcVertexCount = positions.Length();
    if (srcIndexCount < 3 || srcVertexCount < 3 || (srcIndexCount % 3) != 0)
        return false;
    targetRatio = Math::Saturate(targetRatio);
    int32 targetIndexCount = (int32)(srcIndexCount * targetRatio) / 3 * 3;
    if (targetIndexCount < 3)
        targetIndexCount = 3;

    // Worst case the simplifier can return up to srcIndexCount indices.
    Array<uint32> tmpIndices;
    tmpIndices.Resize(srcIndexCount);
    float resultError = 0.0f;
    size_t resultIndexCount;
    if (mode == MeshSimplifyMode::Sloppy)
    {
        // Silhouette-only; ignores topology/attributes (the proxy is never shaded).
        resultIndexCount = meshopt_simplifySloppy(
            tmpIndices.Get(), indices.Get(), srcIndexCount,
            (const float*)positions.Get(), srcVertexCount, sizeof(Float3),
            nullptr, (size_t)targetIndexCount, targetError, &resultError);
    }
    else
    {
        resultIndexCount = meshopt_simplify(
            tmpIndices.Get(), indices.Get(), srcIndexCount,
            (const float*)positions.Get(), srcVertexCount, sizeof(Float3),
            (size_t)targetIndexCount, targetError, (unsigned int)flags, &resultError);
    }
    if (resultIndexCount < 3)
        return false;
    outError = resultError;

    // Compact the vertex buffer to only the vertices the simplified index buffer still references.
    Array<uint32> remap;
    remap.Resize(srcVertexCount);
    const size_t dstVertexCount = meshopt_optimizeVertexFetchRemap(remap.Get(), tmpIndices.Get(), resultIndexCount, srcVertexCount);

    outIndices.Resize((int32)resultIndexCount);
    meshopt_remapIndexBuffer(outIndices.Get(), tmpIndices.Get(), resultIndexCount, remap.Get());

    outPositions.Resize((int32)dstVertexCount);
    meshopt_remapVertexBuffer(outPositions.Get(), positions.Get(), srcVertexCount, sizeof(Float3), remap.Get());
    return true;
}
