// Loader for the 32-bit ARM game library (ELF shared object).
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "memory.h"

namespace elf {

struct Symbol {
    u32 addr;
    u32 size;
    std::string name;
};

struct Module {
    std::string path;
    u32 base = 0;
    u32 span = 0;
    u32 exidx = 0, exidx_count = 0;  // .ARM.exidx (guest address, entry count)
    std::vector<u32> init_array;
    std::unordered_map<std::string, u32> exports;  // dynamic symbols defined here
    std::vector<Symbol> symbols;                    // full .symtab (sorted, for symbolizing)
    std::unordered_map<std::string, u32> by_name;   // .symtab name -> address
};

// Resolves an undefined dynamic symbol to a guest address (0 = leave unresolved).
using Resolver = std::function<u32(const std::string& name, bool is_function, bool is_weak)>;

bool load(const std::string& path, u32 base, Module& m, const Resolver& resolve);
void run_init_array(const Module& m);

extern Module g_game;

// Address of a symbol in the game (exports first, then .symtab); 0 if missing.
u32 find(const std::string& name);

}  // namespace elf
