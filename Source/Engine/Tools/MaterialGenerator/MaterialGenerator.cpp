// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_MATERIAL_GRAPH

#include "MaterialGenerator.h"
#include "Engine/Visject/ShaderGraphUtilities.h"
#include "Engine/Platform/File.h"
#include "Engine/Graphics/Materials/MaterialShader.h"
#include "Engine/Graphics/Materials/MaterialShaderFeatures.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Threading/Threading.h"

/// <summary>
/// Material shader source code template has special marks for generated code.
/// Each starts with '@' char and index of the mapped string.
/// </summary>
enum MaterialTemplateInputsMapping
{
    In_VersionNumber = 0,
    In_Constants = 1,
    In_ShaderResources = 2,
    In_Defines = 3,
    In_GetMaterialPS = 4,
    In_GetMaterialVS = 5,
    In_GetMaterialDS = 6,
    In_Includes = 7,
    In_Utilities = 8,
    In_Shaders = 9,

    In_MAX
};

/// <summary>
/// Material shader feature source code template has special marks for generated code. Each starts with '@' char and index of the mapped string.
/// </summary>
enum class FeatureTemplateInputsMapping
{
    Defines = 0,
    Includes = 1,
    Constants = 2,
    Resources = 3,
    Utilities = 4,
    Shaders = 5,
    MAX
};

struct FeatureData
{
    MaterialShaderFeature::GeneratorData Data;
    String Inputs[(int32)FeatureTemplateInputsMapping::MAX];

    bool Init();
};

namespace
{
    // Loaded and parsed features data cache
    Dictionary<StringAnsi, FeatureData> Features;
    CriticalSection FeaturesLock;
}

bool FeatureData::Init()
{
    // Load template file
    const String path = Globals::EngineContentFolder / TEXT("Editor/MaterialTemplates/") + Data.Template;
    String contents;
    if (File::ReadAllText(path, contents))
    {
        LOG(Error, "Cannot open file {0}", path);
        return true;
    }

    int32 i = 0;
    const int32 length = contents.Length();

    // Skip until input start
    for (; i < length; i++)
    {
        if (contents[i] == '@')
            break;
    }

    // Load all inputs
    do
    {
        // Parse input type
        i++;
        const int32 inIndex = contents[i++] - '0';
        ASSERT_LOW_LAYER(Math::IsInRange(inIndex, 0, (int32)FeatureTemplateInputsMapping::MAX - 1));

        // Read until next input start
        const Char* start = &contents[i];
        for (; i < length; i++)
        {
            const auto c = contents[i];
            if (c == '@')
                break;
        }
        const Char* end = contents.Get() + i;

        // Set input
        Inputs[inIndex].Set(start, (int32)(end - start));
    } while (i < length);

    return false;
}

MaterialValue MaterialGenerator::getUVs(VariantType::Float2, TEXT("input.TexCoord"));
MaterialValue MaterialGenerator::getTime(VariantType::Float, TEXT("TimeParam"));
MaterialValue MaterialGenerator::getNormal(VariantType::Float3, TEXT("input.TBN[2]"));
MaterialValue MaterialGenerator::getNormalZero(VariantType::Float3, TEXT("float3(0, 0, 1)"));
MaterialValue MaterialGenerator::getVertexColor(VariantType::Float4, TEXT("GetVertexColor(input)"));

MaterialGenerator::MaterialGenerator()
{
    // Register per group type processing events
    // Note: index must match group id
    _perGroupProcessCall[1].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupMaterial>(this);
    _perGroupProcessCall[3].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupMath>(this);
    _perGroupProcessCall[5].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupTextures>(this);
    _perGroupProcessCall[6].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupParameters>(this);
    _perGroupProcessCall[7].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupTools>(this);
    _perGroupProcessCall[8].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupLayers>(this);
    _perGroupProcessCall[14].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupParticles>(this);
    _perGroupProcessCall[16].Bind<MaterialGenerator, &MaterialGenerator::ProcessGroupFunction>(this);
}

MaterialGenerator::~MaterialGenerator()
{
    _layers.ClearDelete();
}

bool MaterialGenerator::Generate(WriteStream& source, MaterialInfo& materialInfo, BytesContainer& parametersData)
{
    ASSERT_LOW_LAYER(_layers.Count() > 0);

    String inputs[In_MAX];
    Array<StringAnsiView, FixedAllocation<8>> features;

    // Setup and prepare layers
    _writer.Clear();
    _includes.Clear();
    _callStack.Clear();
    _parameters.Clear();
    _localIndex = 0;
    _vsToPsInterpolants.Clear();
    _treeLayer = nullptr;
    _graphStack.Clear();
    _needsHexTileFunctions = false;
    for (int32 i = 0; i < _layers.Count(); i++)
    {
        auto layer = _layers[i];

        // Prepare
        layer->Prepare();
        prepareLayer(_layers[i], true);

        // Assign layer variable name for initial layers
        layer->Usage[0].VarName = TEXT("material");
        if (i != 0)
            layer->Usage[0].VarName += StringUtils::ToString(i);
    }
    inputs[In_VersionNumber] = StringUtils::ToString(MATERIAL_GRAPH_VERSION);

    // Cache data
    MaterialLayer* baseLayer = GetRootLayer();
    auto* baseNode = baseLayer->Root;
    _treeLayerVarName = baseLayer->GetVariableName(nullptr);
    _treeLayer = baseLayer;
    _graphStack.Add(&_treeLayer->Graph);
    const MaterialGraphBox* layerInputBox = baseLayer->Root->GetBox(0);
    const bool isLayered = layerInputBox->HasConnection();

    // Initialize features
#define ADD_FEATURE(type) \
    { \
        StringAnsiView typeName(#type, ARRAY_COUNT(#type) - 1); \
        features.Add(typeName); \
        if (!Features.ContainsKey(typeName)) \
        { \
            ScopeLock lock(FeaturesLock); \
            auto& feature = Features[typeName]; \
            type::Generate(feature.Data); \
            if (feature.Init()) \
                return true; \
        } \
    }
    switch (baseLayer->Domain)
    {
    case MaterialDomain::Surface:
        if (materialInfo.TessellationMode != TessellationMethod::None)
            ADD_FEATURE(TessellationFeature);
        if (materialInfo.BlendMode == MaterialBlendMode::Opaque)
            ADD_FEATURE(MotionVectorsFeature);
        if (materialInfo.BlendMode == MaterialBlendMode::Opaque)
            ADD_FEATURE(LightmapFeature);
        if (materialInfo.BlendMode == MaterialBlendMode::Opaque)
            ADD_FEATURE(DeferredShadingFeature);
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque && (materialInfo.FeaturesFlags & MaterialFeaturesFlags::DisableDistortion) == MaterialFeaturesFlags::None)
            ADD_FEATURE(DistortionFeature);
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque && EnumHasAnyFlags(materialInfo.FeaturesFlags, MaterialFeaturesFlags::GlobalIllumination))
        {
            ADD_FEATURE(GlobalIlluminationFeature);

            // SDF Reflections is only valid when both GI and SSR is enabled
            if (materialInfo.BlendMode != MaterialBlendMode::Opaque && EnumHasAnyFlags(materialInfo.FeaturesFlags, MaterialFeaturesFlags::ScreenSpaceReflections))
                ADD_FEATURE(SDFReflectionsFeature);
        }
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque)
            ADD_FEATURE(ForwardShadingFeature);
        break;
    case MaterialDomain::Terrain:
        if (materialInfo.TessellationMode != TessellationMethod::None)
            ADD_FEATURE(TessellationFeature);
        ADD_FEATURE(LightmapFeature);
        ADD_FEATURE(DeferredShadingFeature);
        break;
    case MaterialDomain::Particle:
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque && (materialInfo.FeaturesFlags & MaterialFeaturesFlags::DisableDistortion) == MaterialFeaturesFlags::None)
            ADD_FEATURE(DistortionFeature);
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque && EnumHasAnyFlags(materialInfo.FeaturesFlags, MaterialFeaturesFlags::GlobalIllumination))
            ADD_FEATURE(GlobalIlluminationFeature);
        ADD_FEATURE(ForwardShadingFeature);
        break;
    case MaterialDomain::Deformable:
        if (materialInfo.TessellationMode != TessellationMethod::None)
            ADD_FEATURE(TessellationFeature);
        if (materialInfo.BlendMode == MaterialBlendMode::Opaque)
            ADD_FEATURE(DeferredShadingFeature);
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque)
            ADD_FEATURE(ForwardShadingFeature);
        break;
    default:
        break;
    }
#undef ADD_FEATURE

    // Check if material is using special features and update the metadata flags
    if (!isLayered)
    {
        baseLayer->UpdateFeaturesFlags();
    }

    // Pixel Shader
    _treeType = MaterialTreeType::PixelShader;
    Value materialVarPS;
    if (isLayered)
    {
        materialVarPS = eatBox(baseNode, layerInputBox->FirstConnection());
    }
    else
    {
        materialVarPS = Value(VariantType::Void, baseLayer->GetVariableName(nullptr));
        _writer.Write(TEXT("\tMaterial {0} = (Material)0;\n"), materialVarPS.Value);
        if (baseLayer->Domain == MaterialDomain::Surface || baseLayer->Domain == MaterialDomain::Terrain || baseLayer->Domain == MaterialDomain::Particle || baseLayer->Domain == MaterialDomain::Deformable)
        {
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Emissive);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Normal);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Color);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Metalness);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Specular);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::AmbientOcclusion);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Roughness);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Opacity);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Refraction);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::SubsurfaceColor);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Mask);
        }
        else if (baseLayer->Domain == MaterialDomain::Decal)
        {
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Emissive);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Normal);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Color);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Metalness);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Specular);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Roughness);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Opacity);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Mask);

            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::AmbientOcclusion);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Refraction);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::SubsurfaceColor);
        }
        else if (baseLayer->Domain == MaterialDomain::PostProcess)
        {
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Emissive);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Opacity);

            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Normal);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Color);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Metalness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Specular);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::AmbientOcclusion);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Roughness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Refraction);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Mask);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::SubsurfaceColor);
        }
        else if (baseLayer->Domain == MaterialDomain::GUI)
        {
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Emissive);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Opacity);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Mask);

            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Normal);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Color);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Metalness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Specular);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::AmbientOcclusion);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Roughness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Refraction);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::SubsurfaceColor);
        }
        else if (baseLayer->Domain == MaterialDomain::VolumeParticle)
        {
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Emissive);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Opacity);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Mask);
            eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::Color);

            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Normal);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Metalness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Specular);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::AmbientOcclusion);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Roughness);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::Refraction);
            eatMaterialGraphBoxWithDefault(baseLayer, MaterialGraphBoxes::SubsurfaceColor);
        }
        else
        {
            CRASH;
        }
    }
    {
        // Flip normal for inverted triangles (used by two sided materials)
        _writer.Write(TEXT("\t{0}.TangentNormal *= input.TwoSidedSign;\n"), materialVarPS.Value);

        // Normalize and transform to world space if need to
        _writer.Write(TEXT("\t{0}.TangentNormal = normalize({0}.TangentNormal);\n"), materialVarPS.Value);
        if (EnumHasAllFlags(baseLayer->FeaturesFlags, MaterialFeaturesFlags::InputWorldSpaceNormal))
        {
            _writer.Write(TEXT("\t{0}.WorldNormal = {0}.TangentNormal;\n"), materialVarPS.Value);
            _writer.Write(TEXT("\t{0}.TangentNormal = normalize(TransformWorldVectorToTangent(input, {0}.WorldNormal));\n"), materialVarPS.Value);
        }
        else
        {
            _writer.Write(TEXT("\t{0}.WorldNormal = normalize(TransformTangentVectorToWorld(input, {0}.TangentNormal));\n"), materialVarPS.Value);
        }

        // Clamp values
        _writer.Write(TEXT("\t{0}.Metalness = saturate({0}.Metalness);\n"), materialVarPS.Value);
        _writer.Write(TEXT("\t{0}.Roughness = max(0.04, {0}.Roughness);\n"), materialVarPS.Value);
        _writer.Write(TEXT("\t{0}.AO = saturate({0}.AO);\n"), materialVarPS.Value);
        _writer.Write(TEXT("\t{0}.Opacity = saturate({0}.Opacity);\n"), materialVarPS.Value);

        // Return result
        _writer.Write(TEXT("\treturn {0};"), materialVarPS.Value);
    }
    inputs[In_GetMaterialPS] = _writer.ToString();
    _writer.Clear();
    clearCache();

    // Domain Shader
    _treeType = MaterialTreeType::DomainShader;
    if (isLayered)
    {
        const Value layer = eatBox(baseNode, layerInputBox->FirstConnection());
        _writer.Write(TEXT("\treturn {0};"), layer.Value);
    }
    else
    {
        _writer.Write(TEXT("\tMaterial material = (Material)0;\n"));
        eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::WorldDisplacement);
        _writer.Write(TEXT("\treturn material;"));
    }
    inputs[In_GetMaterialDS] = _writer.ToString();
    _writer.Clear();
    clearCache();

    // Vertex Shader
    _treeType = MaterialTreeType::VertexShader;
    if (isLayered)
    {
        const Value layer = eatBox(baseNode, layerInputBox->FirstConnection());
        _writer.Write(TEXT("\treturn {0};"), layer.Value);
    }
    else
    {
        _writer.Write(TEXT("\tMaterial material = (Material)0;\n"));
        eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::PositionOffset);
        eatMaterialGraphBox(baseLayer, MaterialGraphBoxes::TessellationMultiplier);
        for (int32 i = 0; i < _vsToPsInterpolants.Count(); i++)
        {
            const auto value = tryGetValue(_vsToPsInterpolants[i], Value::Zero).AsFloat4().Value;
            _writer.Write(TEXT("\tmaterial.CustomVSToPS[{0}] = {1};\n"), i, value);
        }
        _writer.Write(TEXT("\treturn material;"));
    }
    inputs[In_GetMaterialVS] = _writer.ToString();
    _writer.Clear();
    clearCache();

    // Update material usage based on material generator outputs
    materialInfo.UsageFlags = baseLayer->UsageFlags;

    // Find all Custom Global Code nodes
    Array<const MaterialGraph::Node*, InlinedAllocation<8>> customGlobalCodeNodes;
    Array<Graph*, InlinedAllocation<8>> graphs;
    _functions.GetValues(graphs);
    for (MaterialLayer* layer : _layers)
        graphs.Add(&layer->Graph);
    for (Graph* graph : graphs)
    {
        for (MaterialGraph::Node& node : graph->Nodes)
        {
            if (node.Type == GRAPH_NODE_MAKE_TYPE(1, 38) && (bool)node.Values[1])
            {
                if (node.Values.Count() == 2)
                    node.Values.Add(In_Utilities); // Upgrade old node data
                customGlobalCodeNodes.Add(&node);
            }
        }
    }

#define WRITE_FEATURES(input) FeaturesLock.Lock(); for (auto f : features) _writer.Write(Features[f].Inputs[(int32)FeatureTemplateInputsMapping::input]); FeaturesLock.Unlock();
    // Defines
    {
        _writer.Write(TEXT("#define MATERIAL_MASK_THRESHOLD ({0})\n"), baseLayer->MaskThreshold);
        _writer.Write(TEXT("#define CUSTOM_VERTEX_INTERPOLATORS_COUNT ({0})\n"), _vsToPsInterpolants.Count());
        _writer.Write(TEXT("#define MATERIAL_OPACITY_THRESHOLD ({0})\n"), baseLayer->OpacityThreshold);
        if (materialInfo.BlendMode != MaterialBlendMode::Opaque && !(materialInfo.FeaturesFlags & MaterialFeaturesFlags::DisableReflections) && EnumHasAnyFlags(materialInfo.FeaturesFlags, MaterialFeaturesFlags::ScreenSpaceReflections))
        {
            // Inject depth and color buffers for Screen Space Reflections used by transparent material
            auto sceneDepthTexture = findOrAddSceneTexture(MaterialSceneTextures::SceneDepth);
            auto sceneColorTexture = findOrAddSceneTexture(MaterialSceneTextures::SceneColor);
            _writer.Write(TEXT("#define MATERIAL_REFLECTIONS_SSR_DEPTH ({0})\n"), sceneDepthTexture.ShaderName);
            _writer.Write(TEXT("#define MATERIAL_REFLECTIONS_SSR_COLOR ({0})\n"), sceneColorTexture.ShaderName);
        }
        WRITE_FEATURES(Defines);
        inputs[In_Defines] = _writer.ToString();
        WriteCustomGlobalCode(customGlobalCodeNodes, In_Defines);
        _writer.Clear();
    }

    // Includes
    {
        for (auto& include : _includes)
            _writer.Write(TEXT("#include \"{0}\"\n"), include.Item);
        WRITE_FEATURES(Includes);
        WriteCustomGlobalCode(customGlobalCodeNodes, In_Includes);
        inputs[In_Includes] = _writer.ToString();
        _writer.Clear();
    }

    // Constants
    {
        WRITE_FEATURES(Constants);
        if (_parameters.HasItems())
            ShaderGraphUtilities::GenerateShaderConstantBuffer(_writer, _parameters);
        WriteCustomGlobalCode(customGlobalCodeNodes, In_Constants);
        inputs[In_Constants] = _writer.ToString();
        _writer.Clear();
    }

    // Resources
    {
        int32 srv = 0, sampler = GPU_STATIC_SAMPLERS_COUNT;
        switch (baseLayer->Domain)
        {
        case MaterialDomain::Surface:
            srv = 3; // Objects + Skinning Bones + Prev Bones
            break;
        case MaterialDomain::Decal:
            srv = 1; // Depth buffer
            break;
        case MaterialDomain::Terrain:
            srv = 3; // Heightmap + 2 splatmaps
            break;
        case MaterialDomain::Particle:
            srv = 2; // Particles data + Sorted indices/Ribbon segments
            break;
        case MaterialDomain::Deformable:
            srv = 1; // Mesh deformation buffer
            break;
        case MaterialDomain::VolumeParticle:
            srv = 1; // Particles data
            break;
        }
        for (auto f : features)
        {
            // Process SRV slots used in template
            const auto& text = Features[f].Inputs[(int32)FeatureTemplateInputsMapping::Resources];
            const Char* str = text.Get();
            int32 prevIdx = 0, idx = 0;
            while (true)
            {
                idx = text.Find(TEXT("__SRV__"), StringSearchCase::CaseSensitive, prevIdx);
                if (idx == -1)
                    break;
                int32 len = idx - prevIdx;
                _writer.Write(StringView(str, len));
                str += len;
                _writer.Write(StringUtils::ToString(srv));
                srv++;
                str += ARRAY_COUNT("__SRV__") - 1;
                prevIdx = idx + ARRAY_COUNT("__SRV__") - 1;
            }
            _writer.Write(StringView(str));
        }
        if (_parameters.HasItems())
        {
            auto error = ShaderGraphUtilities::GenerateShaderResources(_writer, _parameters, srv);
            if (!error)
                error = ShaderGraphUtilities::GenerateSamplers(_writer, _parameters, sampler);
            if (error)
            {
                OnError(nullptr, nullptr, error);
                return true;
            }
        }
        WriteCustomGlobalCode(customGlobalCodeNodes, In_ShaderResources);
        inputs[In_ShaderResources] = _writer.ToString();
        _writer.Clear();
    }

    // Utilities
    {
        WRITE_FEATURES(Utilities);
        WriteCustomGlobalCode(customGlobalCodeNodes, In_Utilities);
        
        // Add hex tile functions if needed
        if (_needsHexTileFunctions)
        {
            // Basic definitions and utility functions
            _writer.Write(TEXT(R"(
#ifndef M_PI
#define M_PI 3.14159265359
#endif

static float g_fallOffContrast = 0.6;
static float g_exp = 7.0;

// Output: weights associated with each hex tile and integer centers
void TriangleGrid(out float w1, out float w2, out float w3, 
                  out int2 vertex1, out int2 vertex2, out int2 vertex3,
                  float2 st)
{
    // Scaling of the input
    st *= 2.0 * sqrt(3.0);

    // Skew input space into simplex triangle grid
    const float2x2 gridToSkewedGrid = 
        float2x2(1.0, -0.57735027, 0.0, 1.15470054);
    float2 skewedCoord = mul(gridToSkewedGrid, st);

    int2 baseId = int2(floor(skewedCoord));
    float3 temp = float3(frac(skewedCoord), 0.0);
    temp.z = 1.0 - temp.x - temp.y;

    float s = step(0.0, -temp.z);
    float s2 = 2.0 * s - 1.0;

    w1 = -temp.z * s2;
    w2 = s - temp.y * s2;
    w3 = s - temp.x * s2;

    vertex1 = baseId + int2(s, s);
    vertex2 = baseId + int2(s, 1 - s);
    vertex3 = baseId + int2(1 - s, s);
}
)"));

            // RWS variant and helper functions
            _writer.Write(TEXT(R"(
// RWS variant for large worlds
void TriangleGridRWS(out float w1, out float w2, out float w3, 
                     out int2 vertex1, out int2 vertex2, out int2 vertex3,
                     float2 st, float2 st_offs)
{
    // Scaling of the input
    st *= 2.0 * sqrt(3.0);
    st_offs *= 2.0 * sqrt(3.0);

    // Skew input space into simplex triangle grid
    const float2x2 gridToSkewedGrid = 
        float2x2(1.0, -0.57735027, 0.0, 1.15470054);
    float2 skewedCoord = mul(gridToSkewedGrid, st);
    float2 skewedCoord_offs = mul(gridToSkewedGrid, st_offs);

    // separate out large 2D integer offset
    int2 baseId_offs = int2(floor(skewedCoord_offs));
    float2 comb_skew = skewedCoord + frac(skewedCoord_offs);
    int2 baseId = int2(floor(comb_skew)) + baseId_offs;
    float3 temp = float3(frac(comb_skew), 0.0);
    temp.z = 1.0 - temp.x - temp.y;

    float s = step(0.0, -temp.z);
    float s2 = 2.0 * s - 1.0;

    w1 = -temp.z * s2;
    w2 = s - temp.y * s2;
    w3 = s - temp.x * s2;

    vertex1 = baseId + int2(s, s);
    vertex2 = baseId + int2(s, 1 - s);
    vertex3 = baseId + int2(1 - s, s);
}

float2 hash(float2 p)
{
    float2 r = mul(float2x2(127.1, 311.7, 269.5, 183.3), p);
    return frac(sin(r) * 43758.5453);
}
)"));

            // More helper functions
            _writer.Write(TEXT(R"(
float2x2 LoadRot2x2(int2 idx, float rotStrength)
{
    float angle = abs(idx.x * idx.y) + abs(idx.x + idx.y) + M_PI;

    // remap to +/-pi
    angle = fmod(angle, 2.0 * M_PI); 
    if (angle < 0.0) angle += 2.0 * M_PI;
    if (angle > M_PI) angle -= 2.0 * M_PI;

    angle *= rotStrength;

    float cs = cos(angle), si = sin(angle);
    return float2x2(cs, -si, si, cs);
}

float2 MakeCenST(int2 Vertex)
{
    float2x2 invSkewMat = float2x2(1.0, 0.5, 0.0, 1.0/1.15470054);
    return mul(invSkewMat, Vertex) / (2.0 * sqrt(3.0));
}

float3 Gain3(float3 x, float r)
{
    // increase contrast when r>0.5 and reduce contrast if less
    float k = log(1.0 - r) / log(0.5);

    float3 s = 2.0 * step(0.5, x);
    float3 m = 2.0 * (1.0 - s);

    float3 res = 0.5 * s + 0.25 * m * pow(max(0.0, s + x * m), k);
    
    return res.xyz / (res.x + res.y + res.z);
}
)"));

            // Weight production function
            _writer.Write(TEXT(R"(
float3 ProduceHexWeights(float3 W, int2 vertex1, int2 vertex2, int2 vertex3)
{
    float3 res = 0.0;

    int v1 = (vertex1.x - vertex1.y) % 3;
    if (v1 < 0) v1 += 3;

    int vh = v1 < 2 ? (v1 + 1) : 0;
    int vl = v1 > 0 ? (v1 - 1) : 2;
    int v2 = vertex1.x < vertex3.x ? vl : vh;
    int v3 = vertex1.x < vertex3.x ? vh : vl;

    res.x = v3 == 0 ? W.z : (v2 == 0 ? W.y : W.x);
    res.y = v3 == 1 ? W.z : (v2 == 1 ? W.y : W.x);
    res.z = v3 == 2 ? W.z : (v2 == 2 ? W.y : W.x);

    return res;
}
)"));

            // Main color sampling functions
            _writer.Write(TEXT(R"(
// Hex tile color sampling function
float4 hex2colTex(Texture2D tex, SamplerState samp, float2 st,
                  float rotStrength, float r)
{
    float2 dSTdx = ddx(st), dSTdy = ddy(st);

    // Get triangle info
    float w1, w2, w3;
    int2 vertex1, vertex2, vertex3;
    TriangleGrid(w1, w2, w3, vertex1, vertex2, vertex3, st);

    float2x2 rot1 = LoadRot2x2(vertex1, rotStrength);
    float2x2 rot2 = LoadRot2x2(vertex2, rotStrength);
    float2x2 rot3 = LoadRot2x2(vertex3, rotStrength);

    float2 cen1 = MakeCenST(vertex1);
    float2 cen2 = MakeCenST(vertex2);
    float2 cen3 = MakeCenST(vertex3);

    float2 st1 = mul(st - cen1, rot1) + cen1 + hash(vertex1);
    float2 st2 = mul(st - cen2, rot2) + cen2 + hash(vertex2);
    float2 st3 = mul(st - cen3, rot3) + cen3 + hash(vertex3);

    // Fetch input
    float4 c1 = tex.SampleGrad(samp, st1, mul(dSTdx, rot1), mul(dSTdy, rot1));
    float4 c2 = tex.SampleGrad(samp, st2, mul(dSTdx, rot2), mul(dSTdy, rot2));
    float4 c3 = tex.SampleGrad(samp, st3, mul(dSTdx, rot3), mul(dSTdy, rot3));

    // use luminance as weight
    float3 Lw = float3(0.299, 0.587, 0.114);
    float3 Dw = float3(dot(c1.xyz, Lw), dot(c2.xyz, Lw), dot(c3.xyz, Lw));
    
    Dw = lerp(1.0, Dw, g_fallOffContrast);
    float3 W = Dw * pow(float3(w1, w2, w3), g_exp);
    W /= (W.x + W.y + W.z);
    if (r != 0.5) W = Gain3(W, r);

    return W.x * c1 + W.y * c2 + W.z * c3;
}
)"));

            // RWS color sampling function
            _writer.Write(TEXT(R"(
// RWS hex tile color sampling function
float4 hex2colTexRWS(Texture2D tex, SamplerState samp, float2 st,
                     float rotStrength, float r)
{
    float2 dSTdx = ddx(st), dSTdy = ddy(st);
    float2 st_offs = frac(st);

    // Get triangle info
    float w1, w2, w3;
    int2 vertex1, vertex2, vertex3;
    TriangleGridRWS(w1, w2, w3, vertex1, vertex2, vertex3, st, st_offs);

    float2x2 rot1 = LoadRot2x2(vertex1, rotStrength);
    float2x2 rot2 = LoadRot2x2(vertex2, rotStrength);
    float2x2 rot3 = LoadRot2x2(vertex3, rotStrength);

    float2 cen1 = MakeCenST(vertex1);
    float2 cen2 = MakeCenST(vertex2);
    float2 cen3 = MakeCenST(vertex3);

    float2 st1 = mul(st, rot1) + frac(mul(st_offs - cen1, rot1) + cen1) + hash(vertex1);
    float2 st2 = mul(st, rot2) + frac(mul(st_offs - cen2, rot2) + cen2) + hash(vertex2);
    float2 st3 = mul(st, rot3) + frac(mul(st_offs - cen3, rot3) + cen3) + hash(vertex3);

    // Fetch input
    float4 c1 = tex.SampleGrad(samp, st1, mul(dSTdx, rot1), mul(dSTdy, rot1));
    float4 c2 = tex.SampleGrad(samp, st2, mul(dSTdx, rot2), mul(dSTdy, rot2));
    float4 c3 = tex.SampleGrad(samp, st3, mul(dSTdx, rot3), mul(dSTdy, rot3));

    // use luminance as weight
    float3 Lw = float3(0.299, 0.587, 0.114);
    float3 Dw = float3(dot(c1.xyz, Lw), dot(c2.xyz, Lw), dot(c3.xyz, Lw));
    
    Dw = lerp(1.0, Dw, g_fallOffContrast);
    float3 W = Dw * pow(float3(w1, w2, w3), g_exp);
    W /= (W.x + W.y + W.z);
    if (r != 0.5) W = Gain3(W, r);

    return W.x * c1 + W.y * c2 + W.z * c3;
}
)"));

            // Normal map sampling functions
            _writer.Write(TEXT(R"(
// Hex tile normal map sampling function
float3 hex2normalTex(Texture2D tex, SamplerState samp, float2 st,
                     float rotStrength, float r)
{
    float2 dSTdx = ddx(st), dSTdy = ddy(st);

    // Get triangle info
    float w1, w2, w3;
    int2 vertex1, vertex2, vertex3;
    TriangleGrid(w1, w2, w3, vertex1, vertex2, vertex3, st);

    float2x2 rot1 = LoadRot2x2(vertex1, rotStrength);
    float2x2 rot2 = LoadRot2x2(vertex2, rotStrength);
    float2x2 rot3 = LoadRot2x2(vertex3, rotStrength);

    float2 cen1 = MakeCenST(vertex1);
    float2 cen2 = MakeCenST(vertex2);
    float2 cen3 = MakeCenST(vertex3);

    float2 st1 = mul(st - cen1, rot1) + cen1 + hash(vertex1);
    float2 st2 = mul(st - cen2, rot2) + cen2 + hash(vertex2);
    float2 st3 = mul(st - cen3, rot3) + cen3 + hash(vertex3);

    // Fetch and unpack normal maps
    float3 n1 = UnpackNormalMap(tex.SampleGrad(samp, st1, mul(dSTdx, rot1), mul(dSTdy, rot1)).rg);
    float3 n2 = UnpackNormalMap(tex.SampleGrad(samp, st2, mul(dSTdx, rot2), mul(dSTdy, rot2)).rg);
    float3 n3 = UnpackNormalMap(tex.SampleGrad(samp, st3, mul(dSTdx, rot3), mul(dSTdy, rot3)).rg);

    // Apply rotation to normal vectors
    n1 = float3(mul(rot1, n1.xy), n1.z);
    n2 = float3(mul(rot2, n2.xy), n2.z);
    n3 = float3(mul(rot3, n3.xy), n3.z);

    // Use normal magnitude as weight (more variation = higher weight)
    float3 Dw = float3(length(n1.xy), length(n2.xy), length(n3.xy));
    
    Dw = lerp(1.0, Dw, g_fallOffContrast);
    float3 W = Dw * pow(float3(w1, w2, w3), g_exp);
    W /= (W.x + W.y + W.z);
    if (r != 0.5) W = Gain3(W, r);

    return normalize(W.x * n1 + W.y * n2 + W.z * n3);
}
)"));

            // RWS normal map sampling function
            _writer.Write(TEXT(R"(
// RWS hex tile normal map sampling function
float3 hex2normalTexRWS(Texture2D tex, SamplerState samp, float2 st,
                        float rotStrength, float r)
{
    float2 dSTdx = ddx(st), dSTdy = ddy(st);
    float2 st_offs = frac(st);

    // Get triangle info
    float w1, w2, w3;
    int2 vertex1, vertex2, vertex3;
    TriangleGridRWS(w1, w2, w3, vertex1, vertex2, vertex3, st, st_offs);

    float2x2 rot1 = LoadRot2x2(vertex1, rotStrength);
    float2x2 rot2 = LoadRot2x2(vertex2, rotStrength);
    float2x2 rot3 = LoadRot2x2(vertex3, rotStrength);

    float2 cen1 = MakeCenST(vertex1);
    float2 cen2 = MakeCenST(vertex2);
    float2 cen3 = MakeCenST(vertex3);

    float2 st1 = mul(st, rot1) + frac(mul(st_offs - cen1, rot1) + cen1) + hash(vertex1);
    float2 st2 = mul(st, rot2) + frac(mul(st_offs - cen2, rot2) + cen2) + hash(vertex2);
    float2 st3 = mul(st, rot3) + frac(mul(st_offs - cen3, rot3) + cen3) + hash(vertex3);

    // Fetch and unpack normal maps
    float3 n1 = UnpackNormalMap(tex.SampleGrad(samp, st1, mul(dSTdx, rot1), mul(dSTdy, rot1)).rg);
    float3 n2 = UnpackNormalMap(tex.SampleGrad(samp, st2, mul(dSTdx, rot2), mul(dSTdy, rot2)).rg);
    float3 n3 = UnpackNormalMap(tex.SampleGrad(samp, st3, mul(dSTdx, rot3), mul(dSTdy, rot3)).rg);

    // Apply rotation to normal vectors
    n1 = float3(mul(rot1, n1.xy), n1.z);
    n2 = float3(mul(rot2, n2.xy), n2.z);
    n3 = float3(mul(rot3, n3.xy), n3.z);

    // Use normal magnitude as weight (more variation = higher weight)
    float3 Dw = float3(length(n1.xy), length(n2.xy), length(n3.xy));
    
    Dw = lerp(1.0, Dw, g_fallOffContrast);
    float3 W = Dw * pow(float3(w1, w2, w3), g_exp);
    W /= (W.x + W.y + W.z);
    if (r != 0.5) W = Gain3(W, r);

    return normalize(W.x * n1 + W.y * n2 + W.z * n3);
}
)"));
        }
        
        inputs[In_Utilities] = _writer.ToString();
        _writer.Clear();
    }

    // Shaders
    {
        WRITE_FEATURES(Shaders);
        WriteCustomGlobalCode(customGlobalCodeNodes, In_Shaders);
        inputs[In_Shaders] = _writer.ToString();
        _writer.Clear();
    }

    // Save material parameters data
    if (_parameters.HasItems())
        MaterialParams::Save(parametersData, &_parameters);
    else
        parametersData.Release();
    _parameters.Clear();

    // Create source code
    {
        // Open template file
        String path = Globals::EngineContentFolder / TEXT("Editor/MaterialTemplates/");
        switch (materialInfo.Domain)
        {
        case MaterialDomain::Surface:
            path /= TEXT("Surface.shader");
            break;
        case MaterialDomain::PostProcess:
            path /= TEXT("PostProcess.shader");
            break;
        case MaterialDomain::GUI:
            path /= TEXT("GUI.shader");
            break;
        case MaterialDomain::Decal:
            path /= TEXT("Decal.shader");
            break;
        case MaterialDomain::Terrain:
            path /= TEXT("Terrain.shader");
            break;
        case MaterialDomain::Particle:
            path /= TEXT("Particle.shader");
            break;
        case MaterialDomain::Deformable:
            path /= TEXT("Deformable.shader");
            break;
        case MaterialDomain::VolumeParticle:
            path /= TEXT("VolumeParticle.shader");
            break;
        default:
            LOG(Warning, "Unknown material domain.");
            return true;
        }
        auto file = FileReadStream::Open(path);
        if (file == nullptr)
        {
            LOG(Error, "Cannot open file {0}", path);
            return true;
        }

        // Format template
        const uint32 length = file->GetLength();
        Array<char> tmp;
        for (uint32 i = 0; i < length; i++)
        {
            char c = file->ReadByte();

            if (c != '@')
            {
                source.WriteByte(c);
            }
            else
            {
                i++;
                int32 inIndex = file->ReadByte() - '0';
                ASSERT_LOW_LAYER(Math::IsInRange(inIndex, 0, In_MAX - 1));

                const String& in = inputs[inIndex];
                if (in.Length() > 0)
                {
                    tmp.EnsureCapacity(in.Length() + 1, false);
                    StringUtils::ConvertUTF162ANSI(*in, tmp.Get(), in.Length());
                    source.WriteBytes(tmp.Get(), in.Length());
                }
            }
        }

        // Close file
        Delete(file);

        // Ensure to have null-terminated source code
        source.WriteByte(0);
    }

    return false;
}

void MaterialGenerator::clearCache()
{
    for (int32 i = 0; i < _layers.Count(); i++)
        _layers[i]->ClearCache();
    for (auto& e : _functions)
    {
        for (auto& node : e.Value->Nodes)
        {
            for (int32 j = 0; j < node.Boxes.Count(); j++)
                node.Boxes[j].Cache.Clear();
        }
    }
    _ddx = Value();
    _ddy = Value();
    _cameraVector = Value();
}

void MaterialGenerator::writeBlending(MaterialGraphBoxes box, Value& result, const Value& bottom, const Value& top, const Value& alpha)
{
    const auto& boxInfo = GetMaterialRootNodeBox(box);
    _writer.Write(TEXT("\t{0}.{1} = lerp({2}.{1}, {3}.{1}, {4});\n"), result.Value, boxInfo.SubName, bottom.Value, top.Value, alpha.Value);
    if (box == MaterialGraphBoxes::Normal)
    {
        _writer.Write(TEXT("\t{0}.{1} = normalize({0}.{1});\n"), result.Value, boxInfo.SubName);
    }
}

SerializedMaterialParam* MaterialGenerator::findParam(const Guid& id, MaterialLayer* layer)
{
    // Use per material layer params mapping
    return findParam(layer->GetMappedParamId(id));
}

MaterialGraphParameter* MaterialGenerator::findGraphParam(const Guid& id)
{
    MaterialGraphParameter* result = nullptr;

    for (int32 i = 0; i < _layers.Count(); i++)
    {
        result = _layers[i]->Graph.GetParameter(id);
        if (result)
            break;
    }

    return result;
}

void MaterialGenerator::createGradients(Node* caller)
{
    if (_ddx.IsInvalid())
        _ddx = writeLocal(VariantType::Float2, TEXT("ddx(input.TexCoord.xy)"), caller);
    if (_ddy.IsInvalid())
        _ddy = writeLocal(VariantType::Float2, TEXT("ddy(input.TexCoord.xy)"), caller);
}

MaterialGenerator::Value MaterialGenerator::getCameraVector(Node* caller)
{
    if (_cameraVector.IsInvalid())
        _cameraVector = writeLocal(VariantType::Float3, TEXT("normalize(ViewPos.xyz - input.WorldPosition.xyz)"), caller);
    return _cameraVector;
}

void MaterialGenerator::eatMaterialGraphBox(String& layerVarName, MaterialGraphBox* nodeBox, MaterialGraphBoxes box)
{
    // Cache data
    auto& boxInfo = GetMaterialRootNodeBox(box);

    // Get value
    const auto value = Value::Cast(tryGetValue(nodeBox, boxInfo.DefaultValue), boxInfo.DefaultValue.Type);

    // Write formatted value
    _writer.WriteLine(TEXT("\t{0}.{1} = {2};"), layerVarName, boxInfo.SubName, value.Value);
}

void MaterialGenerator::eatMaterialGraphBox(MaterialLayer* layer, MaterialGraphBoxes box)
{
    auto& boxInfo = GetMaterialRootNodeBox(box);
    const auto nodeBox = layer->Root->GetBox(boxInfo.ID);
    eatMaterialGraphBox(_treeLayerVarName, nodeBox, box);
}

void MaterialGenerator::eatMaterialGraphBoxWithDefault(MaterialLayer* layer, MaterialGraphBoxes box)
{
    auto& boxInfo = GetMaterialRootNodeBox(box);
    _writer.WriteLine(TEXT("\t{0}.{1} = {2};"), _treeLayerVarName, boxInfo.SubName, boxInfo.DefaultValue.Value);
}

void MaterialGenerator::ProcessGroupMath(Box* box, Node* node, Value& value)
{
    switch (node->TypeID)
    {
    // Vector Transform
    case 30:
    {
        // Get input vector
        auto v = tryGetValue(node->GetBox(0), Value::InitForZero(VariantType::Float3));

        // Select transformation spaces
        ASSERT(node->Values[0].Type == VariantType::Int && node->Values[1].Type == VariantType::Int);
        ASSERT(Math::IsInRange(node->Values[0].AsInt, 0, (int32)TransformCoordinateSystem::MAX - 1));
        ASSERT(Math::IsInRange(node->Values[1].AsInt, 0, (int32)TransformCoordinateSystem::MAX - 1));
        auto inputType = static_cast<TransformCoordinateSystem>(node->Values[0].AsInt);
        auto outputType = static_cast<TransformCoordinateSystem>(node->Values[1].AsInt);
        if (inputType == outputType)
        {
            // No space change at all
            value = v;
        }
        else
        {
            // Switch by source space type
            const Char* format = nullptr;
            switch (inputType)
            {
            case TransformCoordinateSystem::Tangent:
                switch (outputType)
                {
                case TransformCoordinateSystem::Tangent:
                    format = TEXT("{0}");
                    break;
                case TransformCoordinateSystem::World:
                    format = TEXT("TransformTangentVectorToWorld(input, {0})");
                    break;
                case TransformCoordinateSystem::View:
                    format = TEXT("TransformWorldVectorToView(input, TransformTangentVectorToWorld(input, {0}))");
                    break;
                case TransformCoordinateSystem::Local:
                    format = TEXT("TransformWorldVectorToLocal(input, TransformTangentVectorToWorld(input, {0}))");
                    break;
                }
                break;
            case TransformCoordinateSystem::World:
                switch (outputType)
                {
                case TransformCoordinateSystem::Tangent:
                    format = TEXT("TransformWorldVectorToTangent(input, {0})");
                    break;
                case TransformCoordinateSystem::World:
                    format = TEXT("{0}");
                    break;
                case TransformCoordinateSystem::View:
                    format = TEXT("TransformWorldVectorToView(input, {0})");
                    break;
                case TransformCoordinateSystem::Local:
                    format = TEXT("TransformWorldVectorToLocal(input, {0})");
                    break;
                }
                break;
            case TransformCoordinateSystem::View:
                switch (outputType)
                {
                case TransformCoordinateSystem::Tangent:
                    format = TEXT("TransformWorldVectorToTangent(input, TransformViewVectorToWorld(input, {0}))");
                    break;
                case TransformCoordinateSystem::World:
                    format = TEXT("TransformViewVectorToWorld(input, {0})");
                    break;
                case TransformCoordinateSystem::View:
                    format = TEXT("{0}");
                    break;
                case TransformCoordinateSystem::Local:
                    format = TEXT("TransformWorldVectorToLocal(input, TransformViewVectorToWorld(input, {0}))");
                    break;
                }
                break;
            case TransformCoordinateSystem::Local:
                switch (outputType)
                {
                case TransformCoordinateSystem::Tangent:
                    format = TEXT("TransformWorldVectorToTangent(input, TransformLocalVectorToWorld(input, {0}))");
                    break;
                case TransformCoordinateSystem::World:
                    format = TEXT("TransformLocalVectorToWorld(input, {0})");
                    break;
                case TransformCoordinateSystem::View:
                    format = TEXT("TransformWorldVectorToView(input, TransformLocalVectorToWorld(input, {0}))");
                    break;
                case TransformCoordinateSystem::Local:
                    format = TEXT("{0}");
                    break;
                }
                break;
            }
            ASSERT(format != nullptr);

            // Write operation
            value = writeLocal(VariantType::Float3, String::Format(format, v.Value), node);
        }
        break;
    }
    default:
        ShaderGenerator::ProcessGroupMath(box, node, value);
        break;
    }
}

void MaterialGenerator::WriteCustomGlobalCode(const Array<const MaterialGraph::Node*, InlinedAllocation<8>>& nodes, int32 templateInputsMapping)
{
    for (const MaterialGraph::Node* node : nodes)
    {
        if ((int32)node->Values[2] == templateInputsMapping)
        {
            _writer.Write(TEXT("\n"));
            _writer.Write((StringView)node->Values[0]);
            _writer.Write(TEXT("\n"));
        }
    }
}

ShaderGenerator::Value MaterialGenerator::VsToPs(Node* node, Box* input)
{
    // If used in VS then pass the value from the input box
    if (_treeType == MaterialTreeType::VertexShader)
    {
        return tryGetValue(input, Value::Zero).AsFloat4();
    }

    // Check if can use more interpolants
    if (_vsToPsInterpolants.Count() == 16)
    {
        OnError(node, input, TEXT("Too many VS to PS interpolants used."));
        return Value::Zero;
    }

    // Check if can use interpolants
    const auto layer = GetRootLayer();
    if (!layer || layer->Domain == MaterialDomain::Decal || layer->Domain == MaterialDomain::PostProcess)
    {
        OnError(node, input, TEXT("VS to PS interpolants are not supported in Decal or Post Process materials."));
        return Value::Zero;
    }

    // Indicate the interpolator slot usage
    _vsToPsInterpolants.Add(input);
    return Value(VariantType::Float4, String::Format(TEXT("input.CustomVSToPS[{0}]"), _vsToPsInterpolants.Count() - 1));
}

#endif
