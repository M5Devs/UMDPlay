#include "rco_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace umdplay {

namespace {

bool le32(const uint8_t* d, size_t n, size_t off, uint32_t& out) {
    if (off + 4 > n) return false;
    out = static_cast<uint32_t>(d[off]) |
          (static_cast<uint32_t>(d[off + 1]) << 8) |
          (static_cast<uint32_t>(d[off + 2]) << 16) |
          (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}
bool be32(const uint8_t* d, size_t n, size_t off, uint32_t& out) {
    if (off + 4 > n) return false;
    out = (static_cast<uint32_t>(d[off]) << 24) |
          (static_cast<uint32_t>(d[off + 1]) << 16) |
          (static_cast<uint32_t>(d[off + 2]) << 8) |
          static_cast<uint32_t>(d[off + 3]);
    return true;
}

constexpr size_t kTocStart   = 0x10;
constexpr size_t kTocEnd     = 0xA0;   // table region (verified end)
constexpr size_t kMaxImages  = 4096;

}  // namespace

RcoFile parse_rco(const uint8_t* data, size_t size) {
    RcoFile r;
    r.size_bytes = size;
    if (!data || size < kTocEnd) {
        r.error = "file too small for an RCO header";
        return r;
    }
    // Magic is "\0PRF"; display it without the leading NUL.
    if (data[0] != 0x00 || data[1] != 'P' || data[2] != 'R' || data[3] != 'F') {
        r.error = "bad magic (expected \\0PRF)";
        return r;
    }
    r.magic = "PRF";
    le32(data, size, 0x04, r.version);

    // Table of contents: little-endian (offset, size) pairs, 0xFFFFFFFF = none.
    for (size_t off = kTocStart; off + 8 <= kTocEnd; off += 8) {
        uint32_t o = 0, s = 0;
        if (!le32(data, size, off, o)) break;
        if (!le32(data, size, off + 4, s)) break;
        if (o == 0xFFFFFFFF && s == 0xFFFFFFFF) continue;   // both absent
        RcoEntry e;
        e.slot = static_cast<int>((off - kTocStart) / 8);
        e.offset = o; e.size = s;
        if (o != 0xFFFFFFFF) ++r.present_entries;
        r.toc.push_back(e);
    }

    // Embedded PNG images: scan for the 8-byte signature, read IHDR dims.
    static const uint8_t kPngSig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    for (size_t i = 0; i + 24 <= size && r.images.size() < kMaxImages; ++i) {
        if (std::memcmp(data + i, kPngSig, 8) != 0) continue;
        RcoImage img;
        img.offset = static_cast<uint32_t>(i);
        // IHDR width/height are big-endian at offset 16 / 20 of the file.
        be32(data, size, i + 16, img.width);
        be32(data, size, i + 20, img.height);
        // Find the terminating IEND chunk for an approximate size.
        size_t j = i + 8;
        while (j + 8 <= size) {
            if (std::memcmp(data + j, "IEND", 4) == 0) { j += 8; break; }
            ++j;
        }
        img.size = static_cast<uint32_t>(j - i);
        r.images.push_back(img);
        i = j ? j - 1 : i;   // continue after this image
    }

    // VSMX bytecode block (Sony's menu VM). Presence + extent only.
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (std::memcmp(data + i, "VSMX", 4) == 0) {
            r.has_vsmx = true;
            r.vsmx_offset = static_cast<uint32_t>(i);
            // The VSMX header's first dword after the magic is a version/length
            // word; we report the extent to the next embedded resource when we
            // can, else to EOF.
            uint32_t sz = 0;
            if (le32(data, size, i + 8, sz) && sz > 0 &&
                static_cast<uint64_t>(i) + sz <= size) {
                r.vsmx_size = sz;
            } else {
                r.vsmx_size = static_cast<uint32_t>(size - i);
            }
            break;
        }
    }

    r.valid = true;
    return r;
}

std::string rco_summary(const RcoFile& rco) {
    char buf[256];
    std::string out;
    if (!rco.valid) {
        out = "RCO: INVALID (" + rco.error + ")\n";
        return out;
    }
    std::snprintf(buf, sizeof(buf),
                  "RCO: magic=\\0%s version=0x%02x size=%zu TOC entries=%zu (present=%zu)\n",
                  rco.magic.c_str(), rco.version, rco.size_bytes,
                  rco.toc.size(), rco.present_entries);
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  "     images=%zu  VSMX=%s (offset=0x%x size=%u)\n",
                  rco.images.size(), rco.has_vsmx ? "present" : "absent",
                  rco.vsmx_offset, rco.vsmx_size);
    out += buf;
    if (!rco.images.empty()) {
        // Report the distinct image dimensions, which reveal the menu button
        // art (209x57 / 252x30 / 200x39 are the common UMD menu sizes).
        out += "     image sizes:";
        std::vector<std::string> seen;
        for (const auto& im : rco.images) {
            char d[32];
            std::snprintf(d, sizeof(d), " %ux%u", im.width, im.height);
            if (std::find(seen.begin(), seen.end(), d) == seen.end()) {
                seen.push_back(d);
                out += d;
            }
        }
        out += "\n";
    }
    return out;
}

}  // namespace umdplay
