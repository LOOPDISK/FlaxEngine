// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"

// Interpolants passed from the vertex shader to the PostFx material pixel shader
struct MaterialVertexOutput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : TEXCOORD0;
	float2 TexCoord      : TEXCOORD1;
};

META_CB_BEGIN(0, Data)
float4 Color;
float4 Params;
META_CB_END

// Vertex Shader for screen space quad rendering
META_VS(true, FEATURE_LEVEL_ES2)
Quad_VS2PS VS(float2 Position : POSITION0, float2 TexCoord : TEXCOORD0)
{
	Quad_VS2PS output;
	output.Position = float4(Position, 0, 1);
	output.TexCoord = TexCoord;
	return output;
}

// Vertex Shader function for postFx materials rendering
META_VS(true, FEATURE_LEVEL_ES2)
MaterialVertexOutput VS_PostFx(float2 Position : POSITION0, float2 TexCoord : TEXCOORD0)
{
	MaterialVertexOutput output;
	output.Position = float4(Position, 0, 1);
	output.WorldPosition = output.Position.xyz;
	output.TexCoord = TexCoord;
	return output;
}

#ifdef _PS_CopyLinear

Texture2D Source : register(t0);

// Pixel Shader for screen space quad rendering for image copy (linear sampling)
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_CopyLinear(Quad_VS2PS input) : SV_Target
{
	return Source.SampleLevel(SamplerLinearClamp, input.TexCoord, 0);
}

#endif

// Pixel Shader for clearing a render target with a solid color
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Clear(Quad_VS2PS input) : SV_Target
{
	return Color;
}

// Pixel Shader for clearing depth buffer
META_PS(true, FEATURE_LEVEL_ES2)
float PS_DepthClear(Quad_VS2PS input) : SV_Depth
{
	return Color.r;
}

#ifdef _PS_DepthCopy

Texture2D Source : register(t0);

// Pixel Shader for copying depth buffer
META_PS(true, FEATURE_LEVEL_ES2)
float PS_DepthCopy(Quad_VS2PS input) : SV_Depth
{
    return SAMPLE_RT_DEPTH(Source, input.TexCoord * Color.xy + Color.zw);
}

#endif

#ifdef _PS_DepthVisualize

Texture2D Source : register(t0);

// HACK: visualize a depth texture as grayscale. Color.x = invert flag (0/1), Color.y = contrast power.
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DepthVisualize(Quad_VS2PS input) : SV_Target
{
	float d = Source.SampleLevel(SamplerPointClamp, input.TexCoord, 0).r;
	if (Color.x > 0.5)
		d = 1.0 - d;
	d = pow(saturate(d), max(Color.y, 0.0001));
	return float4(d, d, d, 1);
}

#endif

#ifdef _PS_ClipmapComposite

Texture2D ClipmapDepth : register(t0);

// Pixel Shader for compositing a toroidal shadow clipmap level into a CSM cascade tile.
// Color.xy = UV scale, Color.zw = UV offset (map cascade tile UV to LOGICAL clipmap UV).
// Params.x = depth scale, Params.y = depth bias (remap clipmap depth to cascade depth range).
// Params.zw = toroidal wrap offset: texture UV = frac(logicalUV + Params.zw).
//
// "No occluder" pixels (clipmapDepth ~ 1) are discarded so the cleared cascade-tile depth=1
// stays in place - equivalent to writing 1 but cheaper (skips the MAD + saturate).
//
// For occluder pixels we SATURATE the remapped depth instead of discarding when it falls
// outside [0,1]. The discard variant of this code is silently WRONG for tall geometry:
//
//   - Tall structures (skyscrapers, cliffs, mountains) captured in the clipmap have
//     cascadeDepth < 0 - they occlude FROM ABOVE the cascade's near plane.
//   - Discarding leaves the cleared tile depth=1 at the structure's projected footprint.
//   - Lighting pass: fragment in cascade slice (depth ~ 0.5) tests against 1 -> not in shadow
//     -> light leaks straight through the structure, "cutting holes" in adjacent shadows.
//
// Saturating clamps cascadeDepth < 0 to exactly 0 (treat as "at the cascade near plane"),
// which is the geometrically correct treatment for occluders in front of the cascade slice
// - they shadow everything in the slice behind them. cascadeDepth > 1 saturates to 1, which
// matches the cleared tile and produces no shadow (also correct: occluder behind cascade
// doesn't cast onto cascade fragments).
META_PS(true, FEATURE_LEVEL_ES2)
float PS_ClipmapComposite(Quad_VS2PS input) : SV_Depth
{
	float2 uv = frac(input.TexCoord * Color.xy + Color.zw + Params.zw);
	float clipmapDepth = ClipmapDepth.SampleLevel(SamplerPointClamp, uv, 0).r;
	if (clipmapDepth >= 0.999)
		discard; // No occluder at this clipmap texel - keep cleared tile depth=1.
	float cascadeDepth = clipmapDepth * Params.x + Params.y;
	return saturate(cascadeDepth); // Clamp tall-occluder (< 0) and behind-cascade (> 1) cases.
}

#endif

float4 yuv2rgb(int y, int u, int v)
{
    u -= 128;
    v -= 128;
    float r = y + 1.402 * v;
    float g = y - 0.34414 * u - 0.71414 * v;
    float b = y + 1.772 * u;
    return float4(r, g, b, 256.0f) / 256.0f;
}

#ifdef _PS_DecodeYUY2

// Raw memory with texture of format YUY2 and size passed in Color.xy
Buffer<uint> SourceYUY2 : register(t0);

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DecodeYUY2(Quad_VS2PS input) : SV_Target
{
    // Read YUY2 pixel
    uint p = (uint)input.Position.y * (uint)Color.x + (uint)input.Position.x;
    uint data = SourceYUY2[p / 2];

    // Unpack YUY components
    int v = ((data & 0xff000000) >> 24);
    int y1 = (data & 0xff0000) >> 16;
    int u = ((data & 0xff00) >> 8);
    int y0 = data & 0xff;
    int y = p % 2 == 0 ? y0: y1;

    // Convert yuv to rgb
    return yuv2rgb(y, u, v);
}

#endif

#ifdef _PS_DecodeNV12

// Raw memory with texture of format NV12 and size passed in Color.xy
Buffer<uint> SourceNV12 : register(t0);

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DecodeNV12(Quad_VS2PS input) : SV_Target
{
    // Read NV12 pixel (Y plane of size w*h, followed by interleaved UV plane is of size w*h/2)
    uint size = (uint)(Color.x * Color.y);
    uint p = (uint)input.Position.y * (uint)Color.x + (uint)input.Position.x;
    uint y = (SourceNV12[p / 4] >> ((p % 4) * 8)) & 0xff;
    p = (uint)(input.Position.y * 0.5f) * (uint)Color.x + (uint)input.Position.x;
    p = (p / 2) * 2;
    uint u = (SourceNV12[size / 4 + p / 4] >> ((p % 4) * 8)) & 0xff;
    p = (uint)(input.Position.y * 0.5f) * (uint)Color.x + (uint)input.Position.x;
    p = (p / 2) * 2 + 1;
    uint v = (SourceNV12[size / 4 + p / 4] >> ((p % 4) * 8)) & 0xff;

    // Convert yuv to rgb
    return yuv2rgb(y, u, v);
}

#endif
