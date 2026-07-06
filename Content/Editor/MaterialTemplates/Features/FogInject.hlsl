// Copyright (c) Wojciech Figat. All rights reserved.

@0// Fog Inject: Defines
@1// Fog Inject: Includes
@2// Fog Inject: Constants
@3// Fog Inject: Resources
Texture2D FogInjectSceneDepth : register(t__SRV__);
@4// Fog Inject: Utilities
@5// Fog Inject: Shaders
#if USE_FOG_INJECT

// Pixel Shader function for Fog Inject Pass
// Particles accumulate (additive float target) into a low-res buffer that the distance fog and
// depth haze read to thicken density. R = density push, GBA reserved for future channels
// (color / emission / heat-haze).
META_PS(USE_FOG_INJECT, FEATURE_LEVEL_ES2)
float4 PS_FogInject(PixelInput input) : SV_Target0
{
	MaterialInput materialInput = GetMaterialInput(input);
#if USE_DITHERED_LOD_TRANSITION
	ClipLODTransition(materialInput);
#endif

	// Get material parameters
	Material material = GetMaterialPS(materialInput);

	// Masking
#if MATERIAL_MASKED
	clip(material.Mask - MATERIAL_MASK_THRESHOLD);
#endif

	// Opacity is the fog-density authoring channel (Color/Emissive stay purely visual): 0 = no effect,
	// 1 = full-strength push, so the cleared (0) buffer reads neutral and no midpoint is needed.
	// Push-only: particles thicken the fog/haze; overlapping particles accumulate additively
	// (the consumers clamp the total). Note: additive particles ignore Opacity visually, so
	// author it purely for fog there.
	float push = saturate(material.Opacity);

	// Soft depth occlusion: particles behind geometry must not push fog. SvPosition.w holds the
	// particle's linear view depth; the scene depth is linearized with the same ViewInfo terms as
	// the fog/haze passes. Softness scales with distance (min 1m) to avoid pops at quarter res.
	// ScreenSize matches the low-res inject target during this pass (patched in Renderer.cpp) so
	// screen-space UVs stay [0;1] here and in material graph nodes (depth fade, scene depth).
	float2 depthUV = materialInput.SvPosition.xy * ScreenSize.zw;
	float deviceDepth = FogInjectSceneDepth.SampleLevel(SamplerPointClamp, depthUV, 0).r;
	float sceneDepth = (ViewInfo.w * ViewFar) / (deviceDepth - ViewInfo.z);
	float particleDepth = materialInput.SvPosition.w;
	push *= saturate((sceneDepth - particleDepth) / max(particleDepth * 0.05, 100.0));

	return float4(push, 0, 0, 0);
}

#endif
