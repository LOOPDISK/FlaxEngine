// File generated by Flax Materials Editor
// Version: @0

#define MATERIAL 1
#define USE_PER_VIEW_CONSTANTS 1
@3
#include "./Flax/Common.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/GBufferCommon.hlsl"
@7
// Primary constant buffer (with additional material parameters)
META_CB_BEGIN(0, Data)
float4x4 WorldMatrix;
float4x4 LocalMatrix;
float3 Dummy0;
float WorldDeterminantSign;
float MeshMinZ;
float Segment;
float ChunksPerSegment;
float PerInstanceRandom;
float3 GeometrySize;
float MeshMaxZ;
@1META_CB_END

// Shader resources
@2
// The spline deformation buffer (stored as 4x3, 3 float4 behind each other)
Buffer<float4> SplineDeformation : register(t0);

// Geometry data passed though the graphics rendering stages up to the pixel shader
struct GeometryData
{
	float3 WorldPosition : TEXCOORD0;
	float2 TexCoord : TEXCOORD1;
#if USE_VERTEX_COLOR
	half4 VertexColor : COLOR;
#endif
	float3 WorldNormal : TEXCOORD2;
	float4 WorldTangent : TEXCOORD3;
};

// Interpolants passed from the vertex shader
struct VertexOutput
{
	float4 Position : SV_Position;
	GeometryData Geometry;
#if USE_CUSTOM_VERTEX_INTERPOLATORS
	float4 CustomVSToPS[CUSTOM_VERTEX_INTERPOLATORS_COUNT] : TEXCOORD9;
#endif
#if USE_TESSELLATION
    float TessellationMultiplier : TESS;
#endif
};

// Interpolants passed to the pixel shader
struct PixelInput
{
	float4 Position : SV_Position;
	GeometryData Geometry;
#if USE_CUSTOM_VERTEX_INTERPOLATORS
	float4 CustomVSToPS[CUSTOM_VERTEX_INTERPOLATORS_COUNT] : TEXCOORD9;
#endif
	bool IsFrontFace : SV_IsFrontFace;
};

// Material properties generation input
struct MaterialInput
{
	float3 WorldPosition;
	float TwoSidedSign;
	float2 TexCoord;
#if USE_VERTEX_COLOR
	half4 VertexColor;
#endif
	float3x3 TBN;
	float4 SvPosition;
	float3 PreSkinnedPosition;
	float3 PreSkinnedNormal;
#if USE_CUSTOM_VERTEX_INTERPOLATORS
	float4 CustomVSToPS[CUSTOM_VERTEX_INTERPOLATORS_COUNT];
#endif
};

// Extracts geometry data to the material input
MaterialInput GetGeometryMaterialInput(GeometryData geometry)
{
	MaterialInput output = (MaterialInput)0;
	output.WorldPosition = geometry.WorldPosition;
	output.TexCoord = geometry.TexCoord;
#if USE_VERTEX_COLOR
	output.VertexColor = geometry.VertexColor;
#endif
	output.TBN = CalcTangentBasis(geometry.WorldNormal, geometry.WorldTangent);
	return output;
}

#if USE_TESSELLATION

// Interpolates the geometry positions data only (used by the tessallation when generating vertices)
#define InterpolateGeometryPositions(output, p0, w0, p1, w1, p2, w2, offset) output.WorldPosition = p0.WorldPosition * w0 + p1.WorldPosition * w1 + p2.WorldPosition * w2 + offset

// Offsets the geometry positions data only (used by the tessallation when generating vertices)
#define OffsetGeometryPositions(geometry, offset) geometry.WorldPosition += offset

// Applies the Phong tessallation to the geometry positions (used by the tessallation when doing Phong tess)
#define ApplyGeometryPositionsPhongTess(geometry, p0, p1, p2, U, V, W) \
	float3 posProjectedU = TessalationProjectOntoPlane(p0.WorldNormal, p0.WorldPosition, geometry.WorldPosition); \
	float3 posProjectedV = TessalationProjectOntoPlane(p1.WorldNormal, p1.WorldPosition, geometry.WorldPosition); \
	float3 posProjectedW = TessalationProjectOntoPlane(p2.WorldNormal, p2.WorldPosition, geometry.WorldPosition); \
	geometry.WorldPosition = U * posProjectedU + V * posProjectedV + W * posProjectedW

// Interpolates the geometry data except positions (used by the tessallation when generating vertices)
GeometryData InterpolateGeometry(GeometryData p0, float w0, GeometryData p1, float w1, GeometryData p2, float w2)
{
	GeometryData output = (GeometryData)0;
	output.TexCoord = p0.TexCoord * w0 + p1.TexCoord * w1 + p2.TexCoord * w2;
#if USE_VERTEX_COLOR
	output.VertexColor = p0.VertexColor * w0 + p1.VertexColor * w1 + p2.VertexColor * w2;
#endif
	output.WorldNormal = p0.WorldNormal * w0 + p1.WorldNormal * w1 + p2.WorldNormal * w2;
	output.WorldNormal = normalize(output.WorldNormal);
	output.WorldTangent = p0.WorldTangent * w0 + p1.WorldTangent * w1 + p2.WorldTangent * w2;
	output.WorldTangent.xyz = normalize(output.WorldTangent.xyz);
	return output;
}

#endif

MaterialInput GetMaterialInput(PixelInput input)
{
	MaterialInput output = GetGeometryMaterialInput(input.Geometry);
	output.TwoSidedSign = WorldDeterminantSign * (input.IsFrontFace ? 1.0 : -1.0);
	output.SvPosition = input.Position;
#if USE_CUSTOM_VERTEX_INTERPOLATORS
	output.CustomVSToPS = input.CustomVSToPS;
#endif
	return output;
}

// Removes the scale vector from the local to world transformation matrix
float3x3 RemoveScaleFromLocalToWorld(float3x3 localToWorld)
{
	// Extract per axis scales from localToWorld transform
	float scaleX = length(localToWorld[0]);
	float scaleY = length(localToWorld[1]);
	float scaleZ = length(localToWorld[2]);
	float3 invScale = float3(
		scaleX > 0.00001f ? 1.0f / scaleX : 0.0f,
		scaleY > 0.00001f ? 1.0f / scaleY : 0.0f,
		scaleZ > 0.00001f ? 1.0f / scaleZ : 0.0f);
	localToWorld[0] *= invScale.x;
	localToWorld[1] *= invScale.y;
	localToWorld[2] *= invScale.z;
	return localToWorld;
}

// Transforms a vector from tangent space to world space
float3 TransformTangentVectorToWorld(MaterialInput input, float3 tangentVector)
{
	return mul(tangentVector, input.TBN);
}

// Transforms a vector from world space to tangent space
float3 TransformWorldVectorToTangent(MaterialInput input, float3 worldVector)
{
	return mul(input.TBN, worldVector);
}

// Transforms a vector from world space to view space
float3 TransformWorldVectorToView(MaterialInput input, float3 worldVector)
{
	return mul(worldVector, (float3x3)ViewMatrix);
}

// Transforms a vector from view space to world space
float3 TransformViewVectorToWorld(MaterialInput input, float3 viewVector)
{
	return mul((float3x3)ViewMatrix, viewVector);
}

// Transforms a vector from local space to world space
float3 TransformLocalVectorToWorld(MaterialInput input, float3 localVector)
{
	float3x3 localToWorld = (float3x3)WorldMatrix;
	//localToWorld = RemoveScaleFromLocalToWorld(localToWorld);
	return mul(localVector, localToWorld);
}

// Transforms a vector from local space to world space
float3 TransformWorldVectorToLocal(MaterialInput input, float3 worldVector)
{
	float3x3 localToWorld = (float3x3)WorldMatrix;
	//localToWorld = RemoveScaleFromLocalToWorld(localToWorld);
	return mul(localToWorld, worldVector);
}

// Gets the current object position
float3 GetObjectPosition(MaterialInput input)
{
	return WorldMatrix[3].xyz;
}

// Gets the current object size
float3 GetObjectSize(MaterialInput input)
{
	float4x4 world = WorldMatrix;
	return GeometrySize * float3(world._m00, world._m11, world._m22);
}

// Gets the current object scale (supports instancing)
float3 GetObjectScale(MaterialInput input)
{
    float4x4 world = WorldMatrix;

    // Extract scale from the world matrix
    float3 scale;
    scale.x = length(float3(world._11, world._12, world._13));
    scale.y = length(float3(world._21, world._22, world._23));
    scale.z = length(float3(world._31, world._32, world._33));

    return scale;
}

// Get the current object random value
float GetPerInstanceRandom(MaterialInput input)
{
	return PerInstanceRandom;
}

// Get the current object LOD transition dither factor
float GetLODDitherFactor(MaterialInput input)
{
	return 0;
}

// Gets the interpolated vertex color (in linear space)
float4 GetVertexColor(MaterialInput input)
{
#if USE_VERTEX_COLOR
	return input.VertexColor;
#else
	return 1;
#endif
}

float3 SampleLightmap(Material material, MaterialInput materialInput)
{
	return 0;
}

@8

// Get material properties function (for vertex shader)
Material GetMaterialVS(MaterialInput input)
{
@5
}

// Get material properties function (for domain shader)
Material GetMaterialDS(MaterialInput input)
{
@6
}

// Get material properties function (for pixel shader)
Material GetMaterialPS(MaterialInput input)
{
@4
}

// Calculates the transform matrix from mesh tangent space to local space
float3x3 CalcTangentToLocal(ModelInput input)
{
	float bitangentSign = input.Tangent.w ? -1.0f : +1.0f;
	float3 normal = input.Normal.xyz * 2.0 - 1.0;
	float3 tangent = input.Tangent.xyz * 2.0 - 1.0;
	float3 bitangent = cross(normal, tangent) * bitangentSign;
	return float3x3(tangent, bitangent, normal);
}

// Vertex Shader function for GBuffer Pass and Depth Pass (with full vertex data)
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT,   0, 0,     PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R16G16_FLOAT,      1, 0,     PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(NORMAL,   0, R10G10B10A2_UNORM, 1, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TANGENT,  0, R10G10B10A2_UNORM, 1, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 1, R16G16_FLOAT,      1, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(COLOR,    0, R8G8B8A8_UNORM,    2, 0,     PER_VERTEX, 0, USE_VERTEX_COLOR)
VertexOutput VS_SplineModel(ModelInput input)
{
	VertexOutput output;

	// Apply local transformation of the geometry before deformation
	float3 position = mul(float4(input.Position, 1), LocalMatrix).xyz;
	float4x4 world = LocalMatrix;

	// Apply spline curve deformation
	float splineAlpha = saturate((position.z - MeshMinZ) / (MeshMaxZ - MeshMinZ));
	int splineIndex = (int)((Segment + splineAlpha) * ChunksPerSegment);
	position.z = splineAlpha;
	float3x4 splineMatrix = float3x4(SplineDeformation[splineIndex * 3], SplineDeformation[splineIndex * 3 + 1], SplineDeformation[splineIndex * 3 + 2]);
	position = mul(splineMatrix, float4(position, 1));
	float4x3 splineMatrixT = transpose(splineMatrix);
	world = mul(world, float4x4(float4(splineMatrixT[0], 0), float4(splineMatrixT[1], 0), float4(splineMatrixT[2], 0), float4(splineMatrixT[3], 1)));

	// Compute world space vertex position
	output.Geometry.WorldPosition = mul(float4(position, 1), WorldMatrix).xyz;
	world = mul(world, WorldMatrix);

	// Compute clip space position
	output.Position = mul(float4(output.Geometry.WorldPosition, 1), ViewProjectionMatrix);

	// Pass vertex attributes
	output.Geometry.TexCoord = input.TexCoord0;
#if USE_VERTEX_COLOR
	output.Geometry.VertexColor = input.Color;
#endif

	// Calculate tanget space to world space transformation matrix for unit vectors
	float3x3 tangentToLocal = CalcTangentToLocal(input);
	float3x3 localToWorld = RemoveScaleFromLocalToWorld((float3x3)world);
	float3x3 tangentToWorld = mul(tangentToLocal, localToWorld); 
	output.Geometry.WorldNormal = tangentToWorld[2];
	output.Geometry.WorldTangent.xyz = tangentToWorld[0];
	output.Geometry.WorldTangent.w = input.Tangent.w ? -1.0f : +1.0f;

	// Get material input params if need to evaluate any material property
#if USE_POSITION_OFFSET || USE_TESSELLATION || USE_CUSTOM_VERTEX_INTERPOLATORS
	MaterialInput materialInput = GetGeometryMaterialInput(output.Geometry);
	materialInput.TwoSidedSign = WorldDeterminantSign;
	materialInput.SvPosition = output.Position;
	materialInput.PreSkinnedPosition = input.Position.xyz;
	materialInput.PreSkinnedNormal = tangentToLocal[2].xyz;
	Material material = GetMaterialVS(materialInput);
#endif

	// Apply world position offset per-vertex
#if USE_POSITION_OFFSET
	output.Geometry.WorldPosition += material.PositionOffset;
	output.Position = mul(float4(output.Geometry.WorldPosition, 1), ViewProjectionMatrix);
#endif

	// Get tessalation multiplier (per vertex)
#if USE_TESSELLATION
    output.TessellationMultiplier = material.TessellationMultiplier;
#endif

	// Copy interpolants for other shader stages
#if USE_CUSTOM_VERTEX_INTERPOLATORS
	output.CustomVSToPS = material.CustomVSToPS;
#endif

	return output;
}

#if USE_DITHERED_LOD_TRANSITION

void ClipLODTransition(PixelInput input)
{
}

#endif

// Pixel Shader function for Depth Pass
META_PS(true, FEATURE_LEVEL_ES2)
void PS_Depth(PixelInput input)
{
#if MATERIAL_MASKED || MATERIAL_BLEND != MATERIAL_BLEND_OPAQUE 
	// Get material parameters
	MaterialInput materialInput = GetMaterialInput(input);
	Material material = GetMaterialPS(materialInput);

	// Perform per pixel clipping
#if MATERIAL_MASKED
	clip(material.Mask - MATERIAL_MASK_THRESHOLD);
#endif
#if MATERIAL_BLEND != MATERIAL_BLEND_OPAQUE
	clip(material.Opacity - MATERIAL_OPACITY_THRESHOLD);
#endif
#endif
}

#if _PS_QuadOverdraw

#include "./Flax/Editor/QuadOverdraw.hlsl"

// Pixel Shader function for Quad Overdraw Pass (editor-only)
[earlydepthstencil]
META_PS(USE_EDITOR, FEATURE_LEVEL_SM5)
void PS_QuadOverdraw(float4 svPos : SV_Position, uint primId : SV_PrimitiveID)
{
	DoQuadOverdraw(svPos, primId);
}

#endif

@9
