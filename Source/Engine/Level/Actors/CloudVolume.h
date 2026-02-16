// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "../Actor.h"
#include "Engine/Content/Assets/Model.h"

/// <summary>
/// Mesh-based cloud volume used by stylized cloud rendering.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Visuals/Lighting & PostFX/Cloud Volume\"), ActorToolbox(\"Visuals\")")
class FLAXENGINE_API CloudVolume : public Actor
{
    DECLARE_SCENE_OBJECT(CloudVolume);
private:
    int32 _sceneRenderingKey = -1;

public:
    /// <summary>
    /// The cloud mesh to render.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), DefaultValue(null), EditorDisplay(\"Cloud\")")
    AssetReference<Model> CloudMesh;

    /// <summary>
    /// Directional (sun) lighting contribution scale.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), DefaultValue(1.0f), Limit(0), EditorDisplay(\"Lighting\")")
    float SunIntensity = 1.0f;

    /// <summary>
    /// Ambient sky lighting contribution scale.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), DefaultValue(0.5f), Limit(0), EditorDisplay(\"Lighting\")")
    float SkyIntensity = 0.5f;

    /// <summary>
    /// Distortion amount for wispy edges.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), DefaultValue(1.0f), Limit(0), EditorDisplay(\"Cloud\")")
    float DistortionScale = 1.0f;

    /// <summary>
    /// Alpha cutoff threshold used in cloud composition.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(50), DefaultValue(0.3f), Limit(0, 1, 0.001f), EditorDisplay(\"Cloud\")")
    float AlphaThreshold = 0.3f;

    /// <summary>
    /// Cloud opacity scale.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(55), DefaultValue(1.5f), Limit(0), EditorDisplay(\"Cloud\")")
    float Density = 1.5f;

    /// <summary>
    /// Soft intersection fade distance (in world units).
    /// Lower values keep clouds more opaque except very close to geometry.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(58), DefaultValue(35.0f), Limit(1), EditorDisplay(\"Cloud\")")
    float SoftIntersectionDistance = 35.0f;

    /// <summary>
    /// Optional feature mask override. Use -1 to read feature mask from vertex data.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(60), DefaultValue(-1.0f), Limit(-1, 1, 0.001f), EditorDisplay(\"Cloud\")")
    float FeatureMaskOverride = -1.0f;

    /// <summary>
    /// Enables distance-based threshold sharpening.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(70), DefaultValue(true), EditorDisplay(\"Distance Sharpening\")")
    bool DistanceSharpening = true;

    /// <summary>
    /// Distance at which sharpening starts.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(80), DefaultValue(50000.0f), Limit(0), EditorDisplay(\"Distance Sharpening\")")
    float SharpeningStart = 50000.0f;

    /// <summary>
    /// Distance at which sharpening reaches full effect.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(90), DefaultValue(100000.0f), Limit(0), EditorDisplay(\"Distance Sharpening\")")
    float SharpeningEnd = 100000.0f;

    /// <summary>
    /// Lightning tint color.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(100), DefaultValue(typeof(Color), \"1,1,1,1\"), EditorDisplay(\"Lightning\")")
    Color LightningColor = Color::White;

    /// <summary>
    /// Lightning intensity (0 disables lightning contribution).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(110), DefaultValue(0.0f), Limit(0), EditorDisplay(\"Lightning\")")
    float LightningIntensity = 0.0f;

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(50);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
#endif
    void Draw(RenderContext& renderContext) override;
    bool HasContentLoaded() const override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
};
