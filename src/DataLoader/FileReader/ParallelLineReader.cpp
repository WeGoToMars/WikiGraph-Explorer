#ifdef PARALLEL_DECOMPRESSION

#include "ParallelLineReader.h"

#include <cstring>  // for memchr
#include <filereader/Standard.hpp>
#include <filesystem>
#include <fstream>
#include <rapidgzip/DecodedData.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>
#include <thread>

#include "spdlog/spdlog.h"

ParallelLineReader::ParallelLineReader(const WikiFile& file, size_t parallelization, size_t chunk_size)
    : file_(file), parallelization_(parallelization), chunk_size_(chunk_size) {
    calculate_total_bytes();
    initialize_reader();
    // Resolve index path for loading/saving
    resolved_index_path_ = resolve_index_path(file_.data_path, file_.index_path);
    // The reader thread is started on the first call to get_line()
}

ParallelLineReader::~ParallelLineReader() {
    // Join the reader thread if it is still running
    if (thread_started_.load(std::memory_order_relaxed) && reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool ParallelLineReader::get_line(std::string& line) {
    // Start the reader thread on the first call to get_line
    {
        std::unique_lock<std::mutex> lock(start_mutex_);
        if (!thread_started_.load(std::memory_order_acquire)) {
            reader_thread_ = std::thread(&ParallelLineReader::read_lines, this);
            thread_started_.store(true, std::memory_order_release);
        }
    }

    // Loop until we get a line or we are sure that no more lines will be produced.
    while (true) {
        if (queue_.try_dequeue(line)) {
            return true;
        }

        if (done_.load(std::memory_order_acquire)) {
            return queue_.try_dequeue(line);
        }
        std::this_thread::yield();
    }
}

ReadProgress ParallelLineReader::get_progress() {
    return {.total_bytes = total_bytes_, .current_bytes = current_pos_.load(std::memory_order_relaxed)};
}

// Removed index-based APIs for sequential-only reader

void ParallelLineReader::calculate_total_bytes() {
    if (std::filesystem::exists(file_.data_path)) {
        total_bytes_ = std::filesystem::file_size(file_.data_path);
    } else {
        spdlog::error("File {} does not exist", file_.data_path.string());
        throw std::runtime_error("File does not exist!");
    }
}

void ParallelLineReader::initialize_reader() {
    try {
        file_reader_ = std::make_unique<rapidgzip::StandardFileReader>(file_.data_path.string());
        rapidgzip_reader_ = std::make_unique<rapidgzip::ParallelGzipReader<rapidgzip::ChunkData>>(
            std::move(file_reader_), parallelization_, chunk_size_);
        // Enable index keeping so we can export at the end
        rapidgzip_reader_->setKeepIndex(true);
        rapidgzip_reader_->setCRC32Enabled(false);
        rapidgzip_reader_->setWindowSparsity(true);
        // If an index exists, import it
        try {
            if (!resolved_index_path_.empty() && std::filesystem::exists(resolved_index_path_)) {
                spdlog::info("Importing gzip index: {}", resolved_index_path_.string());
                auto idxReader = std::make_unique<rapidgzip::StandardFileReader>(resolved_index_path_.string());
                rapidgzip_reader_->importIndex(std::move(idxReader));
            }
        } catch (const std::exception& e) {
            spdlog::warn("Failed to import gzip index '{}': {}", resolved_index_path_.string(), e.what());
        }
        spdlog::info("Successfully initialized rapidgzip reader for: {}", file_.data_path.string());
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize rapidgzip reader: {}", e.what());
        throw;
    }
}

void ParallelLineReader::read_lines() {
    try {
        if (rapidgzip_reader_) {
            std::string line_buffer;
            line_buffer.reserve(READ_BUFFER_SIZE);
            // No seek required for sequential reading
            current_pos_.store(0);

            const auto processLines = [this, &line_buffer](const std::shared_ptr<rapidgzip::ChunkData>& chunkData,
                                                           size_t offsetInChunk, size_t dataToWriteSize) {
                this->process_chunk_data(chunkData, offsetInChunk, dataToWriteSize, line_buffer);
            };

            const size_t STRIPE_SIZE = 32 * 1024 * 1024;
            while (true) {
                // Lightweight backpressure to avoid unbounded growth when consumer is slower
                while (queue_.size_approx() > MAX_QUEUE_SIZE) {
                    std::this_thread::yield();
                }
                const auto bytesRead = rapidgzip_reader_->read(processLines, STRIPE_SIZE);
                if (bytesRead == 0) {
                    break;
                }
                current_pos_.store(rapidgzip_reader_->tellCompressed() /
                                   8);  // TellCompressed returns bits, we want bytes NOLINT
            }

            if (!line_buffer.empty()) {
                queue_.enqueue(std::move(line_buffer));
            }

        } else {
            spdlog::error("Cannot read lines: rapidgzip reader not initialized");
        }
    } catch (const std::exception& e) {
        spdlog::error("Error in ParallelLineReader thread: {}", e.what());
    }
    done_.store(true, std::memory_order_release);

    // Export index at the end if we have one
    try {
        if (rapidgzip_reader_ && !resolved_index_path_.empty()) {
            std::filesystem::create_directories(resolved_index_path_.parent_path());
            std::ofstream out(resolved_index_path_, std::ios::binary);
            if (out) {
                auto checkedWrite = [&out](const void* buffer, size_t size) {
                    out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
                };
                rapidgzip_reader_->exportIndex(checkedWrite, rapidgzip::IndexFormat::GZTOOL);
                spdlog::info("Exported gzip index: {}", resolved_index_path_.string());
            } else {
                spdlog::warn("Could not open index file for writing: {}", resolved_index_path_.string());
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to export gzip index '{}': {}", resolved_index_path_.string(), e.what());
    }
}

void ParallelLineReader::process_chunk_data(const std::shared_ptr<rapidgzip::ChunkData>& chunk_data,
                                            size_t offset_in_block, size_t data_size, std::string& line_buffer) {
    using rapidgzip::deflate::DecodedData;

    for (auto it = DecodedData::Iterator(*chunk_data, offset_in_block, data_size); static_cast<bool>(it); ++it) {
        const auto& [buffer, size] = *it;
        const char* data_ptr = reinterpret_cast<const char*>(buffer);
        const char* const data_end = data_ptr + size;
        while (data_ptr < data_end) {
            const char* nl_ptr = static_cast<const char*>(std::memchr(data_ptr, '\n', data_end - data_ptr));
            if (nl_ptr == nullptr) {
                line_buffer.append(data_ptr, data_end - data_ptr);
                break;
            }
            const size_t frag_len = static_cast<size_t>(nl_ptr - data_ptr);
            if (!line_buffer.empty()) {
                line_buffer.append(data_ptr, frag_len);
                queue_.enqueue(line_buffer);  // enqueue copy to avoid moved-from issues
                line_buffer.clear();
            } else {
                // Enqueue directly from this fragment without touching line_buffer
                queue_.enqueue(std::string(data_ptr, frag_len));
            }
            data_ptr = nl_ptr + 1;
        }
    }
}

std::filesystem::path ParallelLineReader::resolve_index_path(const std::filesystem::path& data_path,
                                                             const std::filesystem::path& index_path_hint) const {
    if (!index_path_hint.empty()) {
        return index_path_hint;
    }
    auto idx = data_path;
    idx += ".gzi";
    return idx;
}
#endif
