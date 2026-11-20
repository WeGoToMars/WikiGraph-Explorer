#include "Graph/DumpReader.h"

#include <zlib.h>

#ifdef WG_PARALLEL_GZIP
#include <filereader/Standard.hpp>  // StandardFileReader: rapidgzip.hpp does not include it with every compiler
#include <rapidgzip/rapidgzip.hpp>
#endif

#include <algorithm>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Utils/StringUtils.h"

namespace wg {

void check_columns(const std::filesystem::path& path, std::initializer_list<std::string_view> leading) {
    gzFile f = gzopen(path.string().c_str(), "rb");
    if (f == nullptr) throw std::runtime_error("cannot open " + path.string());
    std::string head(size_t{64} * 1024, '\0');  // the statement comes after a short comment header
    const int n = gzread(f, head.data(), static_cast<unsigned>(head.size()));
    gzclose(f);
    head.resize(n > 0 ? static_cast<size_t>(n) : 0);

    // CREATE TABLE `page` (
    //   `page_id` int(8) unsigned NOT NULL AUTO_INCREMENT,
    //   `page_namespace` int(11) NOT NULL DEFAULT 0,
    //   ...
    //   PRIMARY KEY (`page_id`),
    std::vector<std::string> columns;
    size_t pos = head.find("CREATE TABLE");
    if (pos != std::string::npos) pos = head.find('\n', pos);
    while (pos != std::string::npos && head.compare(pos, 4, "\n  `") == 0) {
        const size_t name = pos + 4, end = head.find('`', name);
        if (end == std::string::npos) break;
        columns.emplace_back(head, name, end - name);
        pos = head.find('\n', end);
    }
    if (columns.size() >= leading.size() && std::ranges::equal(columns | std::views::take(leading.size()), leading))
        return;
    throw std::runtime_error(std::format("{}: expected the columns to start with ({}), found ({}). The dump format "
                                         "has changed; this version of the app cannot read it.",
                                         path.filename().string(), string_utils::join(leading, ", "),
                                         columns.empty() ? "no CREATE TABLE statement" : string_utils::join(columns, ", ")));
}

/// A gzip file read as one stream of uncompressed bytes. read() returns 0 at the end of the stream and throws
/// std::exception (with the reason) when the file is damaged or ends early; both versions check the CRC-32.
#ifdef WG_PARALLEL_GZIP
// rapidgzip decompresses chunks of the file on `threads` threads, ahead of what has been read
class DumpFile::Input {
   public:
    Input(const std::filesystem::path& path, unsigned threads)
        : reader_(std::make_unique<rapidgzip::StandardFileReader>(path.string()), threads, kChunkBytes) {}

    /// Back to the start. After a complete read, the index built during it makes the next read faster.
    void rewind() { reader_.seek(0); }
    size_t read(char* out, size_t n) { return reader_.read(out, n); }
    /// Approximate (rapidgzip knows it to within a deflate block), which is enough for progress
    [[nodiscard]] uint64_t compressed_offset() const { return reader_.tellCompressed(); }

   private:
    // 1 MiB of compressed data per chunk: as fast as rapidgzip's default of 4 MiB, with a third of the memory
    static constexpr uint64_t kChunkBytes = uint64_t{1} << 20;
    rapidgzip::ParallelGzipReader<> reader_;
};
#else
// zlib on the calling thread
class DumpFile::Input {
   public:
    Input(const std::filesystem::path& path, unsigned /*threads*/) : f_(gzopen(path.string().c_str(), "rb")) {
        if (f_ == nullptr) throw std::runtime_error("cannot open " + path.string());
        gzbuffer(f_, 4u << 20);
    }
    ~Input() { gzclose(f_); }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    void rewind() {
        if (gzrewind(f_) != 0) throw std::runtime_error("cannot go back to the start of the file");
    }
    size_t read(char* out, size_t n) {
        const int r = gzread(f_, out, static_cast<unsigned>(n));
        int errnum = Z_OK;
        if (r < 0) throw std::runtime_error(gzerror(f_, &errnum));
        if (r == 0) {
            // gzread also returns 0, not -1, when the file ends in the middle of the gzip stream (a truncated
            // download); only gzerror tells the two apart
            const char* msg = gzerror(f_, &errnum);
            if (errnum != Z_OK) throw std::runtime_error(errnum == Z_BUF_ERROR ? "unexpected end of file" : msg);
        }
        return static_cast<size_t>(r);
    }
    [[nodiscard]] uint64_t compressed_offset() const { return static_cast<uint64_t>(gzoffset(f_)); }

   private:
    gzFile f_;
};
#endif

DumpFile::DumpFile(const std::filesystem::path& path, unsigned threads)
    : path_(path), in_(std::make_unique<Input>(path, std::max(1u, threads))) {}

DumpFile::~DumpFile() = default;

DumpStreamStats stream_dump(const std::filesystem::path& path, uint64_t stop_after, unsigned threads,
                            const std::function<void(DumpChunk&)>& on_chunk,
                            const std::function<void(uint64_t, uint64_t)>& on_progress, std::stop_token stop) {
    return DumpFile(path, threads).stream(stop_after, threads, on_chunk, on_progress, std::move(stop));
}

DumpStreamStats DumpFile::stream(uint64_t stop_after, unsigned threads, const std::function<void(DumpChunk&)>& on_chunk,
                                 const std::function<void(uint64_t, uint64_t)>& on_progress, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::filesystem::path& path = path_;
    const uint64_t compressed_size = std::filesystem::file_size(path);
    Input& in = *in_;
    try {
        in.rewind();
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot read " + path.string() + ": " + e.what());
    }

    std::mutex m;
    std::condition_variable_any cv_items;  // also wakes the workers when the pool is destroyed early
    std::condition_variable cv_space;
    std::deque<DumpChunk> queue;
    bool done = false;
    const unsigned workers = std::max(1u, threads);
    const size_t capacity = static_cast<size_t>(workers) * 2;
    std::exception_ptr worker_error;

    // Declared after what the workers use, so if this function is left by an exception the jthreads are stopped
    // and joined before those are destroyed
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&](std::stop_token pool_stop) {
            for (;;) {
                DumpChunk c;
                {
                    std::unique_lock lk(m);
                    if (!cv_items.wait(lk, pool_stop, [&] { return !queue.empty() || done; })) return;
                    if (queue.empty()) return;
                    c = std::move(queue.front());
                    queue.pop_front();
                }
                cv_space.notify_one();
                try {
                    on_chunk(c);
                } catch (...) {
                    std::lock_guard lk(m);
                    if (!worker_error) worker_error = std::current_exception();
                }
            }
        });
    }

    DumpStreamStats st;
    constexpr size_t kChunk = 8u << 20;
    std::vector<char> carry;
    uint64_t produced = 0, seq = 0;
    std::string read_error;
    for (;;) {
        if (stop.stop_requested()) {
            st.cancelled = true;
            break;
        }
        DumpChunk c;
        c.data.resize(carry.size() + kChunk);
        std::ranges::copy(carry, c.data.begin());
        size_t r = 0;
        try {
            r = in.read(c.data.data() + carry.size(), kChunk);
        } catch (const std::exception& e) {
            read_error = e.what();
            break;
        }
        const size_t have = carry.size() + r;
        size_t cut = have;
        if (r > 0) {
            while (cut > 0 && c.data[cut - 1] != '\n') --cut;
            if (cut == 0) cut = have;  // a line longer than the chunk: hand it over whole
        }
        carry.assign(c.data.begin() + static_cast<std::ptrdiff_t>(cut), c.data.begin() + static_cast<std::ptrdiff_t>(have));
        c.data.resize(cut);
        c.start = produced;
        c.seq = seq++;
        produced += cut;
        if (!c.data.empty()) {
            std::unique_lock lk(m);
            cv_space.wait(lk, [&] { return queue.size() < capacity; });
            queue.push_back(std::move(c));
            cv_items.notify_one();
        }
        if (on_progress) on_progress(std::min(in.compressed_offset(), compressed_size), compressed_size);
        if (r == 0) break;
        if (stop_after != 0 && produced >= stop_after) {
            st.stopped_early = true;
            break;
        }
    }
    {
        std::lock_guard lk(m);
        done = true;
    }
    cv_items.notify_all();
    for (auto& t : pool) t.join();  // no stop request: the workers drain the queue first
    if (!read_error.empty())
        throw std::runtime_error("cannot decompress " + path.string() + ": " + read_error +
                                 ". The file is incomplete or damaged: delete it and select the wiki again to "
                                 "download it (files that are already complete are kept).");
    if (worker_error) std::rethrow_exception(worker_error);
    if (on_progress && !st.cancelled) on_progress(compressed_size, compressed_size);

    st.uncompressed_bytes = produced;
    st.chunks = seq;
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return st;
}

}  // namespace wg
