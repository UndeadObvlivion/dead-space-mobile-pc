#include "elf_loader.h"

#include <algorithm>
#include <cstdio>
#include <cxxabi.h>
#include <fstream>

#include "cpu.h"
#include "log.h"

namespace elf {

Module g_game;

namespace {

#pragma pack(push, 1)
struct Ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Shdr {
    u32 name, type, flags, addr, offset, size, link, info, addralign, entsize;
};
struct Sym {
    u32 name, value, size;
    u8 info, other;
    u16 shndx;
};
struct Rel {
    u32 offset, info;
};
#pragma pack(pop)

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_ARM_EXIDX = 0x70000001 };
enum {
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_REL = 17,
    DT_RELSZ = 18, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27
};
enum { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };
enum { SHT_SYMTAB = 2 };

std::string demangle(const std::string& s) {
    int st = 0;
    char* d = abi::__cxa_demangle(s.c_str(), nullptr, nullptr, &st);
    if (st != 0 || !d) return s;
    std::string r(d);
    std::free(d);
    return r;
}

}  // namespace

bool load(const std::string& path, u32 base, Module& m, const Resolver& resolve) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOGE("cannot open %s", path.c_str());
        return false;
    }
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < sizeof(Ehdr)) return false;
    auto* eh = reinterpret_cast<const Ehdr*>(file.data());
    if (std::memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 1 || eh->machine != 40) {
        LOGE("%s is not a 32-bit ARM ELF", path.c_str());
        return false;
    }
    m.path = path;
    m.base = base;
    auto* ph = reinterpret_cast<const Phdr*>(file.data() + eh->phoff);

    u32 span = 0, dyn = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) span = std::max(span, ph[i].vaddr + ph[i].memsz);
        if (ph[i].type == PT_DYNAMIC) dyn = ph[i].vaddr;
        if (ph[i].type == PT_ARM_EXIDX) {
            m.exidx = base + ph[i].vaddr;
            m.exidx_count = ph[i].memsz / 8;
        }
    }
    span = (span + mem::PAGE_SIZE - 1) & ~(mem::PAGE_SIZE - 1);
    m.span = span;
    mem::commit(base, span);
    std::memset(mem::host(base), 0, span);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD)
            std::memcpy(mem::host(base + ph[i].vaddr), file.data() + ph[i].offset, ph[i].filesz);

    // dynamic section
    u32 strtab = 0, symtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, init_arr = 0, init_arrsz = 0;
    for (u32 d = base + dyn;; d += 8) {
        u32 tag = mem::r32(d), val = mem::r32(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_STRTAB: strtab = base + val; break;
        case DT_SYMTAB: symtab = base + val; break;
        case DT_REL: rel = base + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT_ARRAY: init_arr = base + val; break;
        case DT_INIT_ARRAYSZ: init_arrsz = val; break;
        default: break;
        }
    }

    // dynamic symbol count: highest index referenced by relocations, plus a scan of exports
    // up to strtab (the dynsym table directly precedes .dynstr in this binary).
    u32 nsyms = (strtab - symtab) / sizeof(Sym);
    for (u32 i = 1; i < nsyms; i++) {
        auto* s = mem::ptr<Sym>(symtab + i * sizeof(Sym));
        if (s->shndx != 0 && s->name) m.exports[mem::cstr(strtab + s->name)] = base + s->value;
    }

    int unresolved = 0;
    auto sym_addr = [&](u32 idx) -> u32 {
        auto* s = mem::ptr<Sym>(symtab + idx * sizeof(Sym));
        std::string name = mem::cstr(strtab + s->name);
        if (s->shndx != 0) return base + s->value;  // DT_SYMBOLIC: own definition first
        bool is_func = (s->info & 0xF) == 2;
        bool weak = (s->info >> 4) == 2;
        u32 a = resolve(name, is_func, weak);
        if (!a && !weak) {
            LOGW("unresolved symbol %s", name.c_str());
            unresolved++;
        }
        return a;
    };

    auto apply = [&](u32 table, u32 size) {
        for (u32 r = table; r < table + size; r += sizeof(Rel)) {
            auto* re = mem::ptr<Rel>(r);
            u32 where = base + re->offset;
            u32 type = re->info & 0xFF, si = re->info >> 8;
            switch (type) {
            case R_ARM_RELATIVE: mem::w32(where, mem::r32(where) + base); break;
            case R_ARM_ABS32: mem::w32(where, mem::r32(where) + sym_addr(si)); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: mem::w32(where, sym_addr(si)); break;
            default: LOGE("unsupported relocation type %u at %08x", type, where); break;
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);

    for (u32 i = 0; i < init_arrsz / 4; i++) m.init_array.push_back(mem::r32(init_arr + 4 * i));

    // full symbol table (debug info) from the section headers
    auto* sh = reinterpret_cast<const Shdr*>(file.data() + eh->shoff);
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].type != SHT_SYMTAB) continue;
        const Shdr& strs = sh[sh[i].link];
        auto* syms = reinterpret_cast<const Sym*>(file.data() + sh[i].offset);
        u32 n = sh[i].size / sizeof(Sym);
        for (u32 k = 0; k < n; k++) {
            const Sym& s = syms[k];
            u32 t = s.info & 0xF;
            if (s.shndx == 0 || !s.name || (t != 2 && t != 1)) continue;
            std::string name(reinterpret_cast<const char*>(file.data() + strs.offset + s.name));
            if (name.empty() || name[0] == '$') continue;
            u32 a = base + (t == 2 ? (s.value & ~1u) : s.value);
            m.symbols.push_back({a, s.size, name});
            m.by_name[name] = base + s.value;
        }
    }
    std::sort(m.symbols.begin(), m.symbols.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
    LOGI("loaded %s at %08x (span %x, %zu exports, %zu symbols, %d unresolved)", path.c_str(), base, span,
         m.exports.size(), m.symbols.size(), unresolved);
    return true;
}

void run_init_array(const Module& m) {
    for (u32 fn : m.init_array) {
        if (fn == 0 || fn == 0xFFFFFFFF) continue;
        cpu::call(fn);
    }
}

u32 find(const std::string& name) {
    auto it = g_game.exports.find(name);
    if (it != g_game.exports.end()) return it->second;
    auto it2 = g_game.by_name.find(name);
    return it2 == g_game.by_name.end() ? 0 : it2->second;
}

}  // namespace elf

namespace cpu {
std::string symbolize(u32 addr) {
    std::string hle = hle_name_at(addr & ~1u);
    if (!hle.empty()) return "<hle " + hle + ">";
    const auto& syms = elf::g_game.symbols;
    auto it = std::upper_bound(syms.begin(), syms.end(), addr & ~1u,
                               [](u32 a, const elf::Symbol& s) { return a < s.addr; });
    if (it == syms.begin()) return "?";
    --it;
    u32 off = (addr & ~1u) - it->addr;
    if (it->size && off >= it->size + 64) return "?";
    char buf[32];
    std::snprintf(buf, sizeof buf, "+0x%x", off);
    return elf::demangle(it->name) + buf;
}
}  // namespace cpu
