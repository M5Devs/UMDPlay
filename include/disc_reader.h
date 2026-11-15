#pragma once

// Unified, thread-safe byte-range reader for a disc image. Both the ISO 9660 /
// UDF parser (IsoReader) and the FFmpeg AVIOContext / hand-rolled PSMF audio
// demuxer in Decoder read the disc exclusively through DiscReader::read_at(),
// so a single backend swap makes every consumer work with a new container.
//
// Two backends are provided:
//   * .iso  — a plain image file, read with pread() (stateless, no seek cursor,
//             safe to call from several threads).
//   * .chd  — a Compressed Hunks of Data container (libchdr). Reads are mapped
//             onto 2048-byte UMD/DVD units and assembled from a small hunk
//             cache; unaligned offsets and reads spanning hunk boundaries are
//             handled transparently.
//
// The backend is picked from the file extension (case-insensitive), so callers
// only ever construct a DiscReader from a path.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace umdplay {

// Opaque libchdr handle (defined in disc_reader.cpp).
struct ChdFile;

// True when `path` ends in `.iso` or `.chd` (case-insensitive). Used by the
// Browse System filter and the CLI to recognise supported disc images.
bool is_disc_image_path(const std::string& path);
// True when `path` ends in `.chd` (case-insensitive).
bool is_chd_path(const std::string& path);

// Abstract backend: random access by byte offset.
class DiscBackend {
public:
    virtual ~DiscBackend() = default;

    // Total size of the underlying data in bytes (0 when unknown).
    virtual uint64_t size() const = 0;

    // Copy `size` bytes starting at byte `offset` into `dst`. Returns the number
    // of bytes actually read (short reads at EOF are not an error). Must be
    // safe to call concurrently.
    virtual size_t read_at(uint64_t offset, void* dst, size_t size) const = 0;

    // Human-readable backend name ("iso" / "chd"), for logs.
    virtual const char* name() const = 0;
};

// Plain image-file backend (pread).
class FileBackend : public DiscBackend {
public:
    ~FileBackend() override;
    // Opens `path` read-only. Returns nullptr on failure.
    static std::unique_ptr<FileBackend> open(const std::string& path);

    uint64_t size() const override { return m_size; }
    size_t   read_at(uint64_t offset, void* dst, size_t size) const override;
    const char* name() const override { return "iso"; }

private:
    int      m_fd = -1;
    uint64_t m_size = 0;
};

// Compressed Hunks of Data backend (libchdr). Data is exposed as 2048-byte
// units (UMD sector size); reads are served from a small LRU-ish hunk cache.
class ChdBackend : public DiscBackend {
public:
    ~ChdBackend() override;
    // Opens `path` read-only. Returns nullptr on failure (and sets *error).
    static std::unique_ptr<ChdBackend> open(const std::string& path,
                                            std::string* error = nullptr);

    uint64_t size() const override { return m_size; }
    size_t   read_at(uint64_t offset, void* dst, size_t size) const override;
    const char* name() const override { return "chd"; }

    // Bytes per hunk / per unit as reported by the CHD header (for tests/logs).
    uint32_t hunk_bytes() const { return m_hunk_bytes; }
    uint32_t unit_bytes() const { return m_unit_bytes; }

private:
    explicit ChdBackend(std::unique_ptr<ChdFile> file);

    // Read `size` bytes at absolute byte `offset` from the CHD's logical data,
    // assembling from cached hunks. Caller must hold m_mutex.
    size_t read_locked(uint64_t offset, uint8_t* dst, size_t size) const;
    const uint8_t* hunk_locked(uint32_t hunk) const;

    std::unique_ptr<ChdFile> m_file;
    uint64_t m_size = 0;
    uint32_t m_hunk_bytes = 0;
    uint32_t m_unit_bytes = 0;
    uint32_t m_hunk_count = 0;

    // Small direct-mapped hunk cache (index -> decoded hunk bytes). Guards
    // concurrent read_at() calls; ChdBackend is thus thread-safe.
    mutable std::vector<uint8_t> m_cache_data;   // slots * hunk_bytes
    mutable std::vector<int64_t> m_cache_hunk;   // slot -> hunk number (-1 empty)
    mutable int                  m_next_slot = 0;
    mutable uint32_t             m_slots = 0;
    mutable std::mutex           m_mutex;        // serializes chd_read() calls
};

// Unified disc reader: owns the chosen backend and offers read_at() plus a
// couple of convenience helpers used by IsoReader / Decoder.
class DiscReader {
public:
    DiscReader() = default;
    ~DiscReader();

    DiscReader(const DiscReader&) = delete;
    DiscReader& operator=(const DiscReader&) = delete;

    // Open `path` (choosing the backend by extension). Returns false on error
    // (and sets last_error()).
    bool open(const std::string& path);

    // Release the backend (safe to call repeatedly).
    void close() { m_backend.reset(); m_error.clear(); }

    bool     is_open() const { return m_backend != nullptr; }
    uint64_t size() const { return m_backend ? m_backend->size() : 0; }
    const char* backend_name() const {
        return m_backend ? m_backend->name() : "none";
    }
    const std::string& last_error() const { return m_error; }
    const std::string& path() const { return m_path; }

    // Read `size` bytes at byte `offset`. Returns bytes read (short at EOF).
    // Thread-safe.
    size_t read_at(uint64_t offset, void* dst, size_t size) const {
        return m_backend ? m_backend->read_at(offset, dst, size) : 0;
    }

    // Read one 2048-byte UMD sector at `lba` into `dst`. False on short read.
    bool read_sector(uint32_t lba, void* dst) const {
        return read_at(static_cast<uint64_t>(lba) * 2048u, dst, 2048u) == 2048u;
    }

    // Read `size` bytes at `lba * 2048` into a vector (clamped to a sane cap).
    std::vector<uint8_t> read_range(uint32_t lba, uint32_t size,
                                    uint32_t cap = 64u << 20) const;

private:
    std::unique_ptr<DiscBackend> m_backend;
    std::string m_path;
    std::string m_error;
};

}  // namespace umdplay
