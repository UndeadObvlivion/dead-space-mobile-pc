#include "cpu.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "log.h"

namespace cpu {

namespace {

constexpr u32 SVC_RETURN = 0xFFFFFF;
constexpr u32 RETURN_STUB = mem::STUB_BASE;  // first stub: return to host

struct HleEntry {
    std::string name;
    HleFn fn;
};
std::mutex g_hle_lock;
std::vector<HleEntry> g_hle;  // index = SVC number

std::atomic<u32> g_next_tid{1000};
std::atomic<bool> g_gl_trace_on{false};  // debug GL call trace

}  // namespace

class Cpu final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Cpu(size_t cache_size) {
        Dynarmic::A32::UserConfig cfg{};
        cfg.callbacks = this;
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
        cfg.page_table = mem::g_page_table;
        cfg.absolute_offset_page_table = false;
        cfg.enable_cycle_counting = false;
        cfg.code_cache_size = cache_size;
        cfg.define_unpredictable_behaviour = true;
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
    }

    std::unique_ptr<Dynarmic::A32::Jit> jit;
    bool running = false;

    void fault(const char* what, u32 addr) {
        LOGE("guest %s fault at %08x (pc~%08x %s)", what, addr, jit->Regs()[15],
             symbolize(jit->Regs()[15]).c_str());
        Ctx c{this};
        dump_regs(c);
        backtrace(&c);
        std::fflush(stdout);
        std::abort();
    }

    std::uint8_t MemoryRead8(u32 a) override {
        if (!mem::is_mapped(a)) fault("read8", a);
        return mem::r8(a);
    }
    std::uint16_t MemoryRead16(u32 a) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 1)) fault("read16", a);
        return mem::r16(a);
    }
    std::uint32_t MemoryRead32(u32 a) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 3)) fault("read32", a);
        return mem::r32(a);
    }
    std::uint64_t MemoryRead64(u32 a) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 7)) fault("read64", a);
        u64 v;
        std::memcpy(&v, mem::host(a), 8);
        return v;
    }
    void MemoryWrite8(u32 a, std::uint8_t v) override {
        if (!mem::is_mapped(a)) fault("write8", a);
        mem::w8(a, v);
    }
    void MemoryWrite16(u32 a, std::uint16_t v) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 1)) fault("write16", a);
        mem::w16(a, v);
    }
    void MemoryWrite32(u32 a, std::uint32_t v) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 3)) fault("write32", a);
        mem::w32(a, v);
    }
    void MemoryWrite64(u32 a, std::uint64_t v) override {
        if (!mem::is_mapped(a) || !mem::is_mapped(a + 7)) fault("write64", a);
        std::memcpy(mem::host(a), &v, 8);
    }
    bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t e) override {
        return __atomic_compare_exchange_n(mem::ptr<u8>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t e) override {
        return __atomic_compare_exchange_n(mem::ptr<u16>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t e) override {
        return __atomic_compare_exchange_n(mem::ptr<u32>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t e) override {
        return __atomic_compare_exchange_n(mem::ptr<u64>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    void InterpreterFallback(u32 pc, size_t n) override {
        LOGE("interpreter fallback requested at %08x (%zu insns) %s", pc, n, symbolize(pc).c_str());
        std::abort();
    }

    void CallSVC(std::uint32_t swi) override {
        if (swi == SVC_RETURN) {
            running = false;
            jit->HaltExecution();
            return;
        }
        HleFn fn = nullptr;
        {
            std::lock_guard lk(g_hle_lock);
            if (swi < g_hle.size()) fn = g_hle[swi].fn;
        }
        if (!fn) {
            LOGE("bad SVC %06x at %08x", swi, jit->Regs()[15]);
            std::abort();
        }
        Ctx c{this};
        if (g_gl_trace_on) {
            const std::string& n = g_hle[swi].name;
            if (n.size() > 2 && n[0] == 'g' && n[1] == 'l') {
                auto& r = jit->Regs();
                LOGI("[gltrace] %s(%08x, %08x, %08x, %08x | %08x %08x %08x %08x)", n.c_str(), r[0], r[1], r[2], r[3],
                     mem::r32(r[13]), mem::r32(r[13] + 4), mem::r32(r[13] + 8), mem::r32(r[13] + 12));
            }
        }
        fn(c);
    }

    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        switch (e) {
        case E::Yield:
        case E::SendEvent:
        case E::SendEventLocal:
        case E::WaitForEvent:
        case E::WaitForInterrupt:
        case E::PreloadData:
        case E::PreloadDataWithIntentToWrite:
        case E::PreloadInstruction:
            return;  // hints
        default:
            break;
        }
        LOGE("guest exception %d at %08x %s", int(e), pc, symbolize(pc).c_str());
        Ctx c{this};
        dump_regs(c);
        backtrace(&c);
        std::fflush(stdout);
        std::abort();
    }

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 40; }
};

u32 Ctx::reg(int i) const { return c->jit->Regs()[i]; }
void Ctx::set_reg(int i, u32 v) { c->jit->Regs()[i] = v; }
u32 Ctx::ext(int i) const { return c->jit->ExtRegs()[i]; }
void Ctx::set_ext(int i, u32 v) { c->jit->ExtRegs()[i] = v; }
u32 Ctx::fpscr() const { return c->jit->Fpscr(); }
void Ctx::set_fpscr(u32 v) { c->jit->SetFpscr(v); }

// ---------------------------------------------------------------- thread state
namespace {
struct TState {
    ThreadInfo info;
    std::vector<std::unique_ptr<Cpu>> cpus;
    int depth = 0;
    bool attached = false;
};
thread_local TState t_state;
}  // namespace

ThreadInfo& thread() { return t_state.info; }

void thread_attach(u32 stack_base, u32 stack_size) {
    TState& t = t_state;
    if (t.attached) return;
    t.attached = true;
    t.info.tid = g_next_tid++;
    if (!stack_base) {
        stack_size = 8 << 20;
        stack_base = mem::map_alloc(stack_size);
        t.info.own_stack = true;
    }
    t.info.stack_base = stack_base;
    t.info.stack_size = stack_size;
    t.info.errno_addr = mem::heap_alloc(16);
    t.info.tls_area = mem::heap_alloc(256);
    std::memset(mem::host(t.info.tls_area), 0, 256);
    mem::w32(t.info.tls_area + 4, t.info.tid);
    mem::w32(t.info.tls_area + 8, t.info.errno_addr);
}

void thread_detach() {
    TState& t = t_state;
    if (!t.attached) return;
    t.cpus.clear();
    if (t.info.own_stack) mem::map_free(t.info.stack_base, t.info.stack_size);
    mem::heap_free(t.info.errno_addr);
    mem::heap_free(t.info.tls_area);
    t.attached = false;
}

u32 current_pc() {
    TState& t = t_state;
    if (t.depth == 0) return 0;
    return t.cpus[t.depth - 1]->jit->Regs()[15];
}

// ---------------------------------------------------------------- calls into the guest
u32 call_v(u32 fn, const u32* args, int nargs, u32* r1_out) {
    TState& t = t_state;
    if (!t.attached) thread_attach();
    int d = t.depth;
    if (int(t.cpus.size()) <= d) t.cpus.emplace_back(std::make_unique<Cpu>(d == 0 ? (64u << 20) : (16u << 20)));
    Cpu* c = t.cpus[d].get();

    u32 sp = d == 0 ? t.info.stack_base + t.info.stack_size : t.cpus[d - 1]->jit->Regs()[13];
    sp -= 64;
    int nstack = nargs > 4 ? nargs - 4 : 0;
    sp -= 4 * nstack;
    sp &= ~7u;
    for (int i = 4; i < nargs; i++) mem::w32(sp + 4 * (i - 4), args[i]);

    auto& R = c->jit->Regs();
    for (int i = 0; i < 4; i++) R[i] = i < nargs ? args[i] : 0;
    R[13] = sp;
    R[14] = RETURN_STUB;
    R[15] = fn & ~1u;
    c->jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));

    t.depth++;
    c->running = true;
    while (c->running) c->jit->Run();
    t.depth--;
    if (r1_out) *r1_out = R[1];
    return R[0];
}

u32 call(u32 fn, std::initializer_list<u32> args) { return call_v(fn, args.begin(), int(args.size())); }

u64 call64(u32 fn, std::initializer_list<u32> args) {
    u32 hi = 0;
    u32 lo = call_v(fn, args.begin(), int(args.size()), &hi);
    return u64(lo) | (u64(hi) << 32);
}

// ---------------------------------------------------------------- HLE registry
static void write_stub(u32 idx) {
    u32 a = mem::STUB_BASE + idx * 8;
    mem::w32(a, 0xEF000000 | (idx == 0 ? SVC_RETURN : idx));  // SVC #idx
    mem::w32(a + 4, 0xE12FFF1E);                               // BX LR
}

void set_gl_trace(bool on) { g_gl_trace_on = on; }

u32 register_hle(const std::string& name, HleFn fn) {
    std::lock_guard lk(g_hle_lock);
    u32 idx = u32(g_hle.size());
    if ((idx + 1) * 8 > mem::STUB_SIZE) {
        LOGE("out of HLE stub space");
        std::abort();
    }
    g_hle.push_back({name, fn});
    write_stub(idx);
    return mem::STUB_BASE + idx * 8;
}

std::string hle_name_at(u32 addr) {
    if (addr < mem::STUB_BASE || addr >= mem::STUB_BASE + mem::STUB_SIZE) return "";
    u32 idx = (addr - mem::STUB_BASE) / 8;
    std::lock_guard lk(g_hle_lock);
    return idx < g_hle.size() ? g_hle[idx].name : "";
}

// ---------------------------------------------------------------- function entry hooks
static bool pc_relative(u32 insn) {
    if ((insn >> 28) == 0xF) return true;               // unconditional space
    if (((insn >> 25) & 7) == 5) return true;            // B / BL
    if (((insn >> 16) & 15) == 15) return true;          // Rn == PC
    if (((insn >> 25) & 7) == 4) return insn & 0x8000;   // LDM/STM: register list (PC in list)
    if (((insn >> 12) & 15) == 15) return true;          // Rd == PC
    if (((insn >> 25) & 7) == 0 && (insn & 15) == 15) return true;  // Rm == PC
    return false;
}

std::string current_hle_name(Ctx& c) {
    u32 svc = mem::r32(c.reg(15) - 4) & 0xFFFFFF;
    std::lock_guard lk(g_hle_lock);
    return svc < g_hle.size() ? g_hle[svc].name : "?";
}

void skip_function(Ctx& c, u32 ret) {
    c.set_reg(0, ret);
    c.set_reg(12, 1);
}

bool hook_entry(u32 addr, const std::string& name, HleFn fn) {
    if (addr & 1) {
        LOGE("hook %s: Thumb functions are not supported", name.c_str());
        return false;
    }
    u32 i0 = mem::r32(addr), i1 = mem::r32(addr + 4);
    if (pc_relative(i0) || pc_relative(i1)) {
        LOGE("hook %s: entry instructions are PC-relative (%08x %08x)", name.c_str(), i0, i1);
        return false;
    }
    u32 svc_stub = register_hle("hook:" + name, fn);
    u32 tramp = mem::data_alloc(32, 4);
    // IP (r12) is a scratch register at function entry; the handler sets it to return early
    mem::w32(tramp, 0xE3A0C000);          // MOV IP, #0
    mem::w32(tramp + 4, mem::r32(svc_stub));  // SVC #n (runs the host handler)
    mem::w32(tramp + 8, 0xE35C0000);      // CMP IP, #0
    mem::w32(tramp + 12, 0x112FFF1E);     // BXNE LR (skip_function)
    mem::w32(tramp + 16, i0);             // displaced instructions
    mem::w32(tramp + 20, i1);
    mem::w32(tramp + 24, 0xE51FF004);     // LDR PC, [PC, #-4]
    mem::w32(tramp + 28, addr + 8);
    mem::w32(addr, 0xE51FF004);           // LDR PC, [PC, #-4]
    mem::w32(addr + 4, tramp);
    return true;
}

// ---------------------------------------------------------------- kuser helpers
static void kuser_cmpxchg(Ctx& c) {
    u32 oldv = c.reg(0), newv = c.reg(1), ptr = c.reg(2);
    bool ok = __atomic_compare_exchange_n(mem::ptr<u32>(ptr), &oldv, newv, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST);
    c.ret(ok ? 0 : 1);
    u32 cpsr = c.c->jit->Cpsr();
    c.c->jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
}

static void kuser_cmpxchg64(Ctx& c) {
    u64 oldv, newv;
    std::memcpy(&oldv, mem::host(c.reg(0)), 8);
    std::memcpy(&newv, mem::host(c.reg(1)), 8);
    bool ok = __atomic_compare_exchange_n(mem::ptr<u64>(c.reg(2)), &oldv, newv, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST);
    c.ret(ok ? 0 : 1);
    u32 cpsr = c.c->jit->Cpsr();
    c.c->jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
}

static void kuser_get_tls(Ctx& c) { c.ret(thread().tls_area); }

static void write_kuser(u32 addr, u32 svc_stub) {
    // copy "SVC #n; BX LR" from the registered stub
    mem::w32(addr, mem::r32(svc_stub));
    mem::w32(addr + 4, 0xE12FFF1E);
}

bool init() {
    register_hle("<return>", nullptr);  // index 0 = return to host (SVC_RETURN)
    mem::commit(mem::KUSER_PAGE, mem::PAGE_SIZE);
    std::memset(mem::host(mem::KUSER_PAGE), 0, mem::PAGE_SIZE);
    write_kuser(0xFFFF0FC0, register_hle("__kuser_cmpxchg", kuser_cmpxchg));
    write_kuser(0xFFFF0F60, register_hle("__kuser_cmpxchg64", kuser_cmpxchg64));
    write_kuser(0xFFFF0FE0, register_hle("__kuser_get_tls", kuser_get_tls));
    mem::w32(0xFFFF0FA0, 0xE12FFF1E);  // __kuser_memory_barrier: BX LR
    mem::w32(0xFFFF0FFC, 3);           // __kuser_helper_version
    thread_attach();
    return true;
}

// ---------------------------------------------------------------- diagnostics
void dump_regs(Ctx& c) {
    for (int i = 0; i < 16; i += 4)
        LOGE("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, c.reg(i), i + 1, c.reg(i + 1), i + 2,
             c.reg(i + 2), i + 3, c.reg(i + 3));
    LOGE("  cpsr %08x  pc %s  lr %s", c.c->jit->Cpsr(), symbolize(c.reg(15)).c_str(), symbolize(c.reg(14)).c_str());
}

void backtrace(Ctx* c) {
    u32 sp = c->sp();
    LOGE("  stack scan from sp=%08x:", sp);
    int found = 0;
    for (u32 a = sp; a < sp + 4096 && found < 24; a += 4) {
        if (!mem::is_mapped(a)) break;
        u32 v = mem::r32(a);
        if (v >= mem::ELF_BASE && v < mem::ELF_BASE + 0x480000) {
            std::string s = symbolize(v);
            if (!s.empty() && s[0] != '?') {
                LOGE("    [sp+%03x] %08x %s", a - sp, v, s.c_str());
                found++;
            }
        }
    }
}

}  // namespace cpu
