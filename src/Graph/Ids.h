#pragma once

#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

// The graph builder numbers the same pages in several ways. Each numbering is its own type, so the compiler rejects
// an id of one kind where another is meant (an index into the page table used as an article, say).
namespace wg {

/// page_id from the dumps (sparse).
enum class PageId : uint32_t {};
/// lt_id from the linktarget dump (sparse).
enum class LinkTargetId : uint32_t {};
/// A main-namespace page, article or redirect, by its position in the page dump (dense).
enum class PageRow : uint32_t {};
/// An article (a page that is not a redirect), in page dump order (dense).
enum class Article : uint32_t {};
/// An article in the graph's locality order: the node ids of the graph files and of GraphStore.
enum class Node : uint32_t {};

/// "No such page": a value of any of the id types.
template <class Id>
inline constexpr Id kNoId = static_cast<Id>(UINT32_MAX);

template <class Id>
constexpr uint32_t raw(Id id) {
    return std::to_underlying(id);
}

/// A vector indexed by one id type only.
template <class Id, class T>
class IdVector {
   public:
    IdVector() = default;
    explicit IdVector(size_t n, const T& value = T{}) : v_(n, value) {}

    T& operator[](Id id) { return v_[raw(id)]; }
    const T& operator[](Id id) const { return v_[raw(id)]; }
    /// The value for an id read from a dump, or `missing` if the id is past the end.
    [[nodiscard]] T get(uint64_t raw_id, T missing) const { return raw_id < v_.size() ? v_[raw_id] : missing; }

    [[nodiscard]] size_t size() const { return v_.size(); }
    /// The id the next push_back() gets.
    [[nodiscard]] Id next_id() const { return static_cast<Id>(v_.size()); }
    void push_back(const T& value) { v_.push_back(value); }
    void reserve(size_t n) { v_.reserve(n); }
    void assign(size_t n, const T& value) { v_.assign(n, value); }
    /// Free the memory.
    void release() { std::vector<T>().swap(v_); }

    /// Every id, in order.
    ///
    /// The iota counts in size_t, not uint32_t: MSVC 19.52 and 19.53 at /O2 on x64 miscompile a loop over a 32-bit
    /// iota piped through a transform, and the first iteration reads far past the data. 19.51 is correct.
    [[nodiscard]] auto ids() const {
        return std::views::iota(size_t{0}, v_.size()) |
               std::views::transform([](size_t i) { return static_cast<Id>(i); });
    }
    /// The values without their ids, e.g. to write them to a file.
    [[nodiscard]] std::span<T> values() { return v_; }
    [[nodiscard]] std::span<const T> values() const { return v_; }

   private:
    std::vector<T> v_;
};

}  // namespace wg
