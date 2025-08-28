#pragma once

#ifndef PARALLEL_DECOMPRESSION

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "UI/UIBase.h"
#include <zlib.h>

/**
 * @class AsyncLineReader
 * @brief Asynchronous line reader for gzip files using zlib C API.
 *
 * Provides a background thread that reads lines from a compressed or
 * uncompressed file and exposes byte-level progress compatible with the UI.
 */
class AsyncLineReader {
   public:
    /**
     * @brief Constructs an async line reader
     * @param file WikiFile descriptor (compressed or uncompressed)
     */
    explicit AsyncLineReader(const WikiFile& file);

    /**
     * @brief Destructor that ensures proper cleanup
     */
    ~AsyncLineReader();

    /**
     * @brief Retrieve the next available line.
     * @param line Output parameter receiving the line
     * @return true if a line was produced, false on end of file
     */
    bool get_line(std::string& line);

    /**
     * @brief Get current read progress in compressed bytes.
     * @return ReadProgress structure with total and current bytes
     */
    ReadProgress get_progress();

   private:
    static constexpr size_t MAX_QUEUE_SIZE = 10;

    // File information
    WikiFile file_{};
    uint64_t total_bytes_;
    std::atomic<uint64_t> current_pos_{0};

    // zlib stream
    gzFile gz_file_ = nullptr;

    // Threading
    std::thread reader_thread_;
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;

    /**
     * @brief Background worker that reads lines and enqueues them.
     */
    void read_lines();

    /**
     * @brief Initialize the input stream
     */
    void initialize_reader();

    /**
     * @brief Calculate total bytes for progress tracking
     */
    void calculate_total_bytes();
};
#endif
