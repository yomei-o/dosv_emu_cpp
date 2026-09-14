// i386 COFF loader for DJGPP executables. A DJGPP .EXE is a small real-mode DOS
// stub (go32) followed by an i386 COFF image (machine 0x14C). The stub loads a DPMI
// host, switches to 32-bit protected mode, maps the COFF at a flat base and jumps to
// its entry. This module parses/loads the COFF part; running it needs the 32-bit
// protected-mode CPU + DPMI host that are still to be built (see resume.md).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "memory.h"

namespace dosemu {

struct CoffSection { std::string name; uint32_t vaddr, size, rawptr; };
struct CoffImage {
    bool valid = false;
    uint32_t entry = 0, text_base = 0;
    std::vector<CoffSection> sections;
};

// The DOS stub length is the MZ image size (pages/last-page from the MZ header).
static uint32_t mz_image_len(const std::vector<uint8_t>& f) {
    auto rd16 = [&](size_t o) { return (uint32_t)(f[o] | (f[o + 1] << 8)); };
    uint32_t last = rd16(2), pages = rd16(4);
    uint32_t len = pages * 512;
    if (last) len = len - 512 + last;
    return len;
}

// Is this an MZ stub wrapping an i386 COFF (i.e. a DJGPP program)?
bool is_djgpp_coff(const std::vector<uint8_t>& f) {
    if (f.size() < 64 || f[0] != 'M' || f[1] != 'Z') return false;
    uint32_t off = mz_image_len(f);
    if (off + 2 > f.size()) return false;
    uint16_t machine = f[off] | (f[off + 1] << 8);
    return machine == 0x014C;   // IMAGE_FILE_MACHINE_I386 (COFF)
}

// Parse the COFF headers and section table (no mapping yet — that waits on the
// 32-bit address space). Returns entry, text base and the sections.
CoffImage parse_djgpp_coff(const std::vector<uint8_t>& f) {
    CoffImage img;
    if (!is_djgpp_coff(f)) return img;
    uint32_t o = mz_image_len(f);
    auto rd16 = [&](size_t p) { return (uint32_t)(f[p] | (f[p + 1] << 8)); };
    auto rd32 = [&](size_t p) { return (uint32_t)(f[p] | (f[p+1]<<8) | (f[p+2]<<16) | ((uint32_t)f[p+3]<<24)); };
    uint16_t nsec = rd16(o + 2), opt = rd16(o + 16);
    if (opt >= 28) { img.entry = rd32(o + 20 + 16); img.text_base = rd32(o + 20 + 20); }
    uint32_t so = o + 20 + opt;
    for (uint16_t i = 0; i < nsec; ++i) {
        size_t p = so + i * 40;
        if (p + 40 > f.size()) break;
        CoffSection s;
        char nm[9] = {0}; std::memcpy(nm, &f[p], 8); s.name = nm;
        s.vaddr = rd32(p + 12); s.size = rd32(p + 16); s.rawptr = rd32(p + 20);
        img.sections.push_back(s);
    }
    img.valid = true;
    return img;
}

}  // namespace dosemu
