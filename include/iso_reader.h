#pragma once
#include "param_sfo.h"
#include "disc_reader.h"
#include <string>
#include <vector>
#include <cstdint>

// UMD uses 2048-byte sectors (standard UDF/ISO 9660)
static constexpr size_t UMD_SECTOR_SIZE = 2048;

// UMD Video discs share a conventional layout, but the content root and its
// STREAM/ sub-directory do NOT sit at a fixed LBA: UMD_VIDEO/ is at 23 or 24
// and STREAM/ at 28 or 30 depending on the disc (and STREAM/ spans several
// sectors once a disc carries 90+ streams). The reader therefore locates both
// by walking the ISO 9660 tree; these constants are only the legacy fallback
// used when a disc's tree is absent or unreadable.
//
// Sector 16     : ISO 9660 PVD (root directory record -> content root)
// Sector 24     : legacy UMD_VIDEO/ location (fallback)
// Sector 28     : legacy STREAM/ location (fallback)
static constexpr uint32_t UMD_STREAM_DIR_LBA = 28;
static constexpr uint32_t UMD_ROOT_DIR_LBA   = 24;

struct MpsEntry {
    std::string name;        // e.g. "00001.MPS"
    uint32_t    lba;         // Logical Block Address inside ISO
    uint32_t    size_bytes;  // File size in bytes (0 if unknown)
};

// A file located anywhere in the UMD filesystem (metadata assets, etc.)
struct UmdFile {
    std::string          name;        // e.g. "ICON0.PNG"
    uint32_t             lba = 0;
    uint32_t             size_bytes = 0;
    std::vector<uint8_t> data;        // loaded on demand
};

// Which UMD content root the disc uses. UMD Video discs carry /UMD_VIDEO/,
// UMD Music (audio) discs carry /UMD_AUDIO/ instead.
enum class UmdRoot {
    None,
    Video,   // /UMD_VIDEO/
    Audio,   // /UMD_AUDIO/
};

// A discovered non-stream asset file (PLAYLIST.UMD / CLIPINF/*.CLP / RESOURCE/*.RCO).
struct UmdAsset {
    std::string name;        // file name only, e.g. "PLAYLIST.UMD" / "00001.CLP" / "JA100000.RCO"
    std::string dir;         // "UMD_VIDEO", "UMD_VIDEO/CLIPINF", "UMD_VIDEO/RESOURCE", ...
    uint32_t    lba = 0;
    uint32_t    size_bytes = 0;
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

    // --- Sony metadata (PARAM.SFO / ICON0.PNG / PIC1.PNG / SND0.AT3) ---

    // Parsed PARAM.SFO (TITLE / DISC_ID / CATEGORY). Empty if absent.
    const ParamSfo& param() const { return m_param; }

    // Number of metadata files (PARAM.SFO/ICON0.PNG/...) found in UMD_VIDEO/.
    size_t metadata_count() const { return m_metadata.size(); }

    // Names of all metadata files found in UMD_VIDEO/ (for reporting).
    std::vector<std::string> metadata_names() const {
        std::vector<std::string> names;
        names.reserve(m_metadata.size());
        for (const auto& f : m_metadata) names.push_back(f.name);
        return names;
    }

    // Raw bytes of a named metadata file ("" if not present on this disc).
    std::vector<uint8_t> read_metadata(const std::string& filename) const;
    bool has_metadata(const std::string& filename) const;

    // --- Full UMD disc structure (Phase 4: menus / chapters) ---

    // Which content root this disc uses (UMD_VIDEO or UMD_AUDIO fallback).
    UmdRoot root() const { return m_root; }
    // "UMD_VIDEO" or "UMD_AUDIO" (the active root), or "" when none found.
    const std::string& root_name() const { return m_root_name; }

    // Asset files discovered under the active root: PLAYLIST.UMD, CLIPINF/*.CLP
    // and RESOURCE/*.RCO. Sorted by (dir, name).
    const std::vector<UmdAsset>& assets() const { return m_assets; }
    // First asset whose file name matches (case-insensitive); nullptr if none.
    const UmdAsset* find_asset(const std::string& name) const;
    // All assets in a given sub-directory ("" = root, "CLIPINF", "RESOURCE").
    std::vector<UmdAsset> assets_in(const std::string& dir) const;

    // Raw bytes of any discovered asset ("" when not found / unreadable).
    std::vector<uint8_t> read_asset(const UmdAsset& asset) const;
    std::vector<uint8_t> read_asset(const std::string& name) const;

private:
    std::string           m_iso_path;
    umdplay::DiscReader   m_reader;     // ISO or CHD backend (see disc_reader.h)
    std::vector<MpsEntry> m_entries;
    std::vector<UmdFile>  m_metadata;   // files found in UMD_VIDEO/ root
    ParamSfo              m_param;
    UmdRoot               m_root = UmdRoot::None;
    std::string           m_root_name;  // "UMD_VIDEO" / "UMD_AUDIO"
    std::vector<UmdAsset> m_assets;     // PLAYLIST.UMD / CLP / RCO

    // Read one 2048-byte sector from the ISO
    bool read_sector(uint32_t lba, uint8_t* buf) const;

    // Parse UDF File Identifier Descriptors in STREAM/ directory
    bool parse_udf_stream_dir();

    // Parse UMD_VIDEO/ root (sector 24) for PARAM.SFO / ICON0.PNG / PIC1.PNG etc.
    void parse_metadata_dir();

    // --- ISO 9660 directory traversal (used for full structure discovery) ---

    // One ISO 9660 directory record.
    struct IsoDirRecord {
        std::string name;
        uint32_t    lba = 0;
        uint32_t    size_bytes = 0;
        bool        is_dir = false;
    };

    // List the entries of the ISO 9660 directory at `lba` (size in bytes).
    std::vector<IsoDirRecord> list_iso_dir(uint32_t lba, uint32_t size) const;
    // Read a whole file (clamped to a sane cap) given its LBA and size.
    std::vector<uint8_t> read_file(uint32_t lba, uint32_t size_bytes) const;

    // Locate the content root (UMD_VIDEO, else UMD_AUDIO) and discover the
    // PLAYLIST.UMD / CLIPINF/*.CLP / RESOURCE/*.RCO assets beneath it.
    void discover_assets();
};

