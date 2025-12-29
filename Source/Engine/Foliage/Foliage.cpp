// Copyright (c) Wojciech Figat. All rights reserved.

#include "Foliage.h"
#include "FoliageType.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Content/Deprecated.h"
#if !FOLIAGE_USE_SINGLE_QUAD_TREE
#include "Engine/Threading/JobSystem.h"
#if FOLIAGE_USE_DRAW_CALLS_BATCHING
#include "Engine/Graphics/RenderTools.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Renderer/RenderList.h"
#endif
#endif
#include "Engine/Level/SceneQuery.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Renderer/GlobalSignDistanceFieldPass.h"
#include "Engine/Renderer/GI/GlobalSurfaceAtlasPass.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Utilities/Encryption.h"
#include "Engine/Graphics/Shaders/GPUShader.h"

#define FOLIAGE_GET_DRAW_MODES(renderContext, type) (type.DrawModes & renderContext.View.Pass & renderContext.View.GetShadowsDrawPassMask(type.ShadowsMode))
#define FOLIAGE_CAN_DRAW(renderContext, type) (type.IsReady() && FOLIAGE_GET_DRAW_MODES(renderContext, type) != DrawPass::None && type.Model->CanBeRendered())

namespace
{
    static thread_local Array<Pair<void*, uintptr>> MemPool;
    static thread_local CriticalSection MemPoolLocker;
}

void* FoliageRendererAllocation::Allocate(uintptr size)
{
    void* result = nullptr;
    MemPoolLocker.Lock();
    for (int32 i = 0; i < MemPool.Count(); i++)
    {
        if (MemPool.Get()[i].Second == size)
        {
            result = MemPool.Get()[i].First;
            MemPool.RemoveAt(i);
            break;
        }
    }
    MemPoolLocker.Unlock();
    if (!result)
        result = Platform::Allocate(size, 16);
    return result;
}

void FoliageRendererAllocation::Free(void* ptr, uintptr size)
{
    MemPoolLocker.Lock();
    MemPool.Add({ ptr, size });
    MemPoolLocker.Unlock();
}

Foliage::Foliage(const SpawnParams& params)
    : Actor(params)
    , _disableFoliageTypeEvents(false)
    , _instanceDataBuffer(0, PixelFormat::R32G32B32A32_Float, false, TEXT("Foliage Instance Data"))
{
    _disableFoliageTypeEvents = false;
    _drawCategory = SceneRendering::SceneDraw;

}

int32 Foliage::GetInstancesCount() const
{
    return Instances.Count();
}

FoliageInstance Foliage::GetInstance(int32 index) const
{
    return Instances[index];
}

int32 Foliage::GetFoliageTypesCount() const
{
    return FoliageTypes.Count();
}

FoliageType* Foliage::GetFoliageType(int32 index)
{
    CHECK_RETURN(index >= 0 && index < FoliageTypes.Count(), nullptr)
    return &FoliageTypes[index];
}

void Foliage::AddFoliageType(Model* model)
{
    PROFILE_CPU();

    // Ensure to have unique model
    CHECK(model);
    for (int32 i = 0; i < FoliageTypes.Count(); i++)
    {
        if (FoliageTypes[i].Model == model)
        {
            LOG(Error, "The given model is already used by other foliage type.");
            return;
        }
    }

    // Add
    _disableFoliageTypeEvents = true;
    auto& item = FoliageTypes.AddOne();
    _disableFoliageTypeEvents = false;

    // Setup
    item.Foliage = this;
    item.Index = FoliageTypes.Count() - 1;
    item.Model = model;
}

void Foliage::RemoveFoliageType(int32 index)
{
    PROFILE_CPU();

    // Remove instances using this foliage type
    if (FoliageTypes.Count() != 1)
    {
        for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
        {
            if (i->Type == index)
            {
                Instances.Remove(i);
                --i;
            }
        }

        // Update all instances using foliage types with higher index to point into a valid type
        for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
        {
            if (i->Type > index)
                i->Type--;
        }
    }
    else
    {
        Instances.Clear();
    }

    // Remove foliage instance type
    for (int32 i = index + 1; i < FoliageTypes.Count(); i++)
    {
        FoliageTypes[i].Index--;
    }
    auto& item = FoliageTypes[index];
    item.Model = nullptr;
    item.Entries.Release();
    FoliageTypes.RemoveAtKeepOrder(index);
}

int32 Foliage::GetFoliageTypeInstancesCount(int32 index) const
{
    PROFILE_CPU();
    int32 result = 0;
    for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Type == index)
            result++;
    }
    return result;
}

void Foliage::AddInstance(const FoliageInstance& instance)
{
    ASSERT(instance.Type >= 0 && instance.Type < FoliageTypes.Count());
    auto type = &FoliageTypes[instance.Type];

    // Add instance
    auto data = Instances.Add(instance);
    data->Bounds = BoundingSphere::Empty;
    data->Random = Random::Rand();
    data->CullDistance = type->CullDistance + type->CullDistanceRandomRange * data->Random;

    // Validate foliage type model
    if (!type->IsReady())
        return;

    // Update bounds
    Vector3 corners[8];
    auto& meshes = type->Model->LODs[0].Meshes;
    const Transform transform = _transform.LocalToWorld(data->Transform);
    for (int32 j = 0; j < meshes.Count(); j++)
    {
        meshes[j].GetBox().GetCorners(corners);

        for (int32 k = 0; k < 8; k++)
        {
            Vector3::Transform(corners[k], transform, corners[k]);
        }
        BoundingSphere meshBounds;
        BoundingSphere::FromPoints(corners, 8, meshBounds);
        ASSERT(meshBounds.Radius > ZeroTolerance);

        BoundingSphere::Merge(data->Bounds, meshBounds, data->Bounds);
    }
    data->Bounds.Radius += ZeroTolerance;
}

void Foliage::RemoveInstance(ChunkedArray<FoliageInstance, FOLIAGE_INSTANCE_CHUNKS_SIZE>::Iterator i)
{
    Instances.Remove(i);
}

void Foliage::SetInstanceTransform(int32 index, const Transform& value)
{
    auto& instance = Instances[index];
    auto type = &FoliageTypes[instance.Type];

    // Change transform
    instance.Transform = value;

    // Update bounds
    instance.Bounds = BoundingSphere::Empty;
    if (!type->IsReady())
        return;
    Vector3 corners[8];
    auto& meshes = type->Model->LODs[0].Meshes;
    const Transform transform = _transform.LocalToWorld(instance.Transform);
    for (int32 j = 0; j < meshes.Count(); j++)
    {
        meshes[j].GetBox().GetCorners(corners);

        for (int32 k = 0; k < 8; k++)
        {
            Vector3::Transform(corners[k], transform, corners[k]);
        }
        BoundingSphere meshBounds;
        BoundingSphere::FromPoints(corners, 8, meshBounds);
        ASSERT(meshBounds.Radius > ZeroTolerance);

        BoundingSphere::Merge(instance.Bounds, meshBounds, instance.Bounds);
    }
    instance.Bounds.Radius += ZeroTolerance;
}

void Foliage::OnFoliageTypeModelLoaded(int32 index)
{
    if (_disableFoliageTypeEvents)
        return;
    auto& type = FoliageTypes[index];
    ASSERT(type.IsReady());

    // Update bounds for instances using this type
    PROFILE_CPU_NAMED("Update Bounds");
    Vector3 corners[8];
    auto& meshes = type.Model->LODs[0].Meshes;
    for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
    {
        auto& instance = *i;
        if (instance.Type != index)
            continue;

        instance.Bounds = BoundingSphere::Empty;
        const Transform transform = _transform.LocalToWorld(instance.Transform);

        // Include all meshes
        for (int32 j = 0; j < meshes.Count(); j++)
        {
            meshes[j].GetBox().GetCorners(corners);
            for (int32 k = 0; k < 8; k++)
            {
                Vector3::Transform(corners[k], transform, corners[k]);
            }
            BoundingSphere meshBounds;
            BoundingSphere::FromPoints(corners, 8, meshBounds);
            BoundingSphere::Merge(instance.Bounds, meshBounds, instance.Bounds);
        }
    }

    // Update foliage actor bounds (for coarse culling)
    UpdateBounds();
}

void Foliage::RemoveAllInstances()
{
    Instances.Clear();
}

void Foliage::RemoveLightmap()
{
    for (auto& e : Instances)
        e.RemoveLightmap();
}

static float GlobalDensityScale = 1.0f;

float Foliage::GetGlobalDensityScale()
{
    return GlobalDensityScale;
}

void Foliage::SetGlobalDensityScale(float value)
{
    value = Math::Saturate(value);
    if (Math::NearEqual(value, GlobalDensityScale))
        return;

    PROFILE_CPU();

    GlobalDensityScale = value;

    //TODO: GPU-driven foliage reads this value in compute shader
}

bool Foliage::Intersects(const Ray& ray, Real& distance, Vector3& normal, int32& instanceIndex)
{
    // TODO: Implement linear instance iteration for raycasting
    // (not critical for GPU rendering, needed for editor picking)
    instanceIndex = -1;
    normal = Vector3::Up;
    distance = MAX_Real;
    return false;
}

void Foliage::Draw(RenderContext& renderContext)
{

    static int32 lastReportedCount = -1;
    if (Instances.Count() != lastReportedCount)
    {
        LOG(Warning, "Foliage::Draw - Instance count changed: {0} -> {1}",
            lastReportedCount, Instances.Count());
        lastReportedCount = Instances.Count();
    }


    LOG(Info, "Foliage::Draw called - Instance count: {0}", Instances.Count());


    // Reinitialize buffers if instance count changed or buffers don't exist
    if (!_instanceDataBuffer.GetBuffer() || Instances.Count() != _lastInstanceCount)
    {
        if (!Instances.IsEmpty())
        {
            InitializeGPUBuffers();
            _lastInstanceCount = Instances.Count();
        }
    }

    //LOG(Info, "  _cullingShader is {0}", _cullingShader != nullptr);

    //LOG(Info, "  _instanceDataBuffer is {0}", _instanceDataBuffer != nullptr);

    if (Instances.IsEmpty() || !_cullingShader || !_instanceDataBuffer.GetBuffer())
    {
        LOG(Warning, "  Early return - missing resources!");
        return;
    }

    PROFILE_CPU();

    // Get compute shader program
    auto gpuShader = _cullingShader->GetShader();
    if (!gpuShader) {

        LOG(Info, "Foliage::Draw could not find gpuShader");
        return;
    }
    auto cs = gpuShader->GetCS("CS_CullInstances");
    if (!cs) {

        LOG(Info, "Foliage::Draw could not find CS_CullInstances");
        return;
    }

    auto context = GPUDevice::Instance->GetMainContext();
    const auto& view = renderContext.View;

    // Only run GPU culling once per frame
    const uint64 currentFrame = Engine::FrameCount;
    const bool needsCulling = (_lastCullingFrame != currentFrame);


    if (needsCulling)
    {
        // Calculate total draw calls
        int32 totalDrawCalls = 0;
        for (const auto& type : FoliageTypes)
        {
            if (type.Model && type.Model->IsLoaded())
                totalDrawCalls += type.Model->LODs[0].Meshes.Count();
        }
        // Reset indirect args (clear instance counts to zero, preserve mesh data)
        Array<GPUDrawIndexedIndirectArgs> resetArgs;
        resetArgs.Resize(totalDrawCalls);
        int32 argIndex = 0;

        for (int32 typeIndex = 0; typeIndex < FoliageTypes.Count(); typeIndex++)
        {
            const auto& type = FoliageTypes[typeIndex];
            if (!type.Model || !type.Model->IsLoaded())
                continue;

            const auto& lod = type.Model->LODs[0];
            for (int32 meshIndex = 0; meshIndex < lod.Meshes.Count(); meshIndex++)
            {
                const auto& mesh = lod.Meshes[meshIndex];
                auto& args = resetArgs[argIndex++];
                args.IndicesCount = mesh.GetTriangleCount() * 3;
                args.InstanceCount = 0;  // Reset to zero
                args.StartIndex = 0;
                args.StartVertex = 0;
                args.StartInstance = 0;
            }
        }
        // Prepare camera parameters for compute shader
        struct CameraParams
        {
            Float4 FrustumPlanes[6];
            Float3 CameraPosition;
            float BoundingRadius;
            uint32 InstanceCount;
            uint32 Padding[3];
        } cameraParams;

        // Extract frustum planes from view
        for (int32 i = 0; i < 6; i++)
        {
            const Plane& plane = view.Frustum.GetPlane(i);
            cameraParams.FrustumPlanes[i] = Float4(plane.Normal, plane.D);
        }

        // Calculate bounding radius from first foliage type's model
        float maxRadius = 10.0f;  // Default fallback
        for (const auto& type : FoliageTypes)
        {
            if (type.Model && type.Model->IsLoaded())
            {
                const auto& box = type.Model->GetBox();
                float radius = box.GetSize().Length() * 0.5f;
                maxRadius = Math::Max(maxRadius, radius);
            }
        }
        cameraParams.BoundingRadius = maxRadius;

        LOG(Info, "Using BoundingRadius: {0}, currentFrame: {1}", maxRadius, currentFrame);
        cameraParams.CameraPosition = view.Position;
        cameraParams.InstanceCount = Instances.Count();

        // Upload camera parameters
        context->UpdateCB(_cameraParamsBuffer, &cameraParams);

        context->UpdateBuffer(_indirectArgsUAV, resetArgs.Get(),
            resetArgs.Count() * sizeof(GPUDrawIndexedIndirectArgs));

        // Bind resources for compute shader
        context->BindCB(0, _cameraParamsBuffer);
        context->BindSR(0, _instanceDataBuffer.GetBuffer()->View());
        context->BindSR(1, _typeToMeshIndexBuffer->View());
        context->BindUA(0, _indirectArgsUAV->View());
        context->BindUA(1, _visibleInstancesBuffer->View());

        // Dispatch compute shader (64 threads per group)
        const uint32 threadGroups = (Instances.Count() + 63) / 64;
        context->Dispatch(cs, threadGroups, 1, 1);

        // Unbind UAVs before using as indirect args
        context->ResetUA();
        context->ResetSR();
        context->ResetCB();
        context->FlushState();
        _lastCullingFrame = currentFrame;
        // Copy UAV → Argument buffer
        context->CopyResource(_indirectArgsBuffer, _indirectArgsUAV);
    }


    LOG(Info, "GPU Culling: {0} total instances submitted", Instances.Count());

    // Submit DrawCalls to render list (will execute later with full pipeline state)
    int32 drawCallIndex = 0;
    const StaticFlags flags = StaticFlags::None; // Not lightmapped, dynamic
    const bool receivesDecals = false;
    const int8 sortOrder = 0;

    for (int32 typeIndex = 0; typeIndex < FoliageTypes.Count(); typeIndex++)
    {
        const auto& type = FoliageTypes[typeIndex];
        if (!type.Model || !type.Model->IsLoaded())
            continue;

        const auto& lod = type.Model->LODs[0];
        for (int32 meshIndex = 0; meshIndex < lod.Meshes.Count(); meshIndex++)
        {
            const auto& mesh = lod.Meshes[meshIndex];
            if (!mesh.IsInitialized())
            {
                drawCallIndex++;
                continue;
            }

            MaterialBase* material = type.Model->MaterialSlots[mesh.GetMaterialSlotIndex()].Material.Get();
            if (!material || !material->IsLoaded())
                material = GPUDevice::Instance->GetDefaultMaterial();

            if (!material || !material->IsReady() || !material->IsSurfaceLike())
            {
                //LOG(Warning, "Material not ready - IsLoaded: {0}, IsReady: {1}, Domain: {2}",
                //    material ? material->IsLoaded() : false,
                //    material ? material->IsReady() : false,
                //    material ? (int32)material->GetInfo().Domain : -1);
                drawCallIndex++;
                continue;
            }

            DrawPass drawModes = material->GetDrawModes();
            if (drawModes == DrawPass::None)
            {
                drawCallIndex++;
                continue;
            }

            // Force foliage to disable the drawing of motion vectors
            // This is to circumvent the need to address an instanced variant in DeferredMaterialShader::Load()
            drawModes &= ~DrawPass::MotionVectors;



            // Build DrawCall
            DrawCall drawCall;
            Platform::MemoryClear(&drawCall, sizeof(DrawCall));
            drawCall.Material = material;
            drawCall.Geometry.IndexBuffer = mesh.GetIndexBuffer();
            drawCall.Geometry.VertexBuffers[0] = mesh.GetVertexBuffer(0);
            drawCall.Geometry.VertexBuffers[1] = mesh.GetVertexBuffer(1);
            drawCall.Geometry.VertexBuffers[2] = mesh.GetVertexBuffer(2);
            drawCall.Geometry.VertexBuffersOffsets[0] = 0;
            drawCall.Geometry.VertexBuffersOffsets[1] = 0;
            drawCall.Geometry.VertexBuffersOffsets[2] = 0;

            // Indirect draw setup
            drawCall.InstanceCount = 0;  // Signal: use indirect!
            drawCall.Draw.IndirectArgsBuffer = _indirectArgsBuffer;
            drawCall.Draw.IndirectArgsOffset = drawCallIndex * sizeof(GPUDrawIndexedIndirectArgs);

            // Transform data (needed for culling and sorting)
            drawCall.World = _transform.GetWorld();
            drawCall.ObjectPosition = _transform.Translation;
            drawCall.ObjectRadius = (float)_sphere.Radius;
            drawCall.WorldDeterminantSign = Math::FloatSelect(_transform.Scale.GetAbsolute().MinValue(), 1.0f, -1.0f);
            drawCall.PerInstanceRandom = 0.0f;



            // Surface data (required for Surface materials)
            drawCall.Surface.GeometrySize = mesh.GetBox().GetSize();
            drawCall.Surface.PrevWorld = drawCall.World;
            drawCall.Surface.LODDitherFactor = 0.0f;
            drawCall.Surface.Lightmap = nullptr;
            drawCall.Surface.LightmapUVsArea = Rectangle::Empty;

            // Foliage-specific data
            drawCall.Foliage.Lightmap = nullptr;
            drawCall.Foliage.LightmapUVsArea = Rectangle::Empty;
            drawCall.Foliage.VisibleInstancesBuffer = _visibleInstancesBuffer->View();
            //drawCall.Foliage.InstanceDataBuffer = _instanceDataBuffer->View();
            drawCall.Foliage.InstanceDataBuffer = _instanceDataBuffer.GetBuffer()->View();



            renderContext.List->AddDrawCall(renderContext, drawModes, flags, drawCall, receivesDecals, sortOrder);


            LOG(Warning, "Added foliage DrawCall - Material domain: {0}, IndirectBuf: {1}, VisBuf: {2}, DataBuf: {3}",
                (int32)material->GetInfo().Domain,
                drawCall.Draw.IndirectArgsBuffer != nullptr,
                drawCall.Foliage.VisibleInstancesBuffer != nullptr,
                drawCall.Foliage.InstanceDataBuffer != nullptr);



            drawCallIndex++;
        }
        //LOG(Info, "Added {0} foliage draw calls", drawCallIndex);
    }
}

void Foliage::Draw(RenderContextBatch& renderContextBatch)
{
    if (Instances.IsEmpty())
        return;

    // TODO: GPU-driven rendering doesn't need async CPU jobs
    // GPU handles parallelism automatically via compute dispatch

    // Fallback to default rendering (calls Draw(RenderContext&))
    Actor::Draw(renderContextBatch);
}

bool Foliage::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    int32 instanceIndex;
    return Intersects(ray, distance, normal, instanceIndex);
}

// Layout for encoded instance data (serialized as Base64 string)

static constexpr int32 GetInstanceBase64Size(int32 size)
{
    // 4 * (n / 3) -> align up to 4
    return (size * 4 / 3 + 3) & ~3;
}

// [Deprecated on 30.11.2019, expires on 30.11.2021]
struct InstanceEncoded1
{
    int32 Type;
    float Random;
    Float3 Translation;
    Quaternion Orientation;
    Float3 Scale;

    static constexpr int32 Size = 48;
    static constexpr int32 Base64Size = GetInstanceBase64Size(Size);
};

struct InstanceEncoded2
{
    int32 Type;
    float Random;
    Float3 Translation;
    Quaternion Orientation;
    Float3 Scale;
    LightmapEntry Lightmap;

    static const int32 Size = 68;
    static const int32 Base64Size = GetInstanceBase64Size(Size);
};

typedef InstanceEncoded2 InstanceEncoded;
static_assert(InstanceEncoded::Size == sizeof(InstanceEncoded), "Please update base64 buffer size to match the encoded instance buffer.");
static_assert(InstanceEncoded::Base64Size == GetInstanceBase64Size(sizeof(InstanceEncoded)), "Please update base64 buffer size to match the encoded instance buffer.");

void Foliage::Serialize(SerializeStream& stream, const void* otherObj)
{
    // Base
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(Foliage);

    if (FoliageTypes.IsEmpty())
        return;

    PROFILE_CPU();

    stream.JKEY("Foliage");
    stream.StartArray();
    for (auto& type : FoliageTypes)
    {
        stream.StartObject();
        type.Serialize(stream, nullptr);
        stream.EndObject();
    }
    stream.EndArray();

    stream.JKEY("Instances");
    stream.StartArray();
    InstanceEncoded enc;
    char base64[InstanceEncoded::Base64Size + 2];
    base64[0] = '\"';
    base64[InstanceEncoded::Base64Size + 1] = '\"';
    for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
    {
        auto& instance = *i;

        enc.Type = instance.Type;
        enc.Random = instance.Random;
        enc.Translation = instance.Transform.Translation;
        enc.Orientation = instance.Transform.Orientation;
        enc.Scale = instance.Transform.Scale;
        enc.Lightmap = instance.Lightmap;

        Encryption::Base64Encode((const byte*)&enc, sizeof(enc), base64 + 1);

        stream.RawValue(base64, InstanceEncoded::Base64Size + 2);
    }
    stream.EndArray();
}

void Foliage::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // Base
    Actor::Deserialize(stream, modifier);

    PROFILE_CPU();

    // Clear
    Instances.Release();
    FoliageTypes.Resize(0, false);

    // Deserialize foliage types
    int32 foliageTypesCount = 0;
    const auto& foliageTypesMember = stream.FindMember("Foliage");
    if (foliageTypesMember != stream.MemberEnd() && foliageTypesMember->value.IsArray())
    {
        foliageTypesCount = foliageTypesMember->value.Size();
    }
    if (foliageTypesCount)
    {
        const DeserializeStream& items = foliageTypesMember->value;;
        FoliageTypes.Resize(foliageTypesCount, false);
        for (int32 i = 0; i < foliageTypesCount; i++)
        {
            FoliageTypes[i].Foliage = this;
            FoliageTypes[i].Index = i;
            FoliageTypes[i].Deserialize((DeserializeStream&)items[i], modifier);
        }
    }

    // Skip if no foliage
    if (FoliageTypes.IsEmpty())
        return;

    // Deserialize foliage instances
    int32 foliageInstancesCount = 0;
    const auto& foliageInstancesMember = stream.FindMember("Instances");
    if (foliageInstancesMember != stream.MemberEnd() && foliageInstancesMember->value.IsArray())
    {
        foliageInstancesCount = foliageInstancesMember->value.Size();
    }
    if (foliageInstancesCount)
    {
        const DeserializeStream& items = foliageInstancesMember->value;
        Instances.Resize(foliageInstancesCount);

        if (modifier->EngineBuild <= 6189)
        {
            // [Deprecated on 30.11.2019, expires on 30.11.2021]
            MARK_CONTENT_DEPRECATED();
            InstanceEncoded1 enc;
            for (int32 i = 0; i < foliageInstancesCount; i++)
            {
                auto& instance = Instances[i];
                auto& item = items[i];

                const int32 length = item.GetStringLength();
                if (length != InstanceEncoded1::Base64Size)
                {
                    //LOG(Warning, "Invalid foliage instance data size.");
                    continue;
                }
                Encryption::Base64Decode(item.GetString(), length, (byte*)&enc);

                instance.Type = enc.Type;
                instance.Random = enc.Random;
                instance.Transform.Translation = enc.Translation;
                instance.Transform.Orientation = enc.Orientation;
                instance.Transform.Scale = enc.Scale;
                instance.Lightmap = LightmapEntry();
            }
        }
        else
        {
            InstanceEncoded enc;
            for (int32 i = 0; i < foliageInstancesCount; i++)
            {
                auto& instance = Instances[i];
                auto& item = items[i];

                const int32 length = item.GetStringLength();
                if (length != InstanceEncoded::Base64Size)
                {
                    //LOG(Warning, "Invalid foliage instance data size.");
                    continue;
                }
                Encryption::Base64Decode(item.GetString(), length, (byte*)&enc);

                instance.Type = enc.Type;
                instance.Random = enc.Random;
                instance.Transform.Translation = enc.Translation;
                instance.Transform.Orientation = enc.Orientation;
                instance.Transform.Scale = enc.Scale;
                instance.Lightmap = enc.Lightmap;
            }
        }

#if BUILD_DEBUG
        // Remove invalid instances
        for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
        {
            if (i->Type < 0 || i->Type >= FoliageTypes.Count())
            {
                LOG(Warning, "Removing invalid foliage instance.");
                Instances.Remove(i);
                --i;
            }
        }
#endif

        // Update cull distance
        for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
        {
            auto& instance = *i;
            auto& type = FoliageTypes[instance.Type];
            instance.CullDistance = type.CullDistance + type.CullDistanceRandomRange * instance.Random;
        }
    }

    LOG(Warning, "Foliage::Deserialize END - Instances.Count()={0}", Instances.Count());

    // Rebuild bounds from instances
    if (Instances.HasItems())
    {
        UpdateBounds();
        LOG(Warning, "  After UpdateBounds: _sphere radius={0}, _box size={1}",
            _sphere.Radius, _box.GetSize());
    }

}



void Foliage::BeginPlay(SceneBeginData* data)
{
    LOG(Warning, "Foliage::BeginPlay - Instances.Count()={0}", Instances.Count());
    Actor::BeginPlay(data);

    auto sceneRendering = GetSceneRendering();
    LOG(Warning, "  After BeginPlay: GetSceneRendering() = {0}, _sceneRenderingKey = {1}",
        sceneRendering != nullptr, _sceneRenderingKey);

    // Check bounds
    LOG(Warning, "  _sphere: Center={0}, Radius={1}", _sphere.Center, _sphere.Radius);
    LOG(Warning, "  _box: Min={0}, Max={1}", _box.Minimum, _box.Maximum);
}

void Foliage::EndPlay()
{
    LOG(Warning, "Foliage::EndPlay - Instances.Count()={0}", Instances.Count());
    Actor::EndPlay();
}

void Foliage::OnLayerChanged()
{
    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Layer);
}

void Foliage::OnEnable()
{
    LOG(Warning, "Foliage::OnEnable called - Instances.Count()={0}", Instances.Count());
    //GetSceneRendering()->AddActor(this, _sceneRenderingKey);

    auto sceneRendering = GetSceneRendering();
    LOG(Warning, "  GetSceneRendering() = {0}", sceneRendering != nullptr);

    if (sceneRendering)
    {
        sceneRendering->AddActor(this, _sceneRenderingKey);
        LOG(Warning, "  AddActor succeeded, _sceneRenderingKey = {0}", _sceneRenderingKey);
    }
    else
    {
        LOG(Error, "  GetSceneRendering() returned NULL!");
    }


    // Base
    Actor::OnEnable();

}

void Foliage::OnDisable()
{
    LOG(Warning, "Foliage::OnDisable called");
    GetSceneRendering()->RemoveActor(this, _sceneRenderingKey);

    // Base
    Actor::OnDisable();
    ReleaseGPUBuffers();
}

void Foliage::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    PROFILE_CPU();

    // Update instances matrices and cached world bounds
    Vector3 corners[8];
    Matrix world;
    GetLocalToWorldMatrix(world);
    for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
    {
        auto& instance = *i;
        auto type = &FoliageTypes[instance.Type];

        // Update bounds
        instance.Bounds = BoundingSphere::Empty;
        if (!type->IsReady())
            continue;
        auto& meshes = type->Model->LODs[0].Meshes;
        const Transform transform = _transform.LocalToWorld(instance.Transform);
        for (int32 j = 0; j < meshes.Count(); j++)
        {
            meshes[j].GetBox().GetCorners(corners);

            for (int32 k = 0; k < 8; k++)
            {
                Vector3::Transform(corners[k], transform, corners[k]);
            }
            BoundingSphere meshBounds;
            BoundingSphere::FromPoints(corners, 8, meshBounds);

            BoundingSphere::Merge(instance.Bounds, meshBounds, instance.Bounds);
        }
    }
}
void Foliage::UpdateBounds()
{
    _box = BoundingBox::Empty;

    // Merge all instance bounds into actor bounds
    for (auto i = Instances.Begin(); i.IsNotEnd(); ++i)
    {
        BoundingBox instanceBox;
        BoundingBox::FromSphere(i->Bounds, instanceBox);
        BoundingBox::Merge(_box, instanceBox, _box);
    }

    BoundingSphere::FromBox(_box, _sphere);

    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Bounds);
}

void Foliage::InitializeGPUBuffers()
{
    if (Instances.IsEmpty())
        return;

    PROFILE_CPU();

    // Release existing buffers if any
    ReleaseGPUBuffers();

    const int32 instanceCount = Instances.Count();
    auto context = GPUDevice::Instance->GetMainContext();

    _instanceDataBuffer.Clear();
    _instanceDataBuffer.Data.Resize(instanceCount * 8 * sizeof(Float4));
    Float4* dst = (Float4*)_instanceDataBuffer.Data.Get();

    for (int32 i = 0; i < instanceCount; i++)
    {
        auto& instance = Instances[i];

        Matrix localToWorld;
        const Transform worldTransform = _transform.LocalToWorld(instance.Transform);
        worldTransform.GetWorld(localToWorld);

        // Pack lightmap UV area (matching ShaderObjectData::Store exactly)
        Half4 lightmapUVsAreaPacked(*(Float4*)&instance.Lightmap.UVsArea);
        Float2 lightmapUVsAreaPackedAliased = *(Float2*)&lightmapUVsAreaPacked;

        // Store exactly like ShaderObjectData::Store()
        dst[0] = Float4(localToWorld.M11, localToWorld.M12, localToWorld.M13, localToWorld.M41);
        dst[1] = Float4(localToWorld.M21, localToWorld.M22, localToWorld.M23, localToWorld.M42);
        dst[2] = Float4(localToWorld.M31, localToWorld.M32, localToWorld.M33, localToWorld.M43);
        dst[3] = Float4(localToWorld.M11, localToWorld.M12, localToWorld.M13, localToWorld.M41); // PrevWorld = World for static
        dst[4] = Float4(localToWorld.M21, localToWorld.M22, localToWorld.M23, localToWorld.M42);
        dst[5] = Float4(localToWorld.M31, localToWorld.M32, localToWorld.M33, localToWorld.M43);
        dst[6] = Float4(Float3(1, 1, 1), instance.Random);  // GeometrySize, PerInstanceRandom
        dst[7] = Float4(1.0f, 0.0f, lightmapUVsAreaPackedAliased.X, lightmapUVsAreaPackedAliased.Y);

        // Add instance metadata
        dst[6].W = instance.Random;
        Float4 extraData;
        extraData.X = instance.CullDistance;
        extraData.Y = (float)instance.Type;
        extraData.Z = 0.0f;  // Padding
        extraData.W = 0.0f;  // Padding

        // Repack dst[7] to include Type and CullDistance
        dst[7] = Float4(
            instance.CullDistance,  // x
            (float)instance.Type,   // y
            lightmapUVsAreaPackedAliased.X,  // z
            lightmapUVsAreaPackedAliased.Y   // w
        );


        // DEBUG first instance
        if (i == 0)
        {
            LOG(Warning, "Instance 0: Type={0}, CullDistance={1}", instance.Type, instance.CullDistance);
        }

        dst += 8;
    }



    _instanceDataBuffer.Flush(context);
    
    // Verify buffer was created
    if (_instanceDataBuffer.GetBuffer())
    {
        LOG(Info, "Buffer created successfully: Size={0} bytes, Expected={1} float4s",
            _instanceDataBuffer.Data.Count(),
            instanceCount * 8);
    }
    else
    {
        LOG(Error, "Buffer creation FAILED!");
    }

    // Create visible instances buffer (compute shader writes instance indices here)
    auto visibleBufferDesc = GPUBufferDescription::Structured(
        instanceCount,  // Max capacity = all instances
        sizeof(uint32), // Just store instance indices
        true            // UAV - compute shader writes
    );
    visibleBufferDesc.Usage = GPUResourceUsage::Default;

    _visibleInstancesBuffer = GPUDevice::Instance->CreateBuffer(TEXT("FoliageVisibleInstances"));
    if (_visibleInstancesBuffer->Init(visibleBufferDesc))
    {
        LOG(Error, "Failed to create visible instances buffer");
        ReleaseGPUBuffers();
        return;
    }

    // Create indirect args buffer - structured buffer that compute writes to
    int32 totalDrawCalls = 0;
    for (const auto& type : FoliageTypes)
    {
        if (type.Model && type.Model->IsLoaded())
        {
            // Count meshes in LOD0
            const auto& lod0 = type.Model->LODs[0];
            totalDrawCalls += lod0.Meshes.Count();
        }
    }

    if (totalDrawCalls == 0)
    {
        //LOG(Warning, "No foliage meshes to render");
        return;
    }

    //LOG(Info, "Foliage needs {0} indirect draw calls", totalDrawCalls);
    // Create UAV buffer (compute shader writes here)
    GPUBufferDescription argsUAVDesc = {};
    argsUAVDesc.Size = sizeof(GPUDrawIndexedIndirectArgs) * totalDrawCalls;
    argsUAVDesc.Stride = sizeof(GPUDrawIndexedIndirectArgs);
    argsUAVDesc.Flags = GPUBufferFlags::UnorderedAccess | GPUBufferFlags::Structured;
    argsUAVDesc.Usage = GPUResourceUsage::Default;
    argsUAVDesc.Format = PixelFormat::Unknown;
    _indirectArgsUAV = GPUDevice::Instance->CreateBuffer(TEXT("FoliageIndirectArgsUAV"));
    if (_indirectArgsUAV->Init(argsUAVDesc))
    {
        LOG(Error, "Failed to create foliage indirect args UAV buffer");
        ReleaseGPUBuffers();
        return;
    }

    // Create Argument buffer (GPU reads for indirect draws)
    GPUBufferDescription argsBufferDesc = {};
    argsBufferDesc.Size = sizeof(GPUDrawIndexedIndirectArgs) * totalDrawCalls;
    argsBufferDesc.Stride = 0;//sizeof(GPUDrawIndexedIndirectArgs);
    argsBufferDesc.Flags = GPUBufferFlags::Argument;
    argsBufferDesc.Usage = GPUResourceUsage::Default;
    argsBufferDesc.Format = PixelFormat::Unknown;
    _indirectArgsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("FoliageIndirectArgs"));
    if (_indirectArgsBuffer->Init(argsBufferDesc))
    {
        LOG(Error, "Failed to create foliage indirect args buffer");
        ReleaseGPUBuffers();
        return;
    }

    // Initialize BOTH buffers with mesh metadata
    Array<GPUDrawIndexedIndirectArgs> initialArgs;
    initialArgs.Resize(totalDrawCalls);
    int32 argIndex = 0;

    for (const auto& type : FoliageTypes)
    {
        if (!type.Model || !type.Model->IsLoaded())
            continue;

        const auto& lod = type.Model->LODs[0];
        for (const auto& mesh : lod.Meshes)
        {
            auto& args = initialArgs[argIndex++];
            args.IndicesCount = mesh.GetTriangleCount() * 3;
            args.InstanceCount = 0;
            args.StartIndex = 0;
            args.StartVertex = 0;
            args.StartInstance = 0;
        }
    }

    context->UpdateBuffer(_indirectArgsUAV, initialArgs.Get(),
        initialArgs.Count() * sizeof(GPUDrawIndexedIndirectArgs));
    context->UpdateBuffer(_indirectArgsBuffer, initialArgs.Get(),
        initialArgs.Count() * sizeof(GPUDrawIndexedIndirectArgs));




    // Build type-to-mesh-index mapping
    Array<uint32> typeToMeshIndex;
    typeToMeshIndex.Resize(FoliageTypes.Count());
    int32 meshOffset = 0;

    for (int32 typeIndex = 0; typeIndex < FoliageTypes.Count(); typeIndex++)
    {
        const auto& type = FoliageTypes[typeIndex];
        if (type.Model && type.Model->IsLoaded())
        {
            typeToMeshIndex[typeIndex] = meshOffset;
            meshOffset += type.Model->LODs[0].Meshes.Count();
        }
        else
        {
            typeToMeshIndex[typeIndex] = 0xFFFFFFFF;  // Invalid index
        }
    }

    // Create mapping buffer
    auto mappingDesc = GPUBufferDescription::Structured(
        FoliageTypes.Count(),
        sizeof(uint32),
        false
    );
    _typeToMeshIndexBuffer = GPUDevice::Instance->CreateBuffer(TEXT("FoliageTypeToMeshIndex"));
    if (_typeToMeshIndexBuffer->Init(mappingDesc))
    {
        LOG(Error, "Failed to create type-to-mesh mapping buffer");
        ReleaseGPUBuffers();
        return;
    }

    context->UpdateBuffer(_typeToMeshIndexBuffer, typeToMeshIndex.Get(),
        typeToMeshIndex.Count() * sizeof(uint32));

    LOG(Warning, "TypeToMeshIndex mapping: FoliageTypes.Count()={0}", FoliageTypes.Count());
    for (int32 i = 0; i < typeToMeshIndex.Count(); i++)
    {
        LOG(Warning, "  Type[{0}] -> Mesh[{1}]", i, typeToMeshIndex[i]);
    }

    // Load compute shader
    _cullingShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/FoliageCulling"));

    if (_cullingShader == nullptr || _cullingShader->WaitForLoaded())
    {
        LOG(Error, "Failed to load foliage culling shader");
        ReleaseGPUBuffers();
        return;
    }

    // Create camera params constant buffer (size must match shader struct)
    _cameraParamsBuffer = GPUDevice::Instance->CreateConstantBuffer(sizeof(float) * 32, TEXT("FoliageCameraParams"));
    if (!_cameraParamsBuffer)
    {
        LOG(Error, "Failed to create camera params buffer");
        ReleaseGPUBuffers();
        return;
    }

    LOG(Info, "Initialized GPU buffers for {0} foliage instances", instanceCount);
    _lastInstanceCount = instanceCount;  // Update tracked count

}

void Foliage::ReleaseGPUBuffers()
{
    //SAFE_DELETE_GPU_RESOURCE(_instanceDataBuffer);
    SAFE_DELETE_GPU_RESOURCE(_indirectArgsUAV);
    SAFE_DELETE_GPU_RESOURCE(_indirectArgsBuffer);
    SAFE_DELETE_GPU_RESOURCE(_typeToMeshIndexBuffer);
    SAFE_DELETE_GPU_RESOURCE(_visibleInstancesBuffer);
    SAFE_DELETE_GPU_RESOURCE(_cameraParamsBuffer);

    _instanceDataBuffer.Dispose();

    _cullingShader = nullptr; // Asset reference cleanup
}
