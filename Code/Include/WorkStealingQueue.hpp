/**
 * A fixed-capicity, lock-free Chase-Lev work stealing deque.
 *
 * Owning thread calls Push/Pop on the bottom(LIFO order)
 * Other thread calls Steal on the top, so thieves takes the oldest work first.
 *
 * Fixed capacity, this never allocates during a frame. Capacity must be a power of two.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

inline constexpr std::size_t kCacheLineSize = 64;

template <typename T, std::size_t Capacity = 4096>
class WorkStealingQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two.");
    static_assert(std::is_pointer_v<T>, "Work stealing queue is designed to store pointers.");

public:
    WorkStealingQueue() noexcept
    {
        for (auto &slot : mBuffer)
        {
            slot.store(nullptr, std::memory_order_relaxed);
        }
    }

    WorkStealingQueue(const WorkStealingQueue &) = delete;
    WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

    /**
     * Memory order relaxed means fetching atomic value without synchronization with other threads
     * Safe in cases where thread only needs the local value of bottom to calculate where to insert.
     * Acquire ensures that any writes made by other threads before they update top are visible to owning thread.
     * This prevents reading stale data when checking if queue is full.
     */
    bool Push(T item) noexcept
    {
        // relaxed because this thread only needs the local value of bottom to calculate where to insert.
        const int64_t bottom = mBottom.load(std::memory_order_relaxed);
        // acquire will ensure all the writes made by other threads are visible to owning thread
        const int64_t top = mTop.load(std::memory_order_acquire);

        if (bottom - top >= static_cast<uint64_t>(Capacity) - 1)
        {
            return false; // Full
        }

        // Writing item into buffer doesn't need ordering because the visiblity is controlled by release on bottom
        // The index is just Index % Capacity, but since modulo is expensive we replace it with Index & (Capacity - 1)
        mBuffer[static_cast<std::size_t>(bottom) & kMask].store(item, std::memory_order_relaxed);

        // Release here ensures that the item written into buffer is visible to other threads befor they see the updated bottom.
        // You can also do fetch add (1, release) here but since only owner thread will be accessing this function, store is far cheaper
        mBottom.store(bottom + 1, std::memory_order_release);
        return true;
    }

    // Only ever called by owning thread, LIFO: pops the most recently pushed item.
    // This is almost always still hot in cache
    T Pop() noexcept
    {
        int64_t bottom = mBottom.load(std::memory_order_relaxed) - 1;

        mBottom.store(bottom, std::memory_order_seq_cst);
        int64_t top = mTop.load(std::memory_order_seq_cst);

        if (top <= bottom)
        {
            T item = mBuffer[static_cast<std::size_t>(bottom) & kMask].load(std::memory_order_relaxed);
            if (top == bottom)
            {
                if (!mTop.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    item = nullptr; // lost race to thief
                }
                mBottom.store(bottom + 1, std::memory_order_relaxed);
            }
            return item;
        }

        // Queue was already empty
        mBottom.store(bottom + 1, std::memory_order_relaxed);
        return nullptr;
    }

    T Steal() noexcept
    {
        int64_t top = mTop.load(std::memory_order_seq_cst);
        int64_t bottom = mBottom.load(std::memory_order_seq_cst);

        if (top < bottom)
        {
            T item = mBuffer[static_cast<std::size_t>(top) & kMask].load(std::memory_order_relaxed);
            if (!mTop.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                return nullptr; // lost race to another thief or owner
            }

            return item;
        }

        return nullptr;
    }

    bool Empty() const noexcept
    {
        const int64_t bottom = mBottom.load(std::memory_order_relaxed);
        const int64_t top = mTop.load(std::memory_order_relaxed);

        return bottom <= top;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // top and bottom are hammered by different threads so they get their own cache lines
    // this avoids false sharing between them.
    alignas(kCacheLineSize) std::atomic<int64_t> mTop{0};
    alignas(kCacheLineSize) std::atomic<int64_t> mBottom{0};
    alignas(kCacheLineSize) std::array<std::atomic<T>, Capacity> mBuffer;
};
