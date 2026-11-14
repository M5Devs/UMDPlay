#include "iso_reader.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

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

    parse_metadata_dir();
    return parse_udf_stream_dir();
}

// Scan UMD_VIDEO/ root (sector 24) for File Identifier Descriptors naming
// metadata files (PARAM.SFO, ICON0.PNG, PIC1.PNG, SND0.AT3). Uses the same
// offset convention as the STREAM/ parser: LBA at name_start-31 (uint32 LE),
// size at name_start-23 (uint32 LE).
void IsoReader::parse_metadata_dir() {
    uint8_t sector[UMD_SECTOR_SIZE];
    if (!read_sector(UMD_ROOT_DIR_LBA, sector)) return;

    static const char* kNames[] = {
        "PARAM.SFO", "ICON0.PNG", "ICON1.PNG", "PIC1.PNG", "SND0.AT3"
    };

    size_t offset = 0;
    while (offset < UMD_SECTOR_SIZE - 4) {
        bool found = false;
        for (size_t i = offset; i < UMD_SECTOR_SIZE - 4; ++i) {
            for (const char* nm : kNames) {
                const size_t len = std::strlen(nm);
                if (i + len > UMD_SECTOR_SIZE) continue;
                if (std::memcmp(sector + i, nm, len) != 0) continue;

                size_t name_start = i;
                while (name_start > 0 &&
                       sector[name_start - 1] >= 0x20 &&
                       sector[name_start - 1] < 0x7F) {
                    --name_start;
                }

                if (name_start < 31) { offset = i + len; found = true; break; }

                const uint8_t* p_lba = sector + (name_start - 31);
                uint32_t lba = static_cast<uint32_t>(p_lba[0]) |
                               (static_cast<uint32_t>(p_lba[1]) << 8) |
                               (static_cast<uint32_t>(p_lba[2]) << 16) |
                               (static_cast<uint32_t>(p_lba[3]) << 24);

                const uint8_t* p_size = sector + (name_start - 23);
                uint32_t size_bytes = static_cast<uint32_t>(p_size[0]) |
                                      (static_cast<uint32_t>(p_size[1]) << 8) |
                                      (static_cast<uint32_t>(p_size[2]) << 16) |
                                      (static_cast<uint32_t>(p_size[3]) << 24);

                if (lba > 0 && size_bytes > 0) {
                    UmdFile f;
                    f.name       = nm;
                    f.lba        = lba;
                    f.size_bytes = size_bytes;
                    m_metadata.push_back(f);
                }
                offset = i + len;
                found = true;
                break;
            }
            if (found) break;
        }
        if (!found) break;
    }

    // Parse PARAM.SFO eagerly so the UI can show TITLE / DISC_ID immediately.
    for (const auto& f : m_metadata) {
        if (f.name == "PARAM.SFO") {
            std::vector<uint8_t> raw = read_metadata(f.name);
            if (!raw.empty()) parse_param_sfo(raw.data(), raw.size(), m_param);
            break;
        }
    }

    std::cout << "[IsoReader] Metadata files found: " << m_metadata.size();
    if (!m_param.title.empty()) std::cout << " (TITLE: \"" << m_param.title << "\")";
    std::cout << "\n";
}

bool IsoReader::has_metadata(const std::string& filename) const {
    for (const auto& f : m_metadata) {
        if (f.name == filename) return true;
    }
    return false;
}

std::vector<uint8_t> IsoReader::read_metadata(const std::string& filename) const {
    for (const auto& f : m_metadata) {
        if (f.name != filename) continue;
        std::ifstream in(m_iso_path, std::ios::binary);
        if (!in) return {};
        in.seekg(static_cast<std::streamoff>(f.lba) * UMD_SECTOR_SIZE);
        std::vector<uint8_t> buf(f.size_bytes);
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        buf.resize(static_cast<size_t>(in.gcount()));
        return buf;
    }
    return {};
}


bool IsoReader::parse_udf_stream_dir() {
    uint8_t sector[UMD_SECTOR_SIZE];

    if (!read_sector(UMD_STREAM_DIR_LBA, sector)) {
        std::cerr << "[IsoReader] ERROR: Cannot read STREAM/ sector\n";
        return false;
    }

    size_t offset = 0;
    while (offset < UMD_SECTOR_SIZE - 4) {
        bool found = false;
        for (size_t i = offset; i < UMD_SECTOR_SIZE - 4; ++i) {
            bool is_mps = (sector[i]=='.' && sector[i+1]=='M' &&
                           sector[i+2]=='P' && sector[i+3]=='S');
            bool is_pmf = (sector[i]=='.' && sector[i+1]=='P' &&
                           sector[i+2]=='M' && sector[i+3]=='F');
            
            if (is_mps || is_pmf) {
                // نرجع لورا لبداية اسم الملف
                size_t name_start = i;
                while (name_start > 0 &&
                       sector[name_start-1] >= 0x20 &&
                       sector[name_start-1] <  0x7F) {
                    --name_start;
                }

                std::string filename(reinterpret_cast<char*>(sector + name_start),
                                     i + 4 - name_start);

                // الـ LBA موجود 31 بايت قبل بداية الاسم (uint32 LE)
                uint32_t lba = 0;
                uint32_t size_bytes = 0;

                if (name_start >= 31) {
                    const uint8_t* p_lba = sector + (name_start - 31);
                    lba = static_cast<uint32_t>(p_lba[0]) |
                         (static_cast<uint32_t>(p_lba[1]) << 8) |
                         (static_cast<uint32_t>(p_lba[2]) << 16) |
                         (static_cast<uint32_t>(p_lba[3]) << 24);

                    // حجم الملف موجود 23 بايت قبل بداية الاسم (uint32 LE)
                    const uint8_t* p_size = sector + (name_start - 23);
                    size_bytes = static_cast<uint32_t>(p_size[0]) |
                                (static_cast<uint32_t>(p_size[1]) << 8) |
                                (static_cast<uint32_t>(p_size[2]) << 16) |
                                (static_cast<uint32_t>(p_size[3]) << 24);
                }

                if (lba > 0) {
                    MpsEntry entry;
                    entry.name       = filename;
                    entry.lba        = lba;
                    entry.size_bytes = size_bytes;
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

    std::sort(m_entries.begin(), m_entries.end(),
              [](const MpsEntry& a, const MpsEntry& b){ return a.lba < b.lba; });

    std::cout << "[IsoReader] Successfully found " << m_entries.size() << " stream file(s)!\n";
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
    std::vector<uint8_t> buf(std::min<size_t>(bytes, 4096));
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const size_t got = static_cast<size_t>(f.gcount());

    std::cout << "[IsoReader] " << entry.name << " header ("
              << got << " bytes):\n";
    for (size_t i = 0; i < got; i += 16) {
        std::cout << "  " << std::hex << std::setw(6) << std::setfill('0') << i
                  << "  " << std::dec << std::setfill(' ');
        for (size_t j = 0; j < 16 && i + j < got; ++j) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(buf[i + j]) << ' ';
        }
        std::cout << "\n";
    }
}

bool IsoReader::extract_all(const std::string& output_dir) const {
    if (m_entries.empty()) return false;
    for (const auto& e : m_entries) {
        const std::string path = output_dir + "/" + e.name;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::cerr << "[IsoReader] Cannot write " << path << "\n";
            return false;
        }
        std::ifstream in(m_iso_path, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(e.lba) * UMD_SECTOR_SIZE);
        std::vector<char> chunk(1 << 20);
        uint64_t remaining = e.size_bytes;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
            in.read(chunk.data(), static_cast<std::streamsize>(want));
            const size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) break;
            out.write(chunk.data(), static_cast<std::streamsize>(got));
            remaining -= got;
        }
    }
    return true;
}
