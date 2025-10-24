// Copyright (c) Wojciech Figat. All rights reserved.

#include "JobSystem.h"
#include <atomic>
#include "IRunnable.h"
#include "Engine/Platform/CPUInfo.h"
#include "Engine/Platform/Thread.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/Pair.h"
#include "Engine/Core/Memory/SimpleHeapAllocation.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/RingBuffer.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "ConcurrentQueue.h"
#if USE_CSHARP
#include "Engine/Scripting/ManagedCLR/MCore.h"
#endif

#define JOB_SYSTEM_ENABLED 1

#if JOB_SYSTEM_ENABLED

// Local allocator for job system memory that uses internal pooling and assumes that JobsLocker is taken (write access owned by the calling thread).
class JobSystemAllocation : public SimpleHeapAllocation<JobSystemAllocation>
{
public:
    static void* Allocate(uintptr size);
    static void Free(void* ptr, uintptr size);
};

class JobSystemService : public EngineService
{
public:
    JobSystemService()
        : EngineService(TEXT("JobSystem"), -800)
    {
    }

    bool Init() override;
    void BeforeExit() override;
    void Dispose() override;
};

struct JobData
{
    int32 Index;
    int64 JobKey;
    Function<void(int32)> Job;
};

struct JobContext
{
    std::atomic<int64> JobsLeft;
    std::atomic<int32> DependenciesLeft;
    Function<void(int32)> Job;
    Array<int64, JobSystemAllocation> Dependants;

    // Default constructor
    JobContext() = default;

    // Move constructor
    JobContext(JobContext&& other) noexcept
        : JobsLeft(other.JobsLeft.load()),
          DependenciesLeft(other.DependenciesLeft.load()),
          Job(std::move(other.Job)),
          Dependants(std::move(other.Dependants))
    {
    }

    // Delete copy constructor
    JobContext(const JobContext& other) = delete;

    // Move assignment operator
    JobContext& operator=(JobContext&& other) noexcept
    {
        if (this != &other)
        {
            JobsLeft.store(other.JobsLeft.load());
            DependenciesLeft.store(other.DependenciesLeft.load());
            Job = std::move(other.Job);
            Dependants = std::move(other.Dependants);
        }
        return *this;
    }

    // Delete copy assignment operator
    JobContext& operator=(const JobContext& other) = delete;
};

template<>
struct TIsPODType<JobContext>
{
    enum { Value = false };
};

class JobSystemThread : public IRunnable
{
public:
    uint64 Index;

public:
    // [IRunnable]
    String ToString() const override
    {
        return TEXT("JobSystemThread");
    }

    int32 Run() override;

    void AfterWork(bool wasKilled) override
    {
        Delete(this);
    }
};

namespace
{
    JobSystemService JobSystemInstance;
    Array<Pair<void*, uintptr>> MemPool;
    Thread* Threads[PLATFORM_THREADS_LIMIT / 2] = {};
    int32 ThreadsCount = 0;
    bool JobStartingOnDispatch = true;
    std::atomic<int64> ExitFlag = 0;
    std::atomic<int64> JobLabel = 0;
    Dictionary<int64, JobContext, JobSystemAllocation> JobContexts;
    ConditionVariable JobsSignal;
    CriticalSection JobsMutex;
    ConditionVariable WaitSignal;
    CriticalSection WaitMutex;
    CriticalSection JobsLocker;
    ConcurrentQueue<JobData> Jobs;
}

void* JobSystemAllocation::Allocate(uintptr size)
{
    void* result = nullptr;
    for (int32 i = 0; i < MemPool.Count(); i++)
    {
        if (MemPool.Get()[i].Second == size)
        {
            result = MemPool.Get()[i].First;
            MemPool.RemoveAt(i);
            break;
        }
    }
    if (!result)
        result = Platform::Allocate(size, 16);
    return result;
}

void JobSystemAllocation::Free(void* ptr, uintptr size)
{
    MemPool.Add({ ptr, size });
}

bool JobSystemService::Init()
{
    ThreadsCount = Math::Min<int32>(Platform::GetCPUInfo().LogicalProcessorCount, ARRAY_COUNT(Threads));
    for (int32 i = 0; i < ThreadsCount; i++)
    {
        auto runnable = New<JobSystemThread>();
        runnable->Index = (uint64)i;
        auto thread = Thread::Create(runnable, String::Format(TEXT("Job System {0}"), i), ThreadPriority::AboveNormal);
        if (thread == nullptr)
            return true;
        Threads[i] = thread;
    }
    return false;
}

void JobSystemService::BeforeExit()
{
    ExitFlag.store(1);
    JobsSignal.NotifyAll();
}

void JobSystemService::Dispose()
{
    ExitFlag.store(1);
    JobsSignal.NotifyAll();
    Platform::Sleep(1);

    for (int32 i = 0; i < ThreadsCount; i++)
    {
        if (Threads[i])
        {
            Threads[i]->Kill(true);
            Delete(Threads[i]);
            Threads[i] = nullptr;
        }
    }

    JobContexts.SetCapacity(0);
    for (auto& e : MemPool)
        Platform::Free(e.First);
    MemPool.Clear();
}

int32 JobSystemThread::Run()
{
    // Thread affinity disabled to allow OS load balancing and reduce lock contention
    // Platform::SetThreadAffinityMask(1ull << Index);

    JobData data;
    bool attachCSharpThread = true;
    while (ExitFlag.load() == 0)
    {
        // Try to get a job
        if (Jobs.try_dequeue(data))
        {
#if USE_CSHARP
            // Ensure to have C# thread attached to this thead (late init due to MCore being initialized after Job System)
            if (attachCSharpThread)
            {
                MCore::Thread::Attach();
                attachCSharpThread = false;
            }
#endif

            // Run job (job function is already stored in JobData, no dictionary lookup needed!)
            data.Job(data.Index);

            // Move forward with the job queue
            bool notifyWaiting = false;
            JobsLocker.Lock();
            JobContext* context = JobContexts.TryGet(data.JobKey);
            if (context && context->JobsLeft.fetch_sub(1) <= 1)
            {
                // Update any dependant jobs
                for (int64 dependant : context->Dependants)
                {
                    if (JobContext* dependantContext = JobContexts.TryGet(dependant))
                    {
                        if (dependantContext->DependenciesLeft.fetch_sub(1) <= 1)
                        {
                            // Dispatch dependency when it's ready
                            JobData dependantData;
                            dependantData.JobKey = dependant;
                            dependantData.Job = dependantContext->Job;
                            for (dependantData.Index = 0; dependantData.Index < dependantContext->JobsLeft; dependantData.Index++)
                                Jobs.enqueue(dependantData);
                        }
                    }
                }

                // Remove completed context
                JobContexts.Remove(data.JobKey);
                notifyWaiting = true;
            }
            JobsLocker.Unlock();
            if (notifyWaiting)
                WaitSignal.NotifyAll();
        }
        else
        {
            // Wait for signal
            PROFILE_CPU_NAMED("JobSystem::WaitForJob");
            JobsMutex.Lock();
            JobsSignal.Wait(JobsMutex);
            JobsMutex.Unlock();
        }
    }
    return 0;
}

#endif

void JobSystem::Execute(const Function<void(int32)>& job, int32 jobCount)
{
#if JOB_SYSTEM_ENABLED
    // TODO: disable async if called on job thread? or maybe Wait should handle waiting in job thread to do the processing?
    if (jobCount > 1)
    {
        // Async
        const int64 jobWaitHandle = Dispatch(job, jobCount);
        Wait(jobWaitHandle);
    }
    else
#endif
    {
        // Sync
        for (int32 i = 0; i < jobCount; i++)
            job(i);
    }
}

int64 JobSystem::Dispatch(const Function<void(int32)>& job, int32 jobCount)
{
    if (jobCount <= 0)
        return 0;
    PROFILE_CPU();
#if JOB_SYSTEM_ENABLED
    const auto label = JobLabel.fetch_add(jobCount) + jobCount;

    JobData data;
            data.JobKey = label;
            data.Job = job;
    
            JobContext context;
            context.Job = job;
            context.JobsLeft.store(jobCount);
            context.DependenciesLeft.store(0);
    
            JobsLocker.Lock();
            JobContexts.Add(label, MoveTemp(context));    JobsLocker.Unlock();

    for (data.Index = 0; data.Index < jobCount; data.Index++)
        Jobs.enqueue(data);

    if (JobStartingOnDispatch)
    {
        if (jobCount == 1)
            JobsSignal.NotifyOne();
        else
            JobsSignal.NotifyAll();
    }

    return label;
#else
    for (int32 i = 0; i < jobCount; i++)
        job(i);
    return 0;
#endif
}

int64 JobSystem::Dispatch(const Function<void(int32)>& job, Span<int64> dependencies, int32 jobCount)
{
    if (jobCount <= 0)
        return 0;
    PROFILE_CPU();
#if JOB_SYSTEM_ENABLED
    const auto label = JobLabel.fetch_add(jobCount) + jobCount;

    JobData data;
    data.JobKey = label;
    data.Job = job;

    JobContext context;
    context.Job = job;
    context.JobsLeft.store(jobCount);
    context.DependenciesLeft.store(0);

    JobsLocker.Lock();
    for (int64 dependency : dependencies)
    {
        if (JobContext* dependencyContext = JobContexts.TryGet(dependency))
        {
            context.DependenciesLeft.fetch_add(1);
            dependencyContext->Dependants.Add(label);
        }
    }
    JobContexts.Add(label, MoveTemp(context));
    if (context.DependenciesLeft == 0)
    {
        // No dependencies left to complete so dispatch now
        for (data.Index = 0; data.Index < jobCount; data.Index++)
            Jobs.enqueue(data);
    }
    JobsLocker.Unlock();

    if (context.DependenciesLeft == 0 && JobStartingOnDispatch)
    {
        if (jobCount == 1)
            JobsSignal.NotifyOne();
        else
            JobsSignal.NotifyAll();
    }

    return label;
#else
    for (int32 i = 0; i < jobCount; i++)
        job(i);
    return 0;
#endif
}

void JobSystem::Wait()
{
#if JOB_SYSTEM_ENABLED
    JobsLocker.Lock();
    int32 numJobs = JobContexts.Count();
    JobsLocker.Unlock();

    while (numJobs > 0)
    {
        WaitMutex.Lock();
        WaitSignal.Wait(WaitMutex, 1);
        WaitMutex.Unlock();

        JobsLocker.Lock();
        numJobs = JobContexts.Count();
        JobsLocker.Unlock();
    }
#endif
}

void JobSystem::Wait(int64 label)
{
#if JOB_SYSTEM_ENABLED
    PROFILE_CPU();

    while (ExitFlag.load() == 0)
    {
        JobsLocker.Lock();
        const JobContext* context = JobContexts.TryGet(label);
        JobsLocker.Unlock();

        // Skip if context has been already executed (last job removes it)
        if (!context)
            break;

        // Wait on signal until input label is not yet done
        WaitMutex.Lock();
        WaitSignal.Wait(WaitMutex, 1);
        WaitMutex.Unlock();

        // Wake up any thread to prevent stalling in highly multi-threaded environment
        JobsSignal.NotifyOne();
    }
#endif
}

void JobSystem::SetJobStartingOnDispatch(bool value)
{
#if JOB_SYSTEM_ENABLED
    JobStartingOnDispatch = value;
    if (value)
    {
        const int32 count = Jobs.size_approx();
        if (count == 1)
            JobsSignal.NotifyOne();
        else if (count != 0)
            JobsSignal.NotifyAll();
    }
#endif
}

int32 JobSystem::GetThreadsCount()
{
#if JOB_SYSTEM_ENABLED
    return ThreadsCount;
#else
    return 0;
#endif
}
