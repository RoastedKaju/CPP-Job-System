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
    
}
