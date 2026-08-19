#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

std::atomic<int> someValue{42};
std::atomic<bool> locked{false};

void WorkerFunction()
{
    std::cout << "Thread " << std::this_thread::get_id() << " is using Worker Function.\n";
    for (int32_t i = 0; i < 100000; ++i)
    {
        if (i % 1000 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        someValue.fetch_add(1);
    }
}

void StealWork()
{
    while (true)
    {
        // Compare and exchange is the backbone of lock free programming
        // First define an expected value
        bool expected = false;
        // Then compare, if the expected value is equal to the value of atomic then it will succeed
        if (locked.compare_exchange_strong(expected, true))
        {
            std::cout << "Thread " << std::this_thread::get_id() << " Stole the work.\n";
            // Simulate working
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            // After work set this back to false, so other thread can start working
            locked.store(false);
            break;
        }

        // If the compare fails (which it will after the 1st thread is already doing work) the value of expected will be changed to `true`
        // the value of expected is changed to the actual value after calling compare exchange.
        // std::cout << "Status of expected is: " << std::boolalpha << expected << std::endl;
        // If not able to do work then the thread can yield optionally
        std::this_thread::yield();
    }
}

void ExampleOne()
{
    std::thread t1{WorkerFunction};
    std::thread t2{WorkerFunction};

    t1.join();
    t2.join();

    std::cout << "Value is " << someValue << std::endl; // Correct answer should be 200042
}

void ExampleTwo()
{
    std::thread t3{StealWork};
    std::thread t4{StealWork};

    t3.join();
    t4.join();
}

void ExampleThree()
{
    // Atomic doesn't mean lock free, but for 32 bit integers atomic values are lock free
    // you can check it at runtime using the function `is_lock_free` and `is_always_lock_free`
    // Why runtime? well this is due to nondeterminism
    std::atomic<int> x;

    if (x.is_lock_free())
    {
        // likely implemented without a lock
    }

    // std::atomic<int>::is_always_lock_free
}

void ExampleFour()
{
    // std::memory_order_relaxed
    // std::memory_order_acquire
    // std::memory_order_release
    // std::memory_order_acq_rel
    // std::memory_order_seq_cst

    // Think about it like this:
    // Thread 1: "I finished preparing the data."
    // Thread 2 needs to see: Data and the fact that Thread 1 finished.
}

int main()
{
    // ExampleOne();
    ExampleTwo();
    ExampleThree();

    return EXIT_SUCCESS;
}