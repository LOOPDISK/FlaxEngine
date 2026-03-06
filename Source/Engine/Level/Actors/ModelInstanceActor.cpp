// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelInstanceActor.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Level/Scene/SceneRendering.h"

ModelInstanceActor::ModelInstanceActor(const SpawnParams& params)
    : Actor(params)
{
}

String ModelInstanceActor::MeshReference::ToString() const
{
    return String::Format(TEXT("Actor={},LOD={},Mesh={}"), Actor ? Actor->GetNamePath() : String::Empty, LODIndex, MeshIndex);
}

void ModelInstanceActor::SetEntries(const Array<ModelInstanceEntry>& value)
{
    WaitForModelLoad();
    bool anyChanged = false;
    Entries.Resize(value.Count());
    for (int32 i = 0; i < value.Count(); i++)
    {
        anyChanged |= Entries[i] != value[i];
        Entries[i] = value[i];
    }
    if (anyChanged && _sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Visual);
}

void ModelInstanceActor::SetMaterial(int32 entryIndex, MaterialBase* material)
{
    WaitForModelLoad();
    if (Entries.Count() == 0 && !material)
        return;
    CHECK(entryIndex >= 0 && entryIndex < Entries.Count());
    if (Entries[entryIndex].Material == material)
        return;
    Entries[entryIndex].Material = material;
    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Visual);
}

MaterialInstance* ModelInstanceActor::CreateAndSetVirtualMaterialInstance(int32 entryIndex)
{
    WaitForModelLoad();
    MaterialBase* material = GetMaterial(entryIndex);
    CHECK_RETURN(material && !material->WaitForLoaded(), nullptr);
    MaterialInstance* result = material->CreateVirtualInstance();
    Entries[entryIndex].Material = result;
    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Visual);
    return result;
}

void ModelInstanceActor::SetMeshVisibility(int32 meshIndex, bool visible)
{
    if (meshIndex < 0)
        return;
    if (meshIndex >= _meshVisibility.Count())
    {
        const int32 oldCount = _meshVisibility.Count();
        _meshVisibility.Resize(meshIndex + 1);
        for (int32 i = oldCount; i < _meshVisibility.Count(); i++)
            _meshVisibility[i] = true;
    }
    _meshVisibility[meshIndex] = visible;
}

bool ModelInstanceActor::GetMeshVisibility(int32 meshIndex) const
{
    if (meshIndex < 0 || meshIndex >= _meshVisibility.Count())
        return true;
    return _meshVisibility[meshIndex];
}

void ModelInstanceActor::SetAllMeshVisibility(bool visible, int32 meshCount)
{
    if (meshCount <= 0)
    {
        _meshVisibility.Clear();
        return;
    }
    _meshVisibility.Resize(meshCount);
    for (int32 i = 0; i < meshCount; i++)
        _meshVisibility[i] = visible;
}

void ModelInstanceActor::WaitForModelLoad()
{
}

void ModelInstanceActor::OnLayerChanged()
{
    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::Layer);
}

void ModelInstanceActor::OnStaticFlagsChanged()
{
    if (_sceneRenderingKey != -1)
        GetSceneRendering()->UpdateActor(this, _sceneRenderingKey, ISceneRenderingListener::StaticFlags);
}

void ModelInstanceActor::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    UpdateBounds();
}

void ModelInstanceActor::OnEnable()
{
    GetSceneRendering()->AddActor(this, _sceneRenderingKey);

    // Base
    Actor::OnEnable();
}

void ModelInstanceActor::OnDisable()
{
    // Base
    Actor::OnDisable();

    GetSceneRendering()->RemoveActor(this, _sceneRenderingKey);
}
