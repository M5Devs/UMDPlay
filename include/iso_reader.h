#pragma once
#include <string>
#include <vector>
#include <cstdint>

// UMD uses 2048-byte sectors (standard UDF/ISO 9660)
static constexpr size_t UMD_SECTOR_SIZE = 2048;

// Known UDF layout for UMD Video discs (discovered via analysis)
// Sector 16     : ISO 9660 PVD (dummy - do not use for directory traversal)
// Sectors 18-19 : UDF Path Table (Little-Endian)
// Sectors 20-21 : UDF Path Table (Big-Endian)
// Sector 24     : UMD_VIDEO/ ICB (File Entry)
// Sector 28     : STREAM/ directory entries (File Identifier Descriptors)
static constexpr uint32_t UMD_STREAM_DIR_LBA = 28;

struct MpsEntry {
    std::string name;        // e.g. "00001.MPS"
    uint32_t    lba;         // Logical Block Address inside ISO
    uint32_t    size_bytes;  // File size in bytes (0 if unknown)
};

class IsoReader {
public:
    explicit IsoReader(const std::string& iso_path);
    ~IsoReader();

    // Open ISO and parse UDF STREAM/ directory
    // Returns true if at least one .MPS file was found
    bool open();

    // List of .MPS stream files found in UMD_VIDEO/STREAM/
    std::vector<MpsEntry> get_mps_entries() const;

    // Read the full raw bytes of an MPS entry into memory
    std::vector<uint8_t> read_mps(const MpsEntry& entry) const;

    // Hex-dump the first N bytes of an MPS and verify MPEG-PS magic
    void dump_mps_header(const MpsEntry& entry, size_t bytes = 64) const;

    // Extract all .MPS files to a directory on disk
    bool extract_all(const std::string& output_dir) const;

private:
    std::string           m_iso_path;
    std::vector<MpsEntry> m_entries;

    // Read one 2048-byte sector from the ISO
    bool read_sector(uint32_t lba, uint8_t* buf) const;

    // Parse UDF File Identifier Descriptors in STREAM/ directory
    bool parse_udf_stream_dir();
};
