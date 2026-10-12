// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config/Settings.h"
#include "Engine/Content/SoftAssetReference.h"
#include "Engine/Graphics/Enums.h"
#include "Engine/Graphics/PostProcessSettings.h"

class CubeTexture;
class FontAsset;

/// <summary>
/// Noise locking mode for stylized cloud distortion.
/// </summary>
API_ENUM() enum class StylizedCloudDistortionMode
{
    /// <summary>
    /// Cubemap lookup from view direction (camera-relative). Scrolls when the camera moves, as in Sea of Thieves.
    /// </summary>
    ViewRelative = 0,

    /// <summary>
    /// Cubemap lookup direction from world origin. World-stable, but can have axis-dependent seam artifacts.
    /// </summary>
    WorldOrigin = 1,

    /// <summary>
    /// Cubemap lookup direction from each cloud mesh's object origin. Radial per cloud, no axis artifacts.
    /// </summary>
    CloudOrigin = 2,

    /// <summary>
    /// Reflected view direction off cloud surface normal. View-dependent look with much less translation scrolling.
    /// </summary>
    ReflectedView = 3,

    /// <summary>
    /// Procedural 3D value noise based on world position. No cubemap needed, fully world-stable, no seams.
    /// </summary>
    Procedural3D = 4,
};

/// <summary>
/// Depth sorting mode for stylized clouds against forward-pass objects (glass, particles, light cards).
/// </summary>
API_ENUM() enum class StylizedCloudDepthMode
{
    /// <summary>
    /// No depth writing. Forward-pass objects always render on top of clouds.
    /// </summary>
    None = 0,

    /// <summary>
    /// Hard alpha cutoff. Cloud regions above the alpha threshold write depth and fully occlude forward-pass objects behind them.
    /// </summary>
    HardCutoff = 1,

    /// <summary>
    /// Stochastic dither. Cloud alpha is compared against per-pixel noise so the occlusion boundary dissolves naturally.
    /// </summary>
    StochasticDither = 2,
};

/// <summary>
/// Graphics rendering settings.
/// </summary>
API_CLASS(Sealed, Namespace="FlaxEditor.Content.Settings", NoConstructor) class FLAXENGINE_API GraphicsSettings : public SettingsBase
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(GraphicsSettings);

public:
    /// <summary>
    /// List of pixel formats that can be used by the rendering pipeline (for light buffer and post-processing).
    /// </summary>
    API_ENUM(Attributes="EnumDisplay(EnumDisplayAttribute.FormatMode.None)")
    enum class RenderColorFormats
    {
        // HDR 32-bit buffer without alpha channel support. Offers good performance but might result in colors banding or shift towards yellowish colors due to low data precision.
        R11G11B10,
        // LDR 32-bit buffer with alpha channel support. Offers good performance but doesn't support High Dynamic Range rendering.
        R8G8B8A8,
        // HDR 64-bit buffer with alpha channel support. Offers very good quality for wide range of colors but requires more memory.
        R16G16B16A16,
    };

    /// <summary>
    /// The environment probes cubemap texture storage formats.
    /// </summary>
    API_ENUM(Attributes = "EnumDisplay(EnumDisplayAttribute.FormatMode.None)")
    enum class ProbeCubemapFormats
    {
        // LDR uncompressed format (32-bit per pixel).
        R8G8B8A8,
        // HDR uncompressed format (32-bit per pixel, no alpha).
        R11G11B10,
        // HDR compressed format (8-bit per pixel, no alpha). Converted into ASTC/Basis for mobile/web. Realtime probes will fallback to R11G11B10.
        BC6,
        // HDR compressed format (8-bit per pixel). Converted into ASTC/Basis for mobile/web. Realtime probes will fallback to R11G11B10.
        BC7,
    };

public:
    /// <summary>
    /// Enables rendering synchronization with the refresh rate of the display device to avoid "tearing" artifacts.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"General\", \"Use V-Sync\")")
    bool UseVSync = false;

public:
    /// <summary>
    /// Anti Aliasing quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1000), EditorDisplay(\"Quality\", \"AA Quality\")")
    Quality AAQuality = Quality::High;

    /// <summary>
    /// Screen Space Reflections quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1100), EditorDisplay(\"Quality\", \"SSR Quality\")")
    Quality SSRQuality = Quality::High;

    /// <summary>
    /// Screen Space Ambient Occlusion quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1200), EditorDisplay(\"Quality\", \"SSAO Quality\")")
    Quality SSAOQuality = Quality::High;

    /// <summary>
    /// Volumetric Fog quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1250), EditorDisplay(\"Quality\")")
    Quality VolumetricFogQuality = Quality::High;

    /// <summary>
    /// The shadows quality.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1300), EditorDisplay(\"Quality\")")
    Quality ShadowsQuality = Quality::High;

    /// <summary>
    /// The shadow maps quality (textures resolution).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1310), EditorDisplay(\"Quality\")")
    Quality ShadowMapsQuality = Quality::High;

    /// <summary>
    /// Enables cascades splits blending for directional light shadows.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1320), EditorDisplay(\"Quality\", \"Allow CSM Blending\")")
    bool AllowCSMBlending = false;

public:
    /// <summary>
    /// The type of the occlusion culling (implements IOcclusionCulling) that will be used to test visibility of the objects and skip rendering occluded ones. Can be left empty to use frustum-culling only.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1450), EditorDisplay(\"Culling\"), TypeReference(typeof(IOcclusionCulling)), CustomEditorAlias(\"FlaxEditor.CustomEditors.Editors.TypeNameEditor\")")
    StringAnsi OcclusionCulling;

    /// <summary>
    /// The number of buffered frames for the visibility query results readback from GPU (to avoid stalls). The higher value the more latency but less CPU stalls (to wait for GPU results). Higher values increase object popping artifacts.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1460), EditorDisplay(\"Culling\"), Limit(1, 4)")
    int32 OcclusionBufferedFrames = 2;

    /// <summary>
    /// The object bounds scale for occlusion culling to reduce popping artifacts caused by latency of visibility readback from GPU to CPU. Higher values inflate bounds which improves visual stability but lowers occlusion culling efficiency.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1461), EditorDisplay(\"Culling\"), Limit(1, 1.5f)")
    float OcclusionBoundsScale = 1.1f;

public:
    /// <summary>
    /// Default probes cubemap resolution (use for Environment Probes, can be overriden per-actor).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1500), EditorDisplay(\"Quality\")")
    ProbeCubemapResolution DefaultProbeResolution = ProbeCubemapResolution::_128;

    /// <summary>
    /// Environment Probes texture storage format. Controls the quality fo reflections and memory usage of probes data.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1502), EditorDisplay(\"Quality\")")
    ProbeCubemapFormats DefaultProbeCubemapFormat = ProbeCubemapFormats::BC6;

    /// <summary>
    /// If checked, Environment Probes will use HDR texture format. Improves quality in very bright scenes at cost of higher memory usage.
    /// [Deprecated in v1.13]
    /// </summary>
    DEPRECATED("Use DefaultProbeCubemapFormat instead.")
    bool UseHDRProbes = false;

    /// <summary>
    /// If checked, enables Global SDF rendering. This can be used in materials, shaders, and particles.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2000), EditorDisplay(\"Global SDF\")")
    bool EnableGlobalSDF = false;

    /// <summary>
    /// Draw distance of the Global SDF. Actual value can be larger when using DDGI.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2001), EditorDisplay(\"Global SDF\"), Limit(1000), ValueCategory(Utils.ValueCategory.Distance)")
    float GlobalSDFDistance = 15000.0f;

    /// <summary>
    /// The Global SDF quality. Controls the volume texture resolution and amount of cascades to use.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2005), EditorDisplay(\"Global SDF\")")
    Quality GlobalSDFQuality = Quality::High;

#if USE_EDITOR
    /// <summary>
    /// If checked, the 'Generate SDF' option will be checked on model import options by default. Use it if your project uses Global SDF (eg. for Global Illumination or particles).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2010), EditorDisplay(\"Global SDF\")")
    bool GenerateSDFOnModelImport = false;
#endif

    /// <summary>
    /// The Global Illumination quality. Controls the quality of the GI effect.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2100), EditorDisplay(\"Global Illumination\")")
    Quality GIQuality = Quality::High;

    /// <summary>
    /// The global spacing between Global Illumination probes (in world units). Smaller values improve interior detail at a higher GPU cost. Values around 100-150 are a useful starting point for mixed interiors and exteriors; adjust to 200-500 for mostly outdoor scenes and lower-frequency GI. Changing this value recreates the DDGI probe resources and can change the automatic cascade layout.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2120), Limit(50, 1000), EditorDisplay(\"Global Illumination\"), ValueCategory(Utils.ValueCategory.Distance)")
    float GIProbesSpacing = 100;

    /// <summary>
    /// Enables smooth blending between Global Illumination cascade splits. If disabled, the transition uses dithering intended for temporal anti-aliasing. Smooth blending can expose rounded cascade boundaries when adjacent cascades contain significantly different lighting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2125), EditorDisplay(\"Global Illumination\", \"GI Cascades Blending\")")
    bool GICascadesBlending = false;

    /// <summary>
    /// The Global Surface Atlas resolution. Adjust it if atlas `flickers` due to overflow (eg. to 4096).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2130), Limit(256, 8192), EditorDisplay(\"Global Illumination\")")
    int32 GlobalSurfaceAtlasResolution = 2048;

    // -- Appearance --

    /// <summary>
    /// Minimum density threshold below which cloud pixels are discarded.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3000), DefaultValue(0.3f), Limit(0, 1), EditorDisplay(\"Stylized Clouds\", \"Alpha Threshold\")")
    float StylizedCloudAlphaThreshold = 0.3f;

    /// <summary>
    /// Base gaussian blur sigma for stylized cloud softness.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3010), DefaultValue(2.5f), Limit(0, 100), EditorDisplay(\"Stylized Clouds\", \"Blur Sigma\")")
    float StylizedCloudBlurSigma = 2.5f;

    /// <summary>
    /// How much the blur decreases with depth for stylized clouds.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3020), DefaultValue(4.0f), Limit(0, 100), EditorDisplay(\"Stylized Clouds\", \"Blur Depth Scale\")")
    float StylizedCloudBlurDepthScale = 4.0f;

    // -- Depth & Intersection --

    /// <summary>
    /// How stylized clouds write depth for sorting against forward-pass objects (glass, particles, light cards).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3030), DefaultValue(StylizedCloudDepthMode.StochasticDither), EditorDisplay(\"Stylized Clouds\", \"Depth Mode\")")
    StylizedCloudDepthMode StylizedCloudDepthMode = StylizedCloudDepthMode::StochasticDither;

    /// <summary>
    /// Distance over which clouds fade near geometry intersections to avoid hard clipping.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3040), DefaultValue(35.0f), Limit(0, 500), EditorDisplay(\"Stylized Clouds\", \"Soft Intersection Distance\")")
    float StylizedCloudSoftIntersectionDistance = 35.0f;

    // -- Distance --

    /// <summary>
    /// Distance at which cloud alpha sharpening begins.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3050), DefaultValue(50000.0f), Limit(0), EditorDisplay(\"Stylized Clouds\", \"Distance Sharpen Start\")")
    float StylizedCloudDistanceSharpenStart = 50000.0f;

    /// <summary>
    /// Distance at which cloud alpha sharpening reaches full effect.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3060), DefaultValue(100000.0f), Limit(0), EditorDisplay(\"Stylized Clouds\", \"Distance Sharpen End\")")
    float StylizedCloudDistanceSharpenEnd = 100000.0f;

    // -- Distortion --

    /// <summary>
    /// How the distortion noise cubemap direction is computed for stylized clouds.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3070), DefaultValue(StylizedCloudDistortionMode.CloudOrigin), EditorDisplay(\"Stylized Clouds\", \"Distortion Mode\")")
    StylizedCloudDistortionMode StylizedCloudDistortionMode = StylizedCloudDistortionMode::CloudOrigin;

    /// <summary>
    /// Edge distortion noise strength for stylized clouds. Set to 0 to disable.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3075), DefaultValue(0.0f), Limit(0, 5), EditorDisplay(\"Stylized Clouds\", \"Distortion Strength\")")
    float StylizedCloudDistortionStrength = 0.0f;

    /// <summary>
    /// Speed of the distortion noise drift for stylized clouds. Higher values make the noise scroll faster.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3080), DefaultValue(0.01f), Limit(0, 1), EditorDisplay(\"Stylized Clouds\", \"Distortion Scroll Speed\")")
    float StylizedCloudDistortionScrollSpeed = 0.01f;

    /// <summary>
    /// Spatial scale of the 3D procedural noise. Smaller values produce larger noise features. Only used with Procedural3D distortion mode.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3085), DefaultValue(0.02f), Limit(0.001f, 1.0f), EditorDisplay(\"Stylized Clouds\", \"Noise Scale\"), VisibleIf(nameof(ShowStylizedCloudNoiseScale))")
    float StylizedCloudNoiseScale = 0.02f;

    /// <summary>
    /// Cubemap texture used for edge distortion noise on stylized clouds (RG channels, 0-1 remapped to -1..1). Requires non-zero Distortion Strength.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3090), EditorDisplay(\"Stylized Clouds\", \"Distortion Cube Map\"), VisibleIf(nameof(ShowStylizedCloudCubeMap))")
    SoftAssetReference<CubeTexture> StylizedCloudDistortionCubeMap;

public:
    /// <summary>
    /// If checked, color space workflow will use Gamma instead of Linear. Gamma color space defines colors with an applied a gamma curve (sRGB) so they are perceptually linear.
    /// This makes sense when the output of the rendering represent final color values that will be presented to a non-HDR screen.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3000), EditorDisplay(\"Colors\")")
    bool GammaColorSpace = true;

    /// <summary>
    /// Pixel format used by the rendering pipeline (for light buffer and post-processing).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3010), EditorDisplay(\"Colors\")")
    RenderColorFormats RenderColorFormat = RenderColorFormats::R11G11B10;

public:
    /// <summary>
    /// The default Post Process settings. Can be overridden by PostFxVolume on a level locally, per camera or for a whole map.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10000), EditorDisplay(\"Post Process Settings\", EditorDisplayAttribute.InlineStyle)")
    PostProcessSettings PostProcessSettings;

public:
    /// <summary>
    /// The list of fallback fonts used for text rendering. Ignored if empty.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(5000), EditorDisplay(\"Text\")")
    Array<AssetReference<FontAsset>> FallbackFonts;

private:
    /// <summary>
    /// Renamed UeeHDRProbes into UseHDRProbes
    /// [Deprecated on 12.10.2022, expires on 12.10.2024]
    /// </summary>
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use DefaultProbeCubemapFormat instead.") bool GetUeeHDRProbes() const;
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use DefaultProbeCubemapFormat instead.") void SetUeeHDRProbes(bool value);
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use DefaultProbeCubemapFormat instead.") bool GetUseHDRProbes() const;
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use DefaultProbeCubemapFormat instead.") void SetUseHDRProbes(bool value);
    API_FUNCTION(Attributes="OnDeserializing", Hidden) void OnDeserializing(const CallbackContext& context);

public:
    /// <summary>
    /// Gets the instance of the settings asset (default value if missing). Object returned by this method is always loaded with valid data to use.
    /// </summary>
    static GraphicsSettings* Get();

    // [SettingsBase]
    void Apply() override;
};
