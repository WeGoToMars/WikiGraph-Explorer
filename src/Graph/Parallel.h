#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace wg {

/// Call fn(0) ... fn(n - 1) on `threads` threads (the calling one included), in any order.
///
/// If a call throws, the remaining indexes are skipped and the first exception is rethrown once every thread has
/// stopped.
template <class Fn>
    requires std::invocable<Fn&, size_t>
void parallel_for(size_t n, unsigned threads, Fn&& fn) {
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto work = [&] {
        for (size_t i; !failed.load(std::memory_order_relaxed) && (i = next.fetch_add(1)) < n;) {
            try {
                fn(i);
            } catch (...) {
                std::lock_guard lk(error_mutex);
                if (!error) error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
            }
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
    }  // joins the pool
    if (error) std::rethrow_exception(error);
}

}  // namespace wg
