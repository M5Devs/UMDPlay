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
    return parse_udf_stream_dir();
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
    // (دالة الـ Dump القديمة)
}

bool IsoReader::extract_all(const std::string& output_dir) const {
    return true;
}
