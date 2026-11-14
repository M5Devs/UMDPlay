#pragma once

// Probe / parser for UMD RESOURCE/*.RCO resource files.
//
// An .RCO ("resource container") holds the disc's menu resources: PNG images,
// VSMX (a small Sony bytecode) and lookup tables. UMD Video discs ship one or
// more under RESOURCE/ (e.g. JA100000.RCO). We do not execute the VSMX; this
// module only walks the container's table of contents so --probe can report
// what a disc carries.
//
// Verified layout (see docs/ARCHITECTURE.md):
//   0x00  char[4]  magic  "\0PRF"
//   0x04  dword    version (0x80)
//   0x0C  dword    1
//   0x10  dword[2*N] table of contents: (offset, size) little-endian pairs,
//                    0xFFFFFFFF marks an absent entry
//   ...   payload: embedded PNGs, a "VSMX" block, other tables
//
// Pure and bounds-checked; never throws.

#include <cstdint>
#include <string>
#include <vector>

namespace umdplay {

struct RcoEntry {
    int      slot = 0;       // TOC slot index
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct RcoImage {
    uint32_t offset = 0;     // absolute offset of the PNG signature
    uint32_t size = 0;       // approximate size (signature .. IEND+8)
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RcoFile {
    bool        valid = false;
    std::string error;
    std::string magic;          // "PRF" (the leading NUL is stripped for display)
    uint32_t    version = 0;
    std::vector<RcoEntry> toc;  // non-empty (offset,size) entries
    std::vector<RcoImage> images;
    bool        has_vsmx = false;
    uint32_t    vsmx_offset = 0;
    uint32_t    vsmx_size = 0;
    size_t      size_bytes = 0;

    // Count of TOC slots that are "present" (offset != 0xFFFFFFFF).
    size_t present_entries = 0;
};

// Parse the raw .RCO bytes. Never throws.
RcoFile parse_rco(const uint8_t* data, size_t size);

// One-line-per-fact human summary for --probe.
std::string rco_summary(const RcoFile& rco);

}  // namespace umdplay
