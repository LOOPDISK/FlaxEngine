// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Light.h"

/// <summary>
/// Directional light emits light from direction in space.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Lights/Directional Light\"), ActorToolbox(\"Lights\")")
class FLAXENGINE_API DirectionalLight : public LightWithShadow
{
    DECLARE_SCENE_OBJECT(DirectionalLight);
public:
    /// <summary>
    /// The partitioning mode for the shadow cascades.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(64), DefaultValue(PartitionMode.Manual), EditorDisplay(\"Shadow\")")
    PartitionMode PartitionMode = PartitionMode::Manual;

    /// <summary>
    /// The number of cascades used for slicing the range of depth covered by the light during shadow rendering. Values are 1, 2 or 4 cascades; a typical scene uses 4 cascades.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(65), DefaultValue(4), Limit(1, 4), EditorDisplay(\"Shadow\")")
    int32 CascadeCount = 4;

    /// <summary>
    /// Percentage of the shadow distance used by the first cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(66), DefaultValue(0.05f), VisibleIf(nameof(ShowCascade1)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade1Spacing = 0.05f;

    /// <summary>
    /// Percentage of the shadow distance used by the second cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(67), DefaultValue(0.15f), VisibleIf(nameof(ShowCascade2)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade2Spacing = 0.15f;

    /// <summary>
    /// Percentage of the shadow distance used by the third cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(68), DefaultValue(0.50f), VisibleIf(nameof(ShowCascade3)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade3Spacing = 0.50f;

    /// <summary>
    /// Percentage of the shadow distance used by the fourth cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(69), DefaultValue(1.0f), VisibleIf(nameof(ShowCascade4)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade4Spacing = 1.0f;

    /// <summary>
    /// Enables distant shadow map for rendering shadows of static geometry beyond CSM range. This provides low-resolution, blurred shadows for distant structures.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(70), DefaultValue(true), EditorDisplay(\"Shadow\", \"Enable Distant Shadows\")")
    bool EnableDistantShadows = true;

    /// <summary>
    /// The world-space size (in cm) covered by the distant shadow map. Larger values cover more area but reduce shadow detail. Default is 5000000 (50km).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(71), DefaultValue(5000000.0f), VisibleIf(nameof(ShowDistantShadowSettings)), Limit(100000, 100000000, 100000), EditorDisplay(\"Shadow\", \"Distant Shadow Size\")")
    float DistantShadowSize = 5000000.0f;

    /// <summary>
    /// How often (in frames) to update the distant shadow map. Higher values improve performance. Default is 30 frames. Set to 0 to update only when dirty.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(72), DefaultValue(30), VisibleIf(nameof(ShowDistantShadowSettings)), Limit(0, 300), EditorDisplay(\"Shadow\", \"Distant Shadow Update Rate\")")
    int32 DistantShadowUpdateRate = 30;

    /// <summary>
    /// Resolution of the distant shadow map. Lower resolutions are acceptable due to blur. Default is 1024.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(73), DefaultValue(1024), VisibleIf(nameof(ShowDistantShadowSettings)), EditorDisplay(\"Shadow\", \"Distant Shadow Resolution\")")
    int32 DistantShadowResolution = 1024;

    /// <summary>
    /// Depth bias multiplier for distant shadows to prevent z-fighting. Higher values reduce self-shadowing artifacts. Default is 0.0005.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(74), DefaultValue(0.0005f), VisibleIf(nameof(ShowDistantShadowSettings)), Limit(0, 0.01f, 0.0001f), EditorDisplay(\"Shadow\", \"Distant Shadow Depth Bias\")")
    float DistantShadowDepthBias = 0.0005f;

    /// <summary>
    /// Normal offset bias scale for distant shadows. Reduces peter-panning on grazing angles. Default is 0.5.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(75), DefaultValue(0.5f), VisibleIf(nameof(ShowDistantShadowSettings)), Limit(0, 2.0f, 0.1f), EditorDisplay(\"Shadow\", \"Distant Shadow Normal Bias Scale\")")
    float DistantShadowNormalBiasScale = 0.5f;

public:
    // [LightWithShadow]
    void Draw(RenderContext& renderContext) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;

protected:
    // [LightWithShadow]
    void OnTransformChanged() override;
};
