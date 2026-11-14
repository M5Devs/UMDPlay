#pragma once

// VSMX: the bytecode virtual machine behind UMD Video (and PSP) interactive
// menus. A VSMX program is embedded in a RESOURCE/*.RCO container and drives
// the disc's menu logic: page switching, focus movement, chapter playback,
// controller/play-status queries and so on.
//
// The machine is a stack VM. A program is a flat array of 8-byte groups
// {id:u32, value:u32}; the low byte of `id` is the opcode, and the remaining
// bytes carry small operands (the argument/local-variable counts of a FUNCTION
// group). Operand meaning per opcode is documented in VsmxOpcode below.
//
// This module offers three layers:
//   * decode_vsmx()    - split a raw VSMX blob into code + string tables.
//   * disassemble()    - turn the code array into readable listing lines.
//   * VsmxMachine      - a small, dependency-free interpreter for the subset
//                        of the instruction set that real UMD menus use, with
//                        pluggable native objects (controller, movieplayer,
//                        resource, ...).
//
// The interpreter is deliberately host-agnostic: the caller registers native
// objects/functions by name and drives the top-level script. It is pure C++17
// and never touches SDL or FFmpeg.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace umdplay {

// Opcodes (the low byte of a code group's `id`). Mirrors the Sony VM order.
enum VsmxOpcode {
    VSMX_NOTHING = 0x00,
    VSMX_ASSIGN = 0x01, VSMX_ADD = 0x02, VSMX_SUB = 0x03, VSMX_MUL = 0x04,
    VSMX_DIV = 0x05, VSMX_MOD = 0x06, VSMX_POS = 0x07, VSMX_NEG = 0x08,
    VSMX_NOT = 0x09, VSMX_PREINC = 0x0a, VSMX_PREDEC = 0x0b,
    VSMX_INC = 0x0c, VSMX_DEC = 0x0d,
    VSMX_EQ = 0x0e, VSMX_NE = 0x0f, VSMX_IDENT = 0x10, VSMX_NIDENT = 0x11,
    VSMX_LT = 0x12, VSMX_LTE = 0x13, VSMX_GTE = 0x14, VSMX_GT = 0x15,
    VSMX_INSTANCEOF = 0x16, VSMX_IN = 0x17, VSMX_TYPEOF = 0x18,
    VSMX_BAND = 0x19, VSMX_BXOR = 0x1a, VSMX_BOR = 0x1b, VSMX_BNOT = 0x1c,
    VSMX_LSHIFT = 0x1d, VSMX_RSHIFT = 0x1e, VSMX_URSHIFT = 0x1f,
    VSMX_STACK_COPY = 0x20, VSMX_STACK_SWAP = 0x21,
    VSMX_END_STMT = 0x22, VSMX_CONST_NULL = 0x23, VSMX_CONST_EMPTY_ARRAY = 0x24,
    VSMX_CONST_BOOL = 0x25, VSMX_CONST_INT = 0x26, VSMX_CONST_FLOAT = 0x27,
    VSMX_CONST_STRING = 0x28, VSMX_CONST_OBJECT = 0x29, VSMX_FUNCTION = 0x2a,
    VSMX_ARRAY = 0x2b, VSMX_THIS = 0x2c, VSMX_UNNAMED_VAR = 0x2d,
    VSMX_VARIABLE = 0x2e, VSMX_PROPERTY = 0x2f, VSMX_METHOD = 0x30,
    VSMX_SET_ATTR = 0x31, VSMX_UNSET = 0x32, VSMX_OBJ_ADD_ATTR = 0x33,
    VSMX_ARRAY_INDEX = 0x34, VSMX_ARRAY_INDEX_KEEP = 0x35,
    VSMX_ARRAY_INDEX_ASSIGN = 0x36, VSMX_ARRAY_DELETE = 0x37,
    VSMX_ARRAY_PUSH = 0x38, VSMX_JUMP = 0x39, VSMX_JUMP_TRUE = 0x3a,
    VSMX_JUMP_FALSE = 0x3b, VSMX_CALL_FUNC = 0x3c, VSMX_CALL_METHOD = 0x3d,
    VSMX_CALL_NEW = 0x3e, VSMX_RETURN = 0x3f, VSMX_THROW = 0x40,
    VSMX_TRY_IN = 0x41, VSMX_TRY_OUT = 0x42, VSMX_CATCH_IN = 0x43,
    VSMX_CATCH_OUT = 0x44, VSMX_END = 0x45, VSMX_DEBUG_FILE = 0x46,
    VSMX_DEBUG_LINE = 0x47, VSMX_UNKNOWN_48 = 0x48, VSMX_MAKE_FLOAT_ARRAY = 0x49,
};

// Human-readable opcode name ("ASSIGN", "CONST_INT", ...).
const char* vsmx_opcode_name(int opcode);

// One decoded instruction.
struct VsmxInstr {
    uint32_t id = 0;
    uint32_t value = 0;
    int      opcode() const { return static_cast<int>(id & 0xff); }
    int      args() const { return static_cast<int>((id >> 8) & 0xff); }
    int      local_vars() const { return static_cast<int>((id >> 24) & 0xff); }
    float    float_value() const;
};

// A decoded VSMX program.
struct VsmxProgram {
    bool        valid = false;
    uint32_t    version = 0;
    std::vector<VsmxInstr> code;
    std::vector<std::string> texts;       // UTF-16LE constants
    std::vector<std::string> properties;  // UTF-16LE property names
    std::vector<std::string> names;       // ISO-8859-1 variable names
};

// Decode a raw VSMX blob (must begin with the "VSMX" signature). Never throws.
VsmxProgram decode_vsmx(const uint8_t* data, size_t size);

// Render a decoded program as a disassembly listing (one line per group).
std::string disassemble(const VsmxProgram& prog);

// A short "which opcodes are used" histogram line (for --probe).
std::string vsmx_opcode_histogram(const VsmxProgram& prog);

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

// Opaque base for any runtime object a value can point at (script object,
// array, function, or a host-provided native object).
struct VsmxObjectBase {
    virtual ~VsmxObjectBase() = default;
};

// A runtime value: undefined, null, boolean, number, string, or a reference to
// a runtime object (which may be a script object, array, function or native).
class VsmxValue {
public:
    enum class Kind { Undefined, Null, Bool, Number, String, Object };

    VsmxValue() = default;                       // Undefined
    VsmxValue(Kind k, double n, bool b, std::string s,
              std::shared_ptr<VsmxObjectBase> o)
        : m_kind(k), m_num(n), m_bool(b), m_str(std::move(s)),
          m_obj(std::move(o)) {}

    static VsmxValue null() { return VsmxValue(Kind::Null, 0, false, {}, {}); }
    static VsmxValue boolean(bool b) {
        return VsmxValue(Kind::Bool, 0, b, {}, {});
    }
    static VsmxValue number(double n) {
        return VsmxValue(Kind::Number, n, false, {}, {});
    }
    static VsmxValue string(std::string s) {
        return VsmxValue(Kind::String, 0, false, std::move(s), {});
    }
    static VsmxValue object(std::shared_ptr<VsmxObjectBase> o) {
        return VsmxValue(Kind::Object, 0, false, {}, std::move(o));
    }

    Kind kind() const { return m_kind; }
    bool is_nullish() const {
        return m_kind == Kind::Null || m_kind == Kind::Undefined;
    }
    double      number() const { return m_num; }
    bool        boolean() const { return m_bool; }
    const std::string& str() const { return m_str; }
    std::shared_ptr<VsmxObjectBase> object() const { return m_obj; }

    double      to_number() const;
    bool        to_bool() const;
    std::string to_string() const;

private:
    Kind m_kind = Kind::Undefined;
    double m_num = 0;
    bool   m_bool = false;
    std::string m_str;
    std::shared_ptr<VsmxObjectBase> m_obj;
};

// A host-provided native object exposes named methods/properties to a script.
class VsmxNative : public VsmxObjectBase {
public:
    virtual ~VsmxNative() = default;
    virtual std::string name() const = 0;
    // obj.method(args); may return any value (Undefined when nothing).
    virtual VsmxValue call(const std::string& method,
                           const std::vector<VsmxValue>& args) {
        (void)method; (void)args; return VsmxValue();
    }
    virtual VsmxValue get(const std::string& prop) {
        (void)prop; return VsmxValue();
    }
    virtual void set(const std::string& prop, const VsmxValue& v) {
        (void)prop; (void)v;
    }
};

// The interpreter. Register native objects with bind(), then run().
class VsmxMachine {
public:
    explicit VsmxMachine(const VsmxProgram& prog);
    ~VsmxMachine();

    void bind(const std::string& name, std::shared_ptr<VsmxNative> obj);

    // Run the program from instruction 0. Returns true when it completes.
    bool run();

    // Call a top-level function by name (used to dispatch menu events such as
    // "script:/main/main1Push"). Returns true when the function was found and
    // ran to completion.
    bool call_event(const std::string& event,
                    const std::vector<std::string>& args = {});

    // Diagnostics.
    size_t steps() const { return m_steps; }
    const std::string& last_error() const { return m_error; }
    double last_result_float() const { return m_last_float; }
    int    last_result_int() const { return static_cast<int>(m_last_float); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    size_t      m_steps = 0;
    std::string m_error;
    double      m_last_float = 0.0;
};

}  // namespace umdplay
