// Copyright (c) Wojciech Figat. All rights reserved.

#define SCENE_RENDERING_USE_PROFILER_PER_ACTOR 0

#include "SceneRendering.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Threading/JobSystem.h"
#include "Engine/Physics/Actors/IPhysicsDebug.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Renderer/HierarchialZBufferPass.h"
#include <Engine/Graphics/Graphics.h>
#include "Engine/Profiler/ProfilerMemory.h"
#if !BUILD_RELEASE
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Core/Log.h"
#include "Engine/Debug/DebugDraw.h"
#endif

#if BUILD_RELEASE
#define CHECK_SCENE_EDIT_ACCESS()
#else
#define CHECK_SCENE_EDIT_ACCESS() \
    if (_isRendering && IsInMainThread() && GPUDevice::Instance && GPUDevice::Instance->IsRendering()) \
    { \
        LOG(Error, "Adding/removing actors during rendering is not supported ({}, '{}').", a->ToString(), a->GetNamePath()); \
        return; \
    }
#endif


ISceneRenderingListener::~ISceneRenderingListener()
{
    for (SceneRendering* scene : _scenes)
    {
        scene->_listeners.Remove(this);
    }
}

void ISceneRenderingListener::ListenSceneRendering(SceneRendering* scene)
{
    if (!_scenes.Contains(scene))
    {
        _scenes.Add(scene);
        scene->_listeners.Add(this);
    }
}
 FORCE_INLINE bool MainFrustumCull(const BoundingSphere& bounds, const Array<BoundingFrustum>& frustums)
{
    const int32 count = frustums.Count();
    if (count == 0)
    {
        return false;
    }
    const BoundingFrustum* data = frustums.Get();
    return data[0].Intersects(bounds); // main frustum is always 0
}

FORCE_INLINE bool NonMainFrustumsListCull(const BoundingSphere& bounds, const Array<BoundingFrustum>& frustums, const Float3 mainViewPosition, const float shadowCullRadius, const float shadowCullDistance2)
{
    const int32 count = frustums.Count();
    const BoundingFrustum* data = frustums.Get();
    for (int32 i = 1; i < count; i++) // start at 1 to skip main frustum (always 0)
    {
        if (data[i].Intersects(bounds))
        {
            if (bounds.Radius < shadowCullRadius && Float3::DistanceSquared(mainViewPosition, bounds.Center) > shadowCullDistance2)
            {
                return false;
            }
            return true;
        }
    }
    return false;
}

FORCE_INLINE  bool SceneRendering::CheckVisibility(Actor* actor, const BoundingSphere& bounds, const Array<BoundingFrustum>& frustums)
{
    if (NonMainFrustumsListCull(bounds, frustums, _mainViewPosition, _shadowCullRadius, _shadowCullDistance2))
    { // is in other frustums, like for shadows
        if (_hzb)
        {
            actor->_cullType = 0;
        }
        return true;
    }
    if (MainFrustumCull(bounds, frustums))
    {
        if (_hzb)
        {
            if (!_checkHZB)
            { // return the last HZB occlusion result
                return actor->_cullType != 2;
            }
            if (_hzb->CheckOcclusion(bounds))
            {
            //    DebugDraw::DrawSphere(bounds, Color(0, 0, 1, 0.2f), 0, false); // can't do this in a job
                actor->_cullType = 2;
                return false;
            }
            else
            {
                actor->_cullType = 0;
                return true;
            }
        }
        actor->_cullType = 0;
        return true;
    }
    if (_hzb)
    { // only mark as culled if HZBData was valid, meaning it was the main render task
        actor->_cullType = 1;
    }
    return false;
}
FORCE_INLINE  bool SceneRendering::CheckVisibility(Actor* actor, const BoundingSphere& bounds, const BoundingFrustum& frustum)
{
    if (frustum.Intersects(bounds))
    {
        if (_hzb)
        {
            if (!_checkHZB)
            { // return the last HZB occlusion result
                return actor->_cullType != 2;
            }
            if (_hzb->CheckOcclusion(bounds))
            {
                actor->_cullType = 2;
                return false;
            }
            else
            {
                actor->_cullType = 0;
                return true;
            }
        }
        actor->_cullType = 0;
        return true;
    }
    if (_hzb)
    { // only mark as culled if HZBData was valid, meaning it was the main render task
        actor->_cullType = 1;
    }
    return false;
}

void SceneRendering::Draw(RenderContextBatch& renderContextBatch, DrawCategory category)
{
    PROFILE_MEM(Graphics);
    if (category == PreRender)
    {
        // Add additional lock during scene rendering (prevents any Actors cache modifications on content streaming threads - eg. when model residency changes)
        Locker.ReadLock();
        _isRendering = true;

        // Register scene
        for (const auto& renderContext : renderContextBatch.Contexts)
            renderContext.List->Scenes.Add(this);
    }
    else if (category == PostRender)
    {
        // Release additional lock
        _isRendering = false;
        Locker.ReadUnlock();
    }

    auto& view = renderContextBatch.GetMainContext().View;
    auto& list = Actors[(int32)category];
    _drawListData = list.Get();
    _drawListSize = list.Count();
    _drawBatch = &renderContextBatch;
    _drawCategory = category;

  
    // Setup frustum data
    const int32 frustumsCount = renderContextBatch.Contexts.Count();
    _drawFrustumsData.Resize(frustumsCount);
    for (int32 i = 0; i < frustumsCount; i++)
        _drawFrustumsData.Get()[i] = renderContextBatch.Contexts.Get()[i].View.CullingFrustum;

    // Setup culling info
    _shadowCullDistance2 = Graphics::Shadows::CullingDistance * Graphics::Shadows::CullingDistance;
    _shadowCullRadius = 0.5f * Graphics::Shadows::CullingSize;
    _mainViewPosition = view.Position;

    _hzb = Graphics::OcclusionCulling ? renderContextBatch.GetMainContext().Task->OcclusionInfo : nullptr;
    
    if (_hzb != nullptr)
    {
        _checkHZB = true;
        // only do occlusion on main render task's main draw
        if (_drawCategory == SceneDrawAsync && (int32)view.Pass & (int32)DrawPass::GBuffer)
        {
            // don't do the hzb occlusion check if it already did it with the same data last time
            if ((_hzb->Id == _lastHZBId && _hzb->CurrentFrameIndex == _lastHZBFrame))
            {
                _checkHZB = false;
            }
            else
            {
                _checkHZB = true;
                _lastHZBId = _hzb->Id;
                _lastHZBFrame = _hzb->CurrentFrameIndex;
            }
        }
    }
    else
    {
        _checkHZB = false;
    }

    // Draw all visual components
    _drawListIndex = -1;
    if (_drawListSize >= 64 && category == SceneDrawAsync && renderContextBatch.EnableAsync)
    {
        // Run in async via Job System
        Function<void(int32)> func;
        func.Bind<SceneRendering, &SceneRendering::DrawActorsJob>(this);
        const int64 waitLabel = JobSystem::Dispatch(func, JobSystem::GetThreadsCount());
        renderContextBatch.WaitLabels.Add(waitLabel);
    }
    else
    {
        // Scene is small so draw on a main-thread
        DrawActorsJob(0);
    }

#if USE_EDITOR
    if (EnumHasAnyFlags(view.Pass, DrawPass::GBuffer) && category == SceneDraw)
    {
        // Draw physics shapes
        if (EnumHasAnyFlags(view.Flags, ViewFlags::PhysicsDebug) || view.Mode == ViewMode::PhysicsColliders)
        {
            PROFILE_CPU_NAMED("PhysicsDebug");
            const auto* physicsDebugData = PhysicsDebug.Get();
            for (int32 i = 0; i < PhysicsDebug.Count(); i++)
            {
                physicsDebugData[i]->DrawPhysicsDebug(view);
            }
        }

        // Draw light shapes
        if (EnumHasAnyFlags(view.Flags, ViewFlags::LightsDebug))
        {
            PROFILE_CPU_NAMED("LightsDebug");
            const LightsDebugCallback* lightsDebugData = LightsDebug.Get();
            for (int32 i = 0; i < LightsDebug.Count(); i++)
            {
                lightsDebugData[i](view);
            }
        }
    }
#endif
}

void SceneRendering::CollectPostFxVolumes(RenderContext& renderContext)
{
#if SCENE_RENDERING_USE_PROFILER
    PROFILE_CPU();
#endif
    for (int32 i = 0; i < PostFxProviders.Count(); i++)
    {
        PostFxProviders.Get()[i]->Collect(renderContext);
    }
}

void SceneRendering::Clear()
{
    ScopeWriteLock lock(Locker);
    for (auto* listener : _listeners)
    {
        listener->OnSceneRenderingClear(this);
        listener->_scenes.Remove(this);
    }
    _listeners.Clear();
    for (auto& e : Actors)
        e.Clear();
    for (auto& e : FreeActors)
        e.Clear();
#if USE_EDITOR
    PhysicsDebug.Clear();
#endif
}

void SceneRendering::AddActor(Actor* a, int32& key)
{
    if (key != -1)
        return;
    PROFILE_MEM(Graphics);
    CHECK_SCENE_EDIT_ACCESS();
    const int32 category = a->_drawCategory;
    ScopeWriteLock lock(Locker);
    auto& list = Actors[category];
    if (FreeActors[category].HasItems())
    {
        // Use existing item
        key = FreeActors[category].Pop();
    }
    else
    {
        // Add a new item
        key = list.Count();
        list.AddOne();
    }
    auto& e = list[key];
    e.Actor = a;
    e.LayerMask = a->GetLayerMask();
    e.Bounds = a->GetSphere();
    e.NoCulling = a->_drawNoCulling;
    for (auto* listener : _listeners)
        listener->OnSceneRenderingAddActor(a);
}

void SceneRendering::UpdateActor(Actor* a, int32& key, ISceneRenderingListener::UpdateFlags flags)
{
    const int32 category = a->_drawCategory;
    bool lock = !_isRendering || ((int32)flags & (int32)ISceneRenderingListener::AutoDelayDuringRendering) == 0; // Allow updating actors during rendering
    if (lock)
        Locker.ReadLock(); // Read-access only as list doesn't get resized (like Add/Remove do) so allow updating actors from different threads at once
    auto& list = Actors[category];
    if (list.Count() > key && key >= 0) // Ignore invalid key softly
    {
        auto& e = list[key];
        if (e.Actor == a)
        {
            for (auto* listener : _listeners)
                listener->OnSceneRenderingUpdateActor(a, e.Bounds, flags);
            if (flags & ISceneRenderingListener::Layer)
                e.LayerMask = a->GetLayerMask();
            if (flags & ISceneRenderingListener::Bounds)
                e.Bounds = a->GetSphere();
        }
    }
    if (lock)
        Locker.ReadUnlock();
}

void SceneRendering::RemoveActor(Actor* a, int32& key)
{
    CHECK_SCENE_EDIT_ACCESS();
    const int32 category = a->_drawCategory;
    ScopeWriteLock lock(Locker);
    auto& list = Actors[category];
    if (list.Count() > key || key < 0) // Ignore invalid key softly (eg. list after batch clear during scene unload)
    {
        auto& e = list.Get()[key];
        if (e.Actor == a)
        {
            for (auto* listener : _listeners)
                listener->OnSceneRenderingRemoveActor(a);
            e.Actor = nullptr;
            e.LayerMask = 0;
            FreeActors[category].Add(key);
        }
    }
    key = -1;
}

#define FOR_EACH_BATCH_ACTOR const int64 count = _drawListSize; while (true) { const int64 index = Platform::InterlockedIncrement(&_drawListIndex); if (index >= count) break; auto e = _drawListData[index];
#define CHECK_ACTOR ((view.RenderLayersMask.Mask & e.LayerMask) && (e.NoCulling || CheckVisibility(e.Actor, e.Bounds, _drawFrustumsData)))
#define CHECK_ACTOR_SINGLE_FRUSTUM ((view.RenderLayersMask.Mask & e.LayerMask) && (e.NoCulling || CheckVisibility(e.Actor, e.Bounds, view.CullingFrustum)))
#if SCENE_RENDERING_USE_PROFILER_PER_ACTOR
#define DRAW_ACTOR(mode) PROFILE_CPU_ACTOR(e.Actor); e.Actor->Draw(mode)
#else
#define DRAW_ACTOR(mode) e.Actor->Draw(mode)
#endif

void SceneRendering::DrawActorsJob(int32)
{
    PROFILE_CPU();
    PROFILE_MEM(Graphics);
    auto& mainContext = _drawBatch->GetMainContext();
    const auto& view = mainContext.View;


    if (view.StaticFlagsMask != StaticFlags::None)
    {
        // Static-flags culling
        FOR_EACH_BATCH_ACTOR
            e.Bounds.Center -= view.Origin;
            if (CHECK_ACTOR && (e.Actor->GetStaticFlags() & view.StaticFlagsMask) == view.StaticFlagsCompare)
            {
                DRAW_ACTOR(*_drawBatch);
            }
        }
    }
    else if (view.Origin.IsZero() && _drawFrustumsData.Count() == 1)
    {
        // Fast path for no origin shifting with a single context
        FOR_EACH_BATCH_ACTOR
            if (CHECK_ACTOR_SINGLE_FRUSTUM)
            {
                DRAW_ACTOR(mainContext);
            }

        }
    }
    else if (view.Origin.IsZero())
    {
        // Fast path for no origin shifting
        FOR_EACH_BATCH_ACTOR
            if (CHECK_ACTOR)
            {
                DRAW_ACTOR(*_drawBatch);
            }
        }
    }
    else
    {
        // Generic case
        FOR_EACH_BATCH_ACTOR
            e.Bounds.Center -= view.Origin;
            if (CHECK_ACTOR)
            {
                DRAW_ACTOR(*_drawBatch);
            }
        }
    }
}

#undef FOR_EACH_BATCH_ACTOR
#undef CHECK_ACTOR
#undef DRAW_ACTOR
