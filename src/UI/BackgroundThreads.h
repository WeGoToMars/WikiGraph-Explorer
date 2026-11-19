#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace wg::app {

/// Owns the threads the UI starts (downloads, graph build, searches), so none outlives the state it uses.
///
/// Finished threads are joined when the next one starts; join_all() joins the rest. Before calling join_all() the
/// owner must request a stop of the work the jobs watch. A job that throws is logged instead of terminating the
/// program. The threads are std::jthreads: one is never destroyed unjoined, even when an exception unwinds.
class BackgroundThreads {
   public:
    BackgroundThreads() = default;
    BackgroundThreads(const BackgroundThreads&) = delete;
    BackgroundThreads& operator=(const BackgroundThreads&) = delete;
    ~BackgroundThreads() { join_all(); }

    void start(std::function<void()> job) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard lk(mutex_);
        reap_locked();
        threads_.push_back({std::jthread([task = std::move(job), done] {
                                try {
                                    task();
                                } catch (const std::exception& e) {
                                    spdlog::error("Background task failed: {}", e.what());
                                } catch (...) {
                                    spdlog::error("Background task failed with an unknown exception");
                                }
                                done->store(true);
                            }),
                            done});
    }

    /// Join every thread, including any started while joining.
    void join_all() {
        for (;;) {
            std::list<Entry> batch;
            {
                std::lock_guard lk(mutex_);
                batch.swap(threads_);
            }
            if (batch.empty()) return;
            for (auto& e : batch) e.thread.join();
        }
    }

    /// Threads that have not finished yet.
    [[nodiscard]] size_t running() const {
        std::lock_guard lk(mutex_);
        size_t n = 0;
        for (const auto& e : threads_) n += e.done->load() ? 0 : 1;
        return n;
    }

   private:
    struct Entry {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void reap_locked() {
        for (auto it = threads_.begin(); it != threads_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutable std::mutex mutex_;
    std::list<Entry> threads_;
};

}  // namespace wg::app
