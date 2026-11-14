#pragma once

// Parser for UMD RESOURCE/*.RCO resource files.
//
// An .RCO ("resource container") holds a disc's interactive menu resources:
// PNG images, text strings, an object tree (pages / planes / buttons / text),
// event bindings (onPush, onFocusIn, ...) and a VSMX bytecode program.
//
// This module walks the whole container: the table of contents, the object
// hierarchy, images, localized text and the embedded VSMX program. It is pure
// and bounds-checked and never throws; it does not touch SDL. Rendering the
// extracted PNG bytes is the caller's job.
//
// Layout (little-endian; see docs/ARCHITECTURE.md for the verified dump):
//   0x00  char[4]  magic  "\0PRF"
//   0x04  dword    version (0x80 = UMD)
//   0x08  dword    0
//   0x0C  dword    compression (low nibble: 1 = UMD; high nibble: header comp)
//   0x10  dword[10] table pointers: main, vsmx, text, sound, model, img,
//                   unknown, font, obj, anim
//   0x38  ...       text/label/event data pointers, pointer tables, image and
//                   sound data pointers (see RcoFile)
//   0xA4  ...       entries (40-byte headers + payloads)
//
// Each entry: type(u8) id(u8) pad(2) labelOffset(u32) eHeadSize(u32)
//             entrySize(u32) numSubEntries(u32) next(u32) prev(u32) parent(u32)
//             blanks(8).  Sub-entries are contiguous after the parent payload.

#include <cstdint>
#include <string>
#include <vector>

namespace umdplay {

// RCO entry ids (the `id` byte of an entry header).
enum RcoId {
    RCO_ID_MAIN = 1, RCO_ID_VSMX = 2, RCO_ID_TEXT = 3, RCO_ID_IMG = 4,
    RCO_ID_MODEL = 5, RCO_ID_SOUND = 6, RCO_ID_FONT = 7, RCO_ID_OBJ = 8,
    RCO_ID_ANIM = 9,
};

// Object types (the `type` byte of an OBJ entry).
enum RcoObjType {
    RCO_OBJ_PAGE = 1, RCO_OBJ_PLANE = 2, RCO_OBJ_BUTTON = 3, RCO_OBJ_XMENU = 4,
    RCO_OBJ_XMLIST = 5, RCO_OBJ_XLIST = 6, RCO_OBJ_PROGRESS = 7,
    RCO_OBJ_SCROLL = 8, RCO_OBJ_MLIST = 9, RCO_OBJ_MITEM = 10,
    RCO_OBJ_XITEM = 12, RCO_OBJ_TEXT = 13, RCO_OBJ_MODEL = 14,
    RCO_OBJ_SPIN = 15, RCO_OBJ_ACTION = 16, RCO_OBJ_ITEMSPIN = 17,
    RCO_OBJ_GROUP = 18, RCO_OBJ_LLIST = 19, RCO_OBJ_LITEM = 20,
    RCO_OBJ_EDIT = 21, RCO_OBJ_CLOCK = 22, RCO_OBJ_ILIST = 23,
    RCO_OBJ_IITEM = 24, RCO_OBJ_ICON = 25, RCO_OBJ_UBUTTON = 26,
};

// Reference kinds used by Event/Image/Text/Object fields.
enum RcoRefType {
    RCO_REF_NONE = 0xFFFF, RCO_REF_EVENT = 0x400, RCO_REF_TEXT = 0x401,
    RCO_REF_IMAGE = 0x402, RCO_REF_MODEL = 0x403, RCO_REF_FONT = 0x405,
    RCO_REF_OBJECT = 0x407, RCO_REF_ANIM = 0x408, RCO_REF_POSITION = 0x409,
};

// A resolved reference field: kind + raw value (event byte offset, image /
// object absolute offset, text index, ...).
struct RcoRef {
    uint16_t kind  = RCO_REF_NONE;
    uint16_t aux   = 0;
    uint32_t value = 0xFFFFFFFF;

    bool present() const { return kind != RCO_REF_NONE; }
};

// An embedded image. `offset`/`size` locate the raw image bytes inside the RCO
// (a PNG on every UMD disc we have seen; GIM is possible in theory).
struct RcoImage {
    uint32_t offset = 0;      // absolute offset of the image signature
    uint32_t size = 0;        // bytes from signature to end of image
    uint32_t width = 0;       // IHDR width (PNG)
    uint32_t height = 0;      // IHDR height (PNG)
    uint16_t format = 0;      // RCO_IMG_* (0 = PNG)
    uint32_t entry_offset = 0;// absolute offset of the IMG entry
    std::string name;         // entry label (e.g. "tex_main1")
};

// A localized text string (TEXT table entry).
struct RcoText {
    uint16_t lang = 0;        // RCO_LANG_* (0 = Japanese, 1 = English, ...)
    std::string value;
    std::string label;
};

// One object in the menu hierarchy. Geometry is in the RCO's coordinate space
// (Y grows upward). References are resolved to the referenced object's offset,
// image entry offset or event name where possible.
struct RcoObject {
    uint32_t    offset = 0;        // absolute offset of the OBJ entry
    int         type = 0;          // RcoObjType
    std::string name;              // entry label (object name)
    uint32_t    parent = 0xFFFFFFFF;
    int         tree_depth = 0;    // nesting level in the object tree

    // BasePositionObject geometry (defaults are the identity values).
    float pos_x = 0, pos_y = 0, pos_z = 0;
    float red = 1, green = 1, blue = 1, alpha = 1;
    float width = 0, height = 0, depth = 0;
    float scale_w = 1, scale_h = 1, scale_d = 1;

    // References.
    RcoRef   image;                 // image resource
    RcoRef   text;                  // text resource
    RcoRef   on_init;               // onInit event
    RcoRef   on_push;               // button onPush event
    RcoRef   on_focus_in;
    RcoRef   on_focus_out;
    uint32_t image_offset = 0xFFFFFFFF;  // resolved image entry offset
    std::string on_push_event;           // resolved onPush event name
    std::string on_focus_in_event;
    std::string on_focus_out_event;

    std::vector<uint32_t> children;      // child OBJ entry offsets

    bool is_position() const { return type != RCO_OBJ_PAGE && type != 0; }
    bool is_button() const {
        return type == RCO_OBJ_BUTTON || type == RCO_OBJ_UBUTTON;
    }
    bool is_page() const { return type == RCO_OBJ_PAGE; }
};

// An embedded VSMX program: the raw bytecode plus its decoded constant pools.
struct RcoVsmx {
    bool        present = false;
    uint32_t    offset = 0;          // absolute offset of the "VSMX" signature
    uint32_t    size = 0;            // byte length of the program
    std::string name;                // entry label ("main")

    // Header (see include/vsmx_vm.h for full decoding).
    uint32_t    version = 0;
    uint32_t    code_length = 0;     // bytes of the instruction stream
    std::vector<uint8_t> code;       // raw instruction bytes
    std::vector<std::string> texts;      // UTF-16LE constant strings
    std::vector<std::string> properties; // UTF-16LE property names
    std::vector<std::string> names;      // ISO-8859-1 variable names

    size_t instruction_count() const { return code.size() / 8; }
};

struct RcoFile {
    bool        valid = false;
    std::string error;
    std::string magic;          // "PRF" (the leading NUL is stripped)
    uint32_t    version = 0;
    uint32_t    compression = 0;
    size_t      size_bytes = 0;

    // Pointer table (absolute offsets into the file; 0xFFFFFFFF = absent).
    uint32_t    p_main = 0xFFFFFFFF, p_vsmx = 0xFFFFFFFF, p_text = 0xFFFFFFFF,
                p_sound = 0xFFFFFFFF, p_model = 0xFFFFFFFF, p_img = 0xFFFFFFFF,
                p_font = 0xFFFFFFFF, p_obj = 0xFFFFFFFF, p_anim = 0xFFFFFFFF;
    uint32_t    p_label_data = 0xFFFFFFFF, l_label_data = 0;
    uint32_t    p_event_data = 0xFFFFFFFF, l_event_data = 0;
    uint32_t    p_img_data = 0xFFFFFFFF, l_img_data = 0;

    std::vector<RcoImage>  images;    // in IMG table order
    std::vector<RcoText>   texts;     // localized strings
    std::vector<std::string> events;  // event names (by byte offset -> name)
    std::vector<RcoObject> objects;   // flat list, file order (roots first)
    RcoVsmx    vsmx;

    // --- queries ---------------------------------------------------------
    // Top-level pages (objects with type == RCO_OBJ_PAGE), file order.
    std::vector<uint32_t> pages() const;
    // Object by absolute offset (nullptr when not found).
    const RcoObject* object(uint32_t offset) const;
    // Children of an object (resolved), by offset.
    std::vector<uint32_t> children_of(uint32_t offset) const;
    // Image entry by its entry offset (nullptr when not found).
    const RcoImage* image_at(uint32_t offset) const;
};

// Parse the raw .RCO bytes. Never throws.
RcoFile parse_rco(const uint8_t* data, size_t size);

// One-line-per-fact human summary for --probe.
std::string rco_summary(const RcoFile& rco);

}  // namespace umdplay
