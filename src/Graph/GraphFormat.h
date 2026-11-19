#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

// clang-format off
// On-disk layout of a built graph. One directory per dump, e.g. data/simplewiki-20261101.graph/
//
//   graph.meta      GraphMeta
//   nodes.bin       out-degree u32[n], in-degree u32[n], out-list byte offset u64[n+1], in-list byte offset u64[n+1]
//   fwd.lists       out-neighbour lists, one per node in node order: sorted ids, first absolute, then gaps, LEB128
//   rev.lists       in-neighbour lists, same encoding
//   titles.bin      title offset u32[n+1], title bytes (database form, underscores), node ids sorted by title u32[n]
//   redirects.bin   redirect titles sorted bytewise: { u16 length, bytes, u32 target node } ...
//   redirects.idx   u64 count, then every kRedirectStride-th entry of redirects.bin: { u64 file offset, u16 length, bytes }
//   hot.bin         u64 block keys (direction << 40 | block), most-read first, from a profile of random searches
//
// Nodes are articles (main namespace, not redirects). Redirects are folded into their targets. Node ids follow a
// locality order (the top 1% of nodes by degree first, then breadth-first), so lists read together tend to share
// 16 KB blocks and gaps stay small.
// clang-format on

namespace wg {

inline constexpr std::array<char, 8> kGraphMagic = {'W', 'G', 'G', 'R', 'A', 'P', 'H', '1'};
inline constexpr uint32_t kGraphVersion = 1;
inline constexpr uint64_t kBlockSize = 16384;  // unit of SSD reads and of the block cache
inline constexpr uint32_t kRedirectStride = 64;

/// The files of a graph directory (layout above).
namespace graph_file {
inline constexpr std::string_view kMeta = "graph.meta";
inline constexpr std::string_view kNodes = "nodes.bin";
inline constexpr std::string_view kTitles = "titles.bin";
inline constexpr std::string_view kRedirects = "redirects.bin";
inline constexpr std::string_view kRedirectIndex = "redirects.idx";
inline constexpr std::string_view kHotBlocks = "hot.bin";
inline constexpr std::string_view kFwdLists = "fwd.lists";
inline constexpr std::string_view kRevLists = "rev.lists";
}  // namespace graph_file

/// Bytes of a redirects.bin entry besides its title: the u16 length before it and the u32 target after it.
inline constexpr uint64_t kRedirectEntryOverhead = sizeof(uint16_t) + sizeof(uint32_t);
/// Bytes of a redirects.idx entry before its title: the u64 offset into redirects.bin and the u16 length.
inline constexpr uint64_t kRedirectIndexEntryHeader = sizeof(uint64_t) + sizeof(uint16_t);

/// The list entries are LEB128 varints: 7 bits per byte, low bits first, the high bit set on every byte but the last.
inline constexpr uint32_t kVarintPayloadBits = 7;
inline constexpr uint8_t kVarintPayload = 0x7f;  ///< the bits of a byte that carry the value
inline constexpr uint8_t kVarintMore = 0x80;     ///< set: another byte follows
inline constexpr size_t kVarintMaxBytes = 5;     ///< the most a uint32_t takes

/// Number of kBlockSize blocks that `bytes` bytes occupy.
constexpr uint64_t blocks_for(uint64_t bytes) { return (bytes + kBlockSize - 1) / kBlockSize; }

/// Link direction: out-links (fwd.lists) or in-links (rev.lists). The values are the file and array order.
enum class Dir : uint8_t { Fwd, Rev };
inline constexpr std::array kDirs = {Dir::Fwd, Dir::Rev};

namespace graph_file {
/// The list file of a direction.
constexpr std::string_view lists(Dir d) { return d == Dir::Fwd ? kFwdLists : kRevLists; }
}  // namespace graph_file

/// One value per direction, indexed by Dir.
template <class T>
struct PerDir {
    std::array<T, 2> v{};
    constexpr auto& operator[](this auto& self, Dir d) { return self.v[std::to_underlying(d)]; }
};

/// Block keys (hot.bin and the block cache): the direction above a 40-bit block number.
inline constexpr unsigned kBlockKeyBits = 40;
constexpr uint64_t block_key(Dir d, uint64_t block) { return uint64_t{std::to_underlying(d)} << kBlockKeyBits | block; }
constexpr Dir key_dir(uint64_t key) { return static_cast<Dir>(key >> kBlockKeyBits); }
constexpr uint64_t key_block(uint64_t key) { return key & ((uint64_t{1} << kBlockKeyBits) - 1); }

struct GraphMeta {
    std::array<char, 8> magic;
    uint32_t version;
    uint32_t nodes;
    uint64_t edges;
    std::array<uint64_t, 4> source_size;  // page, linktarget, pagelinks, redirect (0 if built without a redirect dump)
    std::array<uint64_t, 2> list_bytes;   // fwd.lists, rev.lists
    uint64_t title_bytes;
    uint64_t redirects;             // entries in redirects.bin
    uint64_t redirects_unresolved;  // redirect pages whose target could not be resolved (dropped)
    uint64_t duplicate_links;       // links merged because they were duplicates after folding redirects
    double build_seconds;
};

// The files hold the in-memory representation, read and written without conversion
static_assert(std::endian::native == std::endian::little, "graph files are little-endian");
static_assert(std::is_trivially_copyable_v<GraphMeta> && sizeof(GraphMeta) == 112, "graph.meta is 112 bytes");

}  // namespace wg
