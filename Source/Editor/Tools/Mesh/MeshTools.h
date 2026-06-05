// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Mesh simplification algorithm (meshoptimizer).
/// </summary>
API_ENUM(Namespace="FlaxEditor") enum class MeshSimplifyMode
{
    /// <summary>Quadric error metric; preserves topology/appearance (meshopt_simplify). Honors the option flags.</summary>
    Quadric = 0,
    /// <summary>Silhouette-only; ignores topology and attributes (meshopt_simplifySloppy). Best for shadow proxies. Ignores the flags.</summary>
    Sloppy = 1,
};

/// <summary>
/// meshoptimizer simplification option flags (Quadric mode only; ignored by Sloppy).
/// </summary>
API_ENUM(Attributes="Flags", Namespace="FlaxEditor") enum class MeshSimplifyFlags
{
    /// <summary>No options.</summary>
    None = 0,
    /// <summary>Do not move vertices on open (unpaired) edges.</summary>
    LockBorder = 1 << 0,
    /// <summary>Input is a sparse subset of a larger mesh; error becomes relative to the subset extents.</summary>
    Sparse = 1 << 1,
    /// <summary>Treat target/result error as absolute world units, not relative to mesh extents.</summary>
    ErrorAbsolute = 1 << 2,
    /// <summary>Remove disconnected parts of the mesh incrementally during simplification.</summary>
    Prune = 1 << 3,
    /// <summary>Produce more regular triangle sizes/shapes, at some cost to accuracy.</summary>
    Regularize = 1 << 4,
    /// <summary>Allow collapses across attribute discontinuities.</summary>
    Permissive = 1 << 5,
};

/// <summary>
/// Editor mesh utilities (proxy/LOD generation).
/// </summary>
API_CLASS(Static, Namespace="FlaxEditor") class MeshTools
{
DECLARE_SCRIPTING_TYPE_NO_SPAWN(MeshTools);

    /// <summary>
    /// Simplifies a mesh down to a low-poly proxy. For shadow caster proxies use Sloppy mode (depth
    /// passes sample no attributes, so only the silhouette matters). Output is compacted to the
    /// vertices the simplified index buffer still references.
    /// </summary>
    /// <param name="positions">Source vertex positions.</param>
    /// <param name="indices">Source triangle index buffer (3 indices per triangle).</param>
    /// <param name="mode">Simplification algorithm.</param>
    /// <param name="targetRatio">Fraction of triangles to keep (0..1); e.g. 0.05 targets ~5%.</param>
    /// <param name="targetError">Allowed geometric error (0..1, relative to mesh extents unless ErrorAbsolute is set).</param>
    /// <param name="flags">Option flags (Quadric mode only).</param>
    /// <param name="outPositions">Resulting compacted vertex positions.</param>
    /// <param name="outIndices">Resulting triangle index buffer.</param>
    /// <param name="outError">Resulting (relative) error reported by meshoptimizer.</param>
    /// <returns>True on success; false if the input was degenerate or simplification collapsed it.</returns>
    API_FUNCTION() static bool Simplify(const Span<Float3>& positions, const Span<uint32>& indices, MeshSimplifyMode mode, float targetRatio, float targetError, MeshSimplifyFlags flags, API_PARAM(Out) Array<Float3>& outPositions, API_PARAM(Out) Array<uint32>& outIndices, API_PARAM(Out) float& outError);
};
