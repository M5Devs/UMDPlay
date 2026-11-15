#include "disc_reader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <fcntl.h>    // _O_RDONLY / _O_BINARY
#include <io.h>       // _open / _close / _read / _lseeki64
#include <sys/stat.h> // _fstat64 / _S_IFREG
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
#include <libchdr/chd.h>
}

namespace umdplay {

namespace {

// --- file-descriptor primitives -------------------------------------------
// Windows (MinGW/MSVC) has no pread(); emulate the positioned read with a
// seek + read pair. The seek mutates the shared file offset, so the pair is
// serialized with a mutex to keep FileBackend::read_at() thread-safe there.
#if defined(_WIN32)
int fd_open_read(const char* path) {
    return ::_open(path, _O_RDONLY | _O_BINARY);
}
void fd_close(int fd) { ::_close(fd); }
bool fd_regular_size(int fd, uint64_t* size) {
    struct _stat64 st {};
    if (::_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) return false;
    *size = static_cast<uint64_t>(st.st_size);
    return true;
}
size_t fd_pread(int fd, uint64_t offset, void* dst, size_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (::_lseeki64(fd, static_cast<long long>(offset), SEEK_SET) < 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < size) {
        const unsigned int chunk =
            static_cast<unsigned int>(std::min<size_t>(size - done, 1u << 30));
        const int n = ::_read(fd, out + done, chunk);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    return done;
}
#else
int fd_open_read(const char* path) { return ::open(path, O_RDONLY); }
void fd_close(int fd) { ::close(fd); }
bool fd_regular_size(int fd, uint64_t* size) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    *size = static_cast<uint64_t>(st.st_size);
    return true;
}
size_t fd_pread(int fd, uint64_t offset, void* dst, size_t size) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    // pread() is a stateless positioned read, so concurrent callers each see a
    // consistent view without sharing a file cursor.
    while (done < size) {
        const ssize_t n = ::pread(fd, out + done, size - done,
                                  static_cast<off_t>(offset + done));
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    return done;
}
#endif

// Lowercased copy of the extension (including the dot), or "" when there is none.
std::string lower_ext(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "";
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Direct-mapped hunk cache sizing: 256 slots of up to 512 KiB would be too much,
// so the slot count is chosen to keep the cache near ~4 MiB regardless of hunk
// size (a hunk is at most 512 KiB in CHDv5).
uint32_t slot_count_for(uint32_t hunk_bytes) {
    constexpr uint32_t kTargetBytes = 4u << 20;
    if (hunk_bytes == 0) return 1;
    uint32_t n = kTargetBytes / hunk_bytes;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return n;
}

}  // namespace

// --- path helpers ---------------------------------------------------------

bool is_chd_path(const std::string& path) { return lower_ext(path) == ".chd"; }

bool is_disc_image_path(const std::string& path) {
    const std::string ext = lower_ext(path);
    return ext == ".iso" || ext == ".chd";
}

// --- FileBackend ----------------------------------------------------------

FileBackend::~FileBackend() {
    if (m_fd >= 0) fd_close(m_fd);
}

std::unique_ptr<FileBackend> FileBackend::open(const std::string& path) {
    const int fd = fd_open_read(path.c_str());
    if (fd < 0) return nullptr;

    uint64_t size = 0;
    if (!fd_regular_size(fd, &size)) {
        fd_close(fd);
        return nullptr;
    }

    auto be = std::unique_ptr<FileBackend>(new FileBackend());
    be->m_fd = fd;
    be->m_size = size;
    return be;
}

size_t FileBackend::read_at(uint64_t offset, void* dst, size_t size) const {
    if (m_fd < 0 || !dst) return 0;
    return fd_pread(m_fd, offset, dst, size);
}

// --- ChdBackend -----------------------------------------------------------

struct ChdFile {
    chd_file* chd = nullptr;
    ~ChdFile() { if (chd) chd_close(chd); }
};

ChdBackend::ChdBackend(std::unique_ptr<ChdFile> file) : m_file(std::move(file)) {}

ChdBackend::~ChdBackend() = default;

std::unique_ptr<ChdBackend> ChdBackend::open(const std::string& path,
                                             std::string* error) {
    auto file = std::make_unique<ChdFile>();
    chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &file->chd);
    if (err != CHDERR_NONE) {
        if (error) *error = chd_error_string(err);
        return nullptr;
    }

    const chd_header* hdr = chd_get_header(file->chd);
    if (!hdr || hdr->hunkbytes == 0) {
        if (error) *error = "invalid CHD header";
        return nullptr;
    }

    auto be = std::unique_ptr<ChdBackend>(new ChdBackend(std::move(file)));
    be->m_size = hdr->logicalbytes;
    be->m_hunk_bytes = hdr->hunkbytes;
    // UMD/DVD discs are 2048 bytes per unit; a CD-based CHD reports 2448. Both
    // are read the same way (logical byte offsets), the unit size only matters
    // for how chd_read lays hunks out, which libchdr already handles.
    be->m_unit_bytes = hdr->unitbytes ? hdr->unitbytes : 2048u;
    be->m_hunk_count = hdr->totalhunks;
    be->m_slots = slot_count_for(be->m_hunk_bytes);
    be->m_cache_data.assign(static_cast<size_t>(be->m_slots) * be->m_hunk_bytes, 0);
    be->m_cache_hunk.assign(be->m_slots, -1);
    be->m_next_slot = 0;
    return be;
}

const uint8_t* ChdBackend::hunk_locked(uint32_t hunk) const {
    if (hunk >= m_hunk_count) return nullptr;

    // Probe the direct-mapped cache.
    for (uint32_t s = 0; s < m_slots; ++s) {
        if (m_cache_hunk[s] == static_cast<int64_t>(hunk))
            return m_cache_data.data() + static_cast<size_t>(s) * m_hunk_bytes;
    }

    // Miss: evict the next round-robin slot and decode into it.
    const uint32_t slot = m_next_slot;
    m_next_slot = (m_next_slot + 1) % m_slots;
    uint8_t* dst = m_cache_data.data() + static_cast<size_t>(slot) * m_hunk_bytes;
    const chd_error err = chd_read(m_file->chd, hunk, dst);
    if (err != CHDERR_NONE) {
        m_cache_hunk[slot] = -1;
        return nullptr;
    }
    m_cache_hunk[slot] = static_cast<int64_t>(hunk);
    return dst;
}

size_t ChdBackend::read_locked(uint64_t offset, uint8_t* dst, size_t size) const {
    if (offset >= m_size) return 0;
    size = static_cast<size_t>(std::min<uint64_t>(size, m_size - offset));

    size_t done = 0;
    while (done < size) {
        const uint64_t pos = offset + done;
        const uint32_t hunk = static_cast<uint32_t>(pos / m_hunk_bytes);
        const uint32_t in_hunk = static_cast<uint32_t>(pos % m_hunk_bytes);
        const size_t chunk = std::min<size_t>(size - done,
                                              static_cast<size_t>(m_hunk_bytes - in_hunk));
        const uint8_t* src = hunk_locked(hunk);
        if (!src) return done;  // stop at the first unreadable hunk
        std::memcpy(dst + done, src + in_hunk, chunk);
        done += chunk;
    }
    return done;
}

size_t ChdBackend::read_at(uint64_t offset, void* dst, size_t size) const {
    if (!m_file || !m_file->chd || !dst) return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    return read_locked(offset, static_cast<uint8_t*>(dst), size);
}

// --- DiscReader -----------------------------------------------------------

DiscReader::~DiscReader() = default;

bool DiscReader::open(const std::string& path) {
    m_backend.reset();
    m_path = path;
    m_error.clear();

    if (is_chd_path(path)) {
        std::string err;
        auto chd = ChdBackend::open(path, &err);
        if (!chd) {
            m_error = "CHD open failed: " + err;
            return false;
        }
        m_backend = std::move(chd);
    } else {
        auto file = FileBackend::open(path);
        if (!file) {
            m_error = "cannot open image file";
            return false;
        }
        m_backend = std::move(file);
    }
    return true;
}

std::vector<uint8_t> DiscReader::read_range(uint32_t lba, uint32_t size,
                                            uint32_t cap) const {
    if (!m_backend || size == 0) return {};
    const uint32_t n = std::min(size, cap);
    std::vector<uint8_t> buf(n);
    const size_t got = read_at(static_cast<uint64_t>(lba) * 2048u, buf.data(), n);
    buf.resize(got);
    return buf;
}

}  // namespace umdplay
