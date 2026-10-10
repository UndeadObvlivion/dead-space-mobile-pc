// Helpers for building game (guest) objects from the host.
#pragma once
#include <initializer_list>
#include <string>

#include "memory.h"

namespace guest {

// eastl::basic_string<wchar_t, im::StringEASTLAllocator> built by the game's own constructor.
// The object is heap allocated and never freed (call sparingly, or cache the result).
u32 wstring(const std::string& ascii);

// Calls a virtual method: slot = byte offset into the vtable.
u32 vcall(u32 obj, u32 slot, std::initializer_list<u32> extra = {});

}  // namespace guest
