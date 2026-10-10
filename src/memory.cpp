#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace mem {

u8* g_base = nullptr;
std::array<u8*, (1u << 20)>* g_page_table = nullptr;

static std::mutex g_commit_lock;

bool init() {
#ifdef _WIN32
    g_base = static_cast<u8*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_base = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
    if (!g_base) {
        std::fprintf(stderr, "failed to reserve 4 GiB guest address space\n");
        return false;
    }
    g_page_table = new std::array<u8*, (1u << 20)>();
    g_page_table->fill(nullptr);
    commit(STUB_BASE, STUB_SIZE);
    commit(DATA_BASE, DATA_SIZE);
    return true;
}

void commit(u32 addr, u32 size) {
    u32 start = addr & ~(PAGE_SIZE - 1);
    u64 end = (u64(addr) + size + PAGE_SIZE - 1) & ~u64(PAGE_SIZE - 1);
    std::lock_guard lk(g_commit_lock);
#ifdef _WIN32
    if (!VirtualAlloc(g_base + start, size_t(end - start), MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "commit failed %08x+%x\n", start, unsigned(end - start));
        std::abort();
    }
#else
    if (mprotect(g_base + start, size_t(end - start), PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "commit failed %08x+%x\n", start, unsigned(end - start));
        std::abort();
    }
#endif
    for (u64 a = start; a < end; a += PAGE_SIZE) (*g_page_table)[a >> PAGE_BITS] = g_base + a;
}

static void decommit(u32 addr, u32 size) {
    std::lock_guard lk(g_commit_lock);
    for (u64 a = addr; a < u64(addr) + size; a += PAGE_SIZE) (*g_page_table)[a >> PAGE_BITS] = nullptr;
#ifdef _WIN32
    VirtualFree(g_base + addr, size, MEM_DECOMMIT);
#else
    madvise(g_base + addr, size, MADV_DONTNEED);
    mprotect(g_base + addr, size, PROT_NONE);
#endif
}

bool is_mapped(u32 addr) { return (*g_page_table)[addr >> PAGE_BITS] != nullptr; }

// ---------------------------------------------------------------- permanent data
static std::mutex g_data_lock;
static u32 g_data_top = DATA_BASE;

u32 data_alloc(u32 size, u32 align) {
    std::lock_guard lk(g_data_lock);
    g_data_top = (g_data_top + align - 1) & ~(align - 1);
    u32 a = g_data_top;
    g_data_top += size;
    if (g_data_top > DATA_BASE + DATA_SIZE) {
        std::fprintf(stderr, "HLE data region exhausted\n");
        std::abort();
    }
    std::memset(host(a), 0, size);
    return a;
}

u32 data_strdup(const std::string& s) {
    u32 a = data_alloc(u32(s.size() + 1), 1);
    std::memcpy(host(a), s.c_str(), s.size() + 1);
    return a;
}

// ---------------------------------------------------------------- heap
// Small blocks (<= 4 KiB) use exact size classes with free lists; larger blocks
// use an address-ordered free map with coalescing. All block sizes live host side.
namespace {
constexpr u32 SMALL_MAX = 4096;
constexpr u32 GRAIN = 16;
constexpr u32 HEAP_COMMIT_STEP = 16 << 20;

std::mutex g_heap_lock;
u32 g_brk = HEAP_BASE;
u32 g_committed = HEAP_BASE;
std::vector<u32> g_small_free[SMALL_MAX / GRAIN + 1];
std::map<u32, u32> g_large_free;                 // addr -> size
std::multimap<u32, u32> g_large_by_size;         // size -> addr
std::unordered_map<u32, u32> g_alloc_size;       // addr -> block size

u32 bump(u32 size) {
    u32 a = g_brk;
    if (u64(a) + size > HEAP_END) {
        std::fprintf(stderr, "guest heap exhausted (%u bytes)\n", size);
        std::abort();
    }
    g_brk += size;
    while (g_brk > g_committed) {
        commit(g_committed, HEAP_COMMIT_STEP);
        g_committed += HEAP_COMMIT_STEP;
    }
    return a;
}

void large_remove(u32 addr, u32 size) {
    g_large_free.erase(addr);
    auto [b, e] = g_large_by_size.equal_range(size);
    for (auto it = b; it != e; ++it)
        if (it->second == addr) {
            g_large_by_size.erase(it);
            break;
        }
}

void large_insert(u32 addr, u32 size) {
    // coalesce with neighbours
    auto next = g_large_free.find(addr + size);
    if (next != g_large_free.end()) {
        u32 ns = next->second;
        large_remove(next->first, ns);
        size += ns;
    }
    auto it = g_large_free.lower_bound(addr);
    if (it != g_large_free.begin()) {
        --it;
        if (it->first + it->second == addr) {
            u32 pa = it->first, ps = it->second;
            large_remove(pa, ps);
            addr = pa;
            size += ps;
        }
    }
    if (addr + size == g_brk) {  // give back to the top of the heap
        g_brk = addr;
        return;
    }
    g_large_free[addr] = size;
    g_large_by_size.emplace(size, addr);
}

u32 large_alloc(u32 size) {
    auto it = g_large_by_size.lower_bound(size);
    if (it == g_large_by_size.end()) return bump(size);
    u32 bsize = it->first, addr = it->second;
    g_large_by_size.erase(it);
    g_large_free.erase(addr);
    if (bsize - size >= SMALL_MAX + GRAIN) {
        large_insert(addr + size, bsize - size);
        bsize = size;
    }
    g_alloc_size[addr] = bsize;
    return addr;
}
}  // namespace

u32 heap_alloc(u32 size, u32 align) {
    (void)align;  // all blocks are 16-byte aligned
    if (size == 0) size = 1;
    size = (size + GRAIN - 1) & ~(GRAIN - 1);
    std::lock_guard lk(g_heap_lock);
    u32 a;
    if (size <= SMALL_MAX) {
        auto& fl = g_small_free[size / GRAIN];
        if (!fl.empty()) {
            a = fl.back();
            fl.pop_back();
        } else {
            a = bump(size);
        }
        g_alloc_size[a] = size;
        return a;
    }
    a = large_alloc(size);
    g_alloc_size[a] = std::max(g_alloc_size[a], size);
    return a;
}

void heap_free(u32 addr) {
    if (!addr) return;
    std::lock_guard lk(g_heap_lock);
    auto it = g_alloc_size.find(addr);
    if (it == g_alloc_size.end()) {
        std::fprintf(stderr, "[heap] free of unknown block %08x\n", addr);
        return;
    }
    u32 size = it->second;
    g_alloc_size.erase(it);
    if (size <= SMALL_MAX)
        g_small_free[size / GRAIN].push_back(addr);
    else
        large_insert(addr, size);
}

u32 heap_size(u32 addr) {
    std::lock_guard lk(g_heap_lock);
    auto it = g_alloc_size.find(addr);
    return it == g_alloc_size.end() ? 0 : it->second;
}

u32 heap_realloc(u32 addr, u32 size) {
    if (!addr) return heap_alloc(size);
    if (!size) {
        heap_free(addr);
        return 0;
    }
    u32 old = heap_size(addr);
    if (size <= old && old - size < 4096) return addr;
    u32 n = heap_alloc(size);
    std::memcpy(host(n), host(addr), std::min(old, size));
    heap_free(addr);
    return n;
}

u32 heap_strdup(const char* s) {
    size_t n = std::strlen(s) + 1;
    u32 a = heap_alloc(u32(n));
    std::memcpy(host(a), s, n);
    return a;
}

// ---------------------------------------------------------------- page regions
static std::mutex g_map_lock;
static std::map<u32, u32> g_map_free = {{MAP_BASE, MAP_END - MAP_BASE}};

u32 map_alloc(u32 size) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u32 a = 0;
    {
        std::lock_guard lk(g_map_lock);
        for (auto it = g_map_free.begin(); it != g_map_free.end(); ++it) {
            if (it->second >= size) {
                a = it->first;
                u32 rest = it->second - size;
                g_map_free.erase(it);
                if (rest) g_map_free[a + size] = rest;
                break;
            }
        }
    }
    if (!a) return 0;
    commit(a, size);
    std::memset(host(a), 0, size);
    return a;
}

void map_free(u32 addr, u32 size) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    decommit(addr, size);
    std::lock_guard lk(g_map_lock);
    auto [it, ok] = g_map_free.emplace(addr, size);
    auto nx = std::next(it);
    if (nx != g_map_free.end() && it->first + it->second == nx->first) {
        it->second += nx->second;
        g_map_free.erase(nx);
    }
    if (it != g_map_free.begin()) {
        auto pv = std::prev(it);
        if (pv->first + pv->second == it->first) {
            pv->second += it->second;
            g_map_free.erase(it);
        }
    }
}

}  // namespace mem
