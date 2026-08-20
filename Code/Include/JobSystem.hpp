#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

#include "WorkStealingQueue.hpp"

using JobFunction = void (*)(struct Job *, const void *);

/**
 * Job
 *
 * Sized to be exactly one cache line (64 bytes)
 * Any data job needs either fits in the payload or is referenced through pointer
 */
struct alignas(kCacheLineSize) Job
{
    static constexpr std::size_t kPayloadSize = kCacheLineSize - sizeof(JobFunction) - sizeof(Job *) - sizeof(std::atomic<int32_t>);

    JobFunction function;                    // executed as function(this, payload)
    Job *parent;                             // nullptr for root jobs
    std::atomic<int32_t> unfinishedJobCount; // self + all children still pending
    uint8_t payload[kPayloadSize];
};

static_assert(sizeof(Job) == kCacheLineSize, "Job must be exactly one cache line");

/**
 * Job System
 */
class JobSystem
{
public:
    // Spins up worker threads, pass `0` to use hardware concurrency instead
    static void Initialize(uint32_t numThreads = 0);

    // Signals all workers to stop and join.
    // Safe to call only when all outstanding jobs have completed.
    static void Shutdown();

    static uint32_t GetWorkerCount() noexcept;
    static uint32_t GetWorkerIndex() noexcept;

    // Low-level zero allocation POD job creation
    static Job *CreateJob(JobFunction function) noexcept;
    static Job *CreateJobAsChild(Job *parent, JobFunction function) noexcept;

    template <typename T>
    static Job *CreateJobWithData(JobFunction function, const T &data) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "Job POD payload must be trivially copyable");
        static_assert(sizeof(T) <= Job::kPayloadSize, "Payload too large for a Job; store a pointer instead");

        Job *job = CreateJob(function);
        std::memcpy(job->payload, &data, sizeof(T));
        return job;
    }

    template <typename T>
    static Job *CreateChildJobWithData(Job *parent, JobFunction function, const T &data) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "Job POD payload must be trivially copyable");
        static_assert(sizeof(T) <= Job::kPayloadSize, "Payload too large for a Job; store a pointer instead");

        Job *job = CreateJobAsChild(parent, function);
        std::memcpy(job->payload, &data, sizeof(T));
        return job;
    }

    template <typename F>
    static Job *CreateJob(F &&function) noexcept
    {
        using DecayedFunction = std::decay_t<F>;

        static_assert(sizeof(DecayedFunction) <= Job::kPayloadSize, "Lambda capture too large, try a pointer instead");

        Job *job = AllocateJob();
        job->parent = nullptr;
        job->unfinishedJobCount.store(1, std::memory_order_relaxed);
        ::new (static_cast<void *>(job->payload)) DecayedFunction(std::forward<F>(function));
        job->function = &JobSystem::InvokeLambda<DecayedFunction>;
        return job;
    }

    template <typename F>
    static Job *CreateJobAsChild(Job *parent, F &&function) noexcept
    {
        using DecayedFunction = std::decay_t<F>;

        static_assert(sizeof(DecayedFunction) <= Job::kPayloadSize, "Lambda capture too large, try a pointer instead");

        parent->unfinishedJobCount.fetch_add(1, std::memory_order_relaxed);

        Job *job = AllocateJob();
        job->parent = parent;
        job->unfinishedJobCount.store(1, std::memory_order_relaxed);
        ::new (static_cast<void *>(job->payload)) DecayedFunction(std::forward<F>(function));
        job->function = &JobSystem::InvokeLambda<DecayedFunction>;
        return job;
    }

    static void Schedule(Job *job) noexcept;

    static void Wait(const Job *job) noexcept;

    static bool IsFinished(const Job *job) noexcept;

    template <typename F>
    static void ParallelFor(uint32_t count, uint32_t minCountPerJob, const F &function)
    {
        if (count == 0)
        {
            return;
        }

        if (minCountPerJob == 0)
        {
            minCountPerJob = 1;
        }

        // `function` lives on caller's stack; safe to reference from child jobs
        // Wait() ensure they all complete before parallel for returns
        ParallelForData<F> rootData{0, count, minCountPerJob, &function};
        Job *root = CreateJobWithData(&ParallelForJobEntry<F>, rootData);
        Schedule(root);
        Wait(root);
    }

private:
    JobSystem() = delete;

    static Job *AllocateJob() noexcept;
    static Job *GetJobToExecute() noexcept;
    static void Execute(Job *job) noexcept;
    static void Finish(Job *job) noexcept;

    static void WorkerThreadMain(uint32_t workerIndex);

    template <typename F>
    static void InvokeLambda(Job *job, const void *data)
    {

        F &func = *const_cast<F *>(static_cast<const F *>(data));

        if constexpr (std::is_invocable_v<F, Job *>)
        {
            func(job);
        }
        else
        {
            func();
        }

        if constexpr (!std::is_trivially_destructible_v<F>)
        {
            func.~F();
        }
    }

    template <typename F>
    struct ParallelForData
    {
        uint32_t begin;
        uint32_t count;
        uint32_t minCount;
        const F *function;
    };

    template <typename F>
    static void ParallelForJobEntry(Job *job, const void *rawData)
    {
        const auto *data = static_cast<const ParallelForData<F> *>(rawData);

        if (data->count > data->minCount)
        {
            const uint32_t leftCount = data->count / 2;
            const uint32_t rightCount = data->count - leftCount;

            ParallelForData<F> leftData{data->begin, leftCount, data->minCount, data->function};
            Job *left = CreateChildJobWithData(job, &ParallelForJobEntry<F>, leftData);
            Schedule(left);

            ParallelForData<F> rightData{data->begin + leftCount, rightCount, data->minCount, data->function};
            Job *right = CreateChildJobWithData(job, &ParallelForJobEntry<F>, rightData);
            Schedule(right);
        }
        else
        {
            for (uint32_t i = 0; i < data->count; ++i)
            {
                (*data->function)(data->begin + i);
            }
        }
    }
};