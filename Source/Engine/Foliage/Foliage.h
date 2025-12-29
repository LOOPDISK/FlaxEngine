// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Config.h"
#include "FoliageInstance.h"
#include "FoliageType.h"
#include "Engine/Level/Actor.h"
#include "Engine/Core/Memory/SimpleHeapAllocation.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Graphics/DynamicBuffer.h"

class FoliageRendererAllocation : public SimpleHeapAllocation<FoliageRendererAllocation, 1024>
{
public:
    static FLAXENGINE_API void* Allocate(uintptr size);
    static FLAXENGINE_API void Free(void* ptr, uintptr size);
};


/// <summary>
/// GPU representation of a foliage instance (uploaded to structured buffer)
/// </summary>
struct GPUFoliageInstance
{
    Float4 WorldMatrixRow0;     // M11, M12, M13, M41
    Float4 WorldMatrixRow1;     // M21, M22, M23, M42
    Float4 WorldMatrixRow2;     // M31, M32, M33, M43

    Float4 PrevWorldMatrixRow0;
    Float4 PrevWorldMatrixRow1;
    Float4 PrevWorldMatrixRow2;


    Float4 LightmapUVArea;
    float CullDistance;
    float Random;
    int32 Type;
    float Padding[1];
};

/// <summary>
/// Represents a foliage actor that contains a set of instanced meshes.
/// </summary>
/// <seealso cref="Actor" />
API_CLASS() class FLAXENGINE_API Foliage final : public Actor
{
    DECLARE_SCENE_OBJECT(Foliage);
private:
    bool _disableFoliageTypeEvents;
    int32 _sceneRenderingKey = -1;



    DynamicTypedBuffer _instanceDataBuffer;

    GPUBuffer* _indirectArgsUAV = nullptr;
    GPUBuffer* _indirectArgsBuffer = nullptr;

    GPUBuffer* _typeToMeshIndexBuffer = nullptr;  // Maps FoliageType index to indirect args index

    GPUBuffer* _visibleInstancesBuffer = nullptr;

    // Compute shader for culling
    AssetReference<Shader> _cullingShader;
    GPUConstantBuffer* _cameraParamsBuffer = nullptr;

    void InitializeGPUBuffers();
    void ReleaseGPUBuffers();
public:

    void BeginPlay(SceneBeginData* data) override;
    void EndPlay() override;

    /// <summary>
    /// The allocated foliage instances. It's read-only.
    /// </summary>
    ChunkedArray<FoliageInstance, FOLIAGE_INSTANCE_CHUNKS_SIZE> Instances;

    /// <summary>
    /// The foliage instances types used by the current foliage actor. It's read-only.
    /// </summary>
    API_FIELD(ReadOnly, Attributes="HideInEditor, NoSerialize")
    Array<FoliageType> FoliageTypes;

public:
    /// <summary>
    /// Gets the total amount of the instanced of foliage.
    /// </summary>
    /// <returns>The foliage instances count.</returns>
    API_PROPERTY() int32 GetInstancesCount() const;

    /// <summary>
    /// Gets the foliage instance by index.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage instance.</param>
    /// <returns>The foliage instance data.</returns>
    API_FUNCTION() FoliageInstance GetInstance(int32 index) const;

    /// <summary>
    /// Gets the total amount of the types of foliage.
    /// </summary>
    /// <returns>The foliage types count.</returns>
    API_PROPERTY() int32 GetFoliageTypesCount() const;

    /// <summary>
    /// Gets the foliage type.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage type.</param>
    /// <returns>The foliage type.</returns>
    API_FUNCTION() FoliageType* GetFoliageType(int32 index);

    /// <summary>
    /// Adds the type of the foliage.
    /// </summary>
    /// <param name="model">The model to assign. It cannot be null nor already used by the other instance type (it must be unique within the given foliage actor).</param>
    API_FUNCTION() void AddFoliageType(Model* model);

    /// <summary>
    /// Removes the foliage instance type and all foliage instances using this type.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage instance type.</param>
    API_FUNCTION() void RemoveFoliageType(int32 index);

    /// <summary>
    /// Gets the total amount of the instanced that use the given foliage type.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage type.</param>
    /// <returns>The foliage type instances count.</returns>
    API_FUNCTION() int32 GetFoliageTypeInstancesCount(int32 index) const;

    /// <summary>
    /// Adds the new foliage instance.
    /// </summary>
    /// <remarks>Input instance bounds, instance random and world matrix are ignored (recalculated).</remarks>
    /// <param name="instance">The instance.</param>
    API_FUNCTION() void AddInstance(API_PARAM(Ref) const FoliageInstance& instance);

    /// <summary>
    /// Removes the foliage instance.
    /// </summary>
    /// <param name="index">The zero-based index of the instance to remove.</param>
    API_FUNCTION() void RemoveInstance(int32 index)
    {
        RemoveInstance(Instances.IteratorAt(index));
    }

    /// <summary>
    /// Removes the foliage instance.
    /// </summary>
    /// <param name="i">The iterator from foliage instances that points to the instance to remove.</param>
    void RemoveInstance(ChunkedArray<FoliageInstance, FOLIAGE_INSTANCE_CHUNKS_SIZE>::Iterator i);

    /// <summary>
    /// Sets the foliage instance transformation.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage instance.</param>
    /// <param name="value">The value.</param>
    API_FUNCTION() void SetInstanceTransform(int32 index, API_PARAM(Ref) const Transform& value);

    /// <summary>
    /// Called when foliage type model is loaded.
    /// </summary>
    /// <param name="index">The zero-based index of the foliage type.</param>
    void OnFoliageTypeModelLoaded(int32 index);

    /// <summary>
    /// Clears all foliage instances. Preserves the foliage types and other properties.
    /// </summary>
    API_FUNCTION() void RemoveAllInstances();

    /// <summary>
    /// Removes the lightmap data from the foliage instances.
    /// </summary>
    API_FUNCTION() void RemoveLightmap();

public:
    /// <summary>
    /// Gets the global density scale for all foliage instances. The default value is 1. Use values from range 0-1. Lower values decrease amount of foliage instances in-game. Use it to tweak game performance for slower devices.
    /// </summary>
    API_PROPERTY() static float GetGlobalDensityScale();

    /// <summary>
    /// Sets the global density scale for all foliage instances. The default value is 1. Use values from range 0-1. Lower values decrease amount of foliage instances in-game. Use it to tweak game performance for slower devices.
    /// </summary>
    API_PROPERTY() static void SetGlobalDensityScale(float value);


public:
#if FOLIAGE_USE_DRAW_CALLS_BATCHING
    struct DrawKey
    {
        IMaterial* Mat;
        const Mesh* Geo;
        int32 Lightmap;

        bool operator==(const DrawKey& other) const
        {
            return Mat == other.Mat && Geo == other.Geo && Lightmap == other.Lightmap;
        }

        friend uint32 GetHash(const DrawKey& key)
        {
            uint32 hash = (uint32)((int64)(key.Mat) >> 3);
            hash ^= (uint32)((int64)(key.Geo) >> 3) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= (uint32)key.Lightmap;
            return hash;
        }
    };

    struct FoliageBatchedDrawCall
    {
        DrawCall DrawCall;
        uint16 ObjectsStartIndex = 0; // Index of the instances start in the ObjectsBuffer (set internally).
        Array<struct ShaderObjectData, FoliageRendererAllocation> Instances;
    };

    private:

        int32 _lastInstanceCount = 0; // Track last buffer size
        uint64 _lastCullingFrame = 0; // Track which frame we last ran compute on

    typedef Array<struct FoliageBatchedDrawCall, InlinedAllocation<8>> DrawCallsList;
    typedef Dictionary<DrawKey, struct FoliageBatchedDrawCall, class FoliageRendererAllocation> BatchedDrawCalls;

    void DrawInstance(RenderContext& renderContext, FoliageInstance& instance, const FoliageType& type, Model* model, int32 lod, float lodDitherFactor, DrawCallsList* drawCallsLists, BatchedDrawCalls& result) const;
#if !FOLIAGE_USE_SINGLE_QUAD_TREE
    void DrawFoliageJob(int32 i);
    RenderContextBatch* _renderContextBatch;
#endif


#endif




    void DrawType(RenderContext& renderContext, const FoliageType& type, DrawCallsList* drawCallsLists);

public:
    /// <summary>
    /// Determines if there is an intersection between the current object or any it's child and a ray.
    /// </summary>
    /// <param name="ray">The ray to test.</param>
    /// <param name="distance">When the method completes, contains the distance of the intersection (if any valid).</param>
    /// <param name="normal">When the method completes, contains the intersection surface normal vector (if any valid).</param>
    /// <param name="instanceIndex">When the method completes, contains zero-based index of the foliage instance that is the closest to the ray.</param>
    /// <returns>True whether the two objects intersected, otherwise false.</returns>
    API_FUNCTION() bool Intersects(API_PARAM(Ref) const Ray& ray, API_PARAM(Out) Real& distance, API_PARAM(Out) Vector3& normal, API_PARAM(Out) int32& instanceIndex);

public:
    // [Actor]
    void Draw(RenderContext& renderContext) override;
    void Draw(RenderContextBatch& renderContextBatch) override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    void OnLayerChanged() override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
    void UpdateBounds();
};
