#include "param_sfo.h"
#include <cstring>

namespace {

uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Sony SFO data formats we care about.
constexpr uint16_t SFO_FMT_UTF8    = 0x0204;  // NUL-terminated UTF-8
constexpr uint16_t SFO_FMT_UINT32  = 0x0404;  // little-endian uint32

}  // namespace

bool parse_param_sfo(const uint8_t* data, size_t size, ParamSfo& out) {
    out = ParamSfo{};
    if (!data || size < 20) return false;
    if (std::memcmp(data, "\0PSF", 4) != 0) return false;

    const uint32_t key_table   = rd_u32(data + 8);
    const uint32_t value_table = rd_u32(data + 12);
    const uint32_t entry_count = rd_u32(data + 16);

    if (key_table > size || value_table > size) return false;

    constexpr uint32_t ENTRY_SIZE = 16;
    if (entry_count > (size - 20) / ENTRY_SIZE) return false;

    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* e = data + 20 + i * ENTRY_SIZE;
        const uint16_t key_off = static_cast<uint16_t>(e[0] | (e[1] << 8));
        const uint16_t fmt     = static_cast<uint16_t>(e[2] | (e[3] << 8));
        const uint32_t len     = rd_u32(e + 4);
        const uint32_t val_off = rd_u32(e + 12);

        const size_t key_pos = static_cast<size_t>(key_table) + key_off;
        if (key_pos >= size) continue;

        size_t key_end = key_pos;
        while (key_end < size && data[key_end] != 0) ++key_end;
        std::string key(reinterpret_cast<const char*>(data + key_pos), key_end - key_pos);

        const size_t val_pos = static_cast<size_t>(value_table) + val_off;
        if (val_pos > size || len > size - val_pos) continue;

        std::string value;
        if (fmt == SFO_FMT_UTF8) {
            const uint8_t* s = data + val_pos;
            size_t n = 0;
            while (n < len && s[n] != 0) ++n;
            value.assign(reinterpret_cast<const char*>(s), n);
        } else if (fmt == SFO_FMT_UINT32 && len >= 4) {
            value = std::to_string(rd_u32(data + val_pos));
        }

        if (key == "TITLE")          out.title   = value;
        else if (key == "DISC_ID")   out.disc_id = value;
        else if (key == "CATEGORY")  out.category = value;
        else if (key == "APP_VER")   out.app_ver  = value;
    }

    return true;
}
