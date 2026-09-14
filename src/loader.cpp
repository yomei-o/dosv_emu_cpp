// Loads a DOS program into real-mode memory and sets up the PSP and initial
// registers. .COM is a flat image loaded at PSP:0x100; .EXE (MZ) has a header, a
// relocation table, and its own initial CS:IP / SS:SP relative to the load segment.
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include "cpu.h"
#include "memory.h"

namespace dosemu {

bool is_djgpp_coff(const std::vector<uint8_t>&);

// The default environment block's *contents*: PATH so a compiler driver finds its
// passes, and the program's own full path, which a shell like FreeCOM reads to find
// itself. Where it goes is not decided here — Dos::alloc_env() allocates it out of the
// arena like any other block, so that it has an MCB. It used to live at a fixed segment
// below the first PSP, and once the MCB chain became real that was a block outside the
// chain, which FreeCOM noticed immediately.
std::vector<uint8_t> make_default_env(const std::string& dos_name) {
    // DJGPP finds everything else from DJGPP.ENV, so that one variable plus its bin
    // directory on PATH is the whole configuration. LSI C's directory stays first;
    // the two toolchains share no executable names.
    // One entry per toolchain, each pointing at where get_fixtures.sh puts it. Each
    // compiler finds the rest of itself from these: DJGPP from DJGPP.ENV, Watcom from
    // WATCOM plus INCLUDE, LSI C from PATH alone.
    const char* vars[] = { "PATH=A:\\LSIC86\\BIN;A:\\DJGPP\\BIN;A:\\BINW",
                           "COMSPEC=A:\\COMMAND.COM",
                           "DJGPP=A:\\DJGPP\\DJGPP.ENV",
                           "WATCOM=A:\\",
                           "INCLUDE=A:\\H",
                           nullptr };
    std::vector<uint8_t> b;
    for (int i = 0; vars[i]; ++i) {
        for (const char* p = vars[i]; *p; ++p) b.push_back(static_cast<uint8_t>(*p));
        b.push_back(0);
    }
    b.push_back(0);                         // end of strings
    b.push_back(1); b.push_back(0);         // count of trailing strings
    for (char c : dos_name) b.push_back(static_cast<uint8_t>(c));
    b.push_back(0);
    return b;
}

static void build_psp(Memory& mem, uint16_t psp_seg, const std::string& cmdline, uint16_t env_seg) {
    // Minimal PSP. INT 20h at offset 0, the environment segment at 0x2C, and the
    // command tail at 0x80.
    mem.wb(psp_seg, 0x00, 0xCD); mem.wb(psp_seg, 0x01, 0x20);   // INT 20h
    mem.ww(psp_seg, 0x2C, env_seg);                            // environment segment

    // The job file table: 20 bytes at 0x18 mapping this process's handles to DOS's
    // system file table, the count at 0x32, and a far pointer to the table at 0x34.
    // No 16-bit guest ever looked, but DJGPP's fstat() does, and its range check is
    //
    //     if (fhandle >= _farpeekw(_dos_ds, psp_addr + 0x32)) return -1;   -> EBADF
    //
    // With the field left at zero every handle is out of range, so fstat fails on a
    // file that opened perfectly well. gcc 4.7's cc1 reports that as
    // `fatal error: hello.c: Bad file descriptor` immediately after opening it.
    for (int i = 0; i < 20; ++i) mem.wb(psp_seg, 0x18 + i, i < 5 ? i : 0xFF);
    mem.ww(psp_seg, 0x32, 20);
    mem.ww(psp_seg, 0x34, 0x0018);                             // offset of the table
    mem.ww(psp_seg, 0x36, psp_seg);                            // ...and its segment
    std::string tail = cmdline;
    if (tail.size() > 126) tail.resize(126);
    mem.wb(psp_seg, 0x80, static_cast<uint8_t>(tail.size()));
    for (size_t i = 0; i < tail.size(); ++i) mem.wb(psp_seg, 0x81 + i, tail[i]);
    mem.wb(psp_seg, 0x81 + tail.size(), 0x0D);
}

// Loads at psp_seg (0x0100 for the top-level program, higher for EXEC children)
// and fills the CPU's initial state. Returns true on success.
bool load_program(const std::vector<uint8_t>& f, Cpu& cpu, uint16_t psp_seg,
                  const std::string& cmdline, std::string& err, const std::string& dos_name,
                  uint16_t env_seg) {
    Memory& mem = cpu.mem();
    uint16_t load_seg = psp_seg + 0x10;   // program starts 256 bytes (one PSP) above

    // Clear the upper halves of the 32-bit registers. A freshly loaded program gets
    // 16-bit registers from DOS and nothing defines EAX..EDI above bit 15, so leaving
    // the previous program's values there is a leak — and an invisible one, because no
    // 16-bit code can observe it.
    //
    // It stops being invisible the moment the child is a DOS extender. EXEC set r[SP]
    // and left rhi[SP] holding the *parent's* ESP high half, so a DJGPP child started
    // life with SP=0x0760 but ESP=0x00200760. Its 16-bit stack segment meant PUSH used
    // SP while `mov ecx,[esp+4]` addressed through the full ESP — a read 2 MiB away
    // from the write. sbrk() read its own argument as garbage, asked DPMI for 2.1 GB,
    // was refused, and the whole child tore itself down. gcc's driver hung waiting for
    // a cc1 that had already given up; the same cc1 run directly was fine, because
    // nothing had run before it to leave anything in rhi[].
    for (int i = 0; i < 8; ++i) cpu.rhi[i] = 0;

    // The environment block belongs to whoever allocated it, and the caller always has:
    // Dos::alloc_env() for the top-level program, make_child_env() for an EXEC child.
    // Which block a child gets matters: DJGPP passes command lines longer than the PSP's
    // 126-byte tail by putting them in an environment variable and sending " !proxy" as
    // the tail, so rebuilding a default environment here would make the child see an
    // empty command line -- which is how `gcc` came to invoke `cc1` with no arguments.
    build_psp(mem, psp_seg, cmdline, env_seg);

    // A DJGPP program is a real-mode DOS stub (go32) wrapping an i386 COFF image, and
    // it no longer needs special handling: the stub is ordinary 16-bit code, it finds
    // the built-in DPMI host (src/dpmi.cpp), switches to protected mode, loads the COFF
    // itself and jumps into it. So it goes down the normal MZ path below.
    //
    // The one thing that does not work yet is argv — see resume.md. Detection is kept
    // because the loader is where you would notice a 32-bit image, and DOSEMU_DPMI_TRACE
    // is the way to watch one run.
    (void)is_djgpp_coff;

    bool is_mz = f.size() >= 2 && f[0] == 'M' && f[1] == 'Z';
    if (is_mz) {
        auto rd16 = [&](size_t o) { return static_cast<uint16_t>(f[o] | (f[o + 1] << 8)); };
        uint16_t bytes_last = rd16(2), pages = rd16(4), nreloc = rd16(6), hdr_paras = rd16(8);
        uint16_t ss = rd16(14), sp = rd16(16), ip = rd16(20), cs = rd16(22), reloc_off = rd16(24);
        uint32_t hdr_bytes = static_cast<uint32_t>(hdr_paras) * 16;
        uint32_t image_bytes = static_cast<uint32_t>(pages) * 512;
        if (bytes_last) image_bytes = image_bytes - 512 + bytes_last;
        if (image_bytes > f.size()) image_bytes = static_cast<uint32_t>(f.size());
        uint32_t body = image_bytes - hdr_bytes;

        // Load the image body at load_seg:0.
        mem.write(Memory::phys(load_seg, 0), f.data() + hdr_bytes, body);
        // Apply relocations: each entry is an offset:segment pointing at a word that
        // needs load_seg added to it.
        for (uint16_t i = 0; i < nreloc; ++i) {
            uint16_t ro = rd16(reloc_off + i * 4);
            uint16_t rs = rd16(reloc_off + i * 4 + 2);
            uint16_t cur = mem.rw(load_seg + rs, ro);
            mem.ww(load_seg + rs, ro, cur + load_seg);
        }
        // set_seg, not a raw sreg[] write: it also refreshes the descriptor cache and
        // the code segment's width. Identical in real mode, but a raw write leaves
        // sbase[] stale, and protected mode reads sbase[].
        cpu.set_seg(CS, load_seg + cs); cpu.ip = ip;
        cpu.set_seg(SS, load_seg + ss); cpu.r[SP] = sp;
        cpu.set_seg(DS, psp_seg); cpu.set_seg(ES, psp_seg);
        (void)err;
        return true;
    }

    // .COM: flat load at PSP:0x100, all segment registers = PSP, SP at top of segment.
    if (f.size() > 0xFF00) { err = "COM image too large"; return false; }
    mem.write(Memory::phys(psp_seg, 0x100), f.data(), static_cast<uint32_t>(f.size()));
    for (int s : {CS, DS, ES, SS}) cpu.set_seg(s, psp_seg);
    cpu.ip = 0x100;
    cpu.r[SP] = 0xFFFE;
    mem.ww(psp_seg, 0xFFFE, 0x0000);   // a return address (INT 20h at PSP:0)
    return true;
}

}  // namespace dosemu
