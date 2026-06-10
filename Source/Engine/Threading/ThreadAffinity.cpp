// Copyright (c) Wojciech Figat. All rights reserved.

#include "ThreadAffinity.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/CPUInfo.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

namespace ThreadAffinity
{
    bool Enabled = true; // A/B: false = stock baseline build; true = pin main thread
}

namespace
{
    bool Initialized = false;
    uint64 MainMask = 0; // reserved physical-core mask for the main thread (0 = no-op)
    uint64 AudioMask = 0; // reserved core mask for the FMOD audio mixer (0 = none)
    Array<int32> AllowedLogical; // logical core indices workers may use (reserved cores excluded)
    Array<uint32> AllowedCpuSetIds; // CPU-Set ids of the non-reserved cores (process default)
    Array<uint32> ReservedCpuSetIds; // CPU-Set ids of the reserved core (main thread selects these)
}

#if PLATFORM_WINDOWS

#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"

namespace
{
    // One physical core: its logical sibling bitmask (group 0) and OS efficiency class (higher = faster, i.e. P-core).
    struct PhysicalCore
    {
        uint64 Mask;
        int32 Efficiency;
    };

    // Non-SMT core = a single logical processor (E/LP-E core), i.e. no HyperThread sibling to contend.
    bool IsSingleLogical(uint64 mask) { return mask != 0 && (mask & (mask - 1)) == 0; }

    bool EnumeratePhysicalCores(Array<PhysicalCore>& cores)
    {
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0)
            return false;
        Array<byte> buffer;
        buffer.Resize((int32)length);
        auto info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.Get();
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
            return false;
        DWORD offset = 0;
        while (offset < length)
        {
            auto rec = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.Get() + offset);
            if (rec->Relationship == RelationProcessorCore && rec->Processor.GroupCount >= 1 && rec->Processor.GroupMask[0].Group == 0)
            {
                PhysicalCore core;
                core.Mask = (uint64)rec->Processor.GroupMask[0].Mask;
                core.Efficiency = (int32)rec->Processor.EfficiencyClass;
                cores.Add(core);
            }
            offset += rec->Size;
        }
        return cores.HasItems();
    }

    uint32 CpuSetIdByLogical[64] = {};
    bool HasCpuSet[64] = {};

    // Map each logical processor (group 0) to its OS CPU-Set id. Needed for SetProcessDefaultCpuSets /
    // SetThreadSelectedCpuSets, the MS-recommended way to dedicate cores without hard affinity.
    bool BuildCpuSetMap()
    {
        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0)
            return false;
        Array<byte> buffer;
        buffer.Resize((int32)length);
        if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buffer.Get(), length, &length, GetCurrentProcess(), 0))
            return false;
        bool any = false;
        ULONG offset = 0;
        while (offset < length)
        {
            auto rec = (PSYSTEM_CPU_SET_INFORMATION)(buffer.Get() + offset);
            if (rec->Type == CpuSetInformation && rec->CpuSet.Group == 0)
            {
                const int32 lp = (int32)rec->CpuSet.LogicalProcessorIndex;
                if (lp >= 0 && lp < 64)
                {
                    CpuSetIdByLogical[lp] = rec->CpuSet.Id;
                    HasCpuSet[lp] = true;
                    any = true;
                }
            }
            offset += rec->Size;
        }
        return any;
    }
}

#endif

void ThreadAffinity::Init()
{
    if (Initialized)
        return;
    Initialized = true;

    // A/B toggle: flip ThreadAffinity::Enabled to false + rebuild for stock behavior.
    if (!Enabled)
        return;

    const int32 logical = (int32)Platform::GetCPUInfo().LogicalProcessorCount;
    // Too few cores to spare one - reserving would starve the workers. Behave exactly like stock.
    if (logical <= 4 || logical > 64)
        return;

#if PLATFORM_WINDOWS
    Array<PhysicalCore> cores;
    if (!EnumeratePhysicalCores(cores) || cores.Count() < 2)
        return;

    // [PinMain] Dump the real OS topology - the P/E/LP-E map was previously guessed from timings and
    // got it wrong. mask = logical siblings (2 bits => HT/P-core, 1 bit => non-SMT E/LP-E), eff = OS
    // EfficiencyClass (higher = faster tier). Confirms which core the audio pick lands on.
    for (int32 i = 0; i < cores.Count(); i++)
        LOG(Info, "[PinMain] phys core {0}: mask 0x{1:x} ({2}), efficiency {3}", i, cores[i].Mask,
            IsSingleLogical(cores[i].Mask) ? TEXT("non-SMT") : TEXT("SMT"), cores[i].Efficiency);

    // Pick the reserved physical core: prefer the fastest class (P-core), avoid the core that owns
    // logical 0 (soaks OS/driver DPCs+ISRs), and among ties prefer the highest core index.
    int32 best = -1;
    for (int32 i = 0; i < cores.Count(); i++)
    {
        if (cores[i].Mask & 1ull) // owns logical 0
            continue;
        if (best == -1 ||
            cores[i].Efficiency > cores[best].Efficiency ||
            (cores[i].Efficiency == cores[best].Efficiency && cores[i].Mask > cores[best].Mask))
            best = i;
    }
    if (best == -1) // every core owns logical 0 only happens with 1 core - already excluded
        return;

    const uint64 reserved = cores[best].Mask; // main thread P-core

    // Reserve a second core for the FMOD audio mixer when there's headroom. The mixer is a light
    // real-time thread, so an uncontended NON-SMT core (E-core, no HT sibling) is the ideal home.
    uint64 audio = 0;
    if (logical >= 8)
    {
        int32 ab = -1;
        for (int32 i = 0; i < cores.Count(); i++)
        {
            if (cores[i].Mask == reserved || (cores[i].Mask & 1ull))
                continue;
            if (ab == -1) { ab = i; continue; }
            const bool iSolo = IsSingleLogical(cores[i].Mask);
            const bool bSolo = IsSingleLogical(cores[ab].Mask);
            if (iSolo != bSolo) { if (iSolo) ab = i; continue; } // prefer non-SMT (E-core)
            if (cores[i].Efficiency != cores[ab].Efficiency) { if (cores[i].Efficiency > cores[ab].Efficiency) ab = i; continue; } // E over LP-E
            if (cores[i].Mask > cores[ab].Mask) ab = i; // tie -> higher index
        }
        if (ab != -1)
            audio = cores[ab].Mask;
    }

    // Worker-allowed list = every logical core except the reserved main + audio cores.
    const uint64 reservedAll = reserved | audio;
    uint64 full = 0;
    for (int32 i = 0; i < cores.Count(); i++)
        full |= cores[i].Mask;
    for (int32 bit = 0; bit < 64; bit++)
        if ((full & (1ull << bit)) && !(reservedAll & (1ull << bit)))
            AllowedLogical.Add(bit);
    if (AllowedLogical.Count() < 2) // need workers; bail rather than starve them
    {
        AllowedLogical.Clear();
        return;
    }

    MainMask = reserved;
    AudioMask = audio;
    const bool hybrid = cores[best].Efficiency > 0;
    LOG(Info, "[PinMain] Reserved main core 0x{0:x} ({1}); audio core 0x{2:x}; {3} job workers on the rest",
        reserved, hybrid ? TEXT("P-core") : TEXT("core"), audio, AllowedLogical.Count());

    // Isolate the reserved cores via CPU-Sets: confine EVERY other thread (pools, net, mono, FMOD
    // load/stream, and any thread spawned later) to the non-reserved cores, so the main core stays
    // private to the main thread and the audio core stays private to the FMOD mixer. Hard affinity
    // (JobSystem workers, FMOD mixer) still wins over this, landing exactly where we pinned it.
    if (BuildCpuSetMap())
    {
        for (int32 i = 0; i < AllowedLogical.Count(); i++)
            if (HasCpuSet[AllowedLogical[i]])
                AllowedCpuSetIds.Add(CpuSetIdByLogical[AllowedLogical[i]]);
        for (int32 bit = 0; bit < 64; bit++)
            if ((reserved & (1ull << bit)) && HasCpuSet[bit])
                ReservedCpuSetIds.Add(CpuSetIdByLogical[bit]);
        if (AllowedCpuSetIds.HasItems() && ReservedCpuSetIds.HasItems())
        {
            SetProcessDefaultCpuSets(GetCurrentProcess(), (const ULONG*)AllowedCpuSetIds.Get(), (ULONG)AllowedCpuSetIds.Count());
            LOG(Info, "[PinMain] CPU-Sets: process default -> {0} non-reserved cores; reserved core left private for main", AllowedCpuSetIds.Count());
        }
        else
        {
            AllowedCpuSetIds.Clear();
            ReservedCpuSetIds.Clear();
        }
    }
#endif
}

uint64 ThreadAffinity::GetMainThreadMask()
{
    return MainMask;
}

uint64 ThreadAffinity::GetAudioCoreMask()
{
    return AudioMask;
}

int32 ThreadAffinity::GetWorkerCount()
{
    return MainMask ? AllowedLogical.Count() : 0;
}

uint64 ThreadAffinity::GetWorkerMask(int32 workerIndex)
{
    if (!MainMask || AllowedLogical.IsEmpty())
        return 0;
    return 1ull << AllowedLogical[workerIndex % AllowedLogical.Count()];
}

void ThreadAffinity::PinMainThread()
{
    if (!MainMask)
        return;

#if PLATFORM_WINDOWS
    // Prefer CPU-Sets (select the reserved core; the process default keeps everything else off it).
    // Don't combine with hard affinity - affinity intersected with the process-default set could be empty.
    if (ReservedCpuSetIds.HasItems())
        SetThreadSelectedCpuSets(GetCurrentThread(), (const ULONG*)ReservedCpuSetIds.Get(), (ULONG)ReservedCpuSetIds.Count());
    else
        Platform::SetThreadAffinityMask(MainMask); // fallback: hard pin when CPU-Sets unavailable
#if defined(THREAD_POWER_THROTTLING_EXECUTION_SPEED)
    // Disable EcoQoS so Windows never demotes the main thread onto a slow E-core for power saving.
    THREAD_POWER_THROTTLING_STATE state = {};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0; // 0 => run at full performance
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#endif
#else
    Platform::SetThreadAffinityMask(MainMask);
#endif
    LOG(Info, "[PinMain] Main thread bound to reserved core (mask 0x{0:x}), EcoQoS disabled", MainMask);
}
