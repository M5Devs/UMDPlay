#include "vsmx_vm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace umdplay {

namespace {

constexpr uint32_t kVsmxMagic = 0x584D5356;   // "VSMX"
constexpr size_t   kVsmxHeaderSize = 52;

bool le32(const uint8_t* d, size_t n, size_t off, uint32_t& out) {
    if (off + 4 > n) return false;
    out = static_cast<uint32_t>(d[off]) |
          (static_cast<uint32_t>(d[off + 1]) << 8) |
          (static_cast<uint32_t>(d[off + 2]) << 16) |
          (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}

// NUL-separated string table. `width` is 2 for UTF-16LE, 1 for ISO-8859-1.
std::vector<std::string> read_strings(const uint8_t* p, size_t len,
                                      uint32_t entries, int width) {
    std::vector<std::string> v;
    size_t start = 0;
    for (size_t i = 0; i + static_cast<size_t>(width) <= len &&
                       v.size() < entries; i += width) {
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
}

}  // namespace

float VsmxInstr::float_value() const {
    float f;
    std::memcpy(&f, &value, 4);
    return f;
}

const char* vsmx_opcode_name(int op) {
    static const char* names[] = {
        "NOTHING", "ASSIGN", "ADD", "SUB", "MUL", "DIV", "MOD", "POS", "NEG",
        "NOT", "PREINC", "PREDEC", "INC", "DEC", "EQ", "NE", "IDENT", "NIDENT",
        "LT", "LTE", "GTE", "GT", "INSTANCEOF", "IN", "TYPEOF", "BAND", "BXOR",
        "BOR", "BNOT", "LSHIFT", "RSHIFT", "URSHIFT", "STACK_COPY",
        "STACK_SWAP", "END_STMT", "CONST_NULL", "CONST_EMPTY_ARRAY",
        "CONST_BOOL", "CONST_INT", "CONST_FLOAT", "CONST_STRING",
        "CONST_OBJECT", "FUNCTION", "ARRAY", "THIS", "UNNAMED_VAR", "VARIABLE",
        "PROPERTY", "METHOD", "SET_ATTR", "UNSET", "OBJ_ADD_ATTR",
        "ARRAY_INDEX", "ARRAY_INDEX_KEEP", "ARRAY_INDEX_ASSIGN", "ARRAY_DELETE",
        "ARRAY_PUSH", "JUMP", "JUMP_TRUE", "JUMP_FALSE", "CALL_FUNC",
        "CALL_METHOD", "CALL_NEW", "RETURN", "THROW", "TRY_IN", "TRY_OUT",
        "CATCH_IN", "CATCH_OUT", "END", "DEBUG_FILE", "DEBUG_LINE",
        "UNKNOWN_48", "MAKE_FLOAT_ARRAY",
    };
    if (op < 0 || op >= static_cast<int>(sizeof(names) / sizeof(names[0])))
        return "?";
    return names[op];
}

VsmxProgram decode_vsmx(const uint8_t* d, size_t n) {
    VsmxProgram p;
    if (!d || n < kVsmxHeaderSize) return p;
    uint32_t sig = 0;
    if (!le32(d, n, 0, sig) || sig != kVsmxMagic) return p;

    uint32_t code_off = 0, code_len = 0;
    uint32_t text_off = 0, text_len = 0, text_n = 0;
    uint32_t prop_off = 0, prop_len = 0, prop_n = 0;
    uint32_t names_off = 0, names_len = 0, names_n = 0;
    le32(d, n, 0x04, p.version);
    le32(d, n, 0x08, code_off);
    le32(d, n, 0x0C, code_len);
    le32(d, n, 0x10, text_off);
    le32(d, n, 0x14, text_len);
    le32(d, n, 0x18, text_n);
    le32(d, n, 0x1C, prop_off);
    le32(d, n, 0x20, prop_len);
    le32(d, n, 0x24, prop_n);
    le32(d, n, 0x28, names_off);
    le32(d, n, 0x2C, names_len);
    le32(d, n, 0x30, names_n);

    auto in_range = [&](uint32_t o, uint32_t l) {
        return static_cast<uint64_t>(o) + l <= n;
    };
    if (!in_range(code_off, code_len)) return p;

    const size_t groups = code_len / 8;
    p.code.reserve(groups);
    for (size_t i = 0; i < groups; ++i) {
        VsmxInstr ins;
        le32(d, n, code_off + i * 8, ins.id);
        le32(d, n, code_off + i * 8 + 4, ins.value);
        p.code.push_back(ins);
    }
    if (in_range(text_off, text_len))
        p.texts = read_strings(d + text_off, text_len, text_n, 2);
    if (in_range(prop_off, prop_len))
        p.properties = read_strings(d + prop_off, prop_len, prop_n, 2);
    if (in_range(names_off, names_len))
        p.names = read_strings(d + names_off, names_len, names_n, 1);

    p.valid = true;
    return p;
}

std::string disassemble(const VsmxProgram& prog) {
    std::string out;
    char buf[256];
    for (size_t i = 0; i < prog.code.size(); ++i) {
        const VsmxInstr& c = prog.code[i];
        const int op = c.opcode();
        std::snprintf(buf, sizeof(buf), "%5zu  %-18s", i,
                      vsmx_opcode_name(op));
        out += buf;
        switch (op) {
            case VSMX_CONST_INT:
            case VSMX_CONST_BOOL:
            case VSMX_DEBUG_LINE:
                std::snprintf(buf, sizeof(buf), " %d", static_cast<int>(c.value));
                out += buf;
                break;
            case VSMX_CONST_FLOAT:
                std::snprintf(buf, sizeof(buf), " %g",
                              static_cast<double>(c.float_value()));
                out += buf;
                break;
            case VSMX_CONST_STRING:
            case VSMX_DEBUG_FILE:
                if (c.value < prog.texts.size())
                    out += " \"" + prog.texts[c.value] + "\"";
                break;
            case VSMX_VARIABLE:
                if (c.value < prog.names.size())
                    out += " " + prog.names[c.value];
                break;
            case VSMX_PROPERTY:
            case VSMX_METHOD:
            case VSMX_SET_ATTR:
            case VSMX_UNSET:
            case VSMX_OBJ_ADD_ATTR:
                if (c.value < prog.properties.size())
                    out += " " + prog.properties[c.value];
                break;
            case VSMX_FUNCTION:
                std::snprintf(buf, sizeof(buf), " args=%d localVars=%d line=%u",
                              c.args(), c.local_vars(), c.value);
                out += buf;
                break;
            case VSMX_JUMP:
            case VSMX_JUMP_TRUE:
            case VSMX_JUMP_FALSE:
                std::snprintf(buf, sizeof(buf), " -> %u", c.value);
                out += buf;
                break;
            case VSMX_CALL_FUNC:
            case VSMX_CALL_METHOD:
            case VSMX_CALL_NEW:
                std::snprintf(buf, sizeof(buf), " args=%u", c.value);
                out += buf;
                break;
            case VSMX_UNNAMED_VAR:
                std::snprintf(buf, sizeof(buf), " %u", c.value);
                out += buf;
                break;
            default:
                break;
        }
        out += "\n";
    }
    return out;
}

std::string vsmx_opcode_histogram(const VsmxProgram& prog) {
    int counts[0x50] = {0};
    for (const auto& c : prog.code) {
        const int op = c.opcode();
        if (op >= 0 && op < 0x50) counts[op]++;
    }
    std::string out;
    char buf[64];
    bool first = true;
    for (int op = 0; op < 0x50; ++op) {
        if (!counts[op]) continue;
        if (!first) out += ", ";
        first = false;
        std::snprintf(buf, sizeof(buf), "%s=%d", vsmx_opcode_name(op), counts[op]);
        out += buf;
    }
    return out;
}

// ===========================================================================
// Interpreter
// ===========================================================================

double VsmxValue::to_number() const {
    switch (m_kind) {
        case Kind::Number: return m_num;
        case Kind::Bool:   return m_bool ? 1.0 : 0.0;
        case Kind::String: {
            try { return std::stod(m_str); } catch (...) { return 0.0; }
        }
        case Kind::Null:   return 0.0;
        case Kind::Object: return 0.0;
        default:           return std::nan("");
    }
}

bool VsmxValue::to_bool() const {
    switch (m_kind) {
        case Kind::Bool:   return m_bool;
        case Kind::Number: return m_num != 0.0 && !std::isnan(m_num);
        case Kind::String: return !m_str.empty();
        case Kind::Object: return true;
        default:           return false;
    }
}

std::string VsmxValue::to_string() const {
    char buf[64];
    switch (m_kind) {
        case Kind::String: return m_str;
        case Kind::Bool:   return m_bool ? "true" : "false";
        case Kind::Number:
            if (m_num == std::floor(m_num) && std::fabs(m_num) < 1e15)
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(m_num));
            else
                std::snprintf(buf, sizeof(buf), "%g", m_num);
            return buf;
        case Kind::Object: return "[object]";
        case Kind::Null:   return "null";
        default:           return "undefined";
    }
}

namespace {

// A script object: property bag; also used for arrays and functions.
struct ScriptObject : VsmxObjectBase {
    bool is_array = false;
    bool is_function = false;
    int  start_line = 0;
    int  fn_args = 0;
    int  local_vars = 0;
    std::unordered_map<std::string, VsmxValue> props;
    std::vector<VsmxValue> elements;
    std::shared_ptr<VsmxNative> native;   // when wrapping a host object
};

// An assignable reference (a global variable or a local slot).
struct RefObject : VsmxObjectBase {
    enum class Kind { Global, Local } kind = Kind::Global;
    std::string name;        // Global
    int frame = -1;          // Local
    int slot = 0;            // Local
};

std::shared_ptr<ScriptObject> as_script(const VsmxValue& v) {
    return std::dynamic_pointer_cast<ScriptObject>(v.object());
}
std::shared_ptr<RefObject> as_ref(const VsmxValue& v) {
    return std::dynamic_pointer_cast<RefObject>(v.object());
}

}  // namespace

struct VsmxMachine::Impl {
    const VsmxProgram& prog;
    std::vector<VsmxValue> stack;
    std::unordered_map<std::string, std::shared_ptr<VsmxNative>> natives;
    std::shared_ptr<ScriptObject> globals;
    struct Frame {
        std::shared_ptr<ScriptObject> this_obj;
        std::vector<VsmxValue> locals;   // index 1..n hold the arguments
        size_t return_pc = 0;
        bool   return_this = false;
    };
    std::vector<Frame> frames;
    size_t pc = 0;
    size_t budget = 0;
    std::string error;
    double last_float = 0;
    bool finished = false;

    explicit Impl(const VsmxProgram& p) : prog(p) {
        globals = std::make_shared<ScriptObject>();
    }

    void fail(const std::string& msg) {
        if (error.empty()) error = msg;
        finished = true;
    }

    VsmxValue pop() {
        if (stack.empty()) { fail("stack underflow"); return VsmxValue(); }
        VsmxValue v = stack.back();
        stack.pop_back();
        return v;
    }

    VsmxValue deref(const VsmxValue& v) {
        if (auto r = as_ref(v)) {
            if (r->kind == RefObject::Kind::Local) {
                if (r->frame >= 0 &&
                    r->frame < static_cast<int>(frames.size())) {
                    auto& l = frames[r->frame].locals;
                    if (r->slot >= 0 && r->slot < static_cast<int>(l.size()))
                        return l[r->slot];
                }
                return VsmxValue();
            }
            auto it = globals->props.find(r->name);
            return it != globals->props.end() ? it->second : VsmxValue();
        }
        return v;
    }

    void assign_ref(const VsmxValue& ref, const VsmxValue& val) {
        if (auto r = as_ref(ref)) {
            if (r->kind == RefObject::Kind::Local) {
                if (r->frame >= 0 &&
                    r->frame < static_cast<int>(frames.size())) {
                    auto& l = frames[r->frame].locals;
                    if (r->slot >= 0 &&
                        r->slot < static_cast<int>(l.size())) {
                        l[r->slot] = val;
                        return;
                    }
                }
                return;
            }
            globals->props[r->name] = val;
            return;
        }
        fail("assignment to non-reference");
    }

    std::shared_ptr<VsmxNative> native_of(const VsmxValue& v) {
        if (auto s = as_script(v))
            if (s->native) return s->native;
        return nullptr;
    }

    void push_number(double d) { stack.push_back(VsmxValue::number(d)); }
    void push_bool(bool b) { stack.push_back(VsmxValue::boolean(b)); }

    VsmxValue make_native(const std::shared_ptr<VsmxNative>& n) {
        auto s = std::make_shared<ScriptObject>();
        s->native = n;
        return VsmxValue::object(s);
    }

    void call_function(const std::shared_ptr<ScriptObject>& fn,
                       const std::shared_ptr<ScriptObject>& this_obj,
                       const std::vector<VsmxValue>& args,
                       bool return_this = false) {
        Frame fr;
        fr.this_obj = this_obj;
        const size_t n = static_cast<size_t>(fn->fn_args + fn->local_vars) + 1;
        fr.locals.assign(n, VsmxValue());
        for (size_t i = 0;
             i < args.size() && i < static_cast<size_t>(fn->fn_args); ++i)
            fr.locals[i + 1] = args[i];
        fr.return_pc = pc;
        fr.return_this = return_this;
        frames.push_back(std::move(fr));
        if (fn->start_line >= 0 &&
            fn->start_line < static_cast<int>(prog.code.size()))
            pc = static_cast<size_t>(fn->start_line);
        else
            finished = true;
    }

    void execute();

    VsmxValue get_prop(const VsmxValue& obj, const std::string& key);
    void set_prop(const VsmxValue& obj, const std::string& key,
                  const VsmxValue& val);
    VsmxValue index_get(const VsmxValue& obj, const VsmxValue& idx);
    void index_set(const VsmxValue& obj, const VsmxValue& idx,
                   const VsmxValue& val);
};

VsmxValue VsmxMachine::Impl::get_prop(const VsmxValue& obj,
                                      const std::string& key) {
    if (auto nat = native_of(obj)) return nat->get(key);
    if (auto s = as_script(obj)) {
        auto it = s->props.find(key);
        if (it != s->props.end()) return it->second;
    }
    return VsmxValue();
}

void VsmxMachine::Impl::set_prop(const VsmxValue& obj, const std::string& key,
                                 const VsmxValue& val) {
    if (auto nat = native_of(obj)) { nat->set(key, val); return; }
    if (auto s = as_script(obj)) s->props[key] = val;
}

VsmxValue VsmxMachine::Impl::index_get(const VsmxValue& obj,
                                       const VsmxValue& idx) {
    if (auto s = as_script(obj)) {
        if (s->is_array) {
            int i = static_cast<int>(idx.to_number());
            if (i >= 0 && i < static_cast<int>(s->elements.size()))
                return s->elements[i];
            return VsmxValue();
        }
        auto it = s->props.find(idx.to_string());
        return it != s->props.end() ? it->second : VsmxValue();
    }
    if (auto nat = native_of(obj)) return nat->get(idx.to_string());
    return VsmxValue();
}

void VsmxMachine::Impl::index_set(const VsmxValue& obj, const VsmxValue& idx,
                                  const VsmxValue& val) {
    if (auto s = as_script(obj)) {
        if (s->is_array) {
            int i = static_cast<int>(idx.to_number());
            if (i >= 0) {
                if (static_cast<int>(s->elements.size()) <= i)
                    s->elements.resize(i + 1);
                s->elements[i] = val;
            }
            return;
        }
        s->props[idx.to_string()] = val;
        return;
    }
    if (auto nat = native_of(obj)) nat->set(idx.to_string(), val);
}

void VsmxMachine::Impl::execute() {
    finished = false;
    while (!finished && pc < prog.code.size()) {
        if (budget == 0) { fail("step budget exhausted"); break; }
        --budget;

        const VsmxInstr c = prog.code[pc];
        ++pc;
        const int op = c.opcode();

        switch (op) {
            case VSMX_NOTHING:
            case VSMX_DEBUG_FILE:
            case VSMX_DEBUG_LINE:
            case VSMX_CONST_OBJECT:
            case VSMX_OBJ_ADD_ATTR:
            case VSMX_INSTANCEOF:
            case VSMX_IN:
            case VSMX_TRY_IN:
            case VSMX_TRY_OUT:
            case VSMX_CATCH_IN:
            case VSMX_CATCH_OUT:
            case VSMX_MAKE_FLOAT_ARRAY:
                break;

            case VSMX_ASSIGN: {
                VsmxValue val = deref(pop());
                VsmxValue ref = pop();
                assign_ref(ref, val);
                stack.push_back(val);
                break;
            }
            case VSMX_ADD: {
                VsmxValue a = deref(pop());
                VsmxValue b = deref(pop());
                if (a.kind() == VsmxValue::Kind::String ||
                    b.kind() == VsmxValue::Kind::String)
                    stack.push_back(VsmxValue::string(b.to_string() + a.to_string()));
                else
                    push_number(b.to_number() + a.to_number());
                break;
            }
            case VSMX_SUB: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_number(b - a);
                break;
            }
            case VSMX_MUL: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_number(b * a);
                break;
            }
            case VSMX_DIV: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_number(a == 0.0 ? 0.0 : b / a);
                break;
            }
            case VSMX_MOD: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_number(a == 0.0 ? 0.0 : std::fmod(b, a));
                break;
            }
            case VSMX_POS: push_number(deref(pop()).to_number()); break;
            case VSMX_NEG: push_number(-deref(pop()).to_number()); break;
            case VSMX_NOT: push_bool(!deref(pop()).to_bool()); break;
            case VSMX_PREINC: {
                VsmxValue ref = pop();
                double f = deref(ref).to_number() + 1.0;
                assign_ref(ref, VsmxValue::number(f));
                push_number(f);
                break;
            }
            case VSMX_PREDEC: {
                VsmxValue ref = pop();
                double f = deref(ref).to_number() - 1.0;
                assign_ref(ref, VsmxValue::number(f));
                push_number(f);
                break;
            }
            case VSMX_INC: {
                VsmxValue ref = pop();
                double f = deref(ref).to_number();
                push_number(f);
                assign_ref(ref, VsmxValue::number(f + 1.0));
                break;
            }
            case VSMX_DEC: {
                VsmxValue ref = pop();
                double f = deref(ref).to_number();
                push_number(f);
                assign_ref(ref, VsmxValue::number(f - 1.0));
                break;
            }
            case VSMX_EQ:
            case VSMX_NE: {
                VsmxValue a = deref(pop());
                VsmxValue b = deref(pop());
                bool eq;
                if (a.is_nullish() || b.is_nullish())
                    eq = a.is_nullish() && b.is_nullish();
                else if (a.kind() == VsmxValue::Kind::String &&
                         b.kind() == VsmxValue::Kind::String)
                    eq = a.str() == b.str();
                else
                    eq = a.to_number() == b.to_number();
                push_bool(op == VSMX_EQ ? eq : !eq);
                break;
            }
            case VSMX_IDENT:
            case VSMX_NIDENT: {
                VsmxValue a = deref(pop());
                VsmxValue b = deref(pop());
                bool same = a.kind() == b.kind();
                if (same && a.kind() == VsmxValue::Kind::String)
                    same = a.str() == b.str();
                else if (same && a.kind() == VsmxValue::Kind::Number)
                    same = a.number() == b.number();
                else if (same && a.kind() == VsmxValue::Kind::Bool)
                    same = a.boolean() == b.boolean();
                push_bool(op == VSMX_IDENT ? same : !same);
                break;
            }
            case VSMX_LT: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_bool(b < a);
                break;
            }
            case VSMX_LTE: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_bool(b <= a);
                break;
            }
            case VSMX_GTE: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_bool(b >= a);
                break;
            }
            case VSMX_GT: {
                double a = deref(pop()).to_number();
                double b = deref(pop()).to_number();
                push_bool(b > a);
                break;
            }
            case VSMX_TYPEOF: {
                VsmxValue v = deref(pop());
                const char* t = "undefined";
                switch (v.kind()) {
                    case VsmxValue::Kind::Number: t = "number"; break;
                    case VsmxValue::Kind::String: t = "string"; break;
                    case VsmxValue::Kind::Bool:   t = "boolean"; break;
                    case VsmxValue::Kind::Object: t = "object"; break;
                    case VsmxValue::Kind::Null:   t = "object"; break;
                    default: break;
                }
                stack.push_back(VsmxValue::string(t));
                break;
            }
            case VSMX_BAND:
            case VSMX_BXOR:
            case VSMX_BOR:
            case VSMX_LSHIFT:
            case VSMX_RSHIFT:
            case VSMX_URSHIFT: {
                long a = static_cast<long>(deref(pop()).to_number());
                long b = static_cast<long>(deref(pop()).to_number());
                long r = 0;
                switch (op) {
                    case VSMX_BAND: r = a & b; break;
                    case VSMX_BXOR: r = a ^ b; break;
                    case VSMX_BOR:  r = a | b; break;
                    case VSMX_LSHIFT: r = b << (a & 31); break;
                    case VSMX_RSHIFT: r = b >> (a & 31); break;
                    case VSMX_URSHIFT:
                        r = static_cast<long>(static_cast<unsigned long>(b) >>
                                              (a & 31));
                        break;
                    default: break;
                }
                push_number(static_cast<double>(r));
                break;
            }
            case VSMX_BNOT:
                push_number(static_cast<double>(~static_cast<long>(
                    deref(pop()).to_number())));
                break;

            case VSMX_STACK_COPY:
                if (stack.empty()) { fail("stack underflow"); break; }
                stack.push_back(stack.back());
                break;
            case VSMX_STACK_SWAP:
                if (stack.size() < 2) { fail("stack underflow"); break; }
                std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
                break;
            case VSMX_END_STMT:
                stack.clear();
                break;

            case VSMX_CONST_NULL:
                stack.push_back(VsmxValue::null());
                break;
            case VSMX_CONST_EMPTY_ARRAY:
            case VSMX_ARRAY: {
                auto a = std::make_shared<ScriptObject>();
                a->is_array = true;
                stack.push_back(VsmxValue::object(a));
                break;
            }
            case VSMX_CONST_BOOL:
                push_bool(c.value != 0);
                break;
            case VSMX_CONST_INT:
                push_number(static_cast<double>(static_cast<int32_t>(c.value)));
                break;
            case VSMX_CONST_FLOAT:
                push_number(static_cast<double>(c.float_value()));
                break;
            case VSMX_CONST_STRING:
                stack.push_back(VsmxValue::string(
                    c.value < prog.texts.size() ? prog.texts[c.value] : ""));
                break;

            case VSMX_FUNCTION: {
                auto fn = std::make_shared<ScriptObject>();
                fn->is_function = true;
                fn->start_line = static_cast<int>(c.value);
                fn->fn_args = c.args();
                fn->local_vars = c.local_vars();
                stack.push_back(VsmxValue::object(fn));
                break;
            }
            case VSMX_THIS:
                if (!frames.empty())
                    stack.push_back(VsmxValue::object(frames.back().this_obj));
                else
                    stack.push_back(VsmxValue::object(globals));
                break;
            case VSMX_UNNAMED_VAR: {
                auto r = std::make_shared<RefObject>();
                r->kind = RefObject::Kind::Local;
                r->frame = static_cast<int>(frames.size()) - 1;
                r->slot = static_cast<int>(c.value);
                stack.push_back(VsmxValue::object(r));
                break;
            }
            case VSMX_VARIABLE: {
                std::string name = c.value < prog.names.size()
                                       ? prog.names[c.value] : "";
                auto it = natives.find(name);
                if (it != natives.end()) {
                    stack.push_back(make_native(it->second));
                } else {
                    auto r = std::make_shared<RefObject>();
                    r->kind = RefObject::Kind::Global;
                    r->name = name;
                    stack.push_back(VsmxValue::object(r));
                }
                break;
            }
            case VSMX_PROPERTY: {
                VsmxValue obj = deref(pop());
                std::string key = c.value < prog.properties.size()
                                      ? prog.properties[c.value] : "";
                stack.push_back(get_prop(obj, key));
                break;
            }
            case VSMX_METHOD: {
                VsmxValue obj = deref(pop());
                std::string key = c.value < prog.properties.size()
                                      ? prog.properties[c.value] : "";
                auto m = std::make_shared<ScriptObject>();
                m->props["__method__"] = VsmxValue::string(key);
                m->props["__self__"] = obj;
                stack.push_back(VsmxValue::object(m));
                break;
            }
            case VSMX_SET_ATTR: {
                VsmxValue val = deref(pop());
                VsmxValue obj = deref(pop());
                std::string key = c.value < prog.properties.size()
                                      ? prog.properties[c.value] : "";
                set_prop(obj, key, val);
                break;
            }
            case VSMX_UNSET: {
                VsmxValue obj = deref(pop());
                if (auto s = as_script(obj))
                    s->props.erase(c.value < prog.properties.size()
                                       ? prog.properties[c.value] : "");
                break;
            }
            case VSMX_ARRAY_INDEX:
            case VSMX_ARRAY_INDEX_KEEP: {
                VsmxValue idx = deref(pop());
                VsmxValue arr = deref(pop());
                stack.push_back(index_get(arr, idx));
                break;
            }
            case VSMX_ARRAY_INDEX_ASSIGN: {
                VsmxValue val = deref(pop());
                VsmxValue idx = deref(pop());
                VsmxValue arr = deref(pop());
                index_set(arr, idx, val);
                break;
            }
            case VSMX_ARRAY_DELETE: {
                VsmxValue idx = deref(pop());
                VsmxValue arr = deref(pop());
                if (auto s = as_script(arr)) {
                    if (s->is_array) {
                        int i = static_cast<int>(idx.to_number());
                        if (i >= 0 && i < static_cast<int>(s->elements.size()))
                            s->elements[i] = VsmxValue();
                    } else {
                        s->props.erase(idx.to_string());
                    }
                }
                break;
            }
            case VSMX_ARRAY_PUSH: {
                VsmxValue val = deref(pop());
                VsmxValue arr = deref(pop());
                if (auto s = as_script(arr)) s->elements.push_back(val);
                stack.push_back(arr);
                break;
            }

            case VSMX_JUMP:
                pc = c.value;
                break;
            case VSMX_JUMP_TRUE:
                if (deref(pop()).to_bool()) pc = c.value;
                break;
            case VSMX_JUMP_FALSE:
                if (!deref(pop()).to_bool()) pc = c.value;
                break;

            case VSMX_CALL_FUNC:
            case VSMX_CALL_METHOD: {
                std::vector<VsmxValue> args;
                for (uint32_t i = 0; i < c.value; ++i) args.push_back(deref(pop()));
                std::reverse(args.begin(), args.end());
                VsmxValue target = deref(pop());

                if (auto m = as_script(target)) {
                    auto mit = m->props.find("__method__");
                    if (mit != m->props.end()) {
                        VsmxValue self = m->props["__self__"];
                        if (auto nat = native_of(self)) {
                            stack.push_back(nat->call(mit->second.str(), args));
                            break;
                        }
                        if (auto s = as_script(self)) {
                            auto fit = s->props.find(mit->second.str());
                            if (fit != s->props.end()) {
                                if (auto fn = as_script(fit->second)) {
                                    if (fn->is_function) {
                                        call_function(fn, s, args);
                                        break;
                                    }
                                }
                            }
                        }
                        stack.push_back(VsmxValue());
                        break;
                    }
                    if (m->is_function) {
                        call_function(m, globals, args);
                        break;
                    }
                }
                stack.push_back(VsmxValue());
                break;
            }
            case VSMX_CALL_NEW: {
                std::vector<VsmxValue> args;
                for (uint32_t i = 0; i < c.value; ++i) args.push_back(deref(pop()));
                std::reverse(args.begin(), args.end());
                VsmxValue target = deref(pop());
                if (auto fn = as_script(target)) {
                    if (fn->is_function) {
                        auto this_obj = std::make_shared<ScriptObject>();
                        call_function(fn, this_obj, args, /*return_this=*/true);
                        break;
                    }
                    if (fn->is_array) {
                        auto arr = std::make_shared<ScriptObject>();
                        arr->is_array = true;
                        if (c.value == 1)
                            arr->elements.resize(static_cast<size_t>(
                                std::max(0.0, args[0].to_number())));
                        else
                            arr->elements = args;
                        stack.push_back(VsmxValue::object(arr));
                        break;
                    }
                }
                stack.push_back(VsmxValue());
                break;
            }
            case VSMX_RETURN: {
                VsmxValue v = deref(pop());
                if (frames.empty()) { finished = true; break; }
                Frame fr = frames.back();
                frames.pop_back();
                stack.push_back(fr.return_this ? VsmxValue::object(fr.this_obj)
                                               : v);
                pc = fr.return_pc;
                break;
            }
            case VSMX_END:
            case VSMX_THROW:
                finished = true;
                break;

            default:
                fail(std::string("unimplemented opcode ") +
                     vsmx_opcode_name(op));
                break;
        }
    }
    if (!stack.empty()) last_float = deref(stack.back()).to_number();
}

VsmxMachine::VsmxMachine(const VsmxProgram& prog)
    : m_impl(new Impl(prog)) {}

VsmxMachine::~VsmxMachine() = default;

void VsmxMachine::bind(const std::string& name,
                       std::shared_ptr<VsmxNative> obj) {
    m_impl->natives[name] = std::move(obj);
}

bool VsmxMachine::run() {
    m_impl->budget = 2000000;
    m_impl->execute();
    m_steps = 2000000 - m_impl->budget;
    m_error = m_impl->error;
    m_last_float = m_impl->last_float;
    return m_error.empty();
}

bool VsmxMachine::call_event(const std::string& event,
                             const std::vector<std::string>& args) {
    const size_t slash = event.rfind('/');
    if (slash == std::string::npos) {
        m_error = "malformed event '" + event + "'";
        return false;
    }
    const std::string fn_name = event.substr(slash + 1);
    auto it = m_impl->globals->props.find(fn_name);
    if (it == m_impl->globals->props.end()) {
        m_error = "no event function '" + fn_name + "'";
        return false;
    }
    auto fn = as_script(it->second);
    if (!fn || !fn->is_function) {
        m_error = "'" + fn_name + "' is not a function";
        return false;
    }
    std::vector<VsmxValue> vargs;
    for (const auto& a : args) vargs.push_back(VsmxValue::string(a));
    m_impl->budget = 500000;
    m_impl->call_function(fn, m_impl->globals, vargs, false);
    m_impl->execute();
    m_steps = 500000 - m_impl->budget;
    m_error = m_impl->error;
    m_last_float = m_impl->last_float;
    return m_error.empty();
}

}  // namespace umdplay
