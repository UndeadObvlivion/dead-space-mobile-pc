// ARM32 guest CPU (dynarmic) and the host<->guest call bridge.
//
// Every imported function is an 8-byte ARM stub "SVC #n; BX LR" in guest memory. When the
// guest executes it, the SVC callback runs HLE handler n on the host with access to the
// guest registers. Host code calls into the guest with cpu::call(); nested calls (guest ->
// host -> guest) use a separate JIT instance per nesting depth, because a dynarmic JIT
// cannot be re-entered from inside its own callbacks.
#pragma once
#include <cstring>
#include <initializer_list>
#include <string>

#include "memory.h"

namespace cpu {

class Cpu;

struct Ctx {
    Cpu* c;

    u32 reg(int i) const;
    void set_reg(int i, u32 v);
    u32 sp() const { return reg(13); }
    u32 lr() const { return reg(14); }
    u32 ext(int i) const;  // VFP single-precision register file s0..s63
    void set_ext(int i, u32 v);
    u32 fpscr() const;
    void set_fpscr(u32 v);

    // AAPCS argument slots: 0-3 are r0-r3, then the caller's stack.
    u32 arg(int slot) const { return slot < 4 ? reg(slot) : mem::r32(sp() + 4 * (slot - 4)); }
    u64 arg64(int slot) const { return u64(arg(slot)) | (u64(arg(slot + 1)) << 32); }
    float argf(int slot) const { u32 v = arg(slot); float f; std::memcpy(&f, &v, 4); return f; }
    double argd(int slot) const { u64 v = arg64(slot); double d; std::memcpy(&d, &v, 8); return d; }
    const char* argstr(int slot) const { return mem::cstr(arg(slot)); }
    template <class T> T* argp(int slot) const { u32 a = arg(slot); return a ? mem::ptr<T>(a) : nullptr; }

    void ret(u32 v) { set_reg(0, v); }
    void ret64(u64 v) { set_reg(0, u32(v)); set_reg(1, u32(v >> 32)); }
    void retf(float f) { u32 v; std::memcpy(&v, &f, 4); ret(v); }
    void retd(double d) { u64 v; std::memcpy(&v, &d, 8); ret64(v); }
};

using HleFn = void (*)(Ctx&);

// Registers a host function and returns the guest address of its stub.
u32 register_hle(const std::string& name, HleFn fn);
// Name of the HLE function whose stub lives at addr ("" if none).
std::string hle_name_at(u32 addr);

bool init();

// Call a guest function. fn may have the Thumb bit set. Up to any number of 32-bit args
// (beyond four go on the stack). Returns r0 (call64 returns r1:r0).
u32 call(u32 fn, std::initializer_list<u32> args = {});
u64 call64(u32 fn, std::initializer_list<u32> args = {});
u32 call_v(u32 fn, const u32* args, int nargs, u32* r1_out = nullptr);

// Per host thread guest state. thread_attach() must run on each host thread before it
// calls into the guest (the main thread is attached by init()).
struct ThreadInfo {
    u32 tid = 0;
    u32 stack_base = 0, stack_size = 0;
    u32 errno_addr = 0;
    u32 tls_area = 0;  // returned by the kuser get_tls helper
    bool own_stack = false;
};
ThreadInfo& thread();
void thread_attach(u32 stack_base = 0, u32 stack_size = 0);
void thread_detach();

// Runs `fn` on entry to the ARM-mode guest function at addr, before its first instruction (the
// handler sees the call's arguments in r0-r3/stack and may call into the guest). The function
// then continues normally unless the handler calls skip_function. Must be installed before the
// function first executes.
bool hook_entry(u32 addr, const std::string& name, HleFn fn);
// Inside a hook_entry handler: return `ret` to the caller without running the hooked function.
void skip_function(Ctx& c, u32 ret);
// Debug: log every GL entry point call (name + first 8 argument words) while on.
void set_gl_trace(bool on);
// Inside an HLE/hook handler: the name it was registered with.
std::string current_hle_name(Ctx& c);

// Debug helpers
std::string symbolize(u32 addr);  // provided by the ELF loader
void dump_regs(Ctx& c);
// Current guest PC of the innermost running JIT on this thread (for diagnostics).
u32 current_pc();
// Unwind the guest stack a little (frame-pointer-less heuristic: scan for code addresses).
void backtrace(Ctx* c);

}  // namespace cpu
