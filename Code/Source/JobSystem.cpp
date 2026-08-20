#include "JobSystem.hpp"

#include <cassert>
#include <memory>
#include <random>

namespace
{
    // Per-thread job pool: a flat ring buffer of jobs
    // Only owning thread ever allocates from its own pool, allocation needs no synchronization at all

    constexpr std::size_t kJobPoolSize = 65536; // Power of two
    constexpr std::size_t kJobPoolMask = kJobPoolSize - 1;
    static_assert((kJobPoolSize & kJobPoolMask) == 0, "Job pool size must be power of two");

    constexpr std::size_t kQueueCapacity = 2048; // Power of two

    using Queue = WorkStealingQueue<Job *, kQueueCapacity>;

    thread_local Job tJobPool[kJobPoolSize];
    thread_local std::size_t tJobPoolIndex = 0;
    thread_local uint32_t tWorkerIndex = 0;
    thread_local Queue *tQueue = nullptr;
    thread_local std::mt19937 tRNG{std::random_device{}()};

    std::vector<std::unique_ptr<Queue>> gQueues;
    std::vector<std::thread> gThreads;
    std::atomic<bool> gRunning{false};
    uint32_t gWorkerCount = 0;

    uint32_t RandomOtherWorker(uint32_t self, uint32_t count) noexcept
    {
        if (count <= 1)
        {
            return self;
        }

        std::uniform_int_distribution<uint32_t> distance(0, count - 2);
        uint32_t random = distance(tRNG);
        return (random >= self) ? random + 1 : random;
    }
} // namespace

void JobSystem::Initialize(uint32_t numThreads)
{
    if (numThreads == 0)
    {
        const uint32_t HW = std::thread::hardware_concurrency();
        numThreads = (HW > 1) ? HW : 1;
    }

    gWorkerCount = numThreads;
    gQueues.reserve(numThreads);
    for (uint32_t i = 0; i < numThreads; ++i)
    {
        gQueues.emplace_back(std::make_unique<Queue>());
    }

    gRunning.store(true, std::memory_order_relaxed);

    // Worker 0 is the calling (main) thread
    tWorkerIndex = 0;
    tQueue = gQueues[0].get();

    gThreads.reserve(numThreads - 1);
    for (uint32_t i = 1; i < numThreads; ++i)
    {
        gThreads.emplace_back(&JobSystem::WorkerThreadMain, i);
    }
}

void JobSystem::Shutdown()
{
    gRunning.store(false, std::memory_order_relaxed);
    for (auto &thread : gThreads)
    {
        thread.join();
    }
    gThreads.clear();
    gQueues.clear();
    gWorkerCount = 0;
}

uint32_t JobSystem::GetWorkerCount() noexcept
{
    return gWorkerCount;
}

uint32_t JobSystem::GetWorkerIndex() noexcept
{
    return tWorkerIndex;
}

Job *JobSystem::CreateJob(JobFunction function) noexcept
{
    Job *job = AllocateJob();
    job->function = function;
    job->parent = nullptr;
    job->unfinishedJobCount.store(1, std::memory_order_relaxed);
    return job;
}

Job *JobSystem::CreateJobAsChild(Job *parent, JobFunction function) noexcept
{
    parent->unfinishedJobCount.fetch_add(1, std::memory_order_relaxed);

    Job *job = AllocateJob();
    job->function = function;
    job->parent = parent;
    job->unfinishedJobCount.store(1, std::memory_order_relaxed);
    return job;
}

void JobSystem::Schedule(Job *job) noexcept
{
    assert(tQueue != nullptr && "Schedule() called from a thread that isn't part of job system");

    if (!tQueue->Push(job))
    {
        Execute(job);
    }
}

void JobSystem::Wait(const Job *job) noexcept
{
    while (!IsFinished(job))
    {
        Job *next = GetJobToExecute();
        if (next != nullptr)
        {
            Execute(next);
        }
        else
        {
            std::this_thread::yield();
        }
    }
}

bool JobSystem::IsFinished(const Job *job) noexcept
{
    return job->unfinishedJobCount.load(std::memory_order_acquire) == 0;
}

Job *JobSystem::AllocateJob() noexcept
{
    Job *job = &tJobPool[tJobPoolIndex & kJobPoolMask];

    assert((tJobPoolIndex < kJobPoolSize || job->unfinishedJobCount.load(std::memory_order_acquire) == 0) && "Job Pool exhausted");
    ++tJobPoolIndex;
    return job;
}

Job *JobSystem::GetJobToExecute() noexcept
{
    Job *job = tQueue->Pop();
    if (job != nullptr)
    {
        return job;
    }

    if (gWorkerCount <= 1)
    {
        return nullptr;
    }

    const uint32_t victim = RandomOtherWorker(tWorkerIndex, gWorkerCount);
    return gQueues[victim]->Steal();
}

void JobSystem::Execute(Job *job) noexcept
{
    job->function(job, job->payload);
    Finish(job);
}

void JobSystem::Finish(Job *job) noexcept
{
    const int32_t unfinished = job->unfinishedJobCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (unfinished == 0 && job->parent != nullptr)
    {
        Finish(job->parent);
    }
}

void JobSystem::WorkerThreadMain(uint32_t workerIndex)
{
    tWorkerIndex = workerIndex;
    tQueue = gQueues[workerIndex].get();

    while (gRunning.load(std::memory_order_relaxed))
    {
        Job *job = GetJobToExecute();
        if (job != nullptr)
        {
            Execute(job);
        }
        else
        {
            std::this_thread::yield();
        }
    }
}
