#include "guest_util.h"

#include <cstring>
#include <vector>

#include "cpu.h"
#include "elf_loader.h"
#include "log.h"

namespace guest {

u32 wstring(const std::string& ascii) {
    static u32 alloc = 0, fn_ctor = 0;
    if (!alloc) {
        u32 fn_alloc = elf::find("_ZN2im20StringEASTLAllocatorC1EPKc");
        fn_ctor = elf::find("_ZN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEC1EPKwRKS2_");
        if (!fn_alloc || !fn_ctor) {
            LOGE("guest::wstring: game string functions not found");
            return 0;
        }
        alloc = mem::data_alloc(16);
        cpu::call(fn_alloc, {alloc, mem::data_strdup("EASTL basic_string")});
    }
    u32 text = mem::heap_alloc(u32(ascii.size() * 2 + 2));
    for (size_t i = 0; i <= ascii.size(); i++) mem::w16(text + u32(2 * i), u16(u8(ascii.c_str()[i])));
    u32 s = mem::heap_alloc(32);
    std::memset(mem::host(s), 0, 32);
    cpu::call(fn_ctor, {s, text, alloc});
    return s;
}

u32 vcall(u32 obj, u32 slot, std::initializer_list<u32> extra) {
    std::vector<u32> args{obj};
    args.insert(args.end(), extra.begin(), extra.end());
    u32 fn = mem::r32(mem::r32(obj) + slot);
    return cpu::call_v(fn, args.data(), int(args.size()));
}

}  // namespace guest
