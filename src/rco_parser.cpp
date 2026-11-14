#include "rco_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace umdplay {

namespace {

constexpr uint32_t kNull = 0xFFFFFFFF;
constexpr size_t   kHeaderSize = 0xA4;   // 164 bytes
constexpr size_t   kEntrySize = 40;
constexpr size_t   kMaxEntries = 20000;
constexpr size_t   kMaxDepth = 32;

bool le16(const uint8_t* d, size_t n, size_t off, uint16_t& out) {
    if (off + 2 > n) return false;
    out = static_cast<uint16_t>(d[off]) |
          static_cast<uint16_t>(d[off + 1] << 8);
    return true;
}
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
float as_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Object data size (bytes after the 40-byte entry header) by OBJ type.
size_t obj_data_size(int type) {
    switch (type) {
        case RCO_OBJ_PAGE:     return 36;
        case RCO_OBJ_PLANE:    return 76;
        case RCO_OBJ_BUTTON:   return 164;
        case RCO_OBJ_XMENU:    return 108;
        case RCO_OBJ_XMLIST:   return 24;
        case RCO_OBJ_XLIST:    return 124;
        case RCO_OBJ_PROGRESS: return 100;
        case RCO_OBJ_SCROLL:   return 120;
        case RCO_OBJ_MLIST:    return 176;
        case RCO_OBJ_MITEM:    return 24;
        case RCO_OBJ_XITEM:    return 24;
        case RCO_OBJ_TEXT:     return 224;
        case RCO_OBJ_MODEL:    return 72;
        case RCO_OBJ_SPIN:     return 156;
        case RCO_OBJ_ACTION:   return 8;
        case RCO_OBJ_ITEMSPIN: return 168;
        case RCO_OBJ_GROUP:    return 64;
        case RCO_OBJ_LLIST:    return 144;
        case RCO_OBJ_LITEM:    return 24;
        case RCO_OBJ_EDIT:     return 184;
        case RCO_OBJ_CLOCK:    return 176;
        case RCO_OBJ_ILIST:    return 100;
        case RCO_OBJ_IITEM:    return 16;
        case RCO_OBJ_ICON:     return 88;
        case RCO_OBJ_UBUTTON:  return 132;
        default:               return 0;
    }
}

struct Entry {
    uint32_t offset = 0;
    uint8_t  type = 0;
    uint8_t  id = 0;
    uint32_t label_offset = kNull;
    uint32_t entry_size = 0;
    uint32_t num_sub = 0;
    uint32_t parent_tbl = 0;
    uint32_t parent_off = kNull;   // resolved parent entry offset
    int      depth = 0;
    std::string label;
    uint32_t payload = 0;    // start of the entry payload
    uint32_t end = 0;        // offset just past this entry (incl. children)
    // VSMX
    uint32_t vsmx_off = 0, vsmx_len = 0;
    // IMG/MODEL
    uint16_t img_format = 0, img_comp = 0;
    uint32_t img_packed = 0, img_off = 0;
    // TEXT
    uint16_t text_lang = 0, text_fmt = 0;
    uint32_t text_num = 0, text_idx_off = 0;
    // SOUND
    uint16_t snd_format = 0, snd_channels = 0;
};

// Read a NUL-terminated ASCII label at pLabelData+labelOffset.
std::string read_label(const uint8_t* d, size_t n, uint32_t base, uint32_t len,
                       uint32_t label_offset) {
    if (label_offset == kNull || base == kNull) return {};
    size_t o = static_cast<size_t>(base) + label_offset;
    if (o >= n) return {};
    size_t max = n;
    if (len && static_cast<size_t>(base) + len <= n)
        max = static_cast<size_t>(base) + len;
    size_t e = o;
    while (e < max && d[e] != 0) ++e;
    return std::string(reinterpret_cast<const char*>(d + o), e - o);
}

// Read a NUL-terminated ASCII event name at pEventData+value.
std::string read_event(const uint8_t* d, size_t n, uint32_t base, uint32_t len,
                       uint32_t value) {
    if (value == kNull || base == kNull) return {};
    size_t o = static_cast<size_t>(base) + value;
    if (o >= n) return {};
    size_t max = n;
    if (len && static_cast<size_t>(base) + len <= n)
        max = static_cast<size_t>(base) + len;
    size_t e = o;
    while (e < max && d[e] != 0) ++e;
    return std::string(reinterpret_cast<const char*>(d + o), e - o);
}

// Parse the VSMX blob at `off`. `avail` is the number of bytes available.
void parse_vsmx(const uint8_t* d, size_t n, size_t off, size_t avail,
                const std::string& name, RcoVsmx& out) {
    if (off + 52 > n || std::memcmp(d + off, "VSMX", 4) != 0) return;
    uint32_t sig = 0, ver = 0, code_off = 0, code_len = 0;
    uint32_t text_off = 0, text_len = 0, text_n = 0;
    uint32_t prop_off = 0, prop_len = 0, prop_n = 0;
    uint32_t names_off = 0, names_len = 0, names_n = 0;
    le32(d, n, off + 0x00, sig);
    le32(d, n, off + 0x04, ver);
    le32(d, n, off + 0x08, code_off);
    le32(d, n, off + 0x0C, code_len);
    le32(d, n, off + 0x10, text_off);
    le32(d, n, off + 0x14, text_len);
    le32(d, n, off + 0x18, text_n);
    le32(d, n, off + 0x1C, prop_off);
    le32(d, n, off + 0x20, prop_len);
    le32(d, n, off + 0x24, prop_n);
    le32(d, n, off + 0x28, names_off);
    le32(d, n, off + 0x2C, names_len);
    le32(d, n, off + 0x30, names_n);
    if (sig != 0x584D5356) return;

    const size_t limit = std::min(static_cast<size_t>(avail), n - off);
    auto in_range = [&](uint32_t o, uint32_t l) {
        return static_cast<uint64_t>(o) + l <= limit;
    };

    out.present = true;
    out.offset = static_cast<uint32_t>(off);
    out.size = static_cast<uint32_t>(limit);
    out.name = name;
    out.version = ver;

    if (in_range(code_off, code_len)) {
        out.code.assign(d + off + code_off, d + off + code_off + code_len);
        out.code_length = code_len;
    }

    auto read_strings = [&](uint32_t o, uint32_t l, uint32_t entries, int width) {
        std::vector<std::string> v;
        if (!in_range(o, l)) return v;
        const uint8_t* p = d + off + o;
        size_t start = 0;
        for (size_t i = 0; i + width <= l && v.size() < entries; i += width) {
            bool zero = true;
            for (int k = 0; k < width; ++k)
                if (p[i + k] != 0) { zero = false; break; }
            if (!zero) continue;
            std::string s;
            if (width == 2) {
                for (size_t j = start; j + 1 < i; j += 2)
                    s.push_back(static_cast<char>(p[j]));
            } else {
                s.assign(reinterpret_cast<const char*>(p + start), i - start);
            }
            v.push_back(std::move(s));
            start = i + width;
        }
        return v;
    };

    out.texts = read_strings(text_off, text_len, text_n, 2);
    out.properties = read_strings(prop_off, prop_len, prop_n, 2);
    out.names = read_strings(names_off, names_len, names_n, 1);
}

}  // namespace

// ---------------------------------------------------------------------------

std::vector<uint32_t> RcoFile::pages() const {
    std::vector<uint32_t> out;
    for (const auto& o : objects)
        if (o.is_page()) out.push_back(o.offset);
    return out;
}

const RcoObject* RcoFile::object(uint32_t offset) const {
    for (const auto& o : objects)
        if (o.offset == offset) return &o;
    return nullptr;
}

std::vector<uint32_t> RcoFile::children_of(uint32_t offset) const {
    const RcoObject* o = object(offset);
    return o ? o->children : std::vector<uint32_t>{};
}

const RcoImage* RcoFile::image_at(uint32_t offset) const {
    for (const auto& im : images)
        if (im.entry_offset == offset) return &im;
    return nullptr;
}

// ---------------------------------------------------------------------------

RcoFile parse_rco(const uint8_t* d, size_t n) {
    RcoFile r;
    r.size_bytes = n;
    if (!d || n < kHeaderSize) {
        r.error = "file too small for an RCO header";
        return r;
    }
    if (d[0] != 0x00 || d[1] != 'P' || d[2] != 'R' || d[3] != 'F') {
        r.error = "bad magic (expected \\0PRF)";
        return r;
    }
    r.magic = "PRF";
    le32(d, n, 0x04, r.version);
    le32(d, n, 0x0C, r.compression);

    le32(d, n, 0x10, r.p_main);
    le32(d, n, 0x14, r.p_vsmx);
    le32(d, n, 0x18, r.p_text);
    le32(d, n, 0x1C, r.p_sound);
    le32(d, n, 0x20, r.p_model);
    le32(d, n, 0x24, r.p_img);
    le32(d, n, 0x2C, r.p_font);
    le32(d, n, 0x30, r.p_obj);
    le32(d, n, 0x34, r.p_anim);
    le32(d, n, 0x40, r.p_label_data);
    le32(d, n, 0x44, r.l_label_data);
    le32(d, n, 0x48, r.p_event_data);
    le32(d, n, 0x4C, r.l_event_data);
    le32(d, n, 0x80, r.p_img_data);
    le32(d, n, 0x84, r.l_img_data);

    // Events: pEventData blob is a run of NUL-terminated ASCII names; the
    // reference `value` is the byte offset of a name from pEventData.
    if (r.p_event_data != kNull && r.l_event_data > 0 &&
        static_cast<size_t>(r.p_event_data) < n) {
        size_t start = r.p_event_data;
        size_t end = std::min(n, start + static_cast<size_t>(r.l_event_data));
        size_t i = start;
        while (i < end) {
            size_t e = i;
            while (e < end && d[e] != 0) ++e;
            std::string s(reinterpret_cast<const char*>(d + i), e - i);
            r.events.push_back(std::move(s));
            i = (e < end) ? e + 1 : end;
        }
    }

    // Walk the entry tree. Sub-entries are contiguous after the parent payload;
    // each call returns the offset just past the entry (and all its children).
    std::vector<Entry> entries;
    entries.reserve(256);
    std::function<uint32_t(uint32_t, int, uint32_t)> parse_entry =
        [&](uint32_t off, int depth, uint32_t parent_off) -> uint32_t {
            if (entries.size() >= kMaxEntries) return off + kEntrySize;
            if (off + kEntrySize > n) return off + kEntrySize;
            Entry e;
            e.offset = off;
            e.type = d[off];
            e.id = d[off + 1];
            e.parent_off = parent_off;
            e.depth = depth;
            le32(d, n, off + 4, e.label_offset);
            le32(d, n, off + 12, e.entry_size);
            le32(d, n, off + 16, e.num_sub);
            le32(d, n, off + 28, e.parent_tbl);
            e.label = read_label(d, n, r.p_label_data, r.l_label_data,
                                 e.label_offset);
            e.payload = off + kEntrySize;
            uint32_t pos = e.payload;

            switch (e.id) {
                case RCO_ID_VSMX:
                    if (e.type == 1) {
                        le32(d, n, pos, e.vsmx_off);
                        le32(d, n, pos + 4, e.vsmx_len);
                        pos = e.payload + 8 + e.vsmx_off + e.vsmx_len;
                        pos = (pos + 3) & ~3u;
                    }
                    break;
                case RCO_ID_IMG:
                case RCO_ID_MODEL:
                    if (e.type == 1) {
                        le16(d, n, pos, e.img_format);
                        le16(d, n, pos + 2, e.img_comp);
                        le32(d, n, pos + 4, e.img_packed);
                        le32(d, n, pos + 8, e.img_off);
                        pos = e.payload + 12 + (e.img_comp != 0 ? 4u : 0u);
                    }
                    break;
                case RCO_ID_TEXT:
                    if (e.type == 1) {
                        le16(d, n, pos, e.text_lang);
                        le16(d, n, pos + 2, e.text_fmt);
                        le32(d, n, pos + 4, e.text_num);
                        e.text_idx_off = pos + 8;
                        pos = e.payload + 8 + 12 * e.text_num;
                    }
                    break;
                case RCO_ID_SOUND:
                    if (e.type == 1) {
                        le16(d, n, pos, e.snd_format);
                        le16(d, n, pos + 2, e.snd_channels);
                        pos = e.payload + 16;   // 2 channels * (size, offset)
                    }
                    break;
                case RCO_ID_OBJ:
                case RCO_ID_ANIM:
                    if (e.type > 0) {
                        uint32_t es = e.entry_size;
                        if (es == 0)
                            es = static_cast<uint32_t>(kEntrySize +
                                                       obj_data_size(e.type));
                        pos = off + es;
                    }
                    break;
                default:
                    break;
            }

            const size_t idx = entries.size();
            entries.push_back(e);

            if (depth < static_cast<int>(kMaxDepth) &&
                entries[idx].num_sub > 0) {
                uint32_t child = pos;
                for (uint32_t i = 0; i < entries[idx].num_sub; ++i) {
                    if (child + kEntrySize > n) break;
                    uint32_t child_end = parse_entry(child, depth + 1, off);
                    if (child_end <= child) break;
                    child = child_end;
                }
                entries[idx].end = child;
                return child;
            }
            entries[idx].end = pos;
            return pos;
        };

    if (r.p_main != kNull && r.p_main + kEntrySize <= n) {
        parse_entry(r.p_main, 0, kNull);
    }

    // Build the typed output: images, texts, objects, vsmx.
    for (const Entry& e : entries) {
        if (e.id == RCO_ID_IMG && e.type == 1) {
            RcoImage im;
            im.format = e.img_format;
            im.entry_offset = e.offset;
            im.name = e.label;
            im.size = e.img_packed;
            if (r.p_img_data != kNull) {
                im.offset = r.p_img_data + e.img_off;
                if (im.offset + 24 <= n &&
                    std::memcmp(d + im.offset, "\x89PNG", 4) == 0) {
                    be32(d, n, im.offset + 16, im.width);
                    be32(d, n, im.offset + 20, im.height);
                    // Find IEND for a tight size.
                    size_t j = im.offset + 8;
                    while (j + 8 <= n) {
                        if (std::memcmp(d + j, "IEND", 4) == 0) { j += 8; break; }
                        ++j;
                    }
                    im.size = static_cast<uint32_t>(j - im.offset);
                } else {
                    im.offset = e.img_off;
                }
            }
            r.images.push_back(std::move(im));
        } else if (e.id == RCO_ID_TEXT && e.type == 1) {
            for (uint32_t i = 0; i < e.text_num; ++i) {
                uint32_t lab_off = 0, length = 0, offset = 0;
                le32(d, n, e.text_idx_off + 12 * i, lab_off);
                le32(d, n, e.text_idx_off + 12 * i + 4, length);
                le32(d, n, e.text_idx_off + 12 * i + 8, offset);
                RcoText t;
                t.lang = e.text_lang;
                t.label = read_label(d, n, r.p_label_data, r.l_label_data,
                                     lab_off);
                (void)length;
                uint32_t td = 0;
                le32(d, n, 0x38, td);   // pTextData
                if (td != kNull && td + offset < n) {
                    size_t o = td + offset;
                    std::string s;
                    while (o + 1 < n && !(d[o] == 0 && d[o + 1] == 0)) {
                        s.push_back(static_cast<char>(d[o]));
                        o += 2;
                    }
                    t.value = std::move(s);
                }
                r.texts.push_back(std::move(t));
            }
        } else if (e.id == RCO_ID_OBJ && e.type > 0) {
            RcoObject o;
            o.offset = e.offset;
            o.type = e.type;
            o.name = e.label;
            o.parent = e.parent_off;
            o.tree_depth = e.depth;
            uint32_t p = e.payload;

            auto read_float = [&](uint32_t at) {
                uint32_t v = 0; le32(d, n, at, v); return as_float(v);
            };
            auto read_ref = [&](uint32_t at) -> RcoRef {
                RcoRef ref;
                le16(d, n, at, ref.kind);
                le16(d, n, at + 2, ref.aux);
                le32(d, n, at + 4, ref.value);
                return ref;
            };

            if (e.type != RCO_OBJ_PAGE && e.type != RCO_OBJ_ACTION &&
                obj_data_size(e.type) >= 64) {
                o.pos_x   = read_float(p + 0);
                o.pos_y   = read_float(p + 4);
                o.pos_z   = read_float(p + 8);
                o.red     = read_float(p + 12);
                o.green   = read_float(p + 16);
                o.blue    = read_float(p + 20);
                o.alpha   = read_float(p + 24);
                o.width   = read_float(p + 28);
                o.height  = read_float(p + 32);
                o.depth   = read_float(p + 36);
                o.scale_w = read_float(p + 40);
                o.scale_h = read_float(p + 44);
                o.scale_d = read_float(p + 48);
                // iconOffset at +52 (Int), onInit at +56 (Event).
                o.on_init = read_ref(p + 56);

                // Type-specific extras (Button / UButton / Plane).
                if (e.type == RCO_OBJ_BUTTON) {
                    o.image       = read_ref(p + 64);
                    o.text        = read_ref(p + 88);
                    o.on_push     = read_ref(p + 96);
                    o.on_focus_in = read_ref(p + 104);
                    o.on_focus_out= read_ref(p + 112);
                } else if (e.type == RCO_OBJ_UBUTTON) {
                    o.image       = read_ref(p + 64);
                    o.on_push     = read_ref(p + 72);
                    o.on_focus_in = read_ref(p + 80);
                    o.on_focus_out= read_ref(p + 88);
                } else if (e.type == RCO_OBJ_PLANE) {
                    o.image = read_ref(p + 64);
                }
            } else if (e.type == RCO_OBJ_PAGE) {
                // Page: unknownInt0(4) then onInit/onCancel/onContextMenu/
                // onActivate events.
                o.on_init = read_ref(p + 4);
            }

            if (o.image.kind == RCO_REF_IMAGE)
                o.image_offset = o.image.value;
            if (o.on_push.kind == RCO_REF_EVENT)
                o.on_push_event = read_event(d, n, r.p_event_data,
                                             r.l_event_data, o.on_push.value);
            if (o.on_focus_in.kind == RCO_REF_EVENT)
                o.on_focus_in_event = read_event(d, n, r.p_event_data,
                                                 r.l_event_data,
                                                 o.on_focus_in.value);
            if (o.on_focus_out.kind == RCO_REF_EVENT)
                o.on_focus_out_event = read_event(d, n, r.p_event_data,
                                                  r.l_event_data,
                                                  o.on_focus_out.value);
            r.objects.push_back(std::move(o));
        }
    }

    // Resolve the hierarchy: each object's children are the OBJ entries whose
    // traversal parent is this object.
    for (const Entry& child : entries) {
        if (child.id != RCO_ID_OBJ || child.parent_off == kNull) continue;
        for (auto& o : r.objects) {
            if (o.offset == child.parent_off) {
                o.children.push_back(child.offset);
                break;
            }
        }
    }

    // VSMX program.
    for (const Entry& e : entries) {
        if (e.id == RCO_ID_VSMX && e.type == 1 && e.vsmx_len > 0) {
            size_t vsmx_at = e.payload + 8 + e.vsmx_off;
            parse_vsmx(d, n, vsmx_at, e.vsmx_len, e.label, r.vsmx);
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
        return "RCO: INVALID (" + rco.error + ")\n";
    }
    std::snprintf(buf, sizeof(buf),
                  "RCO: magic=\\0%s version=0x%02x size=%zu "
                  "images=%zu texts=%zu objects=%zu\n",
                  rco.magic.c_str(), rco.version, rco.size_bytes,
                  rco.images.size(), rco.texts.size(), rco.objects.size());
    out += buf;
    if (rco.vsmx.present) {
        std::snprintf(buf, sizeof(buf),
                      "     VSMX '%s': %zu instructions, %zu strings, "
                      "%zu props, %zu names\n",
                      rco.vsmx.name.c_str(), rco.vsmx.instruction_count(),
                      rco.vsmx.texts.size(), rco.vsmx.properties.size(),
                      rco.vsmx.names.size());
        out += buf;
    }
    auto pages = rco.pages();
    std::snprintf(buf, sizeof(buf), "     pages=%zu:", pages.size());
    out += buf;
    for (uint32_t p : pages) {
        const RcoObject* o = rco.object(p);
        if (o) out += " " + o->name;
    }
    out += "\n";
    if (!rco.images.empty()) {
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
