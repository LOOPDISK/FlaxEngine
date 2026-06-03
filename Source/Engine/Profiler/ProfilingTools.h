// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#if COMPILE_WITH_PROFILER

#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/MemoryStats.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Profiler/Profiler.h"

class GPUBuffer;

/// <summary>
/// Profiler tools for development. Allows to gather profiling data and events from the engine.
/// </summary>
API_CLASS(Static) class FLAXENGINE_API ProfilingTools
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(ProfilingTools);
public:
    /// <summary>
    /// The GPU memory stats.
    /// </summary>
    API_STRUCT(NoDefault) struct MemoryStatsGPU
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(MemoryStatsGPU);

        /// <summary>
        /// The total amount of memory in bytes (as reported by the driver).
        /// </summary>
        API_FIELD() uint64 Total;

        /// <summary>
        /// The used by the game amount of memory in bytes (estimated).
        /// </summary>
        API_FIELD() uint64 Used;
    };

    /// <summary>
    /// Engine profiling data header. Contains main info and stats.
    /// </summary>
    API_STRUCT(NoDefault) struct MainStats
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(MainStats);

        /// <summary>
        /// The process memory stats.
        /// </summary>
        API_FIELD() ProcessMemoryStats ProcessMemory;

        /// <summary>
        /// The CPU memory stats.
        /// </summary>
        API_FIELD() MemoryStats MemoryCPU;

        /// <summary>
        /// The GPU memory stats.
        /// </summary>
        API_FIELD() MemoryStatsGPU MemoryGPU;

        /// <summary>
        /// The frames per second (fps counter).
        /// </summary>
        API_FIELD() int32 FPS;

        /// <summary>
        /// The update time on CPU (in milliseconds).
        /// </summary>
        API_FIELD() float UpdateTimeMs;

        /// <summary>
        /// The fixed update time on CPU (in milliseconds).
        /// </summary>
        API_FIELD() float PhysicsTimeMs;

        /// <summary>
        /// The draw time on CPU (in milliseconds).
        /// </summary>
        API_FIELD() float DrawCPUTimeMs;

        /// <summary>
        /// The draw time on GPU (in milliseconds).
        /// </summary>
        API_FIELD() float DrawGPUTimeMs;

        /// <summary>
        /// The last rendered frame stats.
        /// </summary>
        API_FIELD() RenderStatsData DrawStats;
    };

    /// <summary>
    /// The CPU thread stats.
    /// </summary>
    API_STRUCT(NoDefault) struct ThreadStats
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(ThreadStats);

        /// <summary>
        /// The thread name.
        /// </summary>
        API_FIELD() String Name;

        /// <summary>
        /// The events list.
        /// </summary>
        API_FIELD() Array<ProfilerCPU::Event> Events;
    };

    /// <summary>
    /// The network stat.
    /// </summary>
    API_STRUCT(NoDefault) struct NetworkEventStat
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(NetworkEventStat);

        // Amount of occurrences.
        API_FIELD() uint16 Count;
        // Transferred data size (in bytes).
        API_FIELD() uint16 DataSize;
        // Transferred message (data+header) size (in bytes).
        API_FIELD() uint16 MessageSize;
        // Amount of peers that will receive this message.
        API_FIELD() uint16 Receivers;
        API_FIELD(Private, NoArray) byte Name[120];
    };

public:
    /// <summary>
    /// Controls the engine profiler (CPU, GPU, etc.) usage.
    /// </summary>
    API_PROPERTY(Attributes="DebugCommand") static bool GetEnabled();

    /// <summary>
    /// Controls the engine profiler (CPU, GPU, etc.) usage.
    /// </summary>
    API_PROPERTY() static void SetEnabled(bool enabled);

    /// <summary>
    /// The current collected main stats by the profiler from the local session. Updated every frame.
    /// </summary>
    API_FIELD(ReadOnly) static MainStats Stats;

    /// <summary>
    /// The CPU threads profiler events.
    /// </summary>
    API_FIELD(ReadOnly) static Array<ThreadStats, InlinedAllocation<64>> EventsCPU;

    /// <summary>
    /// The GPU rendering profiler events.
    /// </summary>
    API_FIELD(ReadOnly) static Array<ProfilerGPU::Event> EventsGPU;

    /// <summary>
    /// The networking profiler events.
    /// </summary>
    API_FIELD(ReadOnly) static Array<NetworkEventStat> EventsNetwork;

    // CPU-event recording ring. Records per-frame CPU thread events into a native ring buffer
    // so a long capture does not marshal EventsCPU to managed each frame (~400KB/frame) - that
    // churn was causing GC stalls that perturbed the captured frames. Stats are small POD and
    // GPU events carry transient name pointers, so callers keep those per-frame themselves;
    // only the heavy CPU events live here. Marshal recorded frames at dump time. 0 = oldest.

    /// <summary>Begins recording up to maxFrames of CPU events into a native ring (alloc-free per frame).</summary>
    API_FUNCTION() static void BeginRecording(int32 maxFrames);

    /// <summary>Stops recording. Buffered frames stay readable until the next BeginRecording.</summary>
    API_FUNCTION() static void EndRecording();

    /// <summary>True while a recording is active.</summary>
    API_PROPERTY() static bool IsRecording();

    /// <summary>Count of frames buffered in the recording ring (0..maxFrames).</summary>
    API_PROPERTY() static int32 GetRecordedFrameCount();

    /// <summary>CPU thread events for buffered frame i (0 = oldest); marshaled on access.</summary>
    API_FUNCTION() static Array<ThreadStats> GetRecordedEventsCPU(int32 i);

    // Shadow caster profiling. Off by default (zero cost). When enabled, RenderShadowMaps
    // brackets its draws and RenderList tallies per-model triangles/draws submitted to the
    // shadow depth passes (CSM cascades, clipmap, atlas, weapon). The model name comes from
    // the index buffer debug name; needs GPU resource naming (non-release). Indirect/GPU-driven
    // draws are skipped - their triangle count is not CPU-visible. Flattened into ShadowCasters
    // each frame; read at dump time to point artists at the heaviest shadow geometry.

    /// <summary>Per-model shadow caster cost for one frame.</summary>
    API_STRUCT(NoDefault) struct ShadowCasterStat
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(ShadowCasterStat);

        /// <summary>Model asset path (from the index buffer name), or empty when unnamed.</summary>
        API_FIELD() String Name;

        /// <summary>Total triangles submitted to shadow depth passes this frame.</summary>
        API_FIELD() uint64 Triangles;

        /// <summary>Draw submissions (batches) this frame.</summary>
        API_FIELD() int32 DrawCalls;

        /// <summary>Total instances drawn this frame.</summary>
        API_FIELD() int32 Instances;
    };

    /// <summary>Enables per-model shadow caster profiling (small per-frame cost while on).</summary>
    API_PROPERTY() static bool GetShadowCasterProfiling();
    API_PROPERTY() static void SetShadowCasterProfiling(bool enabled);

    /// <summary>Per-model shadow caster costs, triangles desc. Valid while profiling is enabled.</summary>
    API_FIELD(ReadOnly) static Array<ShadowCasterStat> ShadowCasters;

    // Native renderer hooks (not for scripting). ShadowTallyActive gates the per-draw tally.
    static bool ShadowTallyActive;
    static void TallyShadowCaster(GPUBuffer* indexBuffer, int32 triangles, int32 instances);

    // RAII bracket: ShadowsPass wraps its submit scope so only shadow draws are tallied.
    struct ShadowTallyScope
    {
        ShadowTallyScope();
        ~ShadowTallyScope();
    };
};

#endif
