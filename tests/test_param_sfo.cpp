#include "param_sfo.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

// Build a minimal PARAM.SFO in memory with the given key/value pairs.
static std::vector<uint8_t> make_sfo(
    const std::vector<std::pair<std::string, std::string>>& kv) {
    const uint32_t count = static_cast<uint32_t>(kv.size());
    const uint32_t header = 20;
    const uint32_t index_size = count * 16;

    // Key table: NUL-terminated keys, indices tracked.
    std::vector<uint8_t> keys;
    std::vector<uint16_t> key_off;
    for (auto& p : kv) {
        key_off.push_back(static_cast<uint16_t>(keys.size()));
        keys.insert(keys.end(), p.first.begin(), p.first.end());
        keys.push_back(0);
    }
    while (keys.size() % 4) keys.push_back(0);

    // Value table: NUL-terminated UTF-8 strings.
    std::vector<uint8_t> vals;
    std::vector<uint32_t> val_off;
    for (auto& p : kv) {
        val_off.push_back(static_cast<uint32_t>(vals.size()));
        vals.insert(vals.end(), p.second.begin(), p.second.end());
        vals.push_back(0);
    }
    while (vals.size() % 4) vals.push_back(0);

    const uint32_t key_table = header + index_size;
    const uint32_t val_table = key_table + static_cast<uint32_t>(keys.size());

    std::vector<uint8_t> out(header + index_size + keys.size() + vals.size(), 0);
    std::memcpy(out.data(), "\0PSF", 4);
    auto put32 = [&](uint32_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>(v);
        out[off + 1] = static_cast<uint8_t>(v >> 8);
        out[off + 2] = static_cast<uint8_t>(v >> 16);
        out[off + 3] = static_cast<uint8_t>(v >> 24);
    };
    put32(4, 0x00000101);
    put32(8, key_table);
    put32(12, val_table);
    put32(16, count);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = header + i * 16;
        out[e + 0] = static_cast<uint8_t>(key_off[i]);
        out[e + 1] = static_cast<uint8_t>(key_off[i] >> 8);
        out[e + 2] = 0x04;   // format 0x0204
        out[e + 3] = 0x02;
        put32(e + 4, static_cast<uint32_t>(kv[i].second.size() + 1));
        put32(e + 8, static_cast<uint32_t>(kv[i].second.size() + 1));
        put32(e + 12, val_off[i]);
    }
    std::memcpy(out.data() + key_table, keys.data(), keys.size());
    std::memcpy(out.data() + val_table, vals.data(), vals.size());
    return out;
}

int main() {
    // Happy path.
    {
        auto sfo = make_sfo({
            {"TITLE", "Miku no Hi 39's Giving Day"},
            {"DISC_ID", "UCJS10001"},
            {"CATEGORY", "UV"},
        });
        ParamSfo p;
        assert(parse_param_sfo(sfo.data(), sfo.size(), p));
        assert(p.title == "Miku no Hi 39's Giving Day");
        assert(p.disc_id == "UCJS10001");
        assert(p.category == "UV");
    }

    // Missing keys -> empty fields, still succeeds.
    {
        auto sfo = make_sfo({{"TITLE", "Bare Disc"}});
        ParamSfo p;
        assert(parse_param_sfo(sfo.data(), sfo.size(), p));
        assert(p.title == "Bare Disc");
        assert(p.disc_id.empty());
        assert(p.category.empty());
    }

    // Bad magic rejected.
    {
        auto sfo = make_sfo({{"TITLE", "x"}});
        sfo[1] = 'X';
        ParamSfo p;
        assert(!parse_param_sfo(sfo.data(), sfo.size(), p));
    }

    // Truncated buffer must not read out of bounds.
    {
        auto sfo = make_sfo({{"TITLE", "Truncated"}});
        ParamSfo p;
        for (size_t n = 0; n < sfo.size(); ++n) {
            parse_param_sfo(sfo.data(), n, p);   // must not crash (ASan-checked)
        }
        assert(!parse_param_sfo(sfo.data(), 4, p));
    }

    // Absurd entry count must be rejected.
    {
        auto sfo = make_sfo({{"TITLE", "y"}});
        sfo[16] = 0xFF; sfo[17] = 0xFF; sfo[18] = 0xFF; sfo[19] = 0x7F;
        ParamSfo p;
        assert(!parse_param_sfo(sfo.data(), sfo.size(), p));
    }

    std::cout << "All PARAM.SFO parser tests passed!\n";
    return 0;
}
