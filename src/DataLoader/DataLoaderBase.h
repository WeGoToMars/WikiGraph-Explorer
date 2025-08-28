#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <string>

#ifdef PARALLEL_DECOMPRESSION
#include "DataLoader/FileReader/ParallelLineReader.h"
using ReaderType = ParallelLineReader;
#include <concurrentqueue.h>

#include "Utils/WThreadPool.h"
#else
#include "DataLoader/FileReader/AsyncLineReader.h"
using ReaderType = AsyncLineReader;
#endif

#include <filesystem>

#include "UI/UIBase.h"

// Base class for all data loaders with common progress callback functionality
class DataLoaderBase {
   public:
    using ProgressCallback = std::function<void(size_t, double, ReadProgress)>;

   protected:
    /**
     * @brief Initialize the underlying line reader for the given wiki file.
     * @param file Wiki file descriptor used to construct the reader
     */
    void init_reader(const WikiFile& file) {
        reader_ = std::make_unique<ReaderType>(file);
        reader_file_path_ = file.data_path;
    }

    /**
     * @brief Conditionally invoke the progress callback based on elapsed time.
     * @param count Number of parsed records so far
     * @param callback Progress callback to invoke
     * @param reader Reader used to query byte progress
     * @param start_time Process start time for speed calculation
     * @param last_time Last time the callback was invoked (updated in-place)
     * @param refresh_rate Minimum interval between callbacks
     * @param force If true, trigger the callback regardless of interval (used for final progress update)
     */
    static void update_progress(size_t count, const ProgressCallback& callback, ReaderType& reader,
                                std::chrono::steady_clock::time_point start_time,
                                std::chrono::steady_clock::time_point& last_time,
                                std::chrono::milliseconds refresh_rate, bool force = false) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time);

        // Compute speed based on total time since start
        auto since_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
        double seconds = static_cast<double>(since_start_ms.count()) / 1000.0;
        double speed = static_cast<double>(count) / std::max(seconds, 1e-6);

        if (force || elapsed_ms >= refresh_rate) {
            if (callback) {
                callback(count, speed, reader.get_progress());
            }
            last_time = now;
        }
    }

    /**
     * @brief Parse only INSERT INTO lines and dispatch results, optionally in parallel.
     * @tparam ParseFn Callable: Result(const std::string&)
     * @tparam OnResultFn Callable: void(const Result&)
     * @tparam OnFirstFn Callable: void(const Result&)
     * @param reader Line reader supplying input
     * @param parse_fn Parser for a single INSERT line
     * @param on_result Consumer invoked for every parsed result
     * @param on_first Consumer invoked once with the first result
     * @param max_concurrent Max concurrent parse tasks (parallel mode only)
     */
    template <typename ParseFn, typename OnResultFn, typename OnFirstFn>
    void parse_insert_lines(ReaderType& reader, ParseFn parse_fn, OnResultFn on_result, OnFirstFn on_first) {
        std::string line;
        bool is_first_emitted = true;

#ifdef PARALLEL_DECOMPRESSION
        moodycamel::ConcurrentQueue<std::future<decltype(parse_fn(line))>> futures;
        moodycamel::ConsumerToken token(futures);
        WThreadPool pool(max_concurrent);
        const size_t max_futures = max_concurrent * 2;

        auto drain_one = [&] {
            std::future<decltype(parse_fn(line))> fut;
            if (futures.try_dequeue(token, fut)) {
                auto res = fut.get();
                if (is_first_emitted) {
                    on_first(res);
                    is_first_emitted = false;
                }
                on_result(res);
                return true;
            }
            return false;
        };

        while (reader.get_line(line)) {
            if (!line.starts_with("INSERT INTO")) continue;
            futures.enqueue(pool.enqueue(parse_fn, line));

            // Backpressure: keep the futures queue bounded
            if (futures.size_approx() > max_futures) {
                // Blockingly process at least one future result
                drain_one();
            }
        }
        // Final drain
        while (drain_one()) {
        }
#else
        while (reader.get_line(line)) {
            if (!line.starts_with("INSERT INTO")) continue;
            auto res = parse_fn(line);
            if (is_first_emitted) {
                on_first(res);
                is_first_emitted = false;
            }
            on_result(res);
        }
#endif
    }

    /**
     * @brief Overload when no first-result handler is needed.
     */
    template <typename ParseFn, typename OnResultFn>
    void parse_insert_lines(ReaderType& reader, ParseFn parse_fn, OnResultFn on_result, size_t max_concurrent = 4) {
        parse_insert_lines(reader, parse_fn, on_result, [](const auto&) {}, max_concurrent);
    }

    std::unique_ptr<ReaderType> reader_;      // NOLINT (cppcoreguidelines-non-private-member-variables-in-classes)
    std::filesystem::path reader_file_path_;  // NOLINT (cppcoreguidelines-non-private-member-variables-in-classes)
};
