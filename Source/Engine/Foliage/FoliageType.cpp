// Copyright (c) Wojciech Figat. All rights reserved.

#include "FoliageType.h"
#include "Foliage.h"
#include "Engine/Core/Collections/ArrayExtensions.h"
#include "Engine/Core/Templates.h"
#include "Engine/Core/Random.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/Models/Mesh.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Profiler/ProfilerMemory.h"

FoliageType::FoliageType()
    : ScriptingObject(SpawnParams(Guid::New(), TypeInitializer))
    , Foliage(nullptr)
    , Index(-1)
{
    _isReady = 0;

    ReceiveDecals = true;
    UseDensityScaling = false;
    PlacementAlignToNormal = true;
    PlacementRandomYaw = true;

    Model.Changed.Bind<FoliageType, &FoliageType::OnModelChanged>(this);
    Model.Loaded.Bind<FoliageType, &FoliageType::OnModelLoaded>(this);
}

FoliageType& FoliageType::operator=(const FoliageType& other)
{
    Foliage = other.Foliage;
    Index = other.Index;
    Model = other.Model;
    Entries = other.Entries;
    CullDistance = other.CullDistance;
    CullDistanceRandomRange = other.CullDistanceRandomRange;
    ScaleInLightmap = other.ScaleInLightmap;
    DrawModes = other.DrawModes;
    ShadowsMode = other.ShadowsMode;
    PaintDensity = other.PaintDensity;
    PaintRadius = other.PaintRadius;
    PaintGroundSlopeAngleMin = other.PaintGroundSlopeAngleMin;
    PaintGroundSlopeAngleMax = other.PaintGroundSlopeAngleMax;
    PaintScaling = other.PaintScaling;
    PaintScaleMin = other.PaintScaleMin;
    PaintScaleMax = other.PaintScaleMax;
    PlacementOffsetY = other.PlacementOffsetY;
    PlacementRandomPitchAngle = other.PlacementRandomPitchAngle;
    PlacementRandomRollAngle = other.PlacementRandomRollAngle;
    DensityScalingScale = other.DensityScalingScale;
    ReceiveDecals = other.ReceiveDecals;
    UseDensityScaling = other.UseDensityScaling;
    PlacementAlignToNormal = other.PlacementAlignToNormal;
    PlacementRandomYaw = other.PlacementRandomYaw;
    InvalidateCachedDrawSetup();
    return *this;
}

Array<MaterialBase*> FoliageType::GetMaterials() const
{
    Array<MaterialBase*> result;
    result.EnsureCapacity(Entries.Count());
    for (const auto& e : Entries)
        result.Add(e.Material);
    return result;
}

void FoliageType::SetMaterials(const Array<MaterialBase*>& value)
{
    PROFILE_MEM(LevelFoliage);
    CHECK(value.Count() == Entries.Count());
    for (int32 i = 0; i < value.Count(); i++)
        Entries[i].Material = value[i];
    InvalidateCachedDrawSetup();
}

Float3 FoliageType::GetRandomScale() const
{
    Float3 result;
    float tmp;
    switch (PaintScaling)
    {
    case FoliageScalingModes::Uniform:
        result.X = Math::Lerp(PaintScaleMin.X, PaintScaleMax.X, Random::Rand());
        result.Y = result.X;
        result.Z = result.X;
        break;
    case FoliageScalingModes::Free:
        result.X = Math::Lerp(PaintScaleMin.X, PaintScaleMax.X, Random::Rand());
        result.Y = Math::Lerp(PaintScaleMin.Y, PaintScaleMax.Y, Random::Rand());
        result.Z = Math::Lerp(PaintScaleMin.Z, PaintScaleMax.Z, Random::Rand());
        break;
    case FoliageScalingModes::LockXY:
        tmp = Random::Rand();
        result.X = Math::Lerp(PaintScaleMin.X, PaintScaleMax.X, tmp);
        result.Y = Math::Lerp(PaintScaleMin.Y, PaintScaleMax.Y, tmp);
        result.Z = Math::Lerp(PaintScaleMin.Z, PaintScaleMax.Z, Random::Rand());
        break;
    case FoliageScalingModes::LockXZ:
        tmp = Random::Rand();
        result.X = Math::Lerp(PaintScaleMin.X, PaintScaleMax.X, tmp);
        result.Y = Math::Lerp(PaintScaleMin.Y, PaintScaleMax.Y, Random::Rand());
        result.Z = Math::Lerp(PaintScaleMin.Z, PaintScaleMax.Z, tmp);
        break;
    case FoliageScalingModes::LockYZ:
        tmp = Random::Rand();
        result.X = Math::Lerp(PaintScaleMin.X, PaintScaleMax.X, Random::Rand());
        result.Y = Math::Lerp(PaintScaleMin.Y, PaintScaleMax.Y, tmp);
        result.Z = Math::Lerp(PaintScaleMin.Z, PaintScaleMax.Z, tmp);
        break;
    }
    return result;
}

void FoliageType::InvalidateCachedDrawSetup()
{
    ScopeLock lock(_cachedDrawSetupLocker);
    _cachedDrawSetupFrame = 0;
    _cachedDrawSetup.Clear();
    _cachedMeshLookup.Clear();
}

void FoliageType::EnsureCachedDrawSetup(uint64 frame) const
{
    if (_cachedDrawSetupFrame == frame)
        return;

    ScopeLock lock(_cachedDrawSetupLocker);
    if (_cachedDrawSetupFrame == frame)
        return;

    _cachedMeshLookup.Clear();

    if (!IsReady() || !Model || !Model->IsLoaded())
    {
        _cachedDrawSetup.Clear();
        _cachedDrawSetupFrame = frame;
        return;
    }

    auto* foliageModel = this->Model.Get();
    if (!foliageModel)
    {
        _cachedDrawSetup.Clear();
        _cachedDrawSetupFrame = frame;
        return;
    }

    const auto& modelLODs = foliageModel->LODs;
    const int32 lodsCount = modelLODs.Count();
    _cachedDrawSetup.Resize(lodsCount);

    const auto& materialSlots = foliageModel->MaterialSlots;

    for (int32 lodIndex = 0; lodIndex < lodsCount; lodIndex++)
    {
        const auto& lod = modelLODs.Get()[lodIndex];
        auto& cachedLOD = _cachedDrawSetup[lodIndex].Meshes;
        const auto& lodMeshes = lod.Meshes;
        const int32 meshesCount = lodMeshes.Count();
        cachedLOD.Resize(meshesCount);

        for (int32 meshIndex = 0; meshIndex < meshesCount; meshIndex++)
        {
            auto& cache = cachedLOD[meshIndex];
            const Mesh& mesh = lodMeshes.Get()[meshIndex];
            cache.Mesh = &mesh;
            cache.Visible = false;
            cache.Material = nullptr;
            cache.MaterialDrawModes = DrawPass::None;
            cache.ShadowsMode = ShadowsCastingMode::None;
            cache.ReceiveDecals = false;
            cache.GeometrySize = Float3::Zero;

            const int32 slotIndex = mesh.GetMaterialSlotIndex();
            if (!Entries.IsValidIndex(slotIndex))
                continue;

            const auto& entry = Entries[slotIndex];
            if (!entry.Visible || !mesh.IsInitialized())
                continue;

            if (!materialSlots.IsValidIndex(slotIndex))
                continue;
            const MaterialSlot& slot = materialSlots[slotIndex];

            MaterialBase* material = nullptr;
            if (entry.Material && entry.Material->IsLoaded())
                material = entry.Material;
            else if (slot.Material && slot.Material->IsLoaded())
                material = slot.Material;
            else
                material = GPUDevice::Instance->GetDefaultMaterial();
            if (!material || !material->IsSurface())
                continue;

            cache.Visible = true;
            cache.Material = material;
            cache.GeometrySize = mesh.GetBox().GetSize();
            cache.MaterialDrawModes = material->GetDrawModes();
            cache.ShadowsMode = (ShadowsCastingMode)(entry.ShadowsMode & slot.ShadowsMode);
            cache.ReceiveDecals = entry.ReceiveDecals != 0;

            _cachedMeshLookup[&mesh] = Pair<int32, int32>(lodIndex, meshIndex);
        }
    }

    _cachedDrawSetupFrame = frame;
}

const FoliageType::CachedMeshSlot* FoliageType::GetCachedMeshSlot(const Mesh* mesh) const
{
    if (!mesh)
        return nullptr;

    const auto* indices = _cachedMeshLookup.TryGet(mesh);
    if (!indices)
        return nullptr;

    const int32 lodIndex = indices->First;
    const int32 meshIndex = indices->Second;
    if (!_cachedDrawSetup.IsValidIndex(lodIndex))
        return nullptr;
    const auto& meshes = _cachedDrawSetup[lodIndex].Meshes;
    if (!meshes.IsValidIndex(meshIndex))
        return nullptr;

    return &meshes[meshIndex];
}

void FoliageType::OnModelChanged()
{
    // Cleanup
    _isReady = 0;
    Entries.Release();
    InvalidateCachedDrawSetup();
}

void FoliageType::OnModelLoaded()
{
    PROFILE_MEM(LevelFoliage);

    // Now it's ready
    _isReady = 1;

    // Prepare buffer (model may be modified so we should synchronize buffer with the actual asset)
    Entries.SetupIfInvalid(Model);

    // Inform foliage that instances may need to be updated (data caching, etc.)
    Foliage->OnFoliageTypeModelLoaded(Index);
    InvalidateCachedDrawSetup();
}

void FoliageType::Serialize(SerializeStream& stream, const void* otherObj)
{
    SERIALIZE_GET_OTHER_OBJ(FoliageType);

    SERIALIZE(Model);

    const Function<bool(const ModelInstanceEntry&)> IsValidMaterial = [](const ModelInstanceEntry& e) -> bool
    {
        return e.Material;
    };
    if (ArrayExtensions::Any(Entries, IsValidMaterial))
    {
        stream.JKEY("Materials");
        stream.StartArray();
        for (auto& e : Entries)
            stream.Guid(e.Material.GetID());
        stream.EndArray();
    }

    SERIALIZE(CullDistance);
    SERIALIZE(CullDistanceRandomRange);
    SERIALIZE(ScaleInLightmap);
    SERIALIZE(DrawModes);
    SERIALIZE(ShadowsMode);
    SERIALIZE_BIT(ReceiveDecals);
    SERIALIZE_BIT(UseDensityScaling);
    SERIALIZE(DensityScalingScale);

    SERIALIZE(PaintDensity);
    SERIALIZE(PaintRadius);
    SERIALIZE(PaintGroundSlopeAngleMin);
    SERIALIZE(PaintGroundSlopeAngleMax);
    SERIALIZE(PaintScaling);
    SERIALIZE(PaintScaleMin);
    SERIALIZE(PaintScaleMax);

    SERIALIZE(PlacementOffsetY);
    SERIALIZE(PlacementRandomPitchAngle);
    SERIALIZE(PlacementRandomRollAngle);
    SERIALIZE_BIT(PlacementAlignToNormal);
    SERIALIZE_BIT(PlacementRandomYaw);
}

void FoliageType::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    PROFILE_MEM(LevelFoliage);
    DESERIALIZE(Model);

    const auto member = stream.FindMember("Materials");
    if (member != stream.MemberEnd() && member->value.IsArray())
    {
        auto& materials = member->value;
        const auto size = materials.Size();
        Entries.Resize(size);
        for (rapidjson::SizeType i = 0; i < size; i++)
        {
            Serialization::Deserialize(materials[i], Entries[i].Material, modifier);
        }
    }

    DESERIALIZE(CullDistance);
    DESERIALIZE(CullDistanceRandomRange);
    DESERIALIZE(ScaleInLightmap);
    DESERIALIZE(DrawModes);
    DESERIALIZE(ShadowsMode);
    DESERIALIZE_BIT(ReceiveDecals);
    DESERIALIZE_BIT(UseDensityScaling);
    DESERIALIZE(DensityScalingScale);

    DESERIALIZE(PaintDensity);
    DESERIALIZE(PaintRadius);
    DESERIALIZE(PaintGroundSlopeAngleMin);
    DESERIALIZE(PaintGroundSlopeAngleMax);
    DESERIALIZE(PaintScaling);
    DESERIALIZE(PaintScaleMin);
    DESERIALIZE(PaintScaleMax);

    DESERIALIZE(PlacementOffsetY);
    DESERIALIZE(PlacementRandomPitchAngle);
    DESERIALIZE(PlacementRandomRollAngle);
    DESERIALIZE_BIT(PlacementAlignToNormal);
    DESERIALIZE_BIT(PlacementRandomYaw);
    InvalidateCachedDrawSetup();
}
