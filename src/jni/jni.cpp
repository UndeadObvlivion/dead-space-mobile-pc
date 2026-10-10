#include "jni/jni.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "cpu.h"
#include "elf_loader.h"
#include "hle/hle.h"
#include "log.h"

namespace jni {

using cpu::Ctx;
using hle::ArgReader;
using s8 = std::int8_t;

// ---------------------------------------------------------------- references
namespace {
std::recursive_mutex g_lock;
std::vector<ObjPtr> g_refs;    // index = (handle >> 2) - 1
std::vector<u8> g_ref_kind;    // 1 local, 2 global
std::vector<u32> g_ref_free;

struct ThreadJni {
    std::vector<std::vector<jref>> frames{std::vector<jref>{}};
    ObjPtr exception;
};
thread_local ThreadJni t_jni;

std::unordered_map<std::string, ObjPtr> g_classes;
std::unordered_map<std::string, std::string> g_super;

struct MethodInfo {
    std::string cls, name, sig;
    bool is_static;
};
std::vector<MethodInfo> g_methods;
std::map<std::string, u32> g_method_ids;
struct FieldInfo {
    std::string cls, name, sig;
    bool is_static;
};
std::vector<FieldInfo> g_fields;
std::map<std::string, u32> g_field_ids;

std::unordered_map<std::string, JavaFn> g_java;        // "cls.name" -> host impl
std::unordered_map<std::string, u32> g_registered;     // "cls.name" -> guest fn (RegisterNatives)
std::set<std::string> g_warned;

u32 g_env = 0, g_vm = 0;

jref add_ref(const ObjPtr& o, u8 kind) {
    if (!o) return 0;
    std::lock_guard lk(g_lock);
    u32 idx;
    if (!g_ref_free.empty()) {
        idx = g_ref_free.back();
        g_ref_free.pop_back();
        g_refs[idx] = o;
        g_ref_kind[idx] = kind;
    } else {
        idx = u32(g_refs.size());
        g_refs.push_back(o);
        g_ref_kind.push_back(kind);
    }
    return (idx + 1) << 2 | kind;
}
}  // namespace

Object::~Object() {
    if (data && owns_data && elem && elem != 'L') mem::heap_free(data);
}

jref new_local(const ObjPtr& o) {
    jref r = add_ref(o, 1);
    if (r) t_jni.frames.back().push_back(r);
    return r;
}

jref new_global(const ObjPtr& o) { return add_ref(o, 2); }

ObjPtr get(jref r) {
    if (!r) return nullptr;
    std::lock_guard lk(g_lock);
    u32 idx = (r >> 2) - 1;
    if (idx >= g_refs.size()) {
        LOGW("JNI: invalid reference %08x", r);
        return nullptr;
    }
    return g_refs[idx];
}

void delete_ref(jref r) {
    if (!r) return;
    std::lock_guard lk(g_lock);
    u32 idx = (r >> 2) - 1;
    if (idx >= g_refs.size() || !g_refs[idx]) return;
    g_refs[idx].reset();
    g_ref_kind[idx] = 0;
    g_ref_free.push_back(idx);
}

namespace {
struct LocalFrame {
    LocalFrame() { t_jni.frames.emplace_back(); }
    ~LocalFrame() {
        for (jref r : t_jni.frames.back()) delete_ref(r);
        t_jni.frames.pop_back();
    }
};
}  // namespace

ObjPtr make(const std::string& cls) {
    auto o = std::make_shared<Object>();
    o->cls = cls;
    return o;
}

ObjPtr make_string(const std::string& s) {
    auto o = make("java/lang/String");
    o->str = s;
    return o;
}

static int elem_size(char e) {
    switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
    }
}

ObjPtr make_array(char elem, u32 len) {
    auto o = make(std::string("[") + elem);
    o->elem = elem;
    o->len = len;
    if (elem != 'L') {
        u32 bytes = std::max<u32>(1, len * elem_size(elem));
        o->data = mem::heap_alloc(bytes);
        std::memset(mem::host(o->data), 0, bytes);
    } else {
        o->objs.resize(len);
    }
    return o;
}

ObjPtr class_object(const std::string& name) {
    std::lock_guard lk(g_lock);
    auto& c = g_classes[name];
    if (!c) {
        c = make("java/lang/Class");
        c->str = name;
    }
    return c;
}

std::string string_value(jref r) {
    ObjPtr o = get(r);
    return o ? o->str : std::string();
}

void throw_new(const std::string& cls, const std::string& msg) {
    auto e = make(cls);
    e->str = msg;
    t_jni.exception = e;
}

void def(const std::string& cls, const std::string& name, JavaFn fn) { g_java[cls + "." + name] = std::move(fn); }
void def_superclass(const std::string& cls, const std::string& super) { g_super[cls] = super; }

u32 env() { return g_env; }
u32 vm() { return g_vm; }

// ---------------------------------------------------------------- signatures
namespace {
struct Sig {
    std::vector<char> params;
    char ret = 'V';
};

Sig parse_sig(const std::string& s) {
    Sig r;
    size_t i = 1;
    while (i < s.size() && s[i] != ')') {
        char ch = s[i];
        if (ch == '[') {
            while (s[i] == '[') i++;
            if (s[i] == 'L') i = s.find(';', i);
            r.params.push_back('L');
            i++;
        } else if (ch == 'L') {
            i = s.find(';', i) + 1;
            r.params.push_back('L');
        } else {
            r.params.push_back(ch);
            i++;
        }
    }
    if (i + 1 < s.size()) r.ret = s[i + 1] == '[' ? 'L' : s[i + 1];
    return r;
}

std::vector<JValue> read_args_reader(const Sig& sig, ArgReader& a) {
    std::vector<JValue> v;
    for (char p : sig.params) {
        switch (p) {
        case 'J': v.push_back(JValue::J(s64(a.next64()))); break;
        case 'D': v.push_back(JValue::D(a.nextd())); break;
        case 'F': v.push_back(JValue::F(float(a.nextd()))); break;  // promoted to double
        default: v.push_back(JValue::I(s32(a.next32()))); break;
        }
    }
    return v;
}

std::vector<JValue> read_args_array(const Sig& sig, u32 arr) {
    std::vector<JValue> v;
    for (size_t i = 0; i < sig.params.size(); i++) {
        u32 e = arr + u32(i) * 8;
        switch (sig.params[i]) {
        case 'J':
        case 'D': {
            JValue j;
            std::memcpy(&j.raw, mem::host(e), 8);
            v.push_back(j);
            break;
        }
        case 'Z': v.push_back(JValue::I(mem::r8(e))); break;
        case 'B': v.push_back(JValue::I(s8(mem::r8(e)))); break;
        case 'C': v.push_back(JValue::I(mem::r16(e))); break;
        case 'S': v.push_back(JValue::I(int16_t(mem::r16(e)))); break;
        default: v.push_back(JValue::I(s32(mem::r32(e)))); break;
        }
    }
    return v;
}

void ret_value(Ctx& c, char t, JValue v) {
    switch (t) {
    case 'V': break;
    case 'J':
    case 'D': c.ret64(v.raw); break;
    case 'Z': c.ret(v.z() ? 1 : 0); break;
    case 'B': c.ret(u32(s32(s8(v.i())))); break;
    case 'C': c.ret(u32(u16(v.i()))); break;
    case 'S': c.ret(u32(s32(int16_t(v.i())))); break;
    default: c.ret(u32(v.raw)); break;
    }
}

std::string mangle(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '/') out += '_';
        else if (ch == '_') out += "_1";
        else if (ch == ';') out += "_2";
        else if (ch == '[') out += "_3";
        else out += ch;
    }
    return out;
}

u32 find_native(const std::string& cls, const std::string& name, const std::string& sig) {
    auto it = g_registered.find(cls + "." + name);
    if (it != g_registered.end()) return it->second;
    std::string base = "Java_" + mangle(cls) + "_" + mangle(name);
    if (u32 a = elf::find(base)) return a;
    std::string params = sig.substr(1, sig.find(')') - 1);
    return elf::find(base + "__" + mangle(params));
}
}  // namespace

bool has_native(const std::string& cls, const std::string& name) { return find_native(cls, name, "()V") != 0; }

JValue call_native(const std::string& cls, const std::string& name, const std::string& sig, const ObjPtr& thiz,
                   const std::vector<JValue>& args) {
    u32 fn = find_native(cls, name, sig);
    if (!fn) {
        LOGW("JNI: no native implementation for %s.%s%s", cls.c_str(), name.c_str(), sig.c_str());
        return {};
    }
    LocalFrame frame;
    Sig s = parse_sig(sig);
    std::vector<u32> regs;
    regs.push_back(g_env);
    regs.push_back(new_local(thiz));
    for (size_t i = 0; i < s.params.size(); i++) {
        char p = s.params[i];
        if (p == 'J' || p == 'D') {
            if (regs.size() & 1) regs.push_back(0);
            regs.push_back(u32(args[i].raw));
            regs.push_back(u32(args[i].raw >> 32));
        } else {
            regs.push_back(u32(args[i].raw));
        }
    }
    u32 hi = 0;
    u32 lo = cpu::call_v(fn, regs.data(), int(regs.size()), &hi);
    JValue r;
    r.raw = (s.ret == 'J' || s.ret == 'D') ? (u64(lo) | (u64(hi) << 32)) : lo;
    if (s.ret == 'L' && lo) {
        // returned reference belongs to the frame we are about to pop; keep the object alive
        r.raw = new_global(get(lo));
    }
    return r;
}

// ---------------------------------------------------------------- method dispatch
namespace {
JValue invoke(const MethodInfo& m, const ObjPtr& thiz, const std::vector<JValue>& args) {
    if (logging::g_verbose && m.name != "write") {
        std::string a;
        for (auto& v : args) a += " " + std::to_string(v.i());
        LOGD("[t%u] JNI call %s.%s%s%s this=%p", cpu::thread().tid, m.cls.c_str(), m.name.c_str(), m.sig.c_str(),
             a.c_str(), static_cast<void*>(thiz.get()));
    }
    // host implementation: method's class chain, then the object's runtime class chain
    for (int pass = 0; pass < 2; pass++) {
        std::string cls = pass == 0 ? m.cls : (thiz ? thiz->cls : "");
        for (int depth = 0; !cls.empty() && depth < 16; depth++) {
            auto it = g_java.find(cls + "." + m.name);
            if (it != g_java.end()) return it->second(thiz, args, m.sig);
            auto s = g_super.find(cls);
            cls = s == g_super.end() ? "" : s->second;
        }
    }
    // the game's own native implementation
    if (find_native(m.cls, m.name, m.sig))
        return call_native(m.cls, m.name, m.sig, m.is_static ? class_object(m.cls) : thiz, args);
    std::string key = m.cls + "." + m.name + m.sig;
    {
        std::lock_guard lk(g_lock);
        if (g_warned.insert(key).second)
            LOGW("JNI: unimplemented Java method %s (called from %s)", key.c_str(),
                 cpu::symbolize(cpu::current_pc()).c_str());
    }
    return {};
}

u32 method_id(const std::string& cls, const std::string& name, const std::string& sig, bool is_static) {
    std::lock_guard lk(g_lock);
    std::string key = cls + "|" + name + "|" + sig + (is_static ? "|s" : "");
    auto it = g_method_ids.find(key);
    if (it != g_method_ids.end()) return it->second;
    g_methods.push_back({cls, name, sig, is_static});
    u32 id = u32(g_methods.size());
    g_method_ids[key] = id;
    return id;
}

MethodInfo method(u32 id) {
    std::lock_guard lk(g_lock);
    if (id == 0 || id > g_methods.size()) return {"?", "?", "()V", false};
    return g_methods[id - 1];
}

u32 field_id(const std::string& cls, const std::string& name, const std::string& sig, bool is_static) {
    std::lock_guard lk(g_lock);
    std::string key = cls + "|" + name + "|" + sig + (is_static ? "|s" : "");
    auto it = g_field_ids.find(key);
    if (it != g_field_ids.end()) return it->second;
    g_fields.push_back({cls, name, sig, is_static});
    u32 id = u32(g_fields.size());
    g_field_ids[key] = id;
    return id;
}

FieldInfo field(u32 id) {
    std::lock_guard lk(g_lock);
    if (id == 0 || id > g_fields.size()) return {"?", "?", "I", false};
    return g_fields[id - 1];
}

constexpr char kRet[10] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'V'};
constexpr char kPrim[8] = {'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D'};
enum { VIRTUAL, NONVIRTUAL, STATIC };

// Call<Type>Method[V|A], CallNonvirtual..., CallStatic...
template <int T, int MODE, int KIND> void call_method(Ctx& c) {
    int first = KIND == NONVIRTUAL ? 3 : 2;  // index of the methodID argument
    ObjPtr thiz = get(c.arg(1));
    MethodInfo m = method(c.arg(first));
    Sig s = parse_sig(m.sig);
    std::vector<JValue> args;
    if (MODE == 0) {
        ArgReader a = ArgReader::slots(c, first + 1);
        args = read_args_reader(s, a);
    } else if (MODE == 1) {
        ArgReader a = ArgReader::valist(c.arg(first + 1));
        args = read_args_reader(s, a);
    } else {
        args = read_args_array(s, c.arg(first + 1));
    }
    if (KIND == STATIC) thiz = nullptr;
    JValue r = invoke(m, thiz, args);
    ret_value(c, kRet[T], r);
}

template <int K, int... I> void reg_calls(std::vector<cpu::HleFn>& t, int base, std::integer_sequence<int, I...>) {
    ((t[base + I] = &call_method<I / 3, I % 3, K>), ...);
}

template <int MODE> void new_object(Ctx& c) {
    ObjPtr cls = get(c.arg(1));
    MethodInfo m = method(c.arg(2));
    Sig s = parse_sig(m.sig);
    std::vector<JValue> args;
    if (MODE == 0) {
        ArgReader a = ArgReader::slots(c, 3);
        args = read_args_reader(s, a);
    } else if (MODE == 1) {
        ArgReader a = ArgReader::valist(c.arg(3));
        args = read_args_reader(s, a);
    } else {
        args = read_args_array(s, c.arg(3));
    }
    ObjPtr o = make(cls ? cls->str : m.cls);
    auto it = g_java.find(o->cls + ".<init>");
    if (it != g_java.end()) it->second(o, args, m.sig);
    c.ret(new_local(o));
}

JValue get_field_value(const ObjPtr& o, const FieldInfo& f) {
    if (!o) return {};
    auto it = o->fields.find(f.name);
    return it == o->fields.end() ? JValue{} : it->second;
}

template <int T, bool STATIC_> void get_field(Ctx& c) {
    FieldInfo f = field(c.arg(2));
    ObjPtr o = STATIC_ ? class_object(f.cls) : get(c.arg(1));
    std::lock_guard lk(g_lock);
    ret_value(c, kRet[T], get_field_value(o, f));
}

template <int T, bool STATIC_> void set_field(Ctx& c) {
    FieldInfo f = field(c.arg(2));
    ObjPtr o = STATIC_ ? class_object(f.cls) : get(c.arg(1));
    if (!o) return;
    JValue v;
    char t = kRet[T];
    if (t == 'J' || t == 'D') {
        // env, obj, fieldID take r0-r2; a 64-bit value needs an even register pair, so it is on the stack
        v.raw = u64(c.arg(4)) | (u64(c.arg(5)) << 32);
    } else {
        v.raw = c.arg(3);
    }
    std::lock_guard lk(g_lock);
    o->fields[f.name] = v;
}

template <int... I> void reg_fields(std::vector<cpu::HleFn>& t, std::integer_sequence<int, I...>) {
    ((t[95 + I] = &get_field<I, false>), ...);
    ((t[104 + I] = &set_field<I, false>), ...);
    ((t[145 + I] = &get_field<I, true>), ...);
    ((t[154 + I] = &set_field<I, true>), ...);
}

template <int T> void new_prim_array(Ctx& c) { c.ret(new_local(make_array(kPrim[T], c.arg(1)))); }
template <int T> void get_array_elements(Ctx& c) {
    ObjPtr a = get(c.arg(1));
    if (c.arg(2)) mem::w8(c.arg(2), 0);
    c.ret(a ? a->data : 0);
}
template <int T> void release_array_elements(Ctx& c) {}
template <int T> void get_array_region(Ctx& c) {
    ObjPtr a = get(c.arg(1));
    if (!a) return;
    int es = elem_size(kPrim[T]);
    u32 start = c.arg(2), len = c.arg(3), buf = c.arg(4);
    if (start + len > a->len) {
        throw_new("java/lang/ArrayIndexOutOfBoundsException", "");
        return;
    }
    std::memcpy(mem::host(buf), mem::host(a->data + start * es), size_t(len) * es);
}
template <int T> void set_array_region(Ctx& c) {
    ObjPtr a = get(c.arg(1));
    if (!a) return;
    int es = elem_size(kPrim[T]);
    u32 start = c.arg(2), len = c.arg(3), buf = c.arg(4);
    if (start + len > a->len) {
        throw_new("java/lang/ArrayIndexOutOfBoundsException", "");
        return;
    }
    std::memcpy(mem::host(a->data + start * es), mem::host(buf), size_t(len) * es);
}

template <int... I> void reg_arrays(std::vector<cpu::HleFn>& t, std::integer_sequence<int, I...>) {
    ((t[175 + I] = &new_prim_array<I>), ...);
    ((t[183 + I] = &get_array_elements<I>), ...);
    ((t[191 + I] = &release_array_elements<I>), ...);
    ((t[199 + I] = &get_array_region<I>), ...);
    ((t[207 + I] = &set_array_region<I>), ...);
}

bool is_instance(const std::string& cls, const std::string& target) {
    std::string c = cls;
    for (int i = 0; i < 16 && !c.empty(); i++) {
        if (c == target) return true;
        auto s = g_super.find(c);
        c = s == g_super.end() ? "" : s->second;
    }
    return target == "java/lang/Object";
}

std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        u8 ch = u8(s[i]);
        u32 cp;
        int n;
        if (ch < 0x80) { cp = ch; n = 1; }
        else if ((ch >> 5) == 6) { cp = ch & 0x1F; n = 2; }
        else if ((ch >> 4) == 14) { cp = ch & 0x0F; n = 3; }
        else { cp = ch & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (u8(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string utf16_to_utf8(const char16_t* s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i++) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

void unimpl_jni(Ctx& c) {
    u32 idx = (c.reg(15) - 4 - mem::STUB_BASE) / 8;
    LOGE("JNI: unimplemented env function (%s) called from %s", cpu::hle_name_at(c.reg(15) - 4).c_str(),
         cpu::symbolize(c.lr()).c_str());
    (void)idx;
    c.ret(0);
}

}  // namespace

// ---------------------------------------------------------------- tables
void init() {
    std::vector<cpu::HleFn> t(233, &unimpl_jni);
    t[4] = [](Ctx& c) { c.ret(0x00010006); };
    t[6] = [](Ctx& c) {  // FindClass
        std::string name = c.argstr(1);
        LOGD("JNI FindClass %s", name.c_str());
        c.ret(new_local(class_object(name)));
    };
    t[10] = [](Ctx& c) {  // GetSuperclass
        ObjPtr cls = get(c.arg(1));
        auto s = cls ? g_super.find(cls->str) : g_super.end();
        c.ret(s == g_super.end() ? new_local(class_object("java/lang/Object")) : new_local(class_object(s->second)));
    };
    t[11] = [](Ctx& c) {  // IsAssignableFrom
        ObjPtr a = get(c.arg(1)), b = get(c.arg(2));
        c.ret(a && b && is_instance(a->str, b->str));
    };
    t[13] = [](Ctx& c) {  // Throw
        t_jni.exception = get(c.arg(1));
        c.ret(0);
    };
    t[14] = [](Ctx& c) {  // ThrowNew
        ObjPtr cls = get(c.arg(1));
        throw_new(cls ? cls->str : "java/lang/Throwable", c.argstr(2) ? c.argstr(2) : "");
        c.ret(0);
    };
    t[15] = [](Ctx& c) { c.ret(t_jni.exception ? new_local(t_jni.exception) : 0); };  // ExceptionOccurred
    t[16] = [](Ctx& c) {                                                           // ExceptionDescribe
        if (t_jni.exception)
            LOGW("JNI exception: %s: %s", t_jni.exception->cls.c_str(), t_jni.exception->str.c_str());
    };
    t[17] = [](Ctx& c) { t_jni.exception.reset(); };  // ExceptionClear
    t[18] = [](Ctx& c) {                              // FatalError
        LOGE("JNI FatalError: %s", c.argstr(1));
        std::abort();
    };
    t[19] = [](Ctx& c) { t_jni.frames.emplace_back(); c.ret(0); };  // PushLocalFrame
    t[20] = [](Ctx& c) {                                           // PopLocalFrame
        ObjPtr keep = get(c.arg(1));
        if (t_jni.frames.size() > 1) {
            for (jref r : t_jni.frames.back()) delete_ref(r);
            t_jni.frames.pop_back();
        }
        c.ret(keep ? new_local(keep) : 0);
    };
    t[21] = [](Ctx& c) { c.ret(new_global(get(c.arg(1)))); };  // NewGlobalRef
    t[22] = [](Ctx& c) { delete_ref(c.arg(1)); };              // DeleteGlobalRef
    t[23] = [](Ctx& c) {                                       // DeleteLocalRef
        jref r = c.arg(1);
        if ((r & 3) != 1) return;
        for (auto it = t_jni.frames.rbegin(); it != t_jni.frames.rend(); ++it) {
            auto f = std::find(it->begin(), it->end(), r);
            if (f != it->end()) {
                it->erase(f);
                delete_ref(r);
                return;
            }
        }
    };
    t[24] = [](Ctx& c) { c.ret(get(c.arg(1)) == get(c.arg(2))); };  // IsSameObject
    t[25] = [](Ctx& c) { c.ret(new_local(get(c.arg(1)))); };         // NewLocalRef
    t[26] = [](Ctx& c) { c.ret(0); };                                // EnsureLocalCapacity
    t[27] = [](Ctx& c) {                                             // AllocObject
        ObjPtr cls = get(c.arg(1));
        c.ret(new_local(make(cls ? cls->str : "java/lang/Object")));
    };
    t[28] = &new_object<0>;
    t[29] = &new_object<1>;
    t[30] = &new_object<2>;
    t[31] = [](Ctx& c) {  // GetObjectClass
        ObjPtr o = get(c.arg(1));
        c.ret(o ? new_local(class_object(o->cls)) : 0);
    };
    t[32] = [](Ctx& c) {  // IsInstanceOf
        ObjPtr o = get(c.arg(1)), cls = get(c.arg(2));
        c.ret(!o || (cls && is_instance(o->cls, cls->str)));
    };
    t[33] = [](Ctx& c) {  // GetMethodID
        ObjPtr cls = get(c.arg(1));
        std::string cn = cls ? cls->str : "?";
        LOGD("JNI GetMethodID %s.%s%s", cn.c_str(), c.argstr(2), c.argstr(3));
        c.ret(method_id(cn, c.argstr(2), c.argstr(3), false));
    };
    reg_calls<VIRTUAL>(t, 34, std::make_integer_sequence<int, 30>{});
    reg_calls<NONVIRTUAL>(t, 64, std::make_integer_sequence<int, 30>{});
    t[94] = [](Ctx& c) {  // GetFieldID
        ObjPtr cls = get(c.arg(1));
        c.ret(field_id(cls ? cls->str : "?", c.argstr(2), c.argstr(3), false));
    };
    reg_fields(t, std::make_integer_sequence<int, 9>{});
    t[113] = [](Ctx& c) {  // GetStaticMethodID
        ObjPtr cls = get(c.arg(1));
        std::string cn = cls ? cls->str : "?";
        LOGD("JNI GetStaticMethodID %s.%s%s", cn.c_str(), c.argstr(2), c.argstr(3));
        c.ret(method_id(cn, c.argstr(2), c.argstr(3), true));
    };
    reg_calls<STATIC>(t, 114, std::make_integer_sequence<int, 30>{});
    t[144] = [](Ctx& c) {  // GetStaticFieldID
        ObjPtr cls = get(c.arg(1));
        c.ret(field_id(cls ? cls->str : "?", c.argstr(2), c.argstr(3), true));
    };
    t[163] = [](Ctx& c) {  // NewString(const jchar*, len)
        const char16_t* s = c.argp<const char16_t>(1);
        c.ret(new_local(make_string(utf16_to_utf8(s, c.arg(2)))));
    };
    t[164] = [](Ctx& c) { c.ret(u32(utf8_to_utf16(string_value(c.arg(1))).size())); };  // GetStringLength
    t[165] = [](Ctx& c) {                                                              // GetStringChars
        auto u = utf8_to_utf16(string_value(c.arg(1)));
        u32 a = mem::heap_alloc(u32(u.size() * 2 + 2));
        std::memcpy(mem::host(a), u.c_str(), u.size() * 2 + 2);
        if (c.arg(2)) mem::w8(c.arg(2), 1);
        c.ret(a);
    };
    t[166] = [](Ctx& c) { mem::heap_free(c.arg(2)); };  // ReleaseStringChars
    t[167] = [](Ctx& c) {                              // NewStringUTF
        if (!c.arg(1)) return c.ret(0);
        c.ret(new_local(make_string(c.argstr(1))));
    };
    t[168] = [](Ctx& c) { c.ret(u32(string_value(c.arg(1)).size())); };  // GetStringUTFLength
    t[169] = [](Ctx& c) {                                               // GetStringUTFChars
        ObjPtr o = get(c.arg(1));
        if (c.arg(2)) mem::w8(c.arg(2), 1);
        c.ret(o ? mem::heap_strdup(o->str.c_str()) : 0);
    };
    t[170] = [](Ctx& c) { mem::heap_free(c.arg(2)); };  // ReleaseStringUTFChars
    t[171] = [](Ctx& c) {                              // GetArrayLength
        ObjPtr o = get(c.arg(1));
        c.ret(o ? o->len : 0);
    };
    t[172] = [](Ctx& c) {  // NewObjectArray
        auto a = make_array('L', c.arg(1));
        ObjPtr cls = get(c.arg(2));
        if (cls) a->cls = "[L" + cls->str + ";";
        ObjPtr init = get(c.arg(3));
        for (auto& e : a->objs) e = init;
        c.ret(new_local(a));
    };
    t[173] = [](Ctx& c) {  // GetObjectArrayElement
        ObjPtr a = get(c.arg(1));
        u32 i = c.arg(2);
        if (!a || i >= a->objs.size()) return c.ret(0);
        c.ret(new_local(a->objs[i]));
    };
    t[174] = [](Ctx& c) {  // SetObjectArrayElement
        ObjPtr a = get(c.arg(1));
        u32 i = c.arg(2);
        if (a && i < a->objs.size()) a->objs[i] = get(c.arg(3));
    };
    reg_arrays(t, std::make_integer_sequence<int, 8>{});
    t[215] = [](Ctx& c) {  // RegisterNatives
        ObjPtr cls = get(c.arg(1));
        u32 m = c.arg(2), n = c.arg(3);
        for (u32 i = 0; i < n; i++) {
            std::string name = mem::str(mem::r32(m + 12 * i));
            std::string sig = mem::str(mem::r32(m + 12 * i + 4));
            u32 fn = mem::r32(m + 12 * i + 8);
            g_registered[(cls ? cls->str : "?") + "." + name] = fn;
            LOGD("JNI RegisterNatives %s.%s%s -> %s", cls ? cls->str.c_str() : "?", name.c_str(), sig.c_str(),
                 cpu::symbolize(fn).c_str());
        }
        c.ret(0);
    };
    t[216] = [](Ctx& c) { c.ret(0); };  // UnregisterNatives
    t[217] = [](Ctx& c) {  // MonitorEnter
        if (ObjPtr o = get(c.arg(1))) o->monitor.lock();
        c.ret(0);
    };
    t[218] = [](Ctx& c) {  // MonitorExit
        if (ObjPtr o = get(c.arg(1))) o->monitor.unlock();
        c.ret(0);
    };
    t[219] = [](Ctx& c) {              // GetJavaVM
        mem::w32(c.arg(1), g_vm);
        c.ret(0);
    };
    t[220] = [](Ctx& c) {  // GetStringRegion
        auto u = utf8_to_utf16(string_value(c.arg(1)));
        u32 start = c.arg(2), len = c.arg(3);
        if (start + len > u.size()) return throw_new("java/lang/StringIndexOutOfBoundsException", "");
        std::memcpy(mem::host(c.arg(4)), u.data() + start, size_t(len) * 2);
    };
    t[221] = [](Ctx& c) {  // GetStringUTFRegion
        auto u = utf8_to_utf16(string_value(c.arg(1)));
        u32 start = c.arg(2), len = c.arg(3);
        if (start + len > u.size()) return throw_new("java/lang/StringIndexOutOfBoundsException", "");
        std::string s = utf16_to_utf8(u.data() + start, len);
        std::memcpy(mem::host(c.arg(4)), s.c_str(), s.size() + 1);
    };
    t[222] = [](Ctx& c) {  // GetPrimitiveArrayCritical
        ObjPtr a = get(c.arg(1));
        if (c.arg(2)) mem::w8(c.arg(2), 0);
        c.ret(a ? a->data : 0);
    };
    t[223] = [](Ctx& c) {};  // ReleasePrimitiveArrayCritical
    t[224] = t[165];         // GetStringCritical
    t[225] = t[166];         // ReleaseStringCritical
    t[226] = t[21];          // NewWeakGlobalRef
    t[227] = t[22];          // DeleteWeakGlobalRef
    t[228] = [](Ctx& c) { c.ret(t_jni.exception ? 1 : 0); };  // ExceptionCheck
    t[229] = [](Ctx& c) {                                    // NewDirectByteBuffer
        auto o = make("java/nio/DirectByteBuffer");
        o->elem = 'B';
        o->data = c.arg(1);
        o->len = u32(c.arg64(2));
        o->owns_data = false;
        c.ret(new_local(o));
    };
    t[230] = [](Ctx& c) {  // GetDirectBufferAddress
        ObjPtr o = get(c.arg(1));
        c.ret(o ? o->data : 0);
    };
    t[231] = [](Ctx& c) {  // GetDirectBufferCapacity
        ObjPtr o = get(c.arg(1));
        c.ret64(o ? o->len : u64(-1));
    };
    t[232] = [](Ctx& c) { c.ret((c.arg(1) & 3) == 2 ? 2 : 1); };  // GetObjectRefType

    u32 table = mem::data_alloc(233 * 4);
    for (int i = 4; i < 233; i++) mem::w32(table + 4 * i, cpu::register_hle("JNIEnv#" + std::to_string(i), t[i]));
    g_env = mem::data_alloc(4);
    mem::w32(g_env, table);

    // JavaVM
    u32 vt = mem::data_alloc(8 * 4);
    mem::w32(vt + 3 * 4, cpu::register_hle("JavaVM::DestroyJavaVM", [](Ctx& c) { c.ret(0); }));
    mem::w32(vt + 4 * 4, cpu::register_hle("JavaVM::AttachCurrentThread", [](Ctx& c) {
                 if (c.arg(1)) mem::w32(c.arg(1), g_env);
                 c.ret(0);
             }));
    mem::w32(vt + 5 * 4, cpu::register_hle("JavaVM::DetachCurrentThread", [](Ctx& c) { c.ret(0); }));
    mem::w32(vt + 6 * 4, cpu::register_hle("JavaVM::GetEnv", [](Ctx& c) {
                 if (c.arg(1)) mem::w32(c.arg(1), g_env);
                 c.ret(0);
             }));
    mem::w32(vt + 7 * 4, mem::r32(vt + 4 * 4));
    g_vm = mem::data_alloc(4);
    mem::w32(g_vm, vt);

    register_java_classes();
}

}  // namespace jni
