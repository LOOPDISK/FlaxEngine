// Copyright (c) Wojciech Figat. All rights reserved.

#include "CloudVolume.h"
#include "Engine/Renderer/DrawCall.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Level/Scene/SceneRendering.h"

CloudVolume::CloudVolume(const SpawnParams& params)
    : Actor(params)
{
    _drawNoCulling = 1;
    _drawCategory = SceneRendering::PreRender;
}

void CloudVolume::Draw(RenderContext& renderContext)
{
    if (!EnumHasAnyFlags(renderContext.View.Pass, DrawPass::GBuffer))
        return;
    if (!CloudMesh || !CloudMesh->IsLoaded() || !CloudMesh->CanBeRendered())
        return;

    renderContext.List->CloudVolumes.Add(this);
}

bool CloudVolume::HasContentLoaded() const
{
    return CloudMesh == nullptr || CloudMesh->IsLoaded();
}

bool CloudVolume::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void CloudVolume::OnEnable()
{
    GetSceneRendering()->AddActor(this, _sceneRenderingKey);
}

void CloudVolume::OnDisable()
{
    GetSceneRendering()->RemoveActor(this, _sceneRenderingKey);
}

