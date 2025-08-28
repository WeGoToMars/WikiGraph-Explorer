#ifndef PARALLEL_DECOMPRESSION

#include "AsyncLineReader.h"

#include <filesystem>
#include <vector>

#include "spdlog/spdlog.h"

AsyncLineReader::AsyncLineReader(const WikiFile& file) : file_(file), total_bytes_(0), gz_file_(nullptr) {
    calculate_total_bytes();
    initialize_reader();

    // Start the background thread
    reader_thread_ = std::thread(&AsyncLineReader::read_lines, this);
}

AsyncLineReader::~AsyncLineReader() {
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (gz_file_ != nullptr) {
        gzclose(gz_file_);
        gz_file_ = nullptr;
    }
}

bool AsyncLineReader::get_line(std::string& line) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || done_; });

    if (!queue_.empty()) {
        line = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
        return true;
    }
    return false;
}

ReadProgress AsyncLineReader::get_progress() {
    return {.total_bytes = total_bytes_, .current_bytes = current_pos_.load(std::memory_order_relaxed)};
}

void AsyncLineReader::calculate_total_bytes() {
    // Use compressed file size for progress tracking
    total_bytes_ = std::filesystem::file_size(file_.data_path);
}

void AsyncLineReader::initialize_reader() {
    gz_file_ = gzopen(file_.data_path.string().c_str(), "rb");
    if (gz_file_ == nullptr) {
        spdlog::error("Failed to open gzip file: {}", file_.data_path.string());
        return;
    }
    gzbuffer(gz_file_, 1 << 20);  // 2^20 = 1 MB buffer
    spdlog::info("Successfully initialized zlib gzip reader for: {}", file_.data_path.string());
}

void AsyncLineReader::read_lines() {
    if (gz_file_ == nullptr) {
        spdlog::error("No valid gzip file available for reading");
    } else {
        std::vector<char> buffer(1 << 16);  // 64 KB buffer
        std::string pending_line;
        pending_line.reserve(1 << 20);  // Wikipedia dumps have 2^20 =1 MB lines

        while (true) {
            int bytes_read = gzread(gz_file_, buffer.data(), static_cast<unsigned int>(buffer.size()));
            if (bytes_read < 0) {  // Decompression/read error
                int errnum = 0;
                const char* errstr = gzerror(gz_file_, &errnum);
                spdlog::error("Error reading gzip file: {} (err {}): {}", file_.data_path.string(), errnum,
                              (errstr ? errstr : "unknown"));
                break;
            }
            if (bytes_read == 0) {
                // EOF
                break;
            }

            // Scan the chunk for newlines
            int start_index = 0;
            for (int i = 0; i < bytes_read; i++) {
                if (buffer[static_cast<size_t>(i)] == '\n') {  // found the newline
                    pending_line.append(buffer.data() + start_index, static_cast<size_t>(i - start_index));

                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return queue_.size() < MAX_QUEUE_SIZE; });
                    queue_.push(std::move(pending_line));
                    pending_line.clear();

                    // compressed-position update for UI progress
                    // gzoffset reports the current location in the compressed stream
                    z_off_t off = gzoffset(gz_file_);
                    if (off >= 0) {
                        current_pos_.store(static_cast<uint64_t>(off), std::memory_order_relaxed);
                    }

                    lock.unlock();
                    cv_.notify_one();
                    start_index = i + 1;
                }
            }

            if (start_index < bytes_read) {
                pending_line.append(buffer.data() + start_index, static_cast<size_t>(bytes_read - start_index));
            }
        }

        // Flush final line
        if (!pending_line.empty()) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return queue_.size() < MAX_QUEUE_SIZE; });
            queue_.push(std::move(pending_line));

            // final progress update
            z_off_t off = gzoffset(gz_file_);
            if (off >= 0) {
                current_pos_.store(static_cast<uint64_t>(off), std::memory_order_relaxed);
            }

            lock.unlock();
            cv_.notify_one();
        }
    }

    // Signal that no more lines will be produced
    std::unique_lock<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_all();
}
#endif
