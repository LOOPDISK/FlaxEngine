// Copyright (c) Wojciech Figat. All rights reserved.

#define SCENE_RENDERING_USE_PROFILER_PER_ACTOR 0

#include "SceneRendering.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUBuffer.h"
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

FORCE_INLINE  bool SceneRendering::CheckVisibility(Actor* actor, int32 index, const BoundingSphere& bounds, const Array<BoundingFrustum>& frustums)
{
    if (NonMainFrustumsListCull(bounds, frustums, _mainViewPosition, _shadowCullRadius, _shadowCullDistance2))
    {
        // Visible through a shadow frustum: keep drawing (shadow casters can't be HZB-culled by the main pyramid).
        if (_drawCull)
            actor->_cullType = 0;
        return true;
    }
    if (MainFrustumCull(bounds, frustums))
    {
        actor->_cullType = 0;
        if (_drawCull && !_drawCull->TestVisibility(index))
        {
            actor->_cullType = 2;
            return false;
        }
        return true;
    }
    if (_drawCull)
        actor->_cullType = 1;
    return false;
}
FORCE_INLINE  bool SceneRendering::CheckVisibility(Actor* actor, int32 index, const BoundingSphere& bounds, const BoundingFrustum& frustum)
{
    if (frustum.Intersects(bounds))
    {
        actor->_cullType = 0;
        if (_drawCull && !_drawCull->TestVisibility(index))
        {
            actor->_cullType = 2;
            return false;
        }
        return true;
    }
    if (_drawCull)
        actor->_cullType = 1;
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

    // HZB cull eligibility: occlusion enabled, single frustum (no shadow contexts), main task has a pyramid,
    // AND this is the actual camera GBuffer pass - not a depth-only collection (shadow CSM, static-shadow
    // clipmap, weapon depth). The static-shadow clipmap creates its own single-context batch using the
    // sun's view, which slips the frustumsCount gate; applying HZB to it would cull casters by the main
    // camera's pyramid (camera-occluded geometry erroneously vanishes from the cached sun depth, and the
    // per-frame HZB churn dirties the clipmap on every camera move).
    _drawCull = nullptr;
    if (Graphics::OcclusionCulling && frustumsCount == 1 && EnumHasAnyFlags(view.Pass, DrawPass::GBuffer))
    {
        SceneRenderTask* mainTask = renderContextBatch.GetMainContext().Task;
        HZBData* pyramid = mainTask ? mainTask->OcclusionInfo : nullptr;
        if (pyramid)
        {
            const int32 cat = (int32)category;
            HZBCullSlot* slot = pyramid->GetOrCreateConsumer(this, cat);
            _drawCull = slot;
            renderContextBatch.GetMainContext().Cull = slot;

            // Schedule bounds CPU refresh + cull dispatch to fire after the draw-collection job
            // sync, inside DrainDelayedDraws (Renderer.cpp). Lambda captures lifetime-safe pointers:
            // - this (SceneRendering): lives > frame
            // - slot: lives > frame (pyramid owns it)
            // - pyramid: lives > frame (task owns it)
            const uint32 count = (uint32)_drawListSize;
            SceneRendering* self = this;
            renderContextBatch.GetMainContext().List->AddDelayedDraw(
                [self, cat, slot, pyramid, count](GPUContext*, RenderContextBatch&, int32)
                {
                    self->RefreshDirtyBoundsCpu(cat);
                    slot->Dispatch(pyramid, self->_boundsCpu[cat].Get(), count);
                });
        }
    }

    // Draw all visual components
#if PLATFORM_THREADS_LIMIT > 1
    if (_drawListSize >= 64 && category == SceneDrawAsync && renderContextBatch.EnableAsync)
    {
        // Run in async via Job System
        Function<void(int32)> func;
        func.Bind<SceneRendering, &SceneRendering::DrawActorsJob>(this);
        _drawJobCount = JobSystem::GetThreadsCount();
        const int64 waitLabel = JobSystem::Dispatch(func, _drawJobCount);
        renderContextBatch.WaitLabels.Add(waitLabel);
    }
    else
#endif
    {
        // Scene is small so draw on a main-thread
        _drawJobCount = 1;
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
    // Drop any HZB occlusion-cull slots keyed by this SceneRendering across all pyramids. Slots are
    // keyed by the raw `this` pointer; a later scene reusing this recycled allocation would otherwise
    // inherit our stale visibility verdicts and cull its geometry (the "meshes vanish until replay" bug).
    HierarchialZBufferPass::ReleaseConsumer(this);

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
    MarkBoundsDirty(category, key);
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
            if (flags & ISceneRenderingListener::NoCulling)
                e.NoCulling = a->_drawNoCulling;
            if (flags & ISceneRenderingListener::Bounds)
            {
                e.Bounds = a->GetSphere();
                MarkBoundsDirty(category, key);
            }
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
            e.Bounds = BoundingSphere(Vector3::Zero, 0.0f);
            MarkBoundsDirty(category, key);
            FreeActors[category].Add(key);
        }
    }
    key = -1;
}

#define FOR_EACH_BATCH_ACTOR  for (int index = i; index < _drawListSize; index += _drawJobCount) { auto e = _drawListData[index];
#define CHECK_ACTOR ((view.RenderLayersMask.Mask & e.LayerMask) && (e.NoCulling || CheckVisibility(e.Actor, index, e.Bounds, _drawFrustumsData)))
#define CHECK_ACTOR_SINGLE_FRUSTUM ((view.RenderLayersMask.Mask & e.LayerMask) && (e.NoCulling || CheckVisibility(e.Actor, index, e.Bounds, view.CullingFrustum)))
#if SCENE_RENDERING_USE_PROFILER_PER_ACTOR
#define DRAW_ACTOR(mode) PROFILE_CPU_ACTOR(e.Actor); e.Actor->Draw(mode)
#else
#define DRAW_ACTOR(mode) e.Actor->Draw(mode)
#endif

void SceneRendering::DrawActorsJob(int32 i)
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

SceneRendering::~SceneRendering()
{
}

void SceneRendering::MarkBoundsDirty(int32 category, int32 key)
{
    if (key < 0)
        return;
    Array<bool>& dirty = _slotDirty[category];
    if (key >= dirty.Count())
        dirty.Resize(key + 1);
    dirty.Get()[key] = true;
}

void SceneRendering::RefreshDirtyBoundsCpu(int32 category)
{
    Array<Float4>& cpu = _boundsCpu[category];
    const DrawActor* actors = Actors[category].Get();
    const int32 actorCount = Actors[category].Count();

    // Grow CPU mirror to actor count; new tail entries must be re-uploaded so mark them dirty.
    if (cpu.Count() < actorCount)
    {
        Array<bool>& dirty = _slotDirty[category];
        const int32 prev = cpu.Count();
        cpu.Resize(actorCount);
        if (dirty.Count() < actorCount)
            dirty.Resize(actorCount);
        bool* d = dirty.Get();
        for (int32 i = prev; i < actorCount; i++)
            d[i] = true;
    }

    Array<bool>& dirty = _slotDirty[category];
    const int32 scanCount = Math::Min(dirty.Count(), actorCount);
    bool* d = dirty.Get();
    Float4* dst = cpu.Get();
    for (int32 i = 0; i < scanCount; i++)
    {
        if (!d[i])
            continue;
        if (actors[i].Actor == nullptr)
        {
            // Vacated slot: zero. CS treats radius<=0 as visible.
            dst[i] = Float4::Zero;
        }
        else
        {
            const BoundingSphere& s = actors[i].Bounds;
            dst[i] = Float4((float)s.Center.X, (float)s.Center.Y, (float)s.Center.Z, (float)s.Radius);
        }
        d[i] = false;
    }
}
