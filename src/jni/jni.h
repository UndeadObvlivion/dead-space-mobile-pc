// A minimal Java VM for the game's JNI calls: guest JNIEnv/JavaVM function tables backed by
// host-side objects, host implementations of the Java methods the game calls, and calls into
// the game's own native (Java_*) methods.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "memory.h"

namespace jni {

using jref = u32;  // guest-visible reference handle (0 = null)

struct JValue {
    u64 raw = 0;
    static JValue I(s32 v) { JValue j; j.raw = u64(s64(v)); return j; }
    static JValue J(s64 v) { JValue j; j.raw = u64(v); return j; }
    static JValue Z(bool v) { JValue j; j.raw = v ? 1 : 0; return j; }
    static JValue F(float v) { JValue j; u32 b; std::memcpy(&b, &v, 4); j.raw = b; return j; }
    static JValue D(double v) { JValue j; std::memcpy(&j.raw, &v, 8); return j; }
    static JValue L(jref r) { JValue j; j.raw = r; return j; }
    s32 i() const { return s32(raw); }
    s64 j() const { return s64(raw); }
    bool z() const { return (raw & 0xFF) != 0; }
    float f() const { u32 b = u32(raw); float v; std::memcpy(&v, &b, 4); return v; }
    double d() const { double v; std::memcpy(&v, &raw, 8); return v; }
    jref l() const { return jref(raw); }
};

struct Object;
using ObjPtr = std::shared_ptr<Object>;

struct Object {
    std::string cls;   // binary class name, e.g. "java/lang/String"
    std::string str;   // java.lang.String value (UTF-8); class name for java.lang.Class objects
    // arrays
    char elem = 0;     // element type for arrays ('B','S','I','F','Z','C','J','D','L'), 0 = not array
    u32 len = 0;
    u32 data = 0;      // guest memory holding primitive array elements
    bool owns_data = true;
    std::vector<ObjPtr> objs;  // object array elements
    std::map<std::string, JValue> fields;
    std::shared_ptr<void> native;  // host payload
    std::recursive_mutex monitor;  // Java "synchronized" (JNI MonitorEnter/MonitorExit)
    ~Object();
};

// Reference management
jref new_local(const ObjPtr& o);
jref new_global(const ObjPtr& o);
ObjPtr get(jref r);
void delete_ref(jref r);

ObjPtr make(const std::string& cls);
ObjPtr make_string(const std::string& s);
ObjPtr make_array(char elem, u32 len);
ObjPtr class_object(const std::string& name);
std::string string_value(jref r);

// Pending Java exception on this thread
void throw_new(const std::string& cls, const std::string& msg);

// Host Java method implementations. sig is the JNI signature the caller used.
using JavaFn = std::function<JValue(const ObjPtr& thiz, const std::vector<JValue>& args, const std::string& sig)>;
void def(const std::string& cls, const std::string& name, JavaFn fn);
void def_superclass(const std::string& cls, const std::string& super);

// Guest pointers to the JNIEnv / JavaVM
u32 env();
u32 vm();

void init();
void register_java_classes();  // java_classes.cpp

// Calls a native method implemented by the game (Java_* export or RegisterNatives).
// thiz is the object (instance methods) or class (static methods). Runs inside a local frame.
JValue call_native(const std::string& cls, const std::string& name, const std::string& sig, const ObjPtr& thiz,
                   const std::vector<JValue>& args);
bool has_native(const std::string& cls, const std::string& name);

}  // namespace jni
