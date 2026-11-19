#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "Graph/GraphFormat.h"
#include "Graph/IoPool.h"
#include "Graph/Platform.h"

namespace wg {

/// Decode one neighbour list (first id absolute, then gaps, LEB128) from `p` into all of `out`.
inline void decode_list(const uint8_t* p, std::span<uint32_t> out) {
    // A 32-bit counter, like the one over the decoded list in PathSearch: faster with GCC (docs/PERF.md)
    const auto deg = static_cast<uint32_t>(out.size());
    uint32_t prev = 0;
    for (uint32_t i = 0; i < deg; ++i) {
        uint32_t x = 0;
        int shift = 0;
        uint8_t b = 0;
        do {
            b = *p++;
            x |= static_cast<uint32_t>(b & kVarintPayload) << shift;
            shift += kVarintPayloadBits;
        } while ((b & kVarintMore) != 0);
        prev = i == 0 ? x : prev + x;
        out[i] = prev;
    }
}

/// The out-link and in-link lists of every node (fwd.lists and rev.lists), read in kBlockSize blocks.
///
/// Degrees and list offsets are always in RAM. A set of resident blocks, chosen when the graph is opened, stays in
/// RAM; any other block a query needs is read from disk, in one batch per frontier, and kept until the next query.
/// One query at a time: GraphStore serialises them.
class NeighbourLists {
   public:
    /// Read the degrees and offsets (nodes.bin) and open the list files. No block is resident yet.
    void open(const std::filesystem::path& dir, const GraphMeta& meta);
    /// Keep every block of both files in RAM: searches then do no I/O.
    void make_all_resident();
    /// Keep these blocks (block_key() values) in RAM.
    void make_resident(std::vector<uint64_t> keys);

    [[nodiscard]] uint32_t degree(Dir d, uint32_t u) const { return deg_[d][u]; }
    [[nodiscard]] std::span<const uint32_t> degrees(Dir d) const { return deg_[d]; }
    /// Byte offset of every node's list in its file, n + 1 entries.
    [[nodiscard]] std::span<const uint64_t> offsets(Dir d) const { return off_[d]; }
    [[nodiscard]] const RandomAccessFile& file(Dir d) const { return files_[d]; }
    [[nodiscard]] uint64_t block_count(Dir d) const { return blocks_[d]; }
    [[nodiscard]] uint64_t resident_bytes() const { return resident_blocks_ * kBlockSize; }

    // ---- queries
    /// Start a query: the blocks read for the previous one are dropped.
    void begin_query();
    /// Read every block that the lists of `frontier` need and that is not in RAM yet, in one batch.
    void fetch(Dir d, std::span<const uint32_t> frontier);

    /// The decoded neighbour list of u, valid until the next call; its blocks must have been fetched.
    // Fetch-then-iterate rather than a callback, so the BFS loop's speed does not hang on inlining (docs/PERF.md)
    [[nodiscard]] std::span<const uint32_t> neighbours(Dir d, uint32_t u) {
        const uint32_t dg = deg_[d][u];
        if (dg == 0) return {};
        const uint64_t a = off_[d][u], b = off_[d][u + 1];
        const uint8_t* src = nullptr;
        if (a / kBlockSize == (b - 1) / kBlockSize) {
            src = reinterpret_cast<const uint8_t*>(block(d, a / kBlockSize)) + a % kBlockSize;
        } else {  // the list spans blocks: copy it together first
            spanning_.resize(b - a);
            for (uint64_t p = a; p < b;) {
                const uint64_t k = p / kBlockSize, in = p % kBlockSize, take = std::min(kBlockSize - in, b - p);
                std::memcpy(spanning_.data() + (p - a), block(d, k) + in, take);
                p += take;
            }
            src = spanning_.data();
        }
        decoded_.resize(dg);
        decode_list(src, decoded_);
        return decoded_;
    }

    /// Blocks read from disk by the current query.
    [[nodiscard]] uint64_t blocks_read() const { return query_blocks_; }

    // ---- profiling: which blocks searches read
    /// Count, from now on, how many queries read each block.
    void start_profile();
    /// Stop counting; returns every block key, the most-read first.
    std::vector<uint64_t> stop_profile();

   private:
    [[nodiscard]] const char* block(Dir d, uint64_t b) const {
        if (const char* p = resident_[d][b]) return p;
        return query_blocks_map_.at(block_key(d, b));
    }
    char* allocate_query_block();

    PerDir<std::vector<uint32_t>> deg_;
    PerDir<std::vector<uint64_t>> off_;
    PerDir<RandomAccessFile> files_;
    PerDir<uint64_t> blocks_;
    PerDir<std::vector<const char*>> resident_;  // block -> its data in RAM, or null
    std::unique_ptr<char[]> resident_memory_;
    uint64_t resident_blocks_ = 0;

    static constexpr unsigned kIoThreads = 32;  // reads in flight; 16 KB random reads on an Apple SSD saturate here
    IoPool pool_{kIoThreads};

    // the current query's blocks, in an arena of 4 MB chunks reused from query to query
    std::unordered_map<uint64_t, const char*> query_blocks_map_;
    std::vector<std::unique_ptr<char[]>> arena_;
    size_t arena_chunk_ = 0, arena_used_ = 0;
    uint64_t query_blocks_ = 0;
    std::vector<uint64_t> wanted_;
    std::vector<IoPool::Req> requests_;
    std::vector<uint8_t> spanning_;  // a list that spans blocks, copied together
    std::vector<uint32_t> decoded_;  // the list neighbours() returns

    bool profiling_ = false;
    uint64_t query_id_ = 0;
    PerDir<std::vector<uint32_t>> reads_;         // per block: queries that read it
    PerDir<std::vector<uint32_t>> last_read_by_;  // per block: the last query that read it
};

}  // namespace wg
