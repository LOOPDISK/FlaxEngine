// Copyright (c) Wojciech Figat. All rights reserved.

// Compute skinning: reads a skinned VB + bone matrices, applies linear-blend skinning, and writes a
// static-layout output (VB0 = Position; VB1 = TexCoord+Normal+Tangent+TexCoord1). At draw time the actor
// nulls Surface.Skinning and binds these, so the static VS variant runs with no VS-time skinning. The
// input layout isn't hardcoded - the host passes per-element offsets, stride and format flags via the CB.

#include "./Flax/Common.hlsl"

#define OUTPUT0_STRIDE      12
#define OUTPUT1_STRIDE      16

// Flags bits (match SkinningPass.cpp): 0x1 weights R8G8B8A8_UNorm, 0x2 indices R16G16B16A16_UInt,
// 0x4 position R16G16B16A16_Float, 0x8 source has Color (passed through to OutputVB2).
#define FLAG_WEIGHTS_R8G8B8A8_UNORM       0x1u
#define FLAG_INDICES_R16G16B16A16_UINT    0x2u
#define FLAG_POSITION_R16G16B16A16_FLOAT  0x4u
#define FLAG_HAS_VERTEX_COLOR             0x8u

META_CB_BEGIN(0, SkinningData)
uint VertexCount;
uint BoneCount;
uint InputStride;
uint Flags;
uint OffsetPosition;
uint OffsetTexCoord;
uint OffsetNormal;
uint OffsetTangent;
uint OffsetBlendIndices;
uint OffsetBlendWeights;
uint OffsetColor;
uint _padding;
META_CB_END

// Bone matrices: 3 float4 per bone (4x3 transposed transform). Matches the existing material binding
// (Buffer<float4>) in Surface.shader, so we reuse SkinnedMeshDrawData::BoneMatrices directly.
Buffer<float4> BoneMatrices : register(t0);

// Skinned source VB as raw bytes (mixed formats, hand-unpacked below).
ByteAddressBuffer InputVB : register(t1);

RWByteAddressBuffer OutputVB0 : register(u0); // Position
RWByteAddressBuffer OutputVB1 : register(u1); // TexCoord + Normal + Tangent + TexCoord1
RWByteAddressBuffer OutputVB2 : register(u2); // Color (only written when FLAG_HAS_VERTEX_COLOR is set; unbound otherwise)

float3x4 GetBoneMatrix(uint index)
{
    float4 a = BoneMatrices[index * 3 + 0];
    float4 b = BoneMatrices[index * 3 + 1];
    float4 c = BoneMatrices[index * 3 + 2];
    return float3x4(a, b, c);
}

// Unpack R10G10B10A2_UNORM -> float4 in [0,1]
float4 UnpackR10G10B10A2_UNORM(uint raw)
{
    float r = ((raw      ) & 0x3FFu) / 1023.0;
    float g = ((raw >> 10) & 0x3FFu) / 1023.0;
    float b = ((raw >> 20) & 0x3FFu) / 1023.0;
    float a = ((raw >> 30) & 0x3u  ) /    3.0;
    return float4(r, g, b, a);
}

// Pack float4 in [0,1] -> R10G10B10A2_UNORM
uint PackR10G10B10A2_UNORM(float4 v)
{
    v = saturate(v);
    uint r = (uint)(v.x * 1023.0 + 0.5);
    uint g = (uint)(v.y * 1023.0 + 0.5);
    uint b = (uint)(v.z * 1023.0 + 0.5);
    uint a = (uint)(v.w *    3.0 + 0.5);
    return r | (g << 10) | (b << 20) | (a << 30);
}

// Pack two floats into a single 32-bit R16G16_Float word
uint PackHalf2(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_Skin(uint3 dispatchId : SV_DispatchThreadID)
{
    uint vid = dispatchId.x;
    if (vid >= VertexCount)
        return;

    uint base = vid * InputStride;

    // Position: either 3 x float32 (12 bytes) or 4 x float16 (8 bytes, w lane unused).
    float3 position;
    if (Flags & FLAG_POSITION_R16G16B16A16_FLOAT)
    {
        uint2 posRaw = InputVB.Load2(base + OffsetPosition);
        position = float3(
            f16tof32(posRaw.x        & 0xFFFFu),
            f16tof32(posRaw.x >> 16),
            f16tof32(posRaw.y        & 0xFFFFu));
    }
    else
    {
        position = asfloat(InputVB.Load3(base + OffsetPosition));
    }

    // TexCoord: copy raw 32-bit R16G16 (no re-encoding needed).
    uint texcoordRaw = InputVB.Load(base + OffsetTexCoord);

    // Normal/Tangent: unpack R10G10B10A2_UNORM, remap [0,1] -> [-1,1] for the unit vectors.
    uint normalRaw  = InputVB.Load(base + OffsetNormal);
    uint tangentRaw = InputVB.Load(base + OffsetTangent);
    float4 normalUnpacked  = UnpackR10G10B10A2_UNORM(normalRaw);
    float4 tangentUnpacked = UnpackR10G10B10A2_UNORM(tangentRaw);
    float3 normal  = normalUnpacked.xyz  * 2.0 - 1.0;
    float3 tangent = tangentUnpacked.xyz * 2.0 - 1.0;
    float  tangentW = tangentUnpacked.w;  // bitangent sign bit; preserve as-is for the VS

    // BlendIndices: either 4 x uint8 (packed in a single 32-bit word) or 4 x uint16 (two 32-bit words).
    uint4 indices;
    if (Flags & FLAG_INDICES_R16G16B16A16_UINT)
    {
        uint2 idxRaw = InputVB.Load2(base + OffsetBlendIndices);
        indices = uint4(
            (idxRaw.x      ) & 0xFFFFu,
            (idxRaw.x >> 16),
            (idxRaw.y      ) & 0xFFFFu,
            (idxRaw.y >> 16));
    }
    else
    {
        uint indicesRaw = InputVB.Load(base + OffsetBlendIndices);
        indices = uint4(
            (indicesRaw      ) & 0xFFu,
            (indicesRaw >>  8) & 0xFFu,
            (indicesRaw >> 16) & 0xFFu,
            (indicesRaw >> 24) & 0xFFu);
    }

    // BlendWeights: either 4 x unorm8 (single 32-bit word) or 4 x float16 (two 32-bit words).
    float4 weights;
    if (Flags & FLAG_WEIGHTS_R8G8B8A8_UNORM)
    {
        uint weightsRaw = InputVB.Load(base + OffsetBlendWeights);
        weights = float4(
            ((weightsRaw      ) & 0xFFu) / 255.0,
            ((weightsRaw >>  8) & 0xFFu) / 255.0,
            ((weightsRaw >> 16) & 0xFFu) / 255.0,
            ((weightsRaw >> 24) & 0xFFu) / 255.0);
    }
    else
    {
        uint2 weightsRaw = InputVB.Load2(base + OffsetBlendWeights);
        weights = float4(
            f16tof32(weightsRaw.x        & 0xFFFFu),
            f16tof32(weightsRaw.x >> 16),
            f16tof32(weightsRaw.y        & 0xFFFFu),
            f16tof32(weightsRaw.y >> 16));
    }

    // Blended bone matrix. Mirrors Surface.shader::GetBoneMatrix exactly so output matches VS_Skinned bit-for-bit.
    float weightsSum = weights.x + weights.y + weights.z + weights.w;
    float mainWeight = weights.x + (1.0 - weightsSum); // re-normalize against 16-bit weight encoding error
    float3x4 boneMatrix  = mainWeight * GetBoneMatrix(indices.x);
    boneMatrix          += weights.y  * GetBoneMatrix(indices.y);
    boneMatrix          += weights.z  * GetBoneMatrix(indices.z);
    boneMatrix          += weights.w  * GetBoneMatrix(indices.w);

    // Apply skinning to position (w=1) and normal/tangent (w=0, direction only).
    float3 skinnedPosition = mul(boneMatrix, float4(position, 1));
    float3 skinnedNormal   = normalize(mul(boneMatrix, float4(normal,  0)));
    float3 skinnedTangent  = normalize(mul(boneMatrix, float4(tangent, 0)));

    // Repack normal/tangent. Remap [-1,1] -> [0,1] to match R10G10B10A2_UNORM input convention.
    uint packedNormal  = PackR10G10B10A2_UNORM(float4(skinnedNormal  * 0.5 + 0.5, 0.0));
    uint packedTangent = PackR10G10B10A2_UNORM(float4(skinnedTangent * 0.5 + 0.5, tangentW));

    // Write VB0 (position only).
    OutputVB0.Store3(vid * OUTPUT0_STRIDE, asuint(skinnedPosition));

    // Write VB1: TexCoord + Normal + Tangent + TexCoord1(=0).
    uint out1Base = vid * OUTPUT1_STRIDE;
    OutputVB1.Store(out1Base +  0, texcoordRaw);
    OutputVB1.Store(out1Base +  4, packedNormal);
    OutputVB1.Store(out1Base +  8, packedTangent);
    OutputVB1.Store(out1Base + 12, 0u); // TexCoord1 unused (skinned source has no second UV)

    // Write VB2: Color (R8G8B8A8_UNorm). Skinning doesn't transform vertex color, so this is a
    // straight passthrough of the source 32-bit word. Skipped entirely for meshes without color.
    if (Flags & FLAG_HAS_VERTEX_COLOR)
    {
        uint colorRaw = InputVB.Load(base + OffsetColor);
        OutputVB2.Store(vid * 4, colorRaw);
    }
}
