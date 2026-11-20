#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Graph/Platform.h"

namespace wg {

// A fixed pool of threads that performs a batch of positional reads; run() returns when all are done, and throws
// the first read error of the batch.
class IoPool {
   public:
    struct Req {
        const RandomAccessFile* file;
        uint64_t offset;
        size_t len;
        char* dst;
    };

    explicit IoPool(unsigned n) : size_(n) {
        threads_.reserve(n);
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { work(); });
    }
    IoPool(const IoPool&) = delete;
    IoPool& operator=(const IoPool&) = delete;
    ~IoPool() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
    }  // then threads_, declared last, joins the workers

    void run(std::span<const Req> reqs) {
        if (reqs.empty()) return;
        std::unique_lock lk(m_);
        job_ = reqs;
        next_ = 0;
        finished_ = 0;
        ++gen_;
        cv_.notify_all();
        cv_done_.wait(lk, [&] { return finished_ == size_; });
        if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    }

   private:
    /// Read one request. Only the last block of a file may be short (it is padded with zeros); a short read before
    /// the end the file had when it was opened means the file was truncated or replaced underneath us.
    static void read(const Req& r) {
        const size_t got = r.file->read_at(r.dst, r.len, r.offset);
        if (got == r.len) return;
        if (r.offset + got < r.file->size())
            throw std::runtime_error("the graph files changed on disk while open; reopen the graph");
        std::memset(r.dst + got, 0, r.len - got);
    }

    void work() {
        uint64_t seen = 0;
        for (;;) {
            std::span<const Req> job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                job = job_;
            }
            std::exception_ptr error;
            for (size_t i; (i = next_.fetch_add(1)) < job.size();) {
                try {
                    read(job[i]);
                } catch (...) {
                    if (!error) error = std::current_exception();
                }
            }
            std::lock_guard lk(m_);
            if (error && !error_) error_ = error;
            if (++finished_ == size_) cv_done_.notify_one();
        }
    }

    // A plain condition_variable and stop flag rather than condition_variable_any with a stop_token: the workers
    // wake once per BFS level, and its second mutex slowed searches from the SSD (docs/PERF.md)
    const size_t size_;
    std::mutex m_;
    std::condition_variable cv_, cv_done_;
    std::span<const Req> job_;
    std::atomic<size_t> next_{0};
    size_t finished_ = 0;
    std::exception_ptr error_;  // first read error of the current batch
    uint64_t gen_ = 0;
    bool stop_ = false;
    std::vector<std::jthread> threads_;  // last: joined before the members above are destroyed
};

}  // namespace wg
