#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Parsed contents of a Sony PARAM.SFO ("\0PSF" format) metadata block.
struct ParamSfo {
    std::string title;      // TITLE
    std::string disc_id;    // DISC_ID
    std::string category;   // CATEGORY (e.g. "UV", "UA", "UG")
    std::string app_ver;    // APP_VER (optional)
};

// Parse a PARAM.SFO image held in memory. Bounds-checked: a truncated or
// malformed buffer yields a default-constructed ParamSfo (empty fields) and
// returns false. Never reads past the buffer.
bool parse_param_sfo(const uint8_t* data, size_t size, ParamSfo& out);
