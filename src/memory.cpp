// High-page reporting for Memory (see the comment on Memory::check).
#include "memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dosemu {

bool Memory::check = getenv("DOSEMU_MEMCHK") != nullptr;

// Same DOSEMU_WATCH range the CPU uses, parsed independently so the two halves of the
// watchpoint cannot drift apart.
uint32_t Memory::watch_lo = [] { const char* s = getenv("DOSEMU_WATCH");
    return s ? (uint32_t)strtoul(s, nullptr, 16) : 0u; }();
uint32_t Memory::watch_hi = [] { const char* s = getenv("DOSEMU_WATCH");
    const char* d = s ? strchr(s, '-') : nullptr;
    return d ? (uint32_t)strtoul(d + 1, nullptr, 16) : 0u; }();
std::function<void(uint32_t)> Memory::watch_write;

void Memory::note_page(uint32_t a) const {
    static int n = 0;
    if (++n > 20) return;   // a guest working up there touches many pages; a few tell the story
    std::fprintf(stderr, "[mem] page at %08X (above the %08X RAM budget)\n",
                 a & ~kPageMask, kSize);
}

}  // namespace dosemu
