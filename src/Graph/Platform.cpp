#include "Graph/Platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

// Each platform's headers only under its #if, and in this order (psapi.h needs windows.h first)
// clang-format off
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
// clang-format on

#ifdef __linux__
#include <fstream>
#endif

namespace wg {

std::filesystem::path executable_path() {
    std::error_code ec;
#if defined(__APPLE__)
    uint32_t len = 0;
    _NSGetExecutablePath(nullptr, &len);
    std::string buf(len, '\0');
    if (_NSGetExecutablePath(buf.data(), &len) != 0) return {};
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    auto p = std::filesystem::weakly_canonical(buf, ec);
    return ec ? std::filesystem::path(buf) : p;
#elif defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf);
#else
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p;
#endif
}

std::filesystem::path user_data_dir(const std::string& app) {
    auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at startup only
        return v != nullptr ? std::string(v) : std::string{};
    };
#if defined(__APPLE__)
    const std::string home = env("HOME");
    if (home.empty()) return {};
    return std::filesystem::path(home) / "Library" / "Application Support" / app;
#elif defined(_WIN32)
    const std::string local = env("LOCALAPPDATA");
    if (local.empty()) return {};
    return std::filesystem::path(local) / app;
#else
    const std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return std::filesystem::path(xdg) / app;
    const std::string home = env("HOME");
    if (home.empty()) return {};
    return std::filesystem::path(home) / ".local" / "share" / app;
#endif
}

uint64_t physical_memory_bytes() {
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return mem;
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st) != 0) return st.ullTotalPhys;
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
#endif
}

uint64_t process_footprint_bytes() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0)
        return pmc.PrivateUsage;
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    uint64_t size = 0, resident = 0;
    statm >> size >> resident;
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

uint64_t process_peak_footprint_bytes() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.ledger_phys_footprint_peak;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0)
        return pmc.PeakPagefileUsage;
    return 0;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            uint64_t kb = 0;
            status >> kb;
            return kb * 1024;
        }
    }
    return 0;
#else
    return 0;
#endif
}

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept { *this = std::move(other); }

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
    if (this != &other) {
        close();
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

RandomAccessFile::~RandomAccessFile() { close(); }

bool RandomAccessFile::is_open() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

void RandomAccessFile::open(const std::filesystem::path& path, bool no_cache) {
    close();
#ifdef _WIN32
    (void)no_cache;  // FILE_FLAG_NO_BUFFERING needs sector-aligned I/O; rely on the system cache instead
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path.string());
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    handle_ = h;
    size_ = static_cast<uint64_t>(sz.QuadPart);
#else
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path.string());
    struct stat st{};
    if (fstat(fd_, &st) != 0) {
        close();
        throw std::runtime_error("cannot stat " + path.string());
    }
    size_ = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
    if (no_cache) {
        fcntl(fd_, F_NOCACHE, 1);
        fcntl(fd_, F_RDAHEAD, 0);
    }
#elif defined(POSIX_FADV_RANDOM)
    if (no_cache) posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#else
    (void)no_cache;
#endif
#endif
}

size_t RandomAccessFile::read_at(void* dst, size_t len, uint64_t offset) const {
    auto* out = static_cast<char*>(dst);
    size_t got = 0;
    while (got < len) {
#ifdef _WIN32
        OVERLAPPED ov{};
        const uint64_t at = offset + got;
        ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(len - got, 1u << 30));
        if (ReadFile(static_cast<HANDLE>(handle_), out + got, want, &n, &ov) == 0) {
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            throw std::system_error(static_cast<int>(err), std::system_category(), "read failed");
        }
        if (n == 0) break;  // end of file
        got += n;
#else
        const ssize_t n =
            ::pread(fd_, out + got, std::min<size_t>(len - got, size_t{1} << 30), static_cast<off_t>(offset + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "read failed");
        }
        if (n == 0) break;  // end of file
        got += static_cast<size_t>(n);
#endif
    }
    return got;
}

void RandomAccessFile::read_exact(void* dst, size_t len, uint64_t offset) const {
    if (read_at(dst, len, offset) != len) throw std::runtime_error("short read");
}

void RandomAccessFile::close() {
#ifdef _WIN32
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
#else
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    size_ = 0;
}

}  // namespace wg
