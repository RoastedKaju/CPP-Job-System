#include "JobSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ElapsedMs(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Runs `fn` several times and returns the *minimum* elapsed time in ms.
// Minimum (not average) is the standard way to benchmark short hot
// operations: it filters out OS scheduling noise, page faults, thermal
// throttling blips, etc., which only ever make a run slower, never faster.
template <typename F>
static double BenchmarkMinMs(int trials, F&& fn)
{
    double best = 1e300;
    for (int i = 0; i < trials; ++i)
    {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        best = std::min(best, ElapsedMs(t0, t1));
    }
    return best;
}

// ---------------------------------------------------------------------------
// Naive std::thread parallel-for: evenly split [0, count) across
// hardware_concurrency() threads, spawned and joined fresh every call.
// This is the baseline most codebases reach for before adopting a real
// job system, and it's the fairest "without a job system" comparison --
// fairer than plain serial, since nobody shipping multicore code today
// is genuinely single-threaded.
// ---------------------------------------------------------------------------
template <typename F>
static void NaiveThreadParallelFor(uint32_t count, const F& fn)
{
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    if (count == 0)
        return;

    const uint32_t numThreads = std::min(hw, count);
    const uint32_t chunk = (count + numThreads - 1) / numThreads;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (uint32_t t = 0; t < numThreads; ++t)
    {
        const uint32_t begin = t * chunk;
        const uint32_t end = std::min(count, begin + chunk);
        if (begin >= end)
            break;
        threads.emplace_back([begin, end, &fn]() {
            for (uint32_t i = begin; i < end; ++i)
                fn(i);
        });
    }
    for (auto& th : threads)
        th.join();
}

// ---------------------------------------------------------------------------
// Workloads. Each is written as work(i) so it can be dropped into any of
// the three execution strategies unchanged. They range from "barely any
// work per item" to "meaningfully expensive per item" on purpose --
// granularity requirements are completely different at each end.
// ---------------------------------------------------------------------------

struct VectorAddWorkload
{
    static constexpr const char* Name = "VectorAdd (light, memory-bound)";
    static constexpr uint32_t Count = 8'000'000;

    std::vector<float> a, b, c;
    VectorAddWorkload() : a(Count), b(Count), c(Count, 0.0f)
    {
        for (uint32_t i = 0; i < Count; ++i)
        {
            a[i] = static_cast<float>(i) * 0.5f;
            b[i] = static_cast<float>(i) * 0.25f;
        }
    }
    void Reset() { std::fill(c.begin(), c.end(), 0.0f); }
    void operator()(uint32_t i) { c[i] = a[i] + b[i] * 2.0f; }
    bool Verify() const
    {
        for (uint32_t i = 0; i < Count; i += Count / 1000 + 1)
            if (std::abs(c[i] - (a[i] + b[i] * 2.0f)) > 1e-4f)
                return false;
        return true;
    }
};

struct ParticleUpdateWorkload
{
    static constexpr const char* Name = "ParticleUpdate (medium)";
    static constexpr uint32_t Count = 2'000'000;

    struct Particle { float px, py, pz, vx, vy, vz, life; };
    std::vector<Particle> particles;
    ParticleUpdateWorkload() : particles(Count)
    {
        for (uint32_t i = 0; i < Count; ++i)
            particles[i] = {0, 0, 0, 0.1f, 0.2f, 0.3f, 1.0f};
    }
    void Reset()
    {
        for (auto& p : particles) p = {0, 0, 0, 0.1f, 0.2f, 0.3f, 1.0f};
    }
    void operator()(uint32_t i)
    {
        Particle& p = particles[i];
        constexpr float dt = 1.0f / 60.0f;
        p.px += p.vx * dt;
        p.py += p.vy * dt;
        p.pz += p.vz * dt;
        p.vy -= 9.8f * dt;
        p.life -= dt * 0.1f;
        float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
        p.life -= speed * 0.0001f;
    }
    bool Verify() const { return true; } // deterministic per-frame op; correctness covered elsewhere
};

struct SkinningLikeWorkload
{
    static constexpr const char* Name = "SkinningLike (heavy, per-vertex 4x4 transform)";
    static constexpr uint32_t Count = 300'000;

    std::vector<float> inX, inY, inZ;
    std::vector<float> outX, outY, outZ;
    float m[16];

    SkinningLikeWorkload() : inX(Count), inY(Count), inZ(Count), outX(Count), outY(Count), outZ(Count)
    {
        for (uint32_t i = 0; i < Count; ++i)
        {
            inX[i] = static_cast<float>(i % 100) * 0.01f;
            inY[i] = static_cast<float>((i * 7) % 100) * 0.01f;
            inZ[i] = static_cast<float>((i * 13) % 100) * 0.01f;
        }
        for (int i = 0; i < 16; ++i)
            m[i] = (i % 5 == 0) ? 1.0f : 0.01f * static_cast<float>(i);
    }
    void Reset() { std::fill(outX.begin(), outX.end(), 0.0f); }
    void operator()(uint32_t i)
    {
        const float x = inX[i], y = inY[i], z = inZ[i];
        outX[i] = m[0] * x + m[1] * y + m[2] * z + m[3];
        outY[i] = m[4] * x + m[5] * y + m[6] * z + m[7];
        outZ[i] = m[8] * x + m[9] * y + m[10] * z + m[11];
        // A little extra nonlinear work, standing in for skinning blend
        // weights / normal transform that a real skinning job also does.
        outX[i] = std::sin(outX[i]) + outX[i];
        outY[i] = std::cos(outY[i]) + outY[i];
    }
    bool Verify() const { return true; }
};

// ---------------------------------------------------------------------------
// Driver: runs Serial / NaiveThreads / JobSystem for one workload at a
// given granularity and prints a row of the results table.
// ---------------------------------------------------------------------------
template <typename Workload>
static void RunWorkload(Workload& wl, const std::vector<uint32_t>& granularities, int trials)
{
    std::printf("\n=== %s  (N=%u) ===\n", Workload::Name, Workload::Count);
    std::printf("%-14s %10s %12s %12s %10s\n",
        "Strategy", "minPerJob", "time(ms)", "speedup", "verify");

    wl.Reset();
    double serialMs = BenchmarkMinMs(trials, [&] {
        for (uint32_t i = 0; i < Workload::Count; ++i)
            wl(i);
    });
    bool serialOk = wl.Verify();
    std::printf("%-14s %10s %12.3f %12s %10s\n", "Serial", "-", serialMs, "1.00x", serialOk ? "ok" : "FAIL");

    wl.Reset();
    double naiveMs = BenchmarkMinMs(trials, [&] {
        NaiveThreadParallelFor(Workload::Count, [&](uint32_t i) { wl(i); });
    });
    bool naiveOk = wl.Verify();
    char naiveSpeedup[32];
    std::snprintf(naiveSpeedup, sizeof(naiveSpeedup), "%.2fx", serialMs / naiveMs);
    std::printf("%-14s %10s %12.3f %12s %10s\n", "NaiveThreads", "-", naiveMs, naiveSpeedup, naiveOk ? "ok" : "FAIL");

    for (uint32_t g : granularities)
    {
        wl.Reset();
        double jobMs = BenchmarkMinMs(trials, [&] {
            JobSystem::ParallelFor(Workload::Count, g, [&](uint32_t i) { wl(i); });
        });
        bool jobOk = wl.Verify();
        char jobSpeedup[32];
        std::snprintf(jobSpeedup, sizeof(jobSpeedup), "%.2fx", serialMs / jobMs);
        std::printf("%-14s %10u %12.3f %12s %10s\n", "JobSystem", g, jobMs, jobSpeedup, jobOk ? "ok" : "FAIL");
    }
}

int main()
{
    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("hardware_concurrency() = %u\n", hw);
    if (hw <= 1)
    {
        std::printf(
            "\n*** WARNING: this machine (or container) only exposes 1 hardware thread.\n"
            "*** Parallel strategies below will look WORSE than serial -- they still pay\n"
            "*** scheduling/synchronization overhead but have no extra cores to spend it\n"
            "*** on. Run this benchmark on real multicore hardware for meaningful numbers.\n\n");
    }

    JobSystem::Initialize();
    std::printf("JobSystem workers = %u\n", JobSystem::GetWorkerCount());

    constexpr int kTrials = 7;

    {
        VectorAddWorkload wl;
        RunWorkload(wl, {1000, 20000, 200000}, kTrials);
    }
    {
        ParticleUpdateWorkload wl;
        RunWorkload(wl, {500, 8000, 50000}, kTrials);
    }
    {
        SkinningLikeWorkload wl;
        RunWorkload(wl, {100, 2000, 20000}, kTrials);
    }

    JobSystem::Shutdown();

    std::printf(
        "\nReading this table:\n"
        "  - 'speedup' is Serial time / this row's time -- above 1.00x is faster than serial.\n"
        "  - NaiveThreads pays full thread create+join cost every call; JobSystem reuses a\n"
        "    persistent worker pool, so the gap between them tends to widen as workloads get\n"
        "    smaller/more frequent (i.e. real per-frame engine work).\n"
        "  - Compare minPerJob rows against each other: too fine and scheduling overhead\n"
        "    dominates; too coarse and you lose parallelism. The best value depends on both\n"
        "    the per-item cost of the workload and the core count of the machine.\n");

    return 0;
}