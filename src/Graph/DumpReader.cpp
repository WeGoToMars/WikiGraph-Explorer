#include "Graph/DumpReader.h"

#include <zlib.h>

#include <algorithm>
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

DumpStreamStats stream_dump(const std::filesystem::path& path, uint64_t stop_after, unsigned threads,
                            const std::function<void(DumpChunk&)>& on_chunk,
                            const std::function<void(uint64_t, uint64_t)>& on_progress, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t compressed_size = std::filesystem::file_size(path);
    gzFile f = gzopen(path.string().c_str(), "rb");
    if (f == nullptr) throw std::runtime_error("cannot open " + path.string());
    gzbuffer(f, 4u << 20);

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
        const int r = gzread(f, c.data.data() + carry.size(), static_cast<unsigned>(kChunk));
        if (r < 0) {
            int errnum = 0;
            read_error = gzerror(f, &errnum);
            break;
        }
        const size_t have = carry.size() + static_cast<size_t>(r);
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
        if (on_progress) on_progress(static_cast<uint64_t>(gzoffset(f)), compressed_size);
        if (r == 0) {
            // gzread also returns 0, not -1, when the file ends in the middle of the gzip stream (a truncated
            // download); only gzerror tells the two apart
            int errnum = Z_OK;
            const char* msg = gzerror(f, &errnum);
            if (errnum != Z_OK) read_error = errnum == Z_BUF_ERROR ? "unexpected end of file" : msg;
            break;
        }
        if (stop_after != 0 && produced >= stop_after) {
            st.stopped_early = true;
            break;
        }
    }
    gzclose(f);
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
