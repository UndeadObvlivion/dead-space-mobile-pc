// Guest (32-bit ARM) address space.
//
// The whole 4 GiB guest space is reserved as one contiguous host region, so a guest
// address converts to a host pointer by adding g_base. Pages are committed on demand.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

namespace mem {

constexpr u32 PAGE_SIZE = 0x1000;
constexpr u32 PAGE_BITS = 12;

// Address map
constexpr u32 STUB_BASE = 0x00100000;  // HLE call stubs (SVC trampolines)
constexpr u32 STUB_SIZE = 0x00100000;
constexpr u32 DATA_BASE = 0x00200000;  // HLE data objects (__sF, _ctype_, JNI tables, ...)
constexpr u32 DATA_SIZE = 0x00E00000;
constexpr u32 ELF_BASE = 0x10000000;   // game library
constexpr u32 HEAP_BASE = 0x20000000;  // guest malloc heap
constexpr u32 HEAP_END = 0xA0000000;
constexpr u32 MAP_BASE = 0xA0000000;   // mmap / thread stacks
constexpr u32 MAP_END = 0xF0000000;
constexpr u32 KUSER_PAGE = 0xFFFF0000; // Linux ARM kernel user helpers

extern u8* g_base;
extern std::array<u8*, (1u << 20)>* g_page_table;

bool init();
void commit(u32 addr, u32 size);
bool is_mapped(u32 addr);

inline u8* host(u32 addr) { return g_base + addr; }
template <class T> inline T* ptr(u32 addr) { return reinterpret_cast<T*>(g_base + addr); }
inline u32 guest(const void* p) { return static_cast<u32>(static_cast<const u8*>(p) - g_base); }

inline u32 r32(u32 a) { u32 v; std::memcpy(&v, host(a), 4); return v; }
inline u16 r16(u32 a) { u16 v; std::memcpy(&v, host(a), 2); return v; }
inline u8 r8(u32 a) { return *host(a); }
inline void w32(u32 a, u32 v) { std::memcpy(host(a), &v, 4); }
inline void w16(u32 a, u16 v) { std::memcpy(host(a), &v, 2); }
inline void w8(u32 a, u8 v) { *host(a) = v; }
inline const char* cstr(u32 a) { return a ? ptr<const char>(a) : nullptr; }
inline std::string str(u32 a) { return a ? std::string(ptr<const char>(a)) : std::string(); }

// Bump allocator for permanent HLE data (never freed).
u32 data_alloc(u32 size, u32 align = 8);
u32 data_strdup(const std::string& s);

// Guest heap (malloc/free). Thread safe.
u32 heap_alloc(u32 size, u32 align = 8);
void heap_free(u32 addr);
u32 heap_realloc(u32 addr, u32 size);
u32 heap_size(u32 addr);
u32 heap_strdup(const char* s);

// Page-granular regions (mmap, stacks).
u32 map_alloc(u32 size);
void map_free(u32 addr, u32 size);

}  // namespace mem
