#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wg {

/// Installed physical memory in bytes (0 if unknown).
uint64_t physical_memory_bytes();

/// Memory currently charged to this process in bytes (macOS: phys_footprint, Linux: RSS, Windows: private).
uint64_t process_footprint_bytes();

/// Highest memory charge of this process so far (0 if unknown).
uint64_t process_peak_footprint_bytes();

/// Absolute path of the running executable (empty if unknown).
std::filesystem::path executable_path();

/// The per-user folder for application data: ~/Library/Application Support/<app> on macOS,
/// $XDG_DATA_HOME/<app> (default ~/.local/share/<app>) on Linux, %LOCALAPPDATA%\<app> on Windows.
/// Empty if the home directory is unknown.
std::filesystem::path user_data_dir(const std::string& app);

/// A read-only file for positional reads from many threads at once.
///
/// With `no_cache`, reads bypass the OS page cache where the platform allows it (macOS F_NOCACHE), so the
/// process's RAM budget is not silently extended by the page cache.
class RandomAccessFile {
   public:
    RandomAccessFile() = default;
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    RandomAccessFile(RandomAccessFile&& other) noexcept;
    RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;
    ~RandomAccessFile();

    /// Open the file; throws std::runtime_error on failure.
    void open(const std::filesystem::path& path, bool no_cache);
    /// Read up to `len` bytes at `offset`; returns the number of bytes read, which is short only at the end
    /// of the file. Throws std::system_error on a read error.
    size_t read_at(void* dst, size_t len, uint64_t offset) const;
    /// Read exactly `len` bytes at `offset`; throws on a short read.
    void read_exact(void* dst, size_t len, uint64_t offset) const;
    [[nodiscard]] uint64_t size() const { return size_; }
    [[nodiscard]] bool is_open() const;
    void close();

   private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    uint64_t size_ = 0;
};

/// `count` values of T read from `f` at `offset`, which then moves past them. Throws on a short read.
template <class T>
std::vector<T> read_array(const RandomAccessFile& f, uint64_t count, uint64_t& offset) {
    std::vector<T> v(count);
    if (count != 0) f.read_exact(v.data(), count * sizeof(T), offset);
    offset += count * sizeof(T);
    return v;
}

}  // namespace wg
