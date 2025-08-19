#ifndef PARALLEL_DECOMPRESSION

#include "AsyncLineReader.h"

#include <filesystem>

#include "spdlog/spdlog.h"

AsyncLineReader::AsyncLineReader(const WikiFile& file) : file_(file), total_bytes_(0), zstr_stream_(nullptr) {
    calculate_total_bytes();
    initialize_reader();

    // Start the background thread
    reader_thread_ = std::thread(&AsyncLineReader::read_lines, this);
}

AsyncLineReader::~AsyncLineReader() {
    if (reader_thread_.joinable()) {
        reader_thread_.join();
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
    // Use compressed file size for progress tracking to match compressed position
    // tracking
    total_bytes_ = std::filesystem::file_size(file_.data_path);
}

void AsyncLineReader::initialize_reader() {
    zstr_stream_ = std::make_unique<zstr::ifstream>(file_.data_path.string());

    if (!zstr_stream_->good()) {
        spdlog::error("Failed to open file with ifstream: {}", file_.data_path.string());
    }

    spdlog::info("Successfully initialized ifstream reader for: {}", file_.data_path.string());
}

void AsyncLineReader::read_lines() {
    std::string line;

    try {
        std::istream* stream = nullptr;

        if (zstr_stream_) {
            stream = zstr_stream_.get();
        } else {
            spdlog::error("No valid stream available for reading");
            return;
        }

        while (std::getline(*stream, line)) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return queue_.size() < MAX_QUEUE_SIZE; });

            queue_.push(std::move(line));

            // Update progress based on current stream position
            if (zstr_stream_) {
                current_pos_.store(static_cast<uint64_t>(zstr_stream_->compressed_tellg()));
            }

            lock.unlock();
            cv_.notify_one();
        }
    } catch (const std::exception& e) {
        spdlog::error("Error in AsyncLineReader thread: {}", e.what());
    }

    // Signal that reading is done
    std::unique_lock<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_all();
}
#endif
