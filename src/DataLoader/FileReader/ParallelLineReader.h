#pragma once

#ifdef PARALLEL_DECOMPRESSION

#include <concurrentqueue.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "UI/UIBase.h"

// Forward declarations to avoid heavy rapidgzip includes in header
namespace rapidgzip {
struct ChunkData;
template <typename T>
class ParallelGzipReader;
class StandardFileReader;
}  // namespace rapidgzip

/**
 * @brief Parallel gzip line reader using rapidgzip for high-throughput decompression.
 *
 * Provides a thread-backed API to read decompressed lines from a compressed
 * Wikipedia dump while tracking byte-level progress for UI updates.
 */
class ParallelLineReader {
   public:
    /**
     * @brief Construct a reader for the given `WikiFile`.
     * @param file Descriptor of the compressed data and optional index paths
     * @param parallelization Number of worker threads (0 = use all cores)
     * @param chunk_size Size of each decompression chunk in bytes
     */
    explicit ParallelLineReader(const WikiFile& file,
                                size_t parallelization = 0,            // 0 means use all cores
                                size_t chunk_size = 4 * 1024 * 1024);  // 4MB chunk size by default NOLINT

    ~ParallelLineReader();

    // Disable copying and moving
    ParallelLineReader(const ParallelLineReader&) = delete;
    ParallelLineReader& operator=(const ParallelLineReader&) = delete;
    ParallelLineReader(ParallelLineReader&&) = delete;
    ParallelLineReader& operator=(ParallelLineReader&&) = delete;

    /**
     * @brief Fetch the next decompressed line.
     * @param line Output parameter receiving the next line
     * @return true if a line was produced, false on end of stream
     */
    bool get_line(std::string& line);

    /**
     * @brief Return current read progress in compressed bytes.
     * @return Structure with total and current byte counters
     */
    ReadProgress get_progress();

   private:
    // Maximum number of lines decompressed by default before we start yielding for parser threads
    static constexpr size_t MAX_QUEUE_SIZE = 32;  // 32 lines = 32 MB

    // Store decompressed data in a buffer
    static constexpr size_t READ_BUFFER_SIZE = 2 * 1024 * 1024;  // 2MB read buffer by default

    // Store file information
    WikiFile file_{};
    uint64_t total_bytes_ = 0;

    // Progress tracking in bytes
    std::atomic<uint64_t> current_pos_{0};

    std::unique_ptr<rapidgzip::ParallelGzipReader<rapidgzip::ChunkData>> rapidgzip_reader_;
    std::unique_ptr<rapidgzip::StandardFileReader> file_reader_;

    // Index handling
    std::filesystem::path resolved_index_path_{};

    // Parallelism coordination
    std::thread reader_thread_;
    std::atomic<bool> done_{false};
    moodycamel::ConcurrentQueue<std::string> queue_;
    std::atomic<bool> thread_started_{false};
    std::mutex start_mutex_;

    // Store rapidgzip reader settings
    size_t parallelization_;
    size_t chunk_size_;

    /**
     * @brief Determine the total compressed size for progress reporting.
     */
    void calculate_total_bytes();

    /**
     * @brief Initialize rapidgzip readers and resolve index paths.
     */
    void initialize_reader();

    /**
     * @brief Reader worker function that decompresses and enqueues lines.
     */
    void read_lines();

    /**
     * @brief Convert a decompressed chunk into newline-delimited lines and enqueue them.
     * @param chunk_data Shared pointer to decompressed block data
     * @param offset_in_block Byte offset within the block to start processing
     * @param data_size Number of bytes to read from the chunk
     * @param line_buffer Scratch buffer for assembling partial lines across chunks
     */
    void process_chunk_data(const std::shared_ptr<rapidgzip::ChunkData>& chunk_data, size_t offset_in_block,
                            size_t data_size, std::string& line_buffer);

    /**
     * @brief Resolve the on-disk path to the rapidgzip index file.
     * @param data_path Path to the compressed data file
     * @param index_path_hint Optional explicit index path
     * @return Resolved index file path
     */
    std::filesystem::path resolve_index_path(const std::filesystem::path& data_path,
                                             const std::filesystem::path& index_path_hint) const;
};
#endif
