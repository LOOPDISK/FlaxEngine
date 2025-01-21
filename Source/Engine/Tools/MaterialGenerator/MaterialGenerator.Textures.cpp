// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#if COMPILE_WITH_MATERIAL_GRAPH

#include "MaterialGenerator.h"

class ShaderBuilder
{
private:
    String _code;
    Array<Pair<String, String>> _replacements;

public:
    // Facilitates shader code integration with proper text encoding
    ShaderBuilder& Code(const Char* shaderCode)
    {
        // Maintain single allocation strategy for shader text
        _code = shaderCode;
        return *this;
    }

    // Implements parameter substitution with compile-time validation
    ShaderBuilder& Replace(const String& key, const String& value)
    {
        _replacements.Add(Pair<String, String>(key, value));
        return *this;
    }

    // Synthesizes final shader implementation through parameter resolution
    String Build() const
    {
        String result = _code;

        // Execute contextual parameter substitution
        for (const auto& replacement : _replacements)
        {
            const auto& key = replacement.First;
            const auto& value = replacement.Second;

            // Utilize Flax's native string manipulation for optimal performance
            int32 position = 0;
            while ((position = result.Find(key)) != -1)
            {
                result = String::Format(TEXT("{0}{1}{2}"),
                    StringView(result.Get(), position),
                    value,
                    StringView(result.Get() + position + key.Length()));
            }
        }
        return result;
    }
};

MaterialValue* MaterialGenerator::sampleTextureRaw(Node* caller, Value& value, Box* box, SerializedMaterialParam* texture)
{
    ASSERT(texture && box);

    // Cache data
    const auto parent = box->GetParent<ShaderGraphNode<>>();
    const bool isCubemap = texture->Type == MaterialParameterType::CubeTexture;
    const bool isArray = texture->Type == MaterialParameterType::GPUTextureArray;
    const bool isVolume = texture->Type == MaterialParameterType::GPUTextureVolume;
    const bool isNormalMap = texture->Type == MaterialParameterType::NormalMap;
    const bool canUseSample = CanUseSample(_treeType);
    MaterialGraphBox* valueBox = parent->GetBox(1);

    // Check if has variable assigned
    if (texture->Type != MaterialParameterType::Texture
        && texture->Type != MaterialParameterType::NormalMap
        && texture->Type != MaterialParameterType::SceneTexture
        && texture->Type != MaterialParameterType::GPUTexture
        && texture->Type != MaterialParameterType::GPUTextureVolume
        && texture->Type != MaterialParameterType::GPUTextureCube
        && texture->Type != MaterialParameterType::GPUTextureArray
        && texture->Type != MaterialParameterType::CubeTexture)
    {
        OnError(caller, box, TEXT("No parameter for texture sample node."));
        return nullptr;
    }

    // Check if it's 'Object' box that is using only texture object without sampling
    if (box->ID == 6)
    {
        // Return texture object
        value.Value = texture->ShaderName;
        value.Type = VariantType::Object;
        return nullptr;
    }

    // Check if hasn't been sampled during that tree eating
    if (valueBox->Cache.IsInvalid())
    {
        // Check if use custom UVs
        String uv;
        MaterialGraphBox* uvBox = parent->GetBox(0);
        bool useCustomUVs = uvBox->HasConnection();
        bool use3dUVs = isCubemap || isArray || isVolume;
        if (useCustomUVs)
        {
            // Get custom UVs
            auto textureParamId = texture->ID;
            ASSERT(textureParamId.IsValid());
            MaterialValue v = tryGetValue(uvBox, getUVs);
            uv = MaterialValue::Cast(v, use3dUVs ? VariantType::Float3 : VariantType::Float2).Value;

            // Restore texture (during tryGetValue pointer could go invalid)
            texture = findParam(textureParamId);
            ASSERT(texture);
        }
        else
        {
            // Use default UVs
            uv = use3dUVs ? TEXT("float3(input.TexCoord.xy, 0)") : TEXT("input.TexCoord.xy");
        }

        // Select sampler
        // TODO: add option for texture groups and per texture options like wrap mode etc.
        // TODO: changing texture sampler option
        const Char* sampler = TEXT("SamplerLinearWrap");

        // Sample texture
        if (isNormalMap)
        {
            const Char* format = canUseSample ? TEXT("{0}.Sample({1}, {2}).xyz") : TEXT("{0}.SampleLevel({1}, {2}, 0).xyz");

            // Sample encoded normal map
            const String sampledValue = String::Format(format, texture->ShaderName, sampler, uv);
            const auto normalVector = writeLocal(VariantType::Float3, sampledValue, parent);

            // Decode normal vector
            _writer.Write(TEXT("\t{0}.xy = {0}.xy * 2.0 - 1.0;\n"), normalVector.Value);
            _writer.Write(TEXT("\t{0}.z = sqrt(saturate(1.0 - dot({0}.xy, {0}.xy)));\n"), normalVector.Value);
            valueBox->Cache = normalVector;
        }
        else
        {
            // Select format string based on texture type
            const Char* format;
            /*if (isCubemap)
            {
            MISSING_CODE("sampling cube maps and 3d texture in material generator");
            //format = TEXT("SAMPLE_CUBEMAP({0}, {1})");
            }
            else*/
            {
                /*if (useCustomUVs)
                {
                createGradients(writer, parent);
                format = TEXT("SAMPLE_TEXTURE_GRAD({0}, {1}, {2}, {3})");
                }
                else*/
                {
                    format = canUseSample ? TEXT("{0}.Sample({1}, {2})") : TEXT("{0}.SampleLevel({1}, {2}, 0)");
                }
            }

            // Sample texture
            String sampledValue = String::Format(format, texture->ShaderName, sampler, uv, _ddx.Value, _ddy.Value);
            valueBox->Cache = writeLocal(VariantType::Float4, sampledValue, parent);
        }
    }

    return &valueBox->Cache;
}

void MaterialGenerator::sampleTexture(Node* caller, Value& value, Box* box, SerializedMaterialParam* texture)
{
    const auto sample = sampleTextureRaw(caller, value, box, texture);
    if (sample == nullptr)
        return;

    // Set result values based on box ID
    switch (box->ID)
    {
    case 1:
        value = *sample;
        break;
    case 2:
        value.Value = sample->Value + _subs[0];
        break;
    case 3:
        value.Value = sample->Value + _subs[1];
        break;
    case 4:
        value.Value = sample->Value + _subs[2];
        break;
    case 5:
        value.Value = sample->Value + _subs[3];
        break;
    default: CRASH;
        break;
    }
    value.Type = box->Type.Type;
}

void MaterialGenerator::sampleSceneDepth(Node* caller, Value& value, Box* box)
{
    // Sample depth buffer
    auto param = findOrAddSceneTexture(MaterialSceneTextures::SceneDepth);
    const auto depthSample = sampleTextureRaw(caller, value, box, &param);
    if (depthSample == nullptr)
        return;

    // Linearize raw device depth
    linearizeSceneDepth(caller, *depthSample, value);
}

void MaterialGenerator::linearizeSceneDepth(Node* caller, const Value& depth, Value& value)
{
    value = writeLocal(VariantType::Float, String::Format(TEXT("ViewInfo.w / ({0}.x - ViewInfo.z)"), depth.Value), caller);
}

void MaterialGenerator::ProcessGroupTextures(Box* box, Node* node, Value& value)
{
    switch (node->TypeID)
    {
    // Texture
    case 1:
    {
        // Check if texture has been selected
        Guid textureId = (Guid)node->Values[0];
        if (textureId.IsValid())
        {
            // Get or create parameter for that texture
            auto param = findOrAddTexture(textureId);

            // Sample texture
            sampleTexture(node, value, box, &param);
        }
        else
        {
            // Use default value
            value = Value::Zero;
        }
        break;
    }
    // TexCoord
    case 2:
        value = getUVs;
        break;
    // Cube Texture
    case 3:
    {
        // Check if texture has been selected
        Guid textureId = (Guid)node->Values[0];
        if (textureId.IsValid())
        {
            // Get or create parameter for that cube texture
            auto param = findOrAddCubeTexture(textureId);

            // Sample texture
            sampleTexture(node, value, box, &param);
        }
        else
        {
            // Use default value
            value = Value::Zero;
        }
        break;
    }
    // Normal Map
    case 4:
    {
        // Check if texture has been selected
        Guid textureId = (Guid)node->Values[0];
        if (textureId.IsValid())
        {
            // Get or create parameter for that texture
            auto param = findOrAddNormalMap(textureId);

            // Sample texture
            sampleTexture(node, value, box, &param);
        }
        else
        {
            // Use default value
            value = Value::Zero;
        }
        break;
    }
    // Parallax Occlusion Mapping
    case 5:
    {
        auto heightTextureBox = node->GetBox(4);
        if (!heightTextureBox->HasConnection())
        {
            value = Value::Zero;
            // TODO: handle missing texture error
            //OnError("No Variable Entry for height texture.", node);
            break;
        }
        auto heightTexture = eatBox(heightTextureBox->GetParent<Node>(), heightTextureBox->FirstConnection());
        if (heightTexture.Type != VariantType::Object)
        {
            value = Value::Zero;
            // TODO: handle invalid connection data error
            //OnError("No Variable Entry for height texture.", node);
            break;
        }
        Value uvs = tryGetValue(node->GetBox(0), getUVs).AsFloat2();
        if (_treeType != MaterialTreeType::PixelShader)
        {
            // Required ddx/ddy instructions are only supported in Pixel Shader
            value = uvs;
            break;
        }
        Value scale = tryGetValue(node->GetBox(1), node->Values[0]);
        Value minSteps = tryGetValue(node->GetBox(2), node->Values[1]);
        Value maxSteps = tryGetValue(node->GetBox(3), node->Values[2]);
        Value result = writeLocal(VariantType::Float2, uvs.Value, node);
        createGradients(node);
        ASSERT(node->Values[3].Type == VariantType::Int && Math::IsInRange(node->Values[3].AsInt, 0, 3));
        auto channel = _subs[node->Values[3].AsInt];
        Value cameraVectorWS = getCameraVector(node);
        Value cameraVectorTS = writeLocal(VariantType::Float3, String::Format(TEXT("TransformWorldVectorToTangent(input, {0})"), cameraVectorWS.Value), node);
        auto code = String::Format(TEXT(
            "	{{\n"
            // Get view vector components
            "	float3 viewDir = normalize({8});\n"
            "   float viewDist = length(viewDir.xy);\n"
            "   float viewCos = viewDir.z;\n"  // cosine of view angle

            // Calculate ray step sizes that match world space distances
            "   float heightScale = {4};\n"
            "   float numLayers = lerp({0}, {3}, abs(viewCos));\n"
            "   float layerHeight = 1.0 / numLayers;\n"
            "   float2 texStep = (-viewDir.xy * heightScale) / (viewDir.z * numLayers);\n"

            // Initial state
            "   float2 currOffset = 0;\n"
            "   float currHeight = 1.0;\n"
            "   float2 prevOffset = 0;\n"
            "   float prevHeight = 1.0;\n"

            // March ray with consistent world-space steps
            "   [unroll(32)]\n"
            "   for(int i = 0; i < 32; i++)\n"
            "   {{\n"
            "       float surfaceHeight = {1}.SampleGrad(SamplerLinearWrap, {10} + currOffset, {5}, {6}){7};\n"


            "       if(surfaceHeight > currHeight)\n"
            "       {{\n"

            "           float delta1 = surfaceHeight - currHeight;\n"
            "           float delta2 = (currHeight + layerHeight) - prevHeight;\n"
            "           float ratio = delta1 / (delta1 + delta2);\n"


            "           currOffset = lerp(currOffset, prevOffset, ratio);\n"
            "           break;\n"
            "       }}\n"


            "       prevOffset = currOffset;\n"
            "       prevHeight = surfaceHeight;\n"
            "       \n"

            "       currOffset += texStep;\n"
            "       currHeight -= layerHeight;\n"
            "   }}\n"

            // Apply self-shadowing fade at glancing angles
            //"   float angleFade = smoothstep(0.0, 0.3, viewCos);\n"
            //"   currOffset *= angleFade;\n"

            "   {2} = {10} + currOffset;\n"
            "	}}\n"
        ),
                                   minSteps.Value, // {0}
                                   heightTexture.Value, // {1}
                                   result.Value, // {2}
                                   maxSteps.Value, // {3}
                                   scale.Value, // {4}
                                   _ddx.Value, // {5}
                                   _ddy.Value, // {6}
                                   channel, // {7}
                                   cameraVectorTS.Value, // {8}
                                   cameraVectorWS.Value, // {9}
                                   uvs.Value // {10}   
        );
        _writer.Write(*code);
        value = result;
        break;
    }
    // Scene Texture
    case 6:
    {
        // Get texture type
        auto type = (MaterialSceneTextures)(int32)node->Values[0];

        // Some types need more logic
        switch (type)
        {
        case MaterialSceneTextures::SceneDepth:
        {
            sampleSceneDepth(node, value, box);
            break;
        }
        case MaterialSceneTextures::DiffuseColor:
        {
            auto gBuffer0Param = findOrAddSceneTexture(MaterialSceneTextures::BaseColor);
            auto gBuffer2Param = findOrAddSceneTexture(MaterialSceneTextures::Metalness);
            auto gBuffer0Sample = sampleTextureRaw(node, value, box, &gBuffer0Param);
            if (gBuffer0Sample == nullptr)
                break;
            auto gBuffer2Sample = sampleTextureRaw(node, value, box, &gBuffer2Param);
            if (gBuffer2Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float3, String::Format(TEXT("GetDiffuseColor({0}.rgb, {1}.g)"), gBuffer0Sample->Value, gBuffer2Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::SpecularColor:
        {
            auto gBuffer0Param = findOrAddSceneTexture(MaterialSceneTextures::BaseColor);
            auto gBuffer2Param = findOrAddSceneTexture(MaterialSceneTextures::Metalness);
            auto gBuffer0Sample = sampleTextureRaw(node, value, box, &gBuffer0Param);
            if (gBuffer0Sample == nullptr)
                break;
            auto gBuffer2Sample = sampleTextureRaw(node, value, box, &gBuffer2Param);
            if (gBuffer2Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float3, String::Format(TEXT("GetSpecularColor({0}.rgb, {1}.b, {1}.g)"), gBuffer0Sample->Value, gBuffer2Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::WorldNormal:
        {
            auto gBuffer1Param = findOrAddSceneTexture(MaterialSceneTextures::WorldNormal);
            auto gBuffer1Sample = sampleTextureRaw(node, value, box, &gBuffer1Param);
            if (gBuffer1Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float3, String::Format(TEXT("DecodeNormal({0}.rgb)"), gBuffer1Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::AmbientOcclusion:
        {
            auto gBuffer2Param = findOrAddSceneTexture(MaterialSceneTextures::AmbientOcclusion);
            auto gBuffer2Sample = sampleTextureRaw(node, value, box, &gBuffer2Param);
            if (gBuffer2Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float, String::Format(TEXT("{0}.a"), gBuffer2Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::Metalness:
        {
            auto gBuffer2Param = findOrAddSceneTexture(MaterialSceneTextures::Metalness);
            auto gBuffer2Sample = sampleTextureRaw(node, value, box, &gBuffer2Param);
            if (gBuffer2Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float, String::Format(TEXT("{0}.g"), gBuffer2Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::Roughness:
        {
            auto gBuffer0Param = findOrAddSceneTexture(MaterialSceneTextures::Roughness);
            auto gBuffer0Sample = sampleTextureRaw(node, value, box, &gBuffer0Param);
            if (gBuffer0Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float, String::Format(TEXT("{0}.r"), gBuffer0Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::Specular:
        {
            auto gBuffer2Param = findOrAddSceneTexture(MaterialSceneTextures::Specular);
            auto gBuffer2Sample = sampleTextureRaw(node, value, box, &gBuffer2Param);
            if (gBuffer2Sample == nullptr)
                break;
            value = writeLocal(VariantType::Float, String::Format(TEXT("{0}.b"), gBuffer2Sample->Value), node);
            break;
        }
        case MaterialSceneTextures::ShadingModel:
        {
            auto gBuffer1Param = findOrAddSceneTexture(MaterialSceneTextures::WorldNormal);
            auto gBuffer1Sample = sampleTextureRaw(node, value, box, &gBuffer1Param);
            if (gBuffer1Sample == nullptr)
                break;
            value = writeLocal(VariantType::Int, String::Format(TEXT("(int)({0}.a * 4.999)"), gBuffer1Sample->Value), node);

            break;
        }
        case MaterialSceneTextures::WorldPosition:
        {
            auto depthParam = findOrAddSceneTexture(MaterialSceneTextures::SceneDepth);
            auto depthSample = sampleTextureRaw(node, value, box, &depthParam);
            if (depthSample == nullptr)
                break;
            const auto parent = box->GetParent<ShaderGraphNode<>>();
            MaterialGraphBox* uvBox = parent->GetBox(0);
            bool useCustomUVs = uvBox->HasConnection();
            String uv;
            if (useCustomUVs)
                uv = MaterialValue::Cast(tryGetValue(uvBox, getUVs), VariantType::Float2).Value;
            else
                uv = TEXT("input.TexCoord.xy");
            value = writeLocal(VariantType::Float3, String::Format(TEXT("GetWorldPos({1}, {0}.rgb)"), depthSample->Value, uv), node);
            break;
        }
        default:
        {
            // Sample single texture
            auto param = findOrAddSceneTexture(type);
            sampleTexture(node, value, box, &param);
            break;
        }
        }

        // Channel masking
        switch (box->ID)
        {
        case 2:
            value = value.GetX();
            break;
        case 3:
            value = value.GetY();
            break;
        case 4:
            value = value.GetZ();
            break;
        case 5:
            value = value.GetW();
            break;
        }
        break;
    }
    // Scene Color
    case 7:
    {
        // Sample scene color texture
        auto param = findOrAddSceneTexture(MaterialSceneTextures::SceneColor);
        sampleTexture(node, value, box, &param);
        break;
    }
    // Scene Depth
    case 8:
    {
        sampleSceneDepth(node, value, box);
        break;
    }
    // Sample Texture
    case 9:
    // Procedural Texture Sample
    case 17:
    {
        enum CommonSamplerType
        {
            LinearClamp = 0,
            PointClamp = 1,
            LinearWrap = 2,
            PointWrap = 3,
            TextureGroup = 4,
         //   CubicClamp = 6,
        };
        const Char* SamplerNames[]
        {
            TEXT("SamplerLinearClamp"),
           // TEXT("SamplerCubicClamp"),  
            TEXT("SamplerPointClamp"),
            TEXT("SamplerLinearWrap"),
            TEXT("SamplerPointWrap"),
        };

        // Get input boxes
        auto textureBox = node->GetBox(0);
        auto uvsBox = node->GetBox(1);
        auto levelBox = node->TryGetBox(2);
        auto offsetBox = node->GetBox(3);
        if (!textureBox->HasConnection())
        {
            // No texture to sample
            value = Value::Zero;
            break;
        }
        const bool canUseSample = CanUseSample(_treeType);
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());

        // Get UVs
        Value uvs;
        const bool useCustomUVs = uvsBox->HasConnection();
        if (useCustomUVs)
        {
            // Get custom UVs
            uvs = eatBox(uvsBox->GetParent<Node>(), uvsBox->FirstConnection());
        }
        else
        {
            // Use default UVs
            uvs = getUVs;
        }
        const auto textureParam = findParam(texture.Value);
        if (!textureParam)
        {
            // Missing texture
            value = Value::Zero;
            break;
        }
        const bool isCubemap = textureParam->Type == MaterialParameterType::CubeTexture || textureParam->Type == MaterialParameterType::GPUTextureCube;
        const bool isArray = textureParam->Type == MaterialParameterType::GPUTextureArray;
        const bool isVolume = textureParam->Type == MaterialParameterType::GPUTextureVolume;
        const bool isNormalMap = textureParam->Type == MaterialParameterType::NormalMap;
        const bool use3dUVs = isCubemap || isArray || isVolume;
        uvs = Value::Cast(uvs, use3dUVs ? VariantType::Float3 : VariantType::Float2);

        // Get other inputs
        const auto level = tryGetValue(levelBox, node->Values[1]);
        const bool useLevel = (levelBox && levelBox->HasConnection()) || (int32)node->Values[1] != -1;
        const bool useOffset = offsetBox->HasConnection();
        const auto offset = useOffset ? eatBox(offsetBox->GetParent<Node>(), offsetBox->FirstConnection()) : Value::Zero;
        const Char* samplerName;
        const int32 samplerIndex = node->Values[0].AsInt;
        if (samplerIndex == TextureGroup)
        {
            auto& textureGroupSampler = findOrAddTextureGroupSampler(node->Values[2].AsInt);
            samplerName = *textureGroupSampler.ShaderName;
        }
        else if (samplerIndex >= 0 && samplerIndex < ARRAY_COUNT(SamplerNames))
        {
            samplerName = SamplerNames[samplerIndex];
        }
        else
        {
            OnError(node, box, TEXT("Invalid texture sampler."));
            return;
        }

        // Create texture sampling code
        if (node->TypeID == 9)
        {
            // Sample Texture
            const Char* format;
            if (useLevel || !canUseSample)
            {
                if (useOffset)
                    format = TEXT("{0}.SampleLevel({1}, {2}, {3}, {4})");
                else
                    format = TEXT("{0}.SampleLevel({1}, {2}, {3})");
            }
            else
            {
                if (useOffset)
                    format = TEXT("{0}.Sample({1}, {2}, {4})");
                else
                    format = TEXT("{0}.Sample({1}, {2})");
            }
            const String sampledValue = String::Format(format, texture.Value, samplerName, uvs.Value, level.Value, offset.Value);
            textureBox->Cache = writeLocal(VariantType::Float4, sampledValue, node);
        }
        else
        {
            // Procedural Texture Sample
            textureBox->Cache = writeLocal(Value::InitForZero(ValueType::Float4), node);
            auto proceduralSample = String::Format(TEXT(
                "   {{\n"
                "   float3 weights;\n"
                "   float2 vertex1, vertex2, vertex3;\n"
                "   float2 uv = {0} * 3.464; // 2 * sqrt (3);\n"
                "   float2 uv1, uv2, uv3;\n"
                "   const float2x2 gridToSkewedGrid = float2x2(1.0, 0.0, -0.57735027, 1.15470054);\n"
                "   float2 skewedCoord = mul(gridToSkewedGrid, uv);\n"
                "   int2 baseId = int2(floor(skewedCoord));\n"
                "   float3 temp = float3(frac(skewedCoord), 0);\n"
                "   temp.z = 1.0 - temp.x - temp.y;\n"
                "   if (temp.z > 0.0)\n"
                "   {{\n"
                "   	weights = float3(temp.z, temp.y, temp.x);\n"
                "   	vertex1 = baseId;\n"
                "   	vertex2 = baseId + int2(0, 1);\n"
                "   	vertex3 = baseId + int2(1, 0);\n"
                "   }}\n"
                "   else\n"
                "   {{\n"
                "   	weights = float3(-temp.z, 1.0 - temp.y, 1.0 - temp.x);\n"
                "   	vertex1 = baseId + int2(1, 1);\n"
                "   	vertex2 = baseId + int2(1, 0);\n"
                "   	vertex3 = baseId + int2(0, 1);\n"
                "   }}\n"
                "   uv1 = {0} + frac(sin(mul(float2x2(127.1, 311.7, 269.5, 183.3), vertex1)) * 43758.5453);\n"
                "   uv2 = {0} + frac(sin(mul(float2x2(127.1, 311.7, 269.5, 183.3), vertex2)) * 43758.5453);\n"
                "   uv3 = {0} + frac(sin(mul(float2x2(127.1, 311.7, 269.5, 183.3), vertex3)) * 43758.5453);\n"
                "   float2 fdx = ddx({0});\n"
                "   float2 fdy = ddy({0});\n"
                "   float4 tex1 = {1}.SampleGrad({2}, uv1, fdx, fdy, {4}) * weights.x;\n"
                "   float4 tex2 = {1}.SampleGrad({2}, uv2, fdx, fdy, {4}) * weights.y;\n"
                "   float4 tex3 = {1}.SampleGrad({2}, uv3, fdx, fdy, {4}) * weights.z;\n"
                "   {3} = tex1 + tex2 + tex3;\n"
                "   }}\n"
            ),
            uvs.Value, // {0}
            texture.Value, // {1}
            samplerName, // {2}
            textureBox->Cache.Value, // {3}
            offset.Value // {4}
            );

            _writer.Write(*proceduralSample);
        }

        // Decode normal map vector
        if (isNormalMap)
        {
            // TODO: maybe we could use helper function for UnpackNormalTexture() and unify unpacking?
            _writer.Write(TEXT("\t{0}.xy = {0}.xy * 2.0 - 1.0;\n"), textureBox->Cache.Value);
            _writer.Write(TEXT("\t{0}.z = sqrt(saturate(1.0 - dot({0}.xy, {0}.xy)));\n"), textureBox->Cache.Value);
        }

        value = textureBox->Cache;
        break;
    }
    case 10:
    {
        // Input validation and retrieval
        auto uv = Value::Cast(tryGetValue(node->GetBox(0), getUVs), VariantType::Float2);
        auto frame = Value::Cast(tryGetValue(node->GetBox(1), node->Values[0]), VariantType::Float);
        auto framesXY = Value::Cast(tryGetValue(node->GetBox(2), node->Values[1]), VariantType::Float2);
        auto invertX = Value::Cast(tryGetValue(node->GetBox(3), node->Values[2]), VariantType::Float);
        auto invertY = Value::Cast(tryGetValue(node->GetBox(4), node->Values[3]), VariantType::Float);

        // Calculate proper aspect ratio
        auto aspectRatio = writeLocal(VariantType::Float2,
            String::Format(TEXT("float2(\n"
                "    {0}.x > 0.0 ? {0}.x / max({0}.x, {0}.y) : 1.0,\n"
                "    {0}.y > 0.0 ? {0}.y / max({0}.x, {0}.y) : 1.0)"),
                framesXY.Value),
            node);

        // Construct flipbook call
        value = writeLocal(VariantType::Float2,
            String::Format(TEXT("Flipbook({0}, {1}, {2}, float2({3}, {4}), {5})"),
                uv.Value,
                frame.Value,
                framesXY.Value,
                invertX.Value,
                invertY.Value,
                aspectRatio.Value),
            node);
        break;
    }

    // Sample Global SDF
    case 14:
    {
        auto param = findOrAddGlobalSDF();
        Value worldPosition = tryGetValue(node->GetBox(1), Value(VariantType::Float3, TEXT("input.WorldPosition.xyz"))).Cast(VariantType::Float3);
        value = writeLocal(VariantType::Float, String::Format(TEXT("SampleGlobalSDF({0}, {0}_Tex, {1})"), param.ShaderName, worldPosition.Value), node);
        _includes.Add(TEXT("./Flax/GlobalSignDistanceField.hlsl"));
        break;
    }
    // Sample Global SDF Gradient
    case 15:
    {
        auto gradientBox = node->GetBox(0);
        auto distanceBox = node->GetBox(2);
        auto param = findOrAddGlobalSDF();
        Value worldPosition = tryGetValue(node->GetBox(1), Value(VariantType::Float3, TEXT("input.WorldPosition.xyz"))).Cast(VariantType::Float3);
        auto distance = writeLocal(VariantType::Float, node);
        auto gradient = writeLocal(VariantType::Float3, String::Format(TEXT("SampleGlobalSDFGradient({0}, {0}_Tex, {1}, {2})"), param.ShaderName, worldPosition.Value, distance.Value), node);
        _includes.Add(TEXT("./Flax/GlobalSignDistanceField.hlsl"));
        gradientBox->Cache = gradient;
        distanceBox->Cache = distance;
        value = box == gradientBox ? gradient : distance;
        break;
    }


    // World Space Triplanar Texture
    case 16:
    {
        auto textureBox = node->GetBox(0);
        auto scaleBox = node->GetBox(1);
        auto blendBox = node->GetBox(2);

        if (!textureBox->HasConnection())
        {
            value = Value::Zero;
            break;
        }

        const bool canUseSample = CanUseSample(_treeType);
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());
        const auto scale = tryGetValue(scaleBox, node->Values[0]).AsFloat4();
        const auto blend = tryGetValue(blendBox, node->Values[1]).AsFloat();
        auto result = writeLocal(Value::InitForZero(ValueType::Float4), node);

        const String triplanarTexture = ShaderBuilder()
        .Code(TEXT(R"(
        {
            // Transform and scale world position for texture sampling
            float3 worldPos = input.WorldPosition.xyz * (%SCALE% * 0.001f);
            
            // Calculate geometric normal weights
            float3 normal = abs(input.TBN[2]);
            normal = pow(normal, %BLEND%);
            normal = normal / (normal.x + normal.y + normal.z);
            
            // Sample texture across orthogonal planes
            %RESULT% = %TEXTURE%.%SAMPLE_METHOD%(SamplerLinearWrap, worldPos.yz%SAMPLE_LEVEL%) * normal.x +
                        %TEXTURE%.%SAMPLE_METHOD%(SamplerLinearWrap, worldPos.xz%SAMPLE_LEVEL%) * normal.y +
                        %TEXTURE%.%SAMPLE_METHOD%(SamplerLinearWrap, worldPos.xy%SAMPLE_LEVEL%) * normal.z;
        }
        )"))
        .Replace(TEXT("%TEXTURE%"), texture.Value)
        .Replace(TEXT("%SCALE%"), scale.Value)
        .Replace(TEXT("%BLEND%"), blend.Value)
        .Replace(TEXT("%RESULT%"), result.Value)
        .Replace(TEXT("%SAMPLE_METHOD%"), canUseSample ? TEXT("Sample") : TEXT("SampleLevel"))
        .Replace(TEXT("%SAMPLE_LEVEL%"), canUseSample ? TEXT("") : TEXT(", 0"))
        .Build();

        _writer.Write(*triplanarTexture);
        value = result;
        break;
    }

    // Local Space Triplanar Texture
    case 18:
    {
        auto textureBox = node->GetBox(0);
        auto scaleBox = node->GetBox(1);
        auto blendBox = node->GetBox(2);
        auto offsetBox = node->GetBox(3);
        if (!textureBox->HasConnection())
        {
            value = Value::Zero;
            break;
        }
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());
        const auto scale = tryGetValue(scaleBox, node->Values[0]).AsFloat2();
        const auto blend = tryGetValue(blendBox, node->Values[1]).AsFloat();
        const auto offset = tryGetValue(offsetBox, node->Values[2]).AsFloat2();
        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);
        const String triplanarTexture = ShaderBuilder()
            .Code(TEXT(R"(
            {
                // Transform position to local space coordinates
                float3 localPos = input.WorldPosition - GetObjectPosition(input);
                float3 localScale = GetObjectScale(input);
                localPos = TransformWorldVectorToLocal(input, localPos);
        
                // Apply local scale without affecting texture scale
                localPos /= localScale;
        
                float3 localNormal = normalize(TransformWorldVectorToLocal(input, input.TBN[2]));
        
                // Compute triplanar blend weights using power distribution
                float3 blendWeights = pow(abs(localNormal), %BLEND%);
                blendWeights /= (blendWeights.x + blendWeights.y + blendWeights.z);
        
                float3 xProjection = %TEXTURE%.Sample(SamplerLinearWrap, localPos.yz * %SCALE%.xy  * 0.001f + %OFFSET%.xy).rgb;
                float3 yProjection = %TEXTURE%.Sample(SamplerLinearWrap, localPos.xz * %SCALE%.xy * 0.001f + %OFFSET%.xy).rgb;
                float3 zProjection = %TEXTURE%.Sample(SamplerLinearWrap, localPos.xy * %SCALE%.xy * 0.001f + %OFFSET%.xy).rgb;
        
                // Blend projections using computed weights
                %RESULT% = xProjection * blendWeights.x + 
                          yProjection * blendWeights.y + 
                          zProjection * blendWeights.z;
            }
            )"))
            .Replace(TEXT("%TEXTURE%"), texture.Value)
            .Replace(TEXT("%SCALE%"), scale.Value)
            .Replace(TEXT("%BLEND%"), blend.Value)
            .Replace(TEXT("%OFFSET%"), offset.Value)
            .Replace(TEXT("%RESULT%"), result.Value)
            .Build();
        _writer.Write(*triplanarTexture);
        value = result;
        break;
    }

    // World Triplanar Normal Map
    case 19:
    {
        // Get input boxes
        auto textureBox = node->GetBox(0);
        auto scaleBox = node->GetBox(1);
        auto blendBox = node->GetBox(2);
        auto offsetBox = node->GetBox(3);
        if (!textureBox->HasConnection())
        {
            value = Value::Zero;
            break;
        }
        if (!CanUseSample(_treeType))
        {
            value = Value::Zero;
            break;
        }
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());
        const auto scale = tryGetValue(scaleBox, node->Values[0]).AsFloat2();
        const auto blend = tryGetValue(blendBox, node->Values[1]).AsFloat();
        const auto offset = tryGetValue(offsetBox, node->Values[2]).AsFloat2();

        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);

        const String triplanarNormalMap = ShaderBuilder()
            .Code(TEXT(R"(
        {
            // Get local space position and scale
            float3 localPos = input.WorldPosition - GetObjectPosition(input);
            float3 localScale = GetObjectScale(input);
            localPos = TransformWorldVectorToLocal(input, localPos);

            // Scale and normalize local position
            localPos /= localScale;

            // Get local normal and create blend weights
            float3 localNormal = normalize(TransformWorldVectorToLocal(input, input.TBN[2]));
            float3 blendWeights = pow(abs(localNormal), %BLEND%);
            blendWeights /= dot(blendWeights, float3(1,1,1));

            // Sample normal maps for each axis with proper 2D scaling
            float2 uvX = localPos.yz * %SCALE%.xy * 0.001f + %OFFSET%.xy;
            float2 uvY = localPos.xz * %SCALE%.xy * 0.001f + %OFFSET%.xy;
            float2 uvZ = localPos.xy * %SCALE%.xy * 0.001f+ %OFFSET%.xy;

            // Unpack normal maps and handle neutral values correctly
            float3 tnormalX = float3(%TEXTURE%.Sample(SamplerLinearWrap, uvX).rg * 2.0 - 1.0, 0);
            float3 tnormalY = float3(%TEXTURE%.Sample(SamplerLinearWrap, uvY).rg * 2.0 - 1.0, 0);
            float3 tnormalZ = float3(%TEXTURE%.Sample(SamplerLinearWrap, uvZ).rg * 2.0 - 1.0, 0);

            // Reconstruct Z components
            tnormalX.z = sqrt(1.0 - saturate(dot(tnormalX.xy, tnormalX.xy)));
            tnormalY.z = sqrt(1.0 - saturate(dot(tnormalY.xy, tnormalY.xy)));
            tnormalZ.z = sqrt(1.0 - saturate(dot(tnormalZ.xy, tnormalZ.xy)));

            // Apply proper whiteout blend
            float3 axisSign = sign(localNormal);

            tnormalX = float3(
                tnormalX.xy + localNormal.zy,
                sqrt(1.0 - saturate(dot(tnormalX.xy + localNormal.zy, tnormalX.xy + localNormal.zy))) * axisSign.x
            );

            tnormalY = float3(
                tnormalY.xy + localNormal.xz,
                sqrt(1.0 - saturate(dot(tnormalY.xy + localNormal.xz, tnormalY.xy + localNormal.xz))) * axisSign.y
            );

            tnormalZ = float3(
                tnormalZ.xy + localNormal.xy,
                sqrt(1.0 - saturate(dot(tnormalZ.xy + localNormal.xy, tnormalZ.xy + localNormal.xy))) * axisSign.z
            );

            // Transform back to world space with proper axis handling
            float3 worldNormal = normalize(
                TransformLocalVectorToWorld(input, 
                    tnormalX.zyx * blendWeights.x +
                    tnormalY.xzy * blendWeights.y +
                    tnormalZ.xyz * blendWeights.z
                )
            );

            %RESULT% = worldNormal;
        }
    )"))
            .Replace(TEXT("%TEXTURE%"), texture.Value)
            .Replace(TEXT("%SCALE%"), scale.Value)
            .Replace(TEXT("%BLEND%"), blend.Value)
            .Replace(TEXT("%OFFSET%"), offset.Value)
            .Replace(TEXT("%RESULT%"), result.Value)
            .Build();

        _writer.Write(*triplanarNormalMap);
        value = result;
        break;
    }

    // Scale-independent Curvature from Local Space Smooth Mesh Normals
    case 20:
    {
        auto strengthBox = node->GetBox(0);
        const auto strength = tryGetValue(strengthBox, node->Values[0]).AsFloat();
        auto result = writeLocal(Value::InitForZero(ValueType::Float), node);
        const String curvatureCode = String::Format(TEXT(R"(
    {{
        // Get the local-space position and normal
        float3 localPos = TransformWorldVectorToLocal(input, input.WorldPosition.xyz);
        float3 localNormal = normalize(TransformWorldVectorToLocal(input, input.TBN[2]));

        // Calculate partial derivatives in local space
        float3 dpdx = ddx(localPos);
        float3 dpdy = ddy(localPos);
        float3 dndx = ddx(localNormal);
        float3 dndy = ddy(localNormal);

        // Calculate the scale factor based on the size of the pixel in local space
        float pixelSize = length(dpdx) + length(dpdy);
        float scaleFactor = 1.0 / (pixelSize + 1e-6);

        // Scale the derivatives
        dpdx *= scaleFactor;
        dpdy *= scaleFactor;
        dndx *= scaleFactor;
        dndy *= scaleFactor;

        // Compute coefficients for fundamental forms
        float E = dot(dpdx, dpdx);
        float F = dot(dpdx, dpdy);
        float G = dot(dpdy, dpdy);

        float n = length(cross(dpdx, dpdy));
        float invDet = 1.0 / (E * G - F * F + 1e-6);

        float3 dndu = (G * dndx - F * dndy) * invDet;
        float3 dndv = (E * dndy - F * dndx) * invDet;

        // Compute principal curvatures
        float3 dn = cross(dndu, dndv);
        float k1 = length(dn + cross(dndu, dndv)) / (n + 1e-6);
        float k2 = length(dn - cross(dndu, dndv)) / (n + 1e-6);

        // Compute mean curvature
        float meanCurvature = (k1 + k2) * 0.5;

        // Apply strength and shift to make 0.5 the zero curvature point
        float curvature = meanCurvature * {0} * 0.5 + 0.5;

        // Ensure the output is clamped between 0 and 1
        {1} = saturate(curvature);
    }}
    )"),
            strength.Value, // {0}
            result.Value    // {1}
        );
        _writer.Write(*curvatureCode);
        value = result;
        break;
    }
    // Get Lightmap UV
    case 21:
    {
        auto output = writeLocal(Value::InitForZero(ValueType::Float2), node);
        auto lightmapUV = String::Format(TEXT(
            "{{\n"
            "#if USE_LIGHTMAP\n"
            "\t {0} = input.LightmapUV;\n"
            "#else\n"
            "\t {0} = float2(0,0);\n"
            "#endif\n"
            "}}\n"
        ), output.Value);
        _writer.Write(*lightmapUV);
        value = output;
        break;
    }
    // Layer Driven UV Twist
    case 22:
    {
        auto uvsBox = node->GetBox(0);
        auto layerWeightBox = node->GetBox(1);
        auto strengthBox = node->GetBox(2);

        // Get inputs
        Value uvs = tryGetValue(uvsBox, getUVs).AsFloat2();
        Value layerWeight = tryGetValue(layerWeightBox, Value::Zero).AsFloat();
        Value strength = tryGetValue(strengthBox, node->Values[0]).AsFloat();

        auto result = writeLocal(Value::InitForZero(ValueType::Float2), node);

        const String uvDisplace = String::Format(TEXT(
            "   {{\n"
            "   float2 uv = {0};\n"
            "   float displacement = ({1} - 0.5) * {2};\n"
            "   uv.y += displacement;\n"  // or uv.x if you want horizontal displacement
            "   {3} = uv;\n"
            "   }}\n"
        ),
            uvs.Value,        // {0}
            layerWeight.Value, // {1}
            strength.Value,    // {2}
            result.Value      // {3}
        );

        _writer.Write(*uvDisplace);
        value = result;
        break;
    }
    // Local Space position
    case 23:
    {


        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);
        const String local_pos = String::Format(TEXT(
            "    {{\n"
            "    // Get local space position\n"

            "    float3 localPos = input.WorldPosition - GetObjectPosition(input) ;\n"

            "    float3 localScale = GetObjectScale(input);\n"
            "    localPos = TransformWorldVectorToLocal(input, localPos);\n"

            "    \n"
            "    // Apply the scale parameter in local space\n"

            "    localPos = localPos  * 0.01f ;\n"
            "    localPos /= localScale;\n"
            "    \n"
            "    // Get local normal\n"
            "    //float3 localNormal = TransformWorldVectorToLocal(input, input.TBN[2]);\n"
            "    \n"

            "    // Output the blended color\n"
            "    {0} = localPos;\n"
            "    }}\n"
        ),

            result.Value     
        );

        _writer.Write(*local_pos);
        value = result;
        break;
    }

    // Tangent To World Space
    case 24:
    {
        auto tangentNormalBox = node->GetBox(0);
        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);

        // Get input normal in tangent space
        Value tangentNormal = tryGetValue(tangentNormalBox, Value(VariantType::Float3, TEXT("float3(0,0,1)"))).AsFloat3();

        const String transformCode = ShaderBuilder()
        .Code(TEXT(R"(
            {
                // Transform geometric normal to local coordinates
                float3 localNormal = normalize(TransformWorldVectorToLocal(input, input.TBN[2]));

                // Decode tangent-space normal through standard transformation
                float2 tangentNormalXY = %TANGENT%.xy * 2.0 - 1.0;
                float3 tnormal = float3(tangentNormalXY, 
                    sqrt(1.0 - saturate(dot(tangentNormalXY, tangentNormalXY))));

                // Transform normal via TBN matrix multiplication
                float3 worldNormal = normalize(
                    mul(tnormal, float3x3(input.TBN[0], input.TBN[1], input.TBN[2])));

                // Output transformed normal
                %RESULT% = normalize(worldNormal);
            }
            )"))
        .Replace(TEXT("%TANGENT%"), tangentNormal.Value)
        .Replace(TEXT("%RESULT%"), result.Value)
        .Build();

        _writer.Write(*transformCode);
        value = result;
        break;
    }


    case 25:
    {
        auto worldNormalBox = node->GetBox(0);
        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);

        // Get input normal in world space
        Value worldNormal = tryGetValue(worldNormalBox, Value(VariantType::Float3, TEXT("float3(0,0,1)"))).AsFloat3();

        const String transformCode = ShaderBuilder()
            .Code(TEXT(R"(
        {
            // Normalize the input world-space normal
            float3 normalizedWorldNormal = normalize(%WORLD%);

            // Transform world normal into tangent space using the transposed TBN matrix
            float3x3 TBN = float3x3(input.TBN[0], input.TBN[1], input.TBN[2]);
            float3 tangentNormal = normalize(mul(normalizedWorldNormal, transpose(TBN)));

            // Decode tangent normal map format (convert to normal map encoding)
            tangentNormal.xy = tangentNormal.xy; // Map to [0, 1]

            // Reconstruct Z component to normalize
            tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)));

            // Output encoded tangent-space normal
            %RESULT% = tangentNormal;
        }
        )"))
            .Replace(TEXT("%WORLD%"), worldNormal.Value)
            .Replace(TEXT("%RESULT%"), result.Value)
            .Build();

        _writer.Write(*transformCode);
        value = result;
        break;
    }



    default:
        break;
    }
}

#endif



