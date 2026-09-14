// Physical memory, in 64 KiB pages allocated on first write.
//
// It used to be one flat array: 1 MiB, then 16, then 64 as each guest asked for more.
// That worked until Watcom's X-32 extender, which does not ask — it enters protected
// mode under its own GDT and puts its entire 32-bit world at linear **256 MiB**. With a
// 64 MiB array every one of those addresses folded back through the size mask into the
// real-mode area, so the extender wrote its environment copy in one place and read it
// from another, found no `$=` variable, and exited quietly. Sizing the array to suit
// would mean committing 512 MiB to serve a program that touches a few hundred KiB of it.
//
// So: a page table over the whole 32-bit address space. 65536 slots of 64 KiB, allocated
// zeroed when first written; a read of an untouched page is zero and allocates nothing.
// Nothing is "out of range" any more, and a guest that works at 256 MiB costs what it
// actually uses. It also means the emulator no longer zeroes tens of megabytes at
// startup, which the browser build was paying for on every run.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace dosemu {

class Memory {
public:
    Memory() : pages_(kPages) {}

    static constexpr uint32_t kPageBits = 16;
    static constexpr uint32_t kPageSize = 1u << kPageBits;          // 64 KiB
    static constexpr uint32_t kPageMask = kPageSize - 1;
    static constexpr uint32_t kPages    = 1u << (32 - kPageBits);   // the whole 32-bit space

    // How much guest RAM we are willing to hand out — a budget for DPMI's "allocate
    // memory" and "how much is free", not a limit on what can be addressed. A client
    // that places itself somewhere high without asking is not bound by it.
    static constexpr uint32_t kSize = 0x4000000;                    // 64 MiB

    // Real-mode address wrap stays at 1 MiB; the A20 gate in cpu.h decides whether it
    // applies. Extended access uses r*/w* with a linear address directly.
    static uint32_t phys(uint16_t seg, uint16_t off) {
        return ((static_cast<uint32_t>(seg) << 4) + off) & 0xFFFFF;
    }

    uint8_t  r8(uint32_t a)  const { const uint8_t* p = ro(a); return p ? p[a & kPageMask] : 0; }
    uint16_t r16(uint32_t a) const {
        if ((a & kPageMask) <= kPageMask - 1) {
            const uint8_t* p = ro(a); if (!p) return 0;
            p += a & kPageMask;
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }
        return static_cast<uint16_t>(r8(a) | (r8(a + 1) << 8));     // crosses a page
    }
    uint32_t r32(uint32_t a) const {
        if ((a & kPageMask) <= kPageMask - 3) {
            const uint8_t* p = ro(a); if (!p) return 0;
            p += a & kPageMask;
            return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        return r8(a) | (r8(a + 1) << 8) | (r8(a + 2) << 16)
             | (static_cast<uint32_t>(r8(a + 3)) << 24);
    }

    void w8(uint32_t a, uint8_t v) { watched(a); page(a)[a & kPageMask] = v; }
    void w16(uint32_t a, uint16_t v) {
        watched(a); watched(a + 1);
        if ((a & kPageMask) <= kPageMask - 1) {
            uint8_t* p = page(a) + (a & kPageMask);
            p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
        } else { w8(a, static_cast<uint8_t>(v)); w8(a + 1, static_cast<uint8_t>(v >> 8)); }
    }
    void w32(uint32_t a, uint32_t v) {
        for (int i = 0; i < 4; ++i) watched(a + i);
        if ((a & kPageMask) <= kPageMask - 3) {
            uint8_t* p = page(a) + (a & kPageMask);
            p[0] = static_cast<uint8_t>(v);       p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
        } else {
            for (int i = 0; i < 4; ++i) w8(a + i, static_cast<uint8_t>(v >> (i * 8)));
        }
    }

    // segment:offset convenience
    uint8_t  rb(uint16_t s, uint16_t o) const { return r8(phys(s, o)); }
    uint16_t rw(uint16_t s, uint16_t o) const { return r16(phys(s, o)); }
    uint32_t rd(uint16_t s, uint16_t o) const { return r32(phys(s, o)); }
    void     wb(uint16_t s, uint16_t o, uint8_t v)  { w8(phys(s, o), v); }
    void     ww(uint16_t s, uint16_t o, uint16_t v) { w16(phys(s, o), v); }
    void     wd(uint16_t s, uint16_t o, uint32_t v) { w32(phys(s, o), v); }

    void write(uint32_t a, const void* src, uint32_t len) {
        if (watch_hi) for (uint32_t i = 0; i < len; ++i) watched(a + i);
        const uint8_t* s = static_cast<const uint8_t*>(src);
        while (len) {
            const uint32_t off = a & kPageMask;
            const uint32_t n = std::min(len, kPageSize - off);
            std::memcpy(page(a) + off, s, n);
            a += n; s += n; len -= n;
        }
    }

    // DOSEMU_MEMCHK=1 reports the first touch of each page above the RAM budget. Not an
    // error any more — a page there is served like any other — but "this guest is
    // working at 256 MiB" is the kind of thing worth being told once rather than
    // discovering three days later.
    static bool check;

    // DOSEMU_WATCH applies here too, for writes. The CPU's copy of the check lives in
    // Cpu::lin() and therefore sees only accesses made through a segment register — which
    // is not how the DOS layer addresses guest memory (Memory::rb/ww take a segment and an
    // offset and come straight here) and not how the host writes its own structures. A
    // watchpoint that cannot see those is worse than no watchpoint, because its silence
    // reads as evidence: two hours went into a conclusion drawn from exactly that.
    static uint32_t watch_lo, watch_hi;
    static std::function<void(uint32_t)> watch_write;
    void watched(uint32_t a) const { if (watch_hi && a >= watch_lo && a <= watch_hi && watch_write) watch_write(a); }

private:
    const uint8_t* ro(uint32_t a) const { return pages_[a >> kPageBits].get(); }
    uint8_t* page(uint32_t a) {
        std::unique_ptr<uint8_t[]>& p = pages_[a >> kPageBits];
        if (!p) {
            p.reset(new uint8_t[kPageSize]());
            if (check && a >= kSize) note_page(a);
        }
        return p.get();
    }
    void note_page(uint32_t a) const;

    std::vector<std::unique_ptr<uint8_t[]>> pages_;
};

}  // namespace dosemu
