// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_MATERIAL_GRAPH

#include "MaterialGenerator.h"
#include "Engine/Visject/ShaderStringBuilder.h"

namespace
{
    enum CommonSamplerType
    {
        LinearClamp = 0,
        PointClamp = 1,
        LinearWrap = 2,
        PointWrap = 3,
        TextureGroup = 4,
    };
    const Char* SamplerNames[]
    {
        TEXT("SamplerLinearClamp"),
        TEXT("SamplerPointClamp"),
        TEXT("SamplerLinearWrap"),
        TEXT("SamplerPointWrap"),
    };

    // Hex tiling constants
    const Char* HexTileFunctions = TEXT(R"(
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

// Hex tile color sampling function
void hex2colTex(out float4 color, out float3 weights,
                Texture2D tex, SamplerState samp, float2 st,
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

    color = W.x * c1 + W.y * c2 + W.z * c3;
    weights = ProduceHexWeights(W.xyz, vertex1, vertex2, vertex3);
}

// RWS hex tile color sampling function
void hex2colTexRWS(out float4 color, out float3 weights,
                   Texture2D tex, SamplerState samp, float2 st, float2 st_offs,
                   float rotStrength, float r)
{
    float2 dSTdx = ddx(st), dSTdy = ddy(st);

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

    color = W.x * c1 + W.y * c2 + W.z * c3;
    weights = ProduceHexWeights(W.xyz, vertex1, vertex2, vertex3);
}
)");
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
    {
        const auto layer = GetRootLayer();
        if (layer && layer->Domain == MaterialDomain::Surface)
        {
            const uint32 channel = node->Values.HasItems() ? Math::Min((uint32)node->Values[0], 3u) : 0u;
            value = Value(VariantType::Float2, String::Format(TEXT("input.TexCoords[{}]"), channel));
        }
        else
        {
            // TODO: migrate all material domain templates to TexCoords array (of size MATERIAL_TEXCOORDS=1)
            value = getUVs;
        }
        break;
    }
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
            "	float vLength = length({8}.rg);\n"
            "	float coeff0 = vLength / {8}.b;\n"
            "	float coeff1 = coeff0 * (-({4}));\n"
            "	float2 vNorm = {8}.rg / vLength;\n"
            "	float2 maxOffset = (vNorm * coeff1);\n"

            "	float numSamples = lerp({0}, {3}, saturate(dot({9}, input.TBN[2])));\n"
            "	float stepSize = 1.0 / numSamples;\n"

            "	float2 currOffset = 0;\n"
            "	float2 lastOffset = 0;\n"
            "	float currRayHeight = 1.0;\n"
            "	float lastSampledHeight = 1;\n"
            "	int currSample = 0;\n"

            "	while (currSample < (int)numSamples)\n"
            "	{{\n"
            "		float currSampledHeight = {1}.SampleGrad(SamplerLinearWrap, {10} + currOffset, {5}, {6}){7};\n"

            "		if (currSampledHeight > currRayHeight)\n"
            "		{{\n"
            "			float delta1 = currSampledHeight - currRayHeight;\n"
            "			float delta2 = (currRayHeight + stepSize) - lastSampledHeight;\n"
            "			float ratio = delta1 / max(delta1 + delta2, 0.00001f);\n"
            "			currOffset = ratio * lastOffset + (1.0 - ratio) * currOffset;\n"
            "			break;\n"
            "		}}\n"

            "		currRayHeight -= stepSize;\n"
            "		lastOffset = currOffset;\n"
            "		currOffset += stepSize * maxOffset;\n"
            "		lastSampledHeight = currSampledHeight;\n"
            "		currSample++;\n"
            "	}}\n"

            "	{2} = {10} + currOffset;\n"
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
            value = writeLocal(VariantType::Int, String::Format(TEXT("(int)({0}.a * 3.999)"), gBuffer1Sample->Value), node);
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
            // Sample Texture - check for hex tile mode
            const bool hexTileEnabled = node->Values.Count() >= 4 ? node->Values[3].AsBool : false;
            const auto rotationStrength = tryGetValue(node->TryGetBox(4), node->Values.Count() >= 5 ? node->Values[4] : 1.0f);
            const auto contrast = tryGetValue(node->TryGetBox(5), node->Values.Count() >= 6 ? node->Values[5] : 0.5f);
            const bool largeWorldStability = node->Values.Count() >= 7 ? node->Values[6].AsBool : false;

            if (hexTileEnabled)
            {
                // Mark that hex tile functions are needed for this material
                _needsHexTileFunctions = true;

                // Use hex tile sampling
                const String hexTileFunction = largeWorldStability ? TEXT("hex2colTexRWS") : TEXT("hex2colTex");
                String sampledValue;

                if (useOffset)
                {
                    sampledValue = String::Format(TEXT("{0}({1}, {2}, {3} + {4}, {5}, {6})"),
                        hexTileFunction, texture.Value, samplerName, uvs.Value, offset.Value,
                        rotationStrength.Value, contrast.Value);
                }
                else
                {
                    sampledValue = String::Format(TEXT("{0}({1}, {2}, {3}, {4}, {5})"),
                        hexTileFunction, texture.Value, samplerName, uvs.Value,
                        rotationStrength.Value, contrast.Value);
                }

                textureBox->Cache = writeLocal(VariantType::Float4, sampledValue, node);
            }
            else
            {
                // Standard texture sampling
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
            _writer.Write(TEXT("\t{0}.xyz = UnpackNormalMap({0}.xy);\n"), textureBox->Cache.Value);
        }

        value = textureBox->Cache;
        break;
    }
    // Flipbook
    case 10:
    {
        auto uv = Value::Cast(tryGetValue(node->GetBox(0), getUVs), VariantType::Float2);
        auto frame = Value::Cast(tryGetValue(node->GetBox(1), node->Values[0]), VariantType::Float);
        auto framesXY = Value::Cast(tryGetValue(node->GetBox(2), node->Values[1]), VariantType::Float2);
        auto invertX = Value::Cast(tryGetValue(node->GetBox(3), node->Values[2]), VariantType::Float);
        auto invertY = Value::Cast(tryGetValue(node->GetBox(4), node->Values[3]), VariantType::Float);
        value = writeLocal(VariantType::Float2, String::Format(TEXT("Flipbook({0}, {1}, {2}, float2({3}, {4}))"), uv.Value, frame.Value, framesXY.Value, invertX.Value, invertY.Value), node);
        break;
    }
    // Sample Global SDF
    case 14:
    {
        auto param = findOrAddGlobalSDF();
        Value worldPosition = tryGetValue(node->GetBox(1), Value(VariantType::Float3, TEXT("input.WorldPosition.xyz"))).Cast(VariantType::Float3);
        Value startCascade = tryGetValue(node->TryGetBox(2), 0, Value::Zero).Cast(VariantType::Uint);
        value = writeLocal(VariantType::Float, String::Format(TEXT("SampleGlobalSDF({0}, {0}_Tex, {0}_Mip, {1}, {2})"), param.ShaderName, worldPosition.Value, startCascade.Value), node);
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
        Value startCascade = tryGetValue(node->TryGetBox(3), 0, Value::Zero).Cast(VariantType::Uint);
        auto distance = writeLocal(VariantType::Float, node);
        auto gradient = writeLocal(VariantType::Float3, String::Format(TEXT("SampleGlobalSDFGradient({0}, {0}_Tex, {0}_Mip, {1}, {2}, {3})"), param.ShaderName, worldPosition.Value, distance.Value, startCascade.Value), node);
        _includes.Add(TEXT("./Flax/GlobalSignDistanceField.hlsl"));
        gradientBox->Cache = gradient;
        distanceBox->Cache = distance;
        value = box == gradientBox ? gradient : distance;
        break;
    }
    // Triplanar Texture
    case 16:
    {
        auto textureBox = node->GetBox(0);
        if (!textureBox->HasConnection())
        {
            // No texture to sample
            value = Value::Zero;
            break;
        }
        const bool canUseSample = CanUseSample(_treeType);
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());
        const auto scale = tryGetValue(node->GetBox(1), node->Values[0]).AsFloat3();
        const auto blend = tryGetValue(node->GetBox(2), node->Values[1]).AsFloat();
        const auto offset = tryGetValue(node->TryGetBox(6), node->Values.Count() >= 3 ? node->Values[2] : Float2::Zero).AsFloat2();
        const bool local = node->Values.Count() >= 5 ? node->Values[4].AsBool : false;
        const bool hexTileEnabled = node->Values.Count() >= 6 ? node->Values[5].AsBool : false;
        const auto rotationStrength = tryGetValue(node->TryGetBox(7), node->Values.Count() >= 7 ? node->Values[6] : 1.0f).AsFloat();
        const auto contrast = tryGetValue(node->TryGetBox(8), node->Values.Count() >= 8 ? node->Values[7] : 0.5f).AsFloat();
        const bool largeWorldStability = node->Values.Count() >= 9 ? node->Values[8].AsBool : false;

        const Char* samplerName;
        const int32 samplerIndex = node->Values.Count() >= 4 ? node->Values[3].AsInt : LinearWrap;
        if (samplerIndex == TextureGroup)
        {
            auto& textureGroupSampler = findOrAddTextureGroupSampler(node->Values[3].AsInt);
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

        auto result = writeLocal(Value::InitForZero(ValueType::Float4), node);

        // Mark that hex tile functions are needed for this material
        if (hexTileEnabled)
        {
            _needsHexTileFunctions = true;
        }

        String triplanarTexture;
        if (hexTileEnabled)
        {
            // Hex tile triplanar implementation
            triplanarTexture = ShaderStringBuilder()
                .Code(TEXT(R"(
        {
            // Get world position and normal
            float3 tiling = %SCALE% * 0.001f;
            float3 position = ((%POSITION%) + GetLargeWorldsTileOffset(1.0f / length(tiling))) * tiling;
            float3 normal = normalize(%NORMAL%);

            // Compute triplanar blend weights using power distribution
            float3 blendWeights = pow(abs(normal), %BLEND%);
            blendWeights /= dot(blendWeights, float3(1, 1, 1));

            // Sample each projection plane using hex tiling
            float4 xProjection, yProjection, zProjection;
            
            // X plane projection (YZ coordinates)
            xProjection = %HEX_FUNCTION%(%TEXTURE%, %SAMPLER%, position.yz + %OFFSET%%EXTRA_PARAMS%, %ROT_STRENGTH%, %CONTRAST%);
            
            // Y plane projection (XZ coordinates)  
            yProjection = %HEX_FUNCTION%(%TEXTURE%, %SAMPLER%, position.xz + %OFFSET%%EXTRA_PARAMS%, %ROT_STRENGTH%, %CONTRAST%);
            
            // Z plane projection (XY coordinates)
            zProjection = %HEX_FUNCTION%(%TEXTURE%, %SAMPLER%, position.xy + %OFFSET%%EXTRA_PARAMS%, %ROT_STRENGTH%, %CONTRAST%);

            // Blend projections using computed weights
            %RESULT% = xProjection * blendWeights.x + yProjection * blendWeights.y + zProjection * blendWeights.z;
        }
)"))
.Replace(TEXT("%TEXTURE%"), texture.Value)
.Replace(TEXT("%SCALE%"), scale.Value)
.Replace(TEXT("%BLEND%"), blend.Value)
.Replace(TEXT("%OFFSET%"), offset.Value)
.Replace(TEXT("%RESULT%"), result.Value)
.Replace(TEXT("%POSITION%"), local ? TEXT("TransformWorldVectorToLocal(input, input.WorldPosition - GetObjectPosition(input)) / GetObjectScale(input)") : TEXT("input.WorldPosition"))
.Replace(TEXT("%NORMAL%"), local ? TEXT("TransformWorldVectorToLocal(input, input.TBN[2])") : TEXT("input.TBN[2]"))
.Replace(TEXT("%SAMPLER%"), samplerName)
.Replace(TEXT("%ROT_STRENGTH%"), rotationStrength.Value)
.Replace(TEXT("%CONTRAST%"), contrast.Value)
.Replace(TEXT("%HEX_FUNCTION%"), largeWorldStability ? TEXT("hex2colTexRWS") : TEXT("hex2colTex"))
.Replace(TEXT("%EXTRA_PARAMS%"), TEXT(""))
.Build();
        }
        else
        {
            // Standard triplanar implementation (unchanged)
            triplanarTexture = ShaderStringBuilder()
                .Code(TEXT(R"(
        {
            // Get world position and normal
            float3 tiling = %SCALE% * 0.001f;
            float3 position = ((%POSITION%) + GetLargeWorldsTileOffset(1.0f / length(tiling))) * tiling;
            float3 normal = normalize(%NORMAL%);

            // Compute triplanar blend weights using power distribution
            float3 blendWeights = pow(abs(normal), %BLEND%);
            blendWeights /= dot(blendWeights, float3(1, 1, 1));

            // Sample projections with proper scaling and offset
            float4 xProjection = %TEXTURE%.%SAMPLE%(%SAMPLER%, position.yz + %OFFSET%%SAMPLE_ARGS%);
            float4 yProjection = %TEXTURE%.%SAMPLE%(%SAMPLER%, position.xz + %OFFSET%%SAMPLE_ARGS%);
            float4 zProjection = %TEXTURE%.%SAMPLE%(%SAMPLER%, position.xy + %OFFSET%%SAMPLE_ARGS%);

            // Blend projections using computed weights
            %RESULT% = xProjection * blendWeights.x + yProjection * blendWeights.y + zProjection * blendWeights.z;
        }
)"))
.Replace(TEXT("%TEXTURE%"), texture.Value)
.Replace(TEXT("%SCALE%"), scale.Value)
.Replace(TEXT("%BLEND%"), blend.Value)
.Replace(TEXT("%OFFSET%"), offset.Value)
.Replace(TEXT("%RESULT%"), result.Value)
.Replace(TEXT("%POSITION%"), local ? TEXT("TransformWorldVectorToLocal(input, input.WorldPosition - GetObjectPosition(input)) / GetObjectScale(input)") : TEXT("input.WorldPosition"))
.Replace(TEXT("%NORMAL%"), local ? TEXT("TransformWorldVectorToLocal(input, input.TBN[2])") : TEXT("input.TBN[2]"))
.Replace(TEXT("%SAMPLER%"), samplerName)
.Replace(TEXT("%SAMPLE%"), canUseSample ? TEXT("Sample") : TEXT("SampleLevel"))
.Replace(TEXT("%SAMPLE_ARGS%"), canUseSample ? TEXT("") : TEXT(", 0")) // Sample mip0 when cannot get auto ddx/ddy in Vertex Shader
.Build();
        }

        _writer.Write(*triplanarTexture);
        value = result;
        break;
    }
    // Get Lightmap UV
    case 18:
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
    // Triplanar Normal Map
    case 24:
    {
        auto textureBox = node->GetBox(0);
        if (!textureBox->HasConnection())
        {
            // No texture to sample
            value = Value::Zero;
            break;
        }
        const bool canUseSample = CanUseSample(_treeType);
        const auto texture = eatBox(textureBox->GetParent<Node>(), textureBox->FirstConnection());
        const auto scale = tryGetValue(node->GetBox(1), node->Values[0]).AsFloat3();
        const auto blend = tryGetValue(node->GetBox(2), node->Values[1]).AsFloat();
        const auto offset = tryGetValue(node->GetBox(6), node->Values[2]).AsFloat2();
        const bool local = node->Values.Count() >= 5 ? node->Values[4].AsBool : false;
        const bool hexTileEnabled = node->Values.Count() >= 6 ? node->Values[5].AsBool : false;
        const auto rotationStrength = tryGetValue(node->TryGetBox(7), node->Values.Count() >= 7 ? node->Values[6] : 1.0f).AsFloat();
        const auto contrast = tryGetValue(node->TryGetBox(8), node->Values.Count() >= 8 ? node->Values[7] : 0.5f).AsFloat();
        const bool largeWorldStability = node->Values.Count() >= 9 ? node->Values[8].AsBool : false;

        // Mark that hex tile functions are needed for this material
        if (hexTileEnabled)
        {
            _needsHexTileFunctions = true;
        }

        const Char* samplerName;
        const int32 samplerIndex = node->Values[3].AsInt;
        if (samplerIndex == TextureGroup)
        {
            auto& textureGroupSampler = findOrAddTextureGroupSampler(node->Values[3].AsInt);
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

        auto result = writeLocal(Value::InitForZero(ValueType::Float3), node);

        String triplanarNormalMap;
        if (hexTileEnabled)
        {
            // Use hex tile triplanar normal map
            const String hexTileFunction = largeWorldStability ? TEXT("hex2normalTexRWS") : TEXT("hex2normalTex");
            triplanarNormalMap = ShaderStringBuilder()
                .Code(TEXT(R"(
            {
                // Get world position and normal
                float3 tiling = %SCALE% * 0.001f;
                float3 position = ((%POSITION%) + GetLargeWorldsTileOffset(1.0f / length(tiling))) * tiling;
                float3 normal = normalize(%NORMAL%);

                // Compute triplanar blend weights using power distribution
                float3 blendWeights = pow(abs(normal), %BLEND%);
                blendWeights /= dot(blendWeights, float3(1, 1, 1));

                // Sample hex tile normal maps for each projection
                float3 tnormalX = %HEXTILE_FUNC%(%TEXTURE%, %SAMPLER%, position.yz + %OFFSET%, %ROTATION_STRENGTH%, %CONTRAST%);
                float3 tnormalY = %HEXTILE_FUNC%(%TEXTURE%, %SAMPLER%, position.xz + %OFFSET%, %ROTATION_STRENGTH%, %CONTRAST%);
                float3 tnormalZ = %HEXTILE_FUNC%(%TEXTURE%, %SAMPLER%, position.xy + %OFFSET%, %ROTATION_STRENGTH%, %CONTRAST%);

                // Apply proper whiteout blend
                normal = normalize(input.TBN[2]);
                float3 axisSign = sign(normal);
                float2 sumX = tnormalX.xy + normal.zy;
                float2 sumY = tnormalY.xy + normal.xz;
                float2 sumZ = tnormalZ.xy + normal.xy;
                tnormalX = float3(sumX, sqrt(1.0 - saturate(dot(sumX, sumX))) * axisSign.x);
                tnormalY = float3(sumY, sqrt(1.0 - saturate(dot(sumY, sumY))) * axisSign.y);
                tnormalZ = float3(sumZ, sqrt(1.0 - saturate(dot(sumZ, sumZ))) * axisSign.z);

                // Blend the normal maps using the blend weights
                float3 blendedNormal = normalize(
                    tnormalX.zyx * blendWeights.x +
                    tnormalY.xzy * blendWeights.y +
                    tnormalZ.xyz * blendWeights.z
                );

                // Transform to tangent space
                %RESULT% = normalize(TransformWorldVectorToTangent(input, blendedNormal));
            }
)"))
.Replace(TEXT("%TEXTURE%"), texture.Value)
.Replace(TEXT("%SCALE%"), scale.Value)
.Replace(TEXT("%BLEND%"), blend.Value)
.Replace(TEXT("%OFFSET%"), offset.Value)
.Replace(TEXT("%ROTATION_STRENGTH%"), rotationStrength.Value)
.Replace(TEXT("%CONTRAST%"), contrast.Value)
.Replace(TEXT("%RESULT%"), result.Value)
.Replace(TEXT("%POSITION%"), local ? TEXT("TransformWorldVectorToLocal(input, input.WorldPosition - GetObjectPosition(input)) / GetObjectScale(input)") : TEXT("input.WorldPosition"))
.Replace(TEXT("%NORMAL%"), local ? TEXT("TransformWorldVectorToLocal(input, input.TBN[2])") : TEXT("input.TBN[2]"))
.Replace(TEXT("%SAMPLER%"), samplerName)
.Replace(TEXT("%HEXTILE_FUNC%"), hexTileFunction)
.Build();
        }
        else
        {
            // Standard triplanar normal map
            triplanarNormalMap = ShaderStringBuilder()
                .Code(TEXT(R"(
            {
                // Get world position and normal
                float3 tiling = %SCALE% * 0.001f;
                float3 position = ((%POSITION%) + GetLargeWorldsTileOffset(1.0f / length(tiling))) * tiling;
                float3 normal = normalize(%NORMAL%);

                // Compute triplanar blend weights using power distribution
                float3 blendWeights = pow(abs(normal), %BLEND%);
                blendWeights /= dot(blendWeights, float3(1, 1, 1));

                // Unpack normal maps
                float3 tnormalX = UnpackNormalMap(%TEXTURE%.%SAMPLE%(%SAMPLER%, position.yz + %OFFSET%%SAMPLE_ARGS%).rg);
                float3 tnormalY = UnpackNormalMap(%TEXTURE%.%SAMPLE%(%SAMPLER%, position.xz + %OFFSET%%SAMPLE_ARGS%).rg);
                float3 tnormalZ = UnpackNormalMap(%TEXTURE%.%SAMPLE%(%SAMPLER%, position.xy + %OFFSET%%SAMPLE_ARGS%).rg);

                // Apply proper whiteout blend
                normal = normalize(input.TBN[2]);
                float3 axisSign = sign(normal);
                float2 sumX = tnormalX.xy + normal.zy;
                float2 sumY = tnormalY.xy + normal.xz;
                float2 sumZ = tnormalZ.xy + normal.xy;
                tnormalX = float3(sumX, sqrt(1.0 - saturate(dot(sumX, sumX))) * axisSign.x);
                tnormalY = float3(sumY, sqrt(1.0 - saturate(dot(sumY, sumY))) * axisSign.y);
                tnormalZ = float3(sumZ, sqrt(1.0 - saturate(dot(sumZ, sumZ))) * axisSign.z);

                // Blend the normal maps using the blend weights
                float3 blendedNormal = normalize(
                    tnormalX.zyx * blendWeights.x +
                    tnormalY.xzy * blendWeights.y +
                    tnormalZ.xyz * blendWeights.z
                );

                // Transform to tangent space
                %RESULT% = normalize(TransformWorldVectorToTangent(input, blendedNormal));
            }
)"))
.Replace(TEXT("%TEXTURE%"), texture.Value)
.Replace(TEXT("%SCALE%"), scale.Value)
.Replace(TEXT("%BLEND%"), blend.Value)
.Replace(TEXT("%OFFSET%"), offset.Value)
.Replace(TEXT("%RESULT%"), result.Value)
.Replace(TEXT("%POSITION%"), local ? TEXT("TransformWorldVectorToLocal(input, input.WorldPosition - GetObjectPosition(input)) / GetObjectScale(input)") : TEXT("input.WorldPosition"))
.Replace(TEXT("%NORMAL%"), local ? TEXT("TransformWorldVectorToLocal(input, input.TBN[2])") : TEXT("input.TBN[2]"))
.Replace(TEXT("%SAMPLER%"), samplerName)
.Replace(TEXT("%SAMPLE%"), canUseSample ? TEXT("Sample") : TEXT("SampleLevel"))
.Replace(TEXT("%SAMPLE_ARGS%"), canUseSample ? TEXT("") : TEXT(", 0")) // Sample mip0 when cannot get auto ddx/ddy in Vertex Shader
.Build();
        }

        _writer.Write(*triplanarNormalMap);
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
    default:
        break;
    }
}

#endif
