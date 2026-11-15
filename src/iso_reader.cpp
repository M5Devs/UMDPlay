#include "iso_reader.h"
#include <cstring>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

IsoReader::IsoReader(const std::string& iso_path)
    : m_iso_path(iso_path) {}

IsoReader::~IsoReader() {}

bool IsoReader::read_sector(uint32_t lba, uint8_t* buf) const {
    return m_reader.read_sector(lba, buf);
}

bool IsoReader::open() {
    if (!m_reader.open(m_iso_path)) {
        std::cerr << "[IsoReader] ERROR: Cannot open image: " << m_iso_path
                  << " (" << m_reader.last_error() << ")\n";
        return false;
    }
    std::cout << "[IsoReader] Opened: " << m_iso_path
              << "  [" << m_reader.backend_name() << " backend, "
              << m_reader.size() << " bytes]\n";
    std::cout << "[IsoReader] Using UDF path (STREAM/ @ LBA "
              << UMD_STREAM_DIR_LBA << ")\n";

    parse_metadata_dir();
    const bool ok = parse_udf_stream_dir();
    discover_assets();
    return ok;
}

// --- ISO 9660 directory traversal ---------------------------------------
// The UMD discs are dual ISO9660/UDF: the ISO9660 root directory is a plain
// directory record tree we can walk to find every file (the hand-rolled UDF
// STREAM/ parser above only knows a fixed LBA). This walker is what lets us
// discover PLAYLIST.UMD, CLIPINF/*.CLP and RESOURCE/*.RCO wherever they live.

std::vector<IsoReader::IsoDirRecord> IsoReader::list_iso_dir(uint32_t lba,
                                                             uint32_t size) const {
    std::vector<IsoDirRecord> out;
    if (size == 0) return out;
    // Clamp to a sane bound (a directory never legitimately exceeds 1 MiB here).
    const uint32_t cap = std::min<uint32_t>(size, 1u << 20);
    std::vector<uint8_t> data(cap);
    const size_t got = m_reader.read_at(static_cast<uint64_t>(lba) * UMD_SECTOR_SIZE,
                                        data.data(), cap);
    data.resize(got);

    size_t off = 0;
    while (off + 33 <= data.size()) {
        const uint8_t len = data[off];
        if (len == 0) {
            // Padding to the next logical sector boundary.
            off = ((off / UMD_SECTOR_SIZE) + 1) * UMD_SECTOR_SIZE;
            continue;
        }
        if (off + len > data.size() || len < 33) break;

        const uint8_t* rec = data.data() + off;
        IsoDirRecord r;
        r.lba        = static_cast<uint32_t>(rec[2]) |
                       (static_cast<uint32_t>(rec[3]) << 8) |
                       (static_cast<uint32_t>(rec[4]) << 16) |
                       (static_cast<uint32_t>(rec[5]) << 24);
        r.size_bytes = static_cast<uint32_t>(rec[10]) |
                       (static_cast<uint32_t>(rec[11]) << 8) |
                       (static_cast<uint32_t>(rec[12]) << 16) |
                       (static_cast<uint32_t>(rec[13]) << 24);
        const uint8_t flags = rec[25];
        const uint8_t name_len = rec[32];
        if (33u + name_len > len) { off += len; continue; }
        const char* name = reinterpret_cast<const char*>(rec + 33);

        // Skip the "." and ".." self/parent entries (name 0x00 / 0x01).
        if (name_len == 1 && (static_cast<uint8_t>(name[0]) == 0 ||
                              static_cast<uint8_t>(name[0]) == 1)) {
            off += len;
            continue;
        }
        r.name.assign(name, name_len);
        // Strip a trailing ";1" ISO9660 version suffix if present.
        const size_t semi = r.name.find(';');
        if (semi != std::string::npos) r.name.resize(semi);
        r.is_dir = (flags & 0x02) != 0;
        out.push_back(std::move(r));
        off += len;
    }
    return out;
}

std::vector<uint8_t> IsoReader::read_file(uint32_t lba, uint32_t size_bytes) const {
    if (size_bytes == 0) return {};
    // Guard against a corrupt size field (cap at 64 MiB — PLAYLIST/CLP/RCO are
    // all far smaller; the biggest .MPS is read through read_mps()).
    const uint32_t cap = std::min<uint32_t>(size_bytes, 64u << 20);
    std::vector<uint8_t> buf(cap);
    const size_t got = m_reader.read_at(static_cast<uint64_t>(lba) * UMD_SECTOR_SIZE,
                                        buf.data(), cap);
    buf.resize(got);
    return buf;
}

// Discover the content root (UMD_VIDEO, else UMD_AUDIO) and the menu assets
// under it. Uses the ISO9660 tree so the layout is found, not hard-coded.
void IsoReader::discover_assets() {
    // ISO9660 primary volume descriptor -> root directory record.
    std::vector<uint8_t> pvd = read_file(16, UMD_SECTOR_SIZE);
    if (pvd.size() < 190) return;
    // Root directory record lives at offset 156 of the PVD.
    const uint8_t* rde = pvd.data() + 156;
    const uint32_t root_lba = static_cast<uint32_t>(rde[2]) |
                              (static_cast<uint32_t>(rde[3]) << 8) |
                              (static_cast<uint32_t>(rde[4]) << 16) |
                              (static_cast<uint32_t>(rde[5]) << 24);
    const uint32_t root_size = static_cast<uint32_t>(rde[10]) |
                               (static_cast<uint32_t>(rde[11]) << 8) |
                               (static_cast<uint32_t>(rde[12]) << 16) |
                               (static_cast<uint32_t>(rde[13]) << 24);

    auto root = list_iso_dir(root_lba, root_size);

    // Pick the content root: UMD_VIDEO preferred, UMD_AUDIO as fallback.
    uint32_t content_lba = 0, content_size = 0;
    for (const auto& e : root) {
        if (!e.is_dir) continue;
        if (e.name == "UMD_VIDEO") {
            content_lba = e.lba; content_size = e.size_bytes;
            m_root = UmdRoot::Video; m_root_name = "UMD_VIDEO";
            break;   // video wins outright
        }
        if (e.name == "UMD_AUDIO" && m_root == UmdRoot::None) {
            content_lba = e.lba; content_size = e.size_bytes;
            m_root = UmdRoot::Audio; m_root_name = "UMD_AUDIO";
        }
    }
    if (m_root == UmdRoot::None) return;

    // Walk the content root and its immediate sub-directories.
    auto content = list_iso_dir(content_lba, content_size);
    for (const auto& e : content) {
        if (e.is_dir) {
            auto sub = list_iso_dir(e.lba, e.size_bytes);
            for (const auto& s : sub) {
                if (s.is_dir) continue;
                m_assets.push_back({s.name, m_root_name + "/" + e.name,
                                    s.lba, s.size_bytes});
            }
        } else {
            m_assets.push_back({e.name, m_root_name, e.lba, e.size_bytes});
        }
    }

    std::sort(m_assets.begin(), m_assets.end(),
              [](const UmdAsset& a, const UmdAsset& b) {
                  if (a.dir != b.dir) return a.dir < b.dir;
                  return a.name < b.name;
              });

    std::cout << "[IsoReader] Content root: " << m_root_name
              << "  (" << m_assets.size() << " menu asset(s))\n";
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
        std::vector<uint8_t> buf(f.size_bytes);
        const size_t got = m_reader.read_at(
            static_cast<uint64_t>(f.lba) * UMD_SECTOR_SIZE, buf.data(), buf.size());
        buf.resize(got);
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

const UmdAsset* IsoReader::find_asset(const std::string& name) const {
    for (const auto& a : m_assets) {
        if (a.name.size() != name.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < a.name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a.name[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) { eq = false; break; }
        }
        if (eq) return &a;
    }
    return nullptr;
}

std::vector<UmdAsset> IsoReader::assets_in(const std::string& dir) const {
    std::vector<UmdAsset> out;
    // Accept either a bare sub-dir ("CLIPINF") or a rooted one ("UMD_VIDEO/CLIPINF").
    std::string want = dir;
    if (!m_root_name.empty()) {
        if (dir.empty()) want = m_root_name;
        else if (dir.rfind(m_root_name, 0) != 0) want = m_root_name + "/" + dir;
    }
    for (const auto& a : m_assets)
        if (a.dir == want) out.push_back(a);
    return out;
}

std::vector<uint8_t> IsoReader::read_asset(const UmdAsset& asset) const {
    return read_file(asset.lba, asset.size_bytes);
}

std::vector<uint8_t> IsoReader::read_asset(const std::string& name) const {
    const UmdAsset* a = find_asset(name);
    return a ? read_file(a->lba, a->size_bytes) : std::vector<uint8_t>{};
}

std::vector<uint8_t> IsoReader::read_mps(const MpsEntry& entry) const {
    std::vector<uint8_t> buf(entry.size_bytes);
    const size_t got = m_reader.read_at(
        static_cast<uint64_t>(entry.lba) * UMD_SECTOR_SIZE, buf.data(), buf.size());
    buf.resize(got);
    return buf;
}

void IsoReader::dump_mps_header(const MpsEntry& entry, size_t bytes) const {
    const size_t want = std::min<size_t>(bytes, 4096);
    std::vector<uint8_t> buf(want);
    const size_t got = m_reader.read_at(
        static_cast<uint64_t>(entry.lba) * UMD_SECTOR_SIZE, buf.data(), want);

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
        std::vector<char> chunk(1 << 20);
        uint64_t remaining = e.size_bytes;
        uint64_t cursor = static_cast<uint64_t>(e.lba) * UMD_SECTOR_SIZE;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
            const size_t got = m_reader.read_at(cursor, chunk.data(), want);
            if (got == 0) break;
            out.write(chunk.data(), static_cast<std::streamsize>(got));
            cursor += got;
            remaining -= got;
        }
    }
    return true;
}
