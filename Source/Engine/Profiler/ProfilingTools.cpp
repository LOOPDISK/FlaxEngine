// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_PROFILER

#include "ProfilingTools.h"
#include "Engine/Core/Types/Pair.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/Time.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Networking/NetworkInternal.h"

ProfilingTools::MainStats ProfilingTools::Stats;
Array<ProfilingTools::ThreadStats, InlinedAllocation<64>> ProfilingTools::EventsCPU;
Array<ProfilerGPU::Event> ProfilingTools::EventsGPU;
Array<ProfilingTools::NetworkEventStat> ProfilingTools::EventsNetwork;
Array<ProfilingTools::ShadowCasterStat> ProfilingTools::ShadowCasters;
bool ProfilingTools::ShadowTallyActive = false;

namespace
{
    // One captured frame's CPU events, deep-copied into native memory (no managed heap).
    struct RecordedFrame
    {
        Array<ProfilingTools::ThreadStats, InlinedAllocation<64>> EventsCPU;
    };

    Array<RecordedFrame> RecordRing; // sized once at BeginRecording, reused in place
    int32 RecordHead = 0;
    int32 RecordCount = 0;
    bool Recording = false;

    // Ring slot for the i-th buffered frame (0 = oldest), or -1 if out of range.
    int32 RecordedRingIndex(int32 i)
    {
        const int32 n = RecordRing.Count();
        if (n == 0 || i < 0 || i >= RecordCount)
            return -1;
        const int32 start = (RecordHead - RecordCount + n) % n;
        return (start + i) % n;
    }

    // Shadow caster tally. Keyed by index buffer (one per model mesh); the buffer debug name
    // resolves to the model path at flatten time. Filled during RenderShadowMaps, drained once
    // per frame in Update so ShadowCasters reflects the previous frame's shadow geometry.
    struct ShadowTallyEntry
    {
        uint64 Triangles = 0;
        int32 DrawCalls = 0;
        int32 Instances = 0;
    };
    Dictionary<GPUBuffer*, ShadowTallyEntry> ShadowTally;
    bool ShadowCasterProfilingEnabled = false;

    bool CompareShadowCasterDesc(const ProfilingTools::ShadowCasterStat& a, const ProfilingTools::ShadowCasterStat& b)
    {
        return a.Triangles > b.Triangles;
    }

    // Merge per-buffer entries by resolved model name, sort by triangles desc, cap rows.
    void FlattenShadowTally()
    {
        Dictionary<String, ProfilingTools::ShadowCasterStat> byName;
        for (auto it = ShadowTally.Begin(); it.IsNotEnd(); ++it)
        {
            GPUBuffer* ib = it->Key;
            const ShadowTallyEntry& e = it->Value;
            String name;
            if (ib)
            {
                name = String(ib->GetName());
                if (name.EndsWith(StringView(TEXT(".IB"))))
                    name = name.Substring(0, name.Length() - 3);
            }
            if (!byName.ContainsKey(name))
            {
                // ShadowCasterStat is NoDefault - its POD fields are not zero-inited by operator[].
                ProfilingTools::ShadowCasterStat init;
                init.Name = name;
                init.Triangles = 0;
                init.DrawCalls = 0;
                init.Instances = 0;
                byName[name] = init;
            }
            ProfilingTools::ShadowCasterStat& s = byName[name];
            s.Triangles += e.Triangles;
            s.DrawCalls += e.DrawCalls;
            s.Instances += e.Instances;
        }
        auto& out = ProfilingTools::ShadowCasters;
        out.Clear();
        for (auto it = byName.Begin(); it.IsNotEnd(); ++it)
            out.Add(it->Value);
        Sorting::QuickSort(out.Get(), out.Count(), &CompareShadowCasterDesc);
        const int32 maxRows = 64;
        if (out.Count() > maxRows)
            out.Resize(maxRows);
        ShadowTally.Clear();
    }
}

class ProfilingToolsService : public EngineService
{
public:
    ProfilingToolsService()
        : EngineService(TEXT("Profiling Tools"))
    {
        Platform::MemoryClear(&ProfilingTools::Stats, sizeof(ProfilingTools::MainStats));
    }

    void Update() override;
    void Dispose() override;
};

ProfilingToolsService ProfilingToolsServiceInstance;

void ProfilingToolsService::Update()
{
    ZoneScoped;
    PROFILE_MEM(Profiler);

    // Capture stats
    {
        auto& stats = ProfilingTools::Stats;

        stats.ProcessMemory = Platform::GetProcessMemoryStats();
        stats.MemoryCPU = Platform::GetMemoryStats();
        stats.MemoryGPU.Total = GPUDevice::Instance->TotalGraphicsMemory;
        stats.MemoryGPU.Used = GPUDevice::Instance->GetMemoryUsage();
        stats.FPS = Engine::GetFramesPerSecond();

        stats.UpdateTimeMs = static_cast<float>(Time::Update.LastLength * 1000.0);
        stats.PhysicsTimeMs = static_cast<float>(Time::Physics.LastLength * 1000.0);
        stats.DrawCPUTimeMs = static_cast<float>(Time::Draw.LastLength * 1000.0);

        float presentTime;
        ProfilerGPU::GetLastFrameData(stats.DrawGPUTimeMs, presentTime, stats.DrawStats);
        stats.DrawCPUTimeMs = Math::Max(stats.DrawCPUTimeMs - presentTime, 0.0f); // Remove swapchain present wait time to exclude from drawing on CPU
    }

    // Extract CPU profiler events
    Platform::MemoryBarrier();
    const auto& threads = ProfilerCPU::Threads;
    Platform::MemoryBarrier();
    for (auto& pt : ProfilingTools::EventsCPU)
        pt.Events.Clear();
    ProfilingTools::EventsCPU.EnsureCapacity(threads.Count());
    for (int32 i = 0; i < threads.Count(); i++)
    {
        ProfilerCPU::Thread* thread = threads[i];
        if (thread == nullptr)
            continue;
        ProfilingTools::ThreadStats* pt = nullptr;
        for (auto& e : ProfilingTools::EventsCPU)
        {
            if (e.Name == thread->GetName())
            {
                pt = &e;
                break;
            }
        }
        if (!pt)
        {
            pt = &ProfilingTools::EventsCPU.AddOne();
            pt->Name = thread->GetName();
        }

        thread->Buffer.Extract(pt->Events, true);
    }

#if 0
    // Print CPU threads events to the log
    for (auto& pt : ProfilingTools::EventsCPU)
    {
        auto& events = pt.Events;
        if (events.HasItems())
        {
            LOG_FLOOR();
            LOG(Info, "Thread: {0}", pt.Name);
            for (int j = 0; j < events.Count(); j++)
            {
                auto e = events[j];
                String prev;
                for (int d = 0; d < e.Depth; d++)
                    prev += TEXT("\t");
                LOG(Warning, "{2}{0}, Time: {1} ms", e.Name, ((int)((e.End - e.Start) * 1000.0f) / 1000.0f), prev);
            }
            LOG(Info, "");
            LOG_FLOOR();
        }
    }
#endif

    // Get the last resolved GPU frame events
    ProfilingTools::EventsGPU.Clear();
    uint64 maxFrame = 0;
    int32 maxFrameIndex = -1;
    auto& frames = ProfilerGPU::Buffers;
    for (uint32 i = 0; i < ARRAY_COUNT(frames); i++)
    {
        if (frames[i].HasData() && frames[i].FrameIndex > maxFrame)
        {
            maxFrame = frames[i].FrameIndex;
            maxFrameIndex = i;
        }
    }
    if (maxFrameIndex != -1)
    {
        auto& frame = frames[maxFrameIndex];
        frame.Extract(ProfilingTools::EventsGPU);
    }

    // Get the last events from networking runtime
    {
        auto& networkEvents = ProfilingTools::EventsNetwork;
        networkEvents.Resize(NetworkInternal::ProfilerEvents.Count());
        int32 i = 0;
        for (const auto& e : NetworkInternal::ProfilerEvents)
        {
            const auto& src = e.Value;
            auto& dst = networkEvents[i++];
            dst.Count = src.Count;
            dst.DataSize = src.DataSize;
            dst.MessageSize = src.MessageSize;
            dst.Receivers = src.Receivers;
            const StringAnsiView& typeName = e.Key.First.GetType().Fullname;
            uint64 len = Math::Min<uint64>(typeName.Length(), ARRAY_COUNT(dst.Name) - 10);
            Platform::MemoryCopy(dst.Name, typeName.Get(), len);
            const StringAnsiView& name = e.Key.Second;
            if (name.HasChars())
            {
                uint64 pos = len;
                dst.Name[pos++] = ':';
                dst.Name[pos++] = ':';
                len = Math::Min<uint64>(name.Length(), ARRAY_COUNT(dst.Name) - pos - 1);
                Platform::MemoryCopy(dst.Name + pos, name.Get(), len);
                dst.Name[pos + len] = 0;
            }
            else
            {
                dst.Name[len] = 0;
            }
        }
        NetworkInternal::ProfilerEvents.Clear();
    }

    // Frame recording: deep-copy this frame's CPU events into the native ring (no managed
    // alloc). Slots are reused in place, so steady-state copies hit existing capacity.
    if (Recording && RecordRing.Count() > 0)
    {
        RecordRing[RecordHead].EventsCPU = ProfilingTools::EventsCPU;
        RecordHead = (RecordHead + 1) % RecordRing.Count();
        if (RecordCount < RecordRing.Count())
            RecordCount++;
    }

    // Drain the shadow caster tally filled by the previous frame's RenderShadowMaps. Done here
    // (Update phase, before this frame's Draw) so the dict is never read and written at once.
    if (ShadowCasterProfilingEnabled)
        FlattenShadowTally();

#if 0
    // Print CPU events to the log
    {
        if (ProfilingTools::EventsCPU.HasItems())
        {
            LOG_FLOOR();
            LOG(Info, "CPU");
            for (auto& pt : ProfilingTools::EventsCPU)
            {
                LOG(Info, "");
                LOG(Warning, "Thread {0}", pt.Name);
                for (auto& e : pt.Events)
                {
                    String prev;
                    for (int32 d = 0; d < e.Depth; d++)
                        prev += TEXT("\t");
                    const double time = e.End - e.Start;
                    LOG(Warning, "\t{2}{0}, Time: {1} ms", e.Name, ((int32)(time * 1000.0f) / 1000.0f), prev);
                }
            }
            LOG(Info, "");
            LOG_FLOOR();
        }
    }
#endif
#if 0
    // Print GPU events to the log
    {
        auto& events = ProfilingTools::EventsGPU;
        if (events.HasItems())
        {
            LOG_FLOOR();
            LOG(Info, "GPU");
            for (int j = 0; j < events.Count(); j++)
            {
                auto e = events[j];
                String prev;
                for (int d = 0; d < e.Depth; d++)
                    prev += TEXT("\t");
                LOG(Warning, "{2}{0}, Time: {1} ms", e.Name, ((int)(e.Time * 1000.0f) / 1000.0f), prev);
            }
            LOG(Info, "");
            LOG_FLOOR();
        }
    }
#endif
}

void ProfilingToolsService::Dispose()
{
    ProfilingTools::EventsCPU.Clear();
    ProfilingTools::EventsCPU.SetCapacity(0);
    ProfilingTools::EventsGPU.SetCapacity(0);
    ProfilingTools::EventsNetwork.SetCapacity(0);
    Recording = false;
    RecordRing.Resize(0); // destructs per-frame EventsCPU arrays
    RecordRing.SetCapacity(0);
    RecordHead = 0;
    RecordCount = 0;
    ShadowCasterProfilingEnabled = false;
    ProfilingTools::ShadowTallyActive = false;
    ShadowTally.Clear();
    ShadowTally.SetCapacity(0);
    ProfilingTools::ShadowCasters.Resize(0);
    ProfilingTools::ShadowCasters.SetCapacity(0);
}

void ProfilingTools::BeginRecording(int32 maxFrames)
{
    if (maxFrames < 1)
        maxFrames = 1;
    // Size the ring once and clear slot contents; per-frame copies reuse this storage.
    RecordRing.Resize(maxFrames);
    for (auto& f : RecordRing)
        f.EventsCPU.Clear();
    RecordHead = 0;
    RecordCount = 0;
    Recording = true;
}

void ProfilingTools::EndRecording()
{
    Recording = false;
}

bool ProfilingTools::IsRecording()
{
    return Recording;
}

int32 ProfilingTools::GetRecordedFrameCount()
{
    return RecordCount;
}

Array<ProfilingTools::ThreadStats> ProfilingTools::GetRecordedEventsCPU(int32 i)
{
    // Allocator differs from the stored InlinedAllocation type, so copy element-by-element.
    Array<ThreadStats> result;
    const int32 r = RecordedRingIndex(i);
    if (r != -1)
    {
        const auto& src = RecordRing[r].EventsCPU;
        result.Resize(src.Count());
        for (int32 t = 0; t < src.Count(); t++)
            result[t] = src[t];
    }
    return result;
}

bool ProfilingTools::GetShadowCasterProfiling()
{
    return ShadowCasterProfilingEnabled;
}

void ProfilingTools::SetShadowCasterProfiling(bool enabled)
{
    ShadowCasterProfilingEnabled = enabled;
    if (!enabled)
    {
        ShadowTallyActive = false;
        ShadowTally.Clear();
        ShadowCasters.Clear();
    }
}

void ProfilingTools::TallyShadowCaster(GPUBuffer* indexBuffer, int32 triangles, int32 instances)
{
    if (triangles <= 0)
        return;
    instances = Math::Max(instances, 1);
    ShadowTallyEntry& e = ShadowTally[indexBuffer];
    e.Triangles += (uint64)triangles * (uint64)instances;
    e.DrawCalls++;
    e.Instances += instances;
}

ProfilingTools::ShadowTallyScope::ShadowTallyScope()
{
    if (ShadowCasterProfilingEnabled)
        ShadowTallyActive = true;
}

ProfilingTools::ShadowTallyScope::~ShadowTallyScope()
{
    ShadowTallyActive = false;
}

bool ProfilingTools::GetEnabled()
{
    return ProfilerCPU::Enabled && ProfilerGPU::Enabled;
}

void ProfilingTools::SetEnabled(bool enabled)
{
    ProfilerCPU::Enabled = enabled;
    ProfilerGPU::Enabled = enabled;
    ProfilerGPU::EventsEnabled = enabled;
    NetworkInternal::EnableProfiling = enabled;
}

#endif
