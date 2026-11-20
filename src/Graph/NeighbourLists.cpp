#include "Graph/NeighbourLists.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Graph/GraphFormat.h"
#include "Graph/IoPool.h"
#include "Graph/Platform.h"

namespace wg {

void NeighbourLists::open(const std::filesystem::path& dir, const GraphMeta& meta) {
    const uint64_t n = meta.nodes;
    {
        RandomAccessFile f;
        f.open(dir / graph_file::kNodes, false);
        uint64_t at = 0;
        for (const Dir d : kDirs) deg_[d] = read_array<uint32_t>(f, n, at);
        for (const Dir d : kDirs) off_[d] = read_array<uint64_t>(f, n + 1, at);
    }
    for (const Dir d : kDirs) files_[d].open(dir / graph_file::lists(d), true);
    for (const Dir d : kDirs) {
        const uint64_t bytes = meta.list_bytes[std::to_underlying(d)];
        if (files_[d].size() != bytes)
            throw std::runtime_error("the graph in " + dir.string() +
                                     " is damaged (a list file has the wrong size); "
                                     "rebuild it");
        blocks_[d] = blocks_for(bytes);
        resident_[d].assign(blocks_[d], nullptr);
    }
}

namespace {
/// Run the reads in batches, so the request list of a whole file is never handed to the pool at once.
void read_in_batches(IoPool& pool, std::span<const IoPool::Req> requests) {
    constexpr size_t kBatch = 8192;
    for (size_t i = 0; i < requests.size(); i += kBatch)
        pool.run(requests.subspan(i, std::min(kBatch, requests.size() - i)));
}
}  // namespace

void NeighbourLists::make_all_resident() {
    resident_blocks_ = blocks_[Dir::Fwd] + blocks_[Dir::Rev];
    resident_memory_ = std::make_unique_for_overwrite<char[]>(resident_blocks_ * kBlockSize);
    // Whole files: large sequential reads
    constexpr uint64_t kReadBytes = uint64_t{4} << 20;
    std::vector<IoPool::Req> requests;
    char* start = resident_memory_.get();
    for (const Dir d : kDirs) {
        for (uint64_t b = 0; b < blocks_[d]; ++b) resident_[d][b] = start + b * kBlockSize;
        const uint64_t bytes = blocks_[d] * kBlockSize;
        for (uint64_t o = 0; o < bytes; o += kReadBytes)
            requests.push_back({&files_[d], o, static_cast<size_t>(std::min(kReadBytes, bytes - o)), start + o});
        start += bytes;
    }
    read_in_batches(pool_, requests);
}

void NeighbourLists::make_resident(std::vector<uint64_t> keys) {
    std::ranges::sort(keys);
    resident_blocks_ = keys.size();
    resident_memory_ = std::make_unique_for_overwrite<char[]>(resident_blocks_ * kBlockSize);
    std::vector<IoPool::Req> requests;
    requests.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        const Dir d = key_dir(keys[i]);
        const uint64_t b = key_block(keys[i]);
        char* dst = resident_memory_.get() + i * kBlockSize;
        resident_[d][b] = dst;
        requests.push_back({&files_[d], b * kBlockSize, kBlockSize, dst});
    }
    read_in_batches(pool_, requests);
}

void NeighbourLists::begin_query() {
    ++query_id_;
    query_blocks_map_.clear();
    arena_chunk_ = 0;
    arena_used_ = 0;
    query_blocks_ = 0;
}

char* NeighbourLists::allocate_query_block() {
    constexpr size_t kChunk = size_t{4} << 20;
    if (arena_chunk_ < arena_.size() && arena_used_ + kBlockSize > kChunk) {
        ++arena_chunk_;
        arena_used_ = 0;
    }
    if (arena_chunk_ == arena_.size()) {
        arena_.push_back(std::make_unique_for_overwrite<char[]>(kChunk));
        arena_used_ = 0;
    }
    char* p = arena_[arena_chunk_].get() + arena_used_;
    arena_used_ += kBlockSize;
    return p;
}

void NeighbourLists::fetch(Dir d, std::span<const uint32_t> frontier) {
    const auto& resident = resident_[d];
    wanted_.clear();
    for (const uint32_t u : frontier) {
        if (deg_[d][u] == 0) continue;
        const uint64_t a = off_[d][u], b = off_[d][u + 1];
        for (uint64_t k = a / kBlockSize; k <= (b - 1) / kBlockSize; ++k) {
            if (profiling_ && last_read_by_[d][k] != static_cast<uint32_t>(query_id_)) {
                last_read_by_[d][k] = static_cast<uint32_t>(query_id_);
                ++reads_[d][k];
            }
            if (resident[k] == nullptr && !query_blocks_map_.contains(block_key(d, k))) wanted_.push_back(k);
        }
    }
    if (wanted_.empty()) return;
    std::ranges::sort(wanted_);
    wanted_.erase(std::ranges::unique(wanted_).begin(), wanted_.end());
    requests_.clear();
    for (const uint64_t k : wanted_) {
        char* dst = allocate_query_block();
        query_blocks_map_[block_key(d, k)] = dst;
        requests_.push_back({&files_[d], k * kBlockSize, kBlockSize, dst});
    }
    query_blocks_ += wanted_.size();
    pool_.run(requests_);
}

void NeighbourLists::start_profile() {
    for (const Dir d : kDirs) {
        reads_[d].assign(blocks_[d], 0);
        last_read_by_[d].assign(blocks_[d], 0);
    }
    profiling_ = true;
}

std::vector<uint64_t> NeighbourLists::stop_profile() {
    profiling_ = false;
    std::vector<uint64_t> keys;
    for (const Dir d : kDirs)
        for (uint64_t b = 0; b < blocks_[d]; ++b) keys.push_back(block_key(d, b));
    std::ranges::stable_sort(keys, std::greater{}, [&](uint64_t k) { return reads_[key_dir(k)][key_block(k)]; });
    reads_ = {};  // a fresh PerDir: frees the counters
    last_read_by_ = {};
    return keys;
}

}  // namespace wg
