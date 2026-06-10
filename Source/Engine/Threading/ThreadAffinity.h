// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"

// [PinMain] Reserves a dedicated physical CPU core for the latency-critical main thread on hybrid
// CPUs (Intel P/E) and keeps the job workers off it, so the serial update/physics/draw thread never
// shares a HyperThread sibling or spills onto a slow E-core. No-op on small/unknown topologies.
namespace ThreadAffinity
{
    // A/B master switch (default true). Set false + rebuild for stock engine behavior.
    extern bool Enabled;

    // Builds the core-reservation plan from the OS topology. Call once, on the main thread, after
    // Platform::Init (CPUInfo ready) and before the job/worker pools spawn.
    void Init();

    // Affinity mask of the physical core reserved for the main thread, or 0 if none reserved.
    uint64 GetMainThreadMask();

    // Affinity mask of the core reserved for the FMOD audio mixer/studio-update threads, or 0 if none.
    // Exported so the FMOD plugin (separate module) can pin its real-time threads off the worker pool.
    FLAXENGINE_API uint64 GetAudioCoreMask();

    // Right-sized job worker count (logical cores minus the reserved core), or 0 to mean "use default".
    int32 GetWorkerCount();

    // Single-core affinity mask a job worker with the given index should pin to, excluding the
    // reserved core. Returns 0 to mean "leave default" (caller keeps its own pinning).
    uint64 GetWorkerMask(int32 workerIndex);

    // Pins the CURRENT thread (call from the main thread) to the reserved core and disables EcoQoS.
    void PinMainThread();
}
