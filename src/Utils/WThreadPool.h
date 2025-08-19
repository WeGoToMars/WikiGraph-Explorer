#pragma once

#ifdef PARALLEL_DECOMPRESSION
// Based on https://github.com/progschj/ThreadPool, modified to use modern C++ and moodycamel::ConcurrentQueue

// TODO: Use std::jthread and std::stop_source once Apple Clang supports them...
// https://en.cppreference.com/w/cpp/compiler_support.html#:~:text=std%3A%3Astop_token%20and%20std%3A%3Ajthread%C2%A0%20(FTM)*

#include <concurrentqueue.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

/**
 * Thread pool implementation using moodycamel::ConcurrentQueue for task management.
 * Provides efficient task scheduling with exception handling and graceful shutdown.
 */
class WThreadPool {
   public:
    /**
     * Constructs a thread pool with the specified number of worker threads.
     * @param threads Number of worker threads to create
     */
    inline WThreadPool(size_t threads);
    
    /**
     * Enqueues a task for execution by the thread pool.
     * @param func Function to execute
     * @param args Arguments to pass to the function
     * @return Future containing the result of the task
     */
    template <class F, class... Args>
    [[nodiscard]]
    auto enqueue(F&& func, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;
    
    /**
     * Destructor that gracefully shuts down all worker threads.
     */
    inline ~WThreadPool();

    // Disable copy and move operations
    WThreadPool(const WThreadPool&) = delete;
    WThreadPool& operator=(const WThreadPool&) = delete;
    WThreadPool(WThreadPool&&) = delete;
    WThreadPool& operator=(WThreadPool&&) = delete;

   private:
    std::vector<std::thread> workers;  // Worker threads
    moodycamel::ConcurrentQueue<std::function<void()>> tasks;  // Task queue
    std::atomic<bool> stop{false};  // Stop flag for graceful shutdown
};

// Constructor: launches worker threads
inline WThreadPool::WThreadPool(size_t threads) {
    for (size_t i = 0; i < threads; i++) {
        workers.emplace_back([this] {
            std::function<void()> task;
            while (true) {
                // Try to dequeue a task from the concurrent queue
                if (tasks.try_dequeue(task)) {
                    try {
                        task();
                    } catch (const std::exception& e) {
                        spdlog::error("Task exception in WThreadPool: {}", e.what());
                    }
                } else {
                    // No task available, check if the processing has finished and thread should stop
                    if (stop.load()) {
                        return;
                    }
                    // Yield to other threads to avoid busy waiting
                    std::this_thread::yield();
                }
            }
        });
    }
}

// Enqueue a new task
template <class F, class... Args>
[[nodiscard]]
auto WThreadPool::enqueue(F&& func, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    // Create packaged task with forwarding of arguments
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [task_func = std::forward<F>(func), ...task_args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(task_func), std::move(task_args)...);
        }
    );

    // Get the future from the packaged task
    std::future<return_type> res = task->get_future();

    // Check if the thread pool is stopped and throw an exception if it is
    if (stop.load()) {
        throw std::runtime_error("enqueue on stopped WThreadPool");
    }

    // Enqueue the task to the concurrent queue
    tasks.enqueue([task]() { (*task)(); });

    return res;
}

// Destructor: gracefully shut down all threads
inline WThreadPool::~WThreadPool() {
    stop.store(true);
    
    // Wait for all threads to finish
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}
#endif
