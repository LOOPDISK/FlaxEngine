// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"

// Batched single-dispatch cull: all consumer slots on this pyramid concatenate their bounds into
// SharedBounds and announce their slice via SlotTable. One CS thread per bound across the whole
// frame. Threads resolve which slot owns them via a prefix-sum binary search on SlotTable.w.

META_CB_BEGIN(0, HZBCullData)
float4x4 ViewProjection;     // view-relative VP; we subtract ViewOrigin from bounds before projection
float3 ViewForward;          // unit vector along view direction
float ViewNear;              // near plane distance (view-space)
float2 PyramidBase;          // pyramid level 0 dimensions in pixels (W/2, H/2)
uint TotalBounds;            // total bound entries across all slots (== dispatch thread count)
uint MaxLevel;               // total number of pyramid levels
float3 ViewOrigin;           // pyramid's captured world-space view origin
uint NumSlots;               // size of SlotTable, for binary search bound
float BoundsInflate;         // camera translation since pyramid capture; added to each test radius
uint _hzbCullPad1;
uint _hzbCullPad2;
uint _hzbCullPad3;
META_CB_END

// SharedBounds: xyz = sphere center (world-space), w = radius. Concatenation of every slot's bounds
// this frame. Float4::Zero (w<=0) is a "dead slot" - treated as visible (no false occluders).
StructuredBuffer<float4> SharedBounds : register(t0);

// SlotTable[s] = (boundsOffset, count, writeOffsetWords, cumulativeThreadStart).
//   .x = start index into SharedBounds for this slot's run
//   .y = number of bounds in this slot (0 for scrubbed slots)
//   .z = start word offset into Visibility for this slot's bit range
//   .w = sum of .y for all prior entries - basis for thread->slot lookup
StructuredBuffer<uint4> SlotTable : register(t1);

// HZB pyramid: levels stacked horizontally; level i lives at x in [GetLevelOffsetX(i), GetLevelOffsetX(i+1))
Texture2D<float> HZBTexture : register(t2);

// 1 bit per bound entry, packed in uint32. Bit set = visible, clear = occluded.
// MUST be cleared to 0 before dispatch.
RWByteAddressBuffer Visibility : register(u0);

// Compute the horizontal pixel offset of a given pyramid level (see HZBData::GetOcclusionBounds layout).
uint GetLevelOffsetX(uint level)
{
    uint offset = 0;
    uint w = uint(PyramidBase.x);
    [loop]
    for (uint i = 0; i < level; i++)
    {
        offset += w;
        w = max(1u, w >> 1);
    }
    return offset;
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_HZBCull(uint3 dispatchId : SV_DispatchThreadID)
{
    uint g = dispatchId.x;
    if (g >= TotalBounds)
        return;

    // Locate the slot owning this thread: rightmost s with SlotTable[s].w <= g.
    // Scrubbed slots have .y == 0 and .w identical to the next slot, so the search lands on the
    // last entry in any run of equal-.w slots, which is the live one when one exists.
    uint lo = 0;
    uint hi = NumSlots - 1;
    [loop]
    while (lo < hi)
    {
        uint mid = (lo + hi + 1u) >> 1;
        if (SlotTable[mid].w <= g) lo = mid;
        else hi = mid - 1u;
    }
    uint4 slot = SlotTable[lo];
    if (slot.y == 0)
        return;

    uint i = g - slot.w;        // local index within slot
    if (i >= slot.y)
        return;                 // safety; shouldn't trigger with correct CPU bookkeeping

    float4 sphere = SharedBounds[slot.x + i];
    float radius = sphere.w;

    // Dead slot (vacated key): visible. Prevents stale CS verdicts and false-occluder feedback.
    if (radius <= 0.0)
    {
        Visibility.InterlockedOr((slot.z + (i >> 5u)) * 4u, 1u << (i & 31u));
        return;
    }

    // Dilate the test sphere by the camera's travel since the (last-frame) depth pyramid was built,
    // so a bound the camera has moved toward isn't culled against a now-stale occluder edge.
    radius += BoundsInflate;

    float3 centerVS = sphere.xyz - ViewOrigin;

    // Project center to clip space
    float4 clipCenter = mul(float4(centerVS, 1.0), ViewProjection);

    // Sphere intersects (or is behind) the near plane: treat as visible. Frustum cull handles the rest.
    if (clipCenter.w - radius < ViewNear)
    {
        Visibility.InterlockedOr((slot.z + (i >> 5u)) * 4u, 1u << (i & 31u));
        return;
    }

    // NDC center -> UV
    float2 ndcCenter = clipCenter.xy / clipCenter.w;
    float2 uvCenter = ndcCenter * float2(0.5, -0.5) + 0.5;

    // Approximate screen-space radius. Uses projection diagonals (correct only on-axis, conservative-ish off-axis).
    // Conservative tuning: add 1-pixel safety margin at base level.
    float radiusNdcX = radius * abs(ViewProjection[0][0]) / clipCenter.w;
    float radiusNdcY = radius * abs(ViewProjection[1][1]) / clipCenter.w;
    float2 radiusUV = float2(radiusNdcX, radiusNdcY) * 0.5;

    // Closest point on sphere along view forward (minimal device depth)
    float3 closestVS = centerVS - ViewForward * radius;
    float4 closestClip = mul(float4(closestVS, 1.0), ViewProjection);
    float closestDepth = saturate(closestClip.z / closestClip.w);

    // Screen-space rect in pixels at base level
    float2 rectMinPx = (uvCenter - radiusUV) * PyramidBase;
    float2 rectMaxPx = (uvCenter + radiusUV) * PyramidBase;
    float extentPx = max(rectMaxPx.x - rectMinPx.x, rectMaxPx.y - rectMinPx.y) + 2.0; // +2px safety
    extentPx = max(extentPx, 1.0); // guard against log2(<=0) on degenerate projections

    // Pick the pyramid level so the rect spans ~2 texels at that level
    int level = (int)ceil(log2(extentPx)) - 1;
    level = clamp(level, 0, (int)MaxLevel - 1);

    // Level texture extent
    uint levelOffsetX = GetLevelOffsetX((uint)level);
    uint levelW = max(1u, uint(PyramidBase.x) >> uint(level));
    uint levelH = max(1u, uint(PyramidBase.y) >> uint(level));

    // Sample range in level-local texel coords
    int2 texMin = int2(floor((uvCenter - radiusUV) * float2(levelW, levelH)));
    int2 texMax = int2(ceil((uvCenter + radiusUV) * float2(levelW, levelH)));
    texMin = clamp(texMin, int2(0, 0), int2((int)levelW - 1, (int)levelH - 1));
    texMax = clamp(texMax, int2(0, 0), int2((int)levelW - 1, (int)levelH - 1));

    // Take max depth across covered texels (bounded loop, ~4-9 samples typical)
    float maxSampled = 0.0;
    [loop]
    for (int y = texMin.y; y <= texMax.y; y++)
    {
        [loop]
        for (int x = texMin.x; x <= texMax.x; x++)
        {
            float d = HZBTexture.Load(int3(x + (int)levelOffsetX, y, 0));
            maxSampled = max(maxSampled, d);
        }
    }

    // Visible if any pixel in covered region is farther than (or equal to) the sphere's closest point.
    // DX convention: near=0, far=1. Pyramid stores max (farthest). Occluded iff maxSampled < closestDepth.
    bool visible = maxSampled >= closestDepth;
    if (visible)
        Visibility.InterlockedOr((slot.z + (i >> 5u)) * 4u, 1u << (i & 31u));
}
