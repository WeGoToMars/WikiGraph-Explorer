#pragma once

#ifndef PARALLEL_DECOMPRESSION

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "UI/UIBase.h"
#include "zstr.hpp"

/**
 * @class AsyncLineReader
 * @brief Asynchronous line reader for gzip files using zstr.
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
     * @brief Gets the next line from the file (same interface as AsyncLineReader)
     * @param line Reference to string where the line will be stored
     * @return True if a line was read, false if end of file
     */
    /**
     * @brief Retrieve the next available line.
     * @param line Output parameter receiving the line
     * @return true if a line was produced, false on end of file
     */
    bool get_line(std::string& line);

    /**
     * @brief Gets the current read progress (same interface as AsyncLineReader)
     * @return ReadProgress structure with total and current bytes
     */
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

    // Only keep fallback components
    std::unique_ptr<zstr::ifstream> zstr_stream_;

    // Threading
    std::thread reader_thread_;
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;

    /**
     * @brief Worker function that reads lines in background thread
     */
    /**
     * @brief Background worker that reads lines and enqueues them.
     */
    void read_lines();

    /**
     * @brief Initializes the reader (tries memory-mapping first, then fallback)
     */
    /**
     * @brief Initialize the input stream (may fallback if memory mapping fails).
     */
    void initialize_reader();

    /**
     * @brief Calculate total bytes for progress tracking
     */
    /**
     * @brief Determine total bytes of the input for progress tracking.
     */
    void calculate_total_bytes();
};
#endif
