#include "iso_reader.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <sstream>

// ---------------------------------------------------------------------------
// Background: UMD Video ISO layout (verified against real discs)
//
// UMD Video discs use UDF 1.02, NOT plain ISO 9660.
// The ISO 9660 PVD at sector 16 exists but contains a dummy root directory
// with size=22 bytes — it cannot be used for filesystem traversal.
//
// Real directory data lives in the UDF structures:
//   Sectors 18-19 : UDF Path Table (LE/BE) — lists UMD_VIDEO at LBA 24
//   Sector  28    : STREAM/ File Identifier Descriptors
//                   Each record = 0x38 bytes, contains:
//                     bytes [2-3] : LBA of file data (uint16 LE)
//                     bytes [8]   : filename length
//                     bytes [9..] : filename (ASCII, no UTF-16 prefix on MPS)
//
// Verified magic bytes on real disc:
//   00001.MPS @ LBA 2880 → 00 00 01 BA (MPEG Program Stream) ✅
//   00002.MPS @ LBA  592 → 00 00 01 BA ✅
//   00098.MPS @ LBA  288 → 00 00 01 BA ✅
// ---------------------------------------------------------------------------

IsoReader::IsoReader(const std::string& iso_path)
    : m_iso_path(iso_path) {}

IsoReader::~IsoReader() {}

bool IsoReader::read_sector(uint32_t lba, uint8_t* buf) const {
    std::ifstream f(m_iso_path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(lba) * UMD_SECTOR_SIZE);
    f.read(reinterpret_cast<char*>(buf), UMD_SECTOR_SIZE);
    return static_cast<size_t>(f.gcount()) == UMD_SECTOR_SIZE;
}

bool IsoReader::open() {
    std::ifstream f(m_iso_path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[IsoReader] ERROR: Cannot open ISO: " << m_iso_path << "\n";
        return false;
    }
    std::cout << "[IsoReader] Opened: " << m_iso_path << "\n";
    std::cout << "[IsoReader] Using UDF path (STREAM/ @ LBA "
              << UMD_STREAM_DIR_LBA << ")\n";
    return parse_udf_stream_dir();
}

bool IsoReader::parse_udf_stream_dir() {
    uint8_t sector[UMD_SECTOR_SIZE];

    // Read the STREAM/ directory sector
    if (!read_sector(UMD_STREAM_DIR_LBA, sector)) {
        std::cerr << "[IsoReader] ERROR: Cannot read STREAM/ sector\n";
        return false;
    }

    // Scan the sector for .MPS / .PMF filenames
    // UDF File Identifier Descriptor layout (simplified, as seen on real UMD):
    //   +0x00 : uint16  record_length
    //   +0x02 : uint16  LBA (little-endian) of file data
    //   +0x04 : uint32  (partition / flags — skip)
    //   +0x08 : uint8   filename_length
    //   +0x09 : char[]  filename (ASCII on these discs)
    //
    // We locate .MPS/.PMF strings and back-calculate record start.

    size_t offset = 0;
    while (offset < UMD_SECTOR_SIZE - 4) {
        // Look for .MPS or .PMF marker
        // Find next occurrence of ".MPS" or ".PMF"
        bool found = false;
        for (size_t i = offset; i < UMD_SECTOR_SIZE - 4; ++i) {
            bool is_mps = (sector[i]=='.' && sector[i+1]=='M' &&
                           sector[i+2]=='P' && sector[i+3]=='S');
            bool is_pmf = (sector[i]=='.' && sector[i+1]=='P' &&
                           sector[i+2]=='M' && sector[i+3]=='F');
            if (is_mps || is_pmf) {
                // Back-scan for start of filename (non-printable boundary)
                size_t name_start = i;
                while (name_start > 0 &&
                       sector[name_start-1] >= 0x20 &&
                       sector[name_start-1] <  0x7F) {
                    --name_start;
                }

                std::string filename(reinterpret_cast<char*>(sector + name_start),
                                     i + 4 - name_start);

                // LBA is 2 bytes before the record start (at record+2, LE uint16)
                // Record starts 0x38 bytes before the name in UDF FID
                uint32_t lba = 0;
                if (name_start >= 0x09) {
                    size_t rec_base = name_start - 0x09;
                    // bytes [2-3] from record base = LBA (uint16 LE)
                    lba = static_cast<uint32_t>(sector[rec_base + 2]) |
                         (static_cast<uint32_t>(sector[rec_base + 3]) << 8);
                }

                if (lba > 0) {
                    MpsEntry entry;
                    entry.name       = filename;
                    entry.lba        = lba;
                    entry.size_bytes = 0; // will be filled by size probe below
                    m_entries.push_back(entry);
                }

                offset = i + 4;
                found = true;
                break;
            }
        }
        if (!found) break;
    }

    if (m_entries.empty()) {
        std::cerr << "[IsoReader] No .MPS/.PMF files found in STREAM/ directory\n";
        return false;
    }

    // Sort by LBA for natural playback order
    std::sort(m_entries.begin(), m_entries.end(),
              [](const MpsEntry& a, const MpsEntry& b){ return a.lba < b.lba; });

    // Probe size: size = (next_lba - this_lba) * SECTOR_SIZE (approximate)
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (i + 1 < m_entries.size()) {
            m_entries[i].size_bytes =
                (m_entries[i+1].lba - m_entries[i].lba) * UMD_SECTOR_SIZE;
        } else {
            // Last file: read until EOF approximation (16 MB cap for safety)
            m_entries[i].size_bytes = 16 * 1024 * 1024;
        }
    }

    std::cout << "[IsoReader] Found " << m_entries.size() << " stream file(s)\n";
    return true;
}

std::vector<MpsEntry> IsoReader::get_mps_entries() const {
    return m_entries;
}

std::vector<uint8_t> IsoReader::read_mps(const MpsEntry& entry) const {
    std::ifstream f(m_iso_path, std::ios::binary);
    if (!f) return {};
    f.seekg(static_cast<std::streamoff>(entry.lba) * UMD_SECTOR_SIZE);
    std::vector<uint8_t> buf(entry.size_bytes);
    f.read(reinterpret_cast<char*>(buf.data()), entry.size_bytes);
    buf.resize(static_cast<size_t>(f.gcount()));
    return buf;
}

void IsoReader::dump_mps_header(const MpsEntry& entry, size_t bytes) const {
    std::ifstream f(m_iso_path, std::ios::binary);
    if (!f) return;
    f.seekg(static_cast<std::streamoff>(entry.lba) * UMD_SECTOR_SIZE);
    std::vector<uint8_t> buf(bytes);
    f.read(reinterpret_cast<char*>(buf.data()), bytes);
    size_t n = static_cast<size_t>(f.gcount());

    std::cout << "\n[dump] " << entry.name
              << " @ LBA " << entry.lba
              << " (offset " << std::hex << entry.lba * UMD_SECTOR_SIZE << std::dec
              << ") — first " << n << " bytes:\n";

    for (size_t i = 0; i < n; ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(buf[i]);
        if ((i+1) % 16 == 0) std::cout << "\n";
        else                  std::cout << " ";
    }
    std::cout << std::dec << "\n";

    // Verify MPEG Program Stream magic: 0x000001BA
    if (n >= 4 && buf[0]==0x00 && buf[1]==0x00 &&
                  buf[2]==0x01 && buf[3]==0xBA) {
        std::cout << "✅ Valid MPEG Program Stream (0x000001BA)\n";
    } else {
        std::cout << "⚠️  Unexpected header — got: ";
        for (size_t i = 0; i < std::min(n, size_t(4)); ++i)
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(buf[i]) << " ";
        std::cout << std::dec << "\n";
    }
}

bool IsoReader::extract_all(const std::string& output_dir) const {
    bool ok = true;
    for (const auto& entry : m_entries) {
        std::string out_path = output_dir + "/" + entry.name;
        auto data = read_mps(entry);
        if (data.empty()) {
            std::cerr << "[IsoReader] Failed to read: " << entry.name << "\n";
            ok = false;
            continue;
        }
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::cerr << "[IsoReader] Cannot write: " << out_path << "\n";
            ok = false;
            continue;
        }
        out.write(reinterpret_cast<char*>(data.data()), data.size());
        std::cout << "✅ Extracted: " << out_path
                  << " (" << data.size() / 1048576 << " MB)\n";
    }
    return ok;
}
