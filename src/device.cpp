// Device drivers, the way CONFIG.SYS loads them.
//
// A DOS/V Japanese FEP is not a program. It is a character device driver that
// takes over the name CON, so that every console read the application makes
// goes through it: the conversion window, the candidate list and the keyboard
// handling all happen inside what looks to the application like INT 21h AH=07h.
// Both of the free FEPs that survive do it that way -- 鳳's source says so in
// as many words (`Header dw -1,-1 / attrib dw 8013h / db 'CON     '`), and
// WXP for J-3100 has the same header with `A.I.Soft,Inc.   WX` behind it.
//
// So loading one means being DOS for a moment:
//
//   * put the .SYS image in memory (EXE or flat, both exist),
//   * hand it an INIT request packet with the text of the CONFIG.SYS line,
//   * call its STRATEGY and then its INTERRUPT entry, as far calls,
//   * keep the memory it says it needs and give the rest back,
//   * and, if it claimed CON, make it the console.
//
// The last point needs a console for it to chain to: a driver that replaces CON
// keeps the old one's entry points and passes on whatever it does not handle.
// 鳳 finds them through the DOS list-of-lists (`mov ah,52h / mov si,es:[bx+0ch]`),
// which is where DOS keeps the pointer to the current CON. So there is a
// built-in CON here, six bytes of code behind a device header, and the FEP
// stacks on top of it.
#include "dos.h"

#include "cpu.h"
#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dosemu {

// Reading a file whole: the same three lines as everywhere else, kept here
// because main.cpp's copy is static and this is the only other user.
static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize(static_cast<size_t>(n)); if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); }
    std::fclose(f);
    return v;
}

// The built-in CON: its header and two entry points go in the spare half of the
// interrupt-stub segment, which ends at 0x0200 and runs to 0x03FF.
static constexpr uint16_t kConSeg = 0x0050;
static constexpr uint16_t kConHdr = 0x0200;
static constexpr uint16_t kConStrat = 0x0230;
static constexpr uint16_t kConInt = 0x0236;

// Where a driver call returns to. Nothing is ever executed there -- the CPU is
// stopped the moment it arrives -- but it has to be an address the driver's own
// RETF can reach, so it is a real place with a HLT in it.
static constexpr uint16_t kRetOff = 0x023C;

void Dos::install_builtin_con() {
    mem_.wd(kConSeg, kConHdr + 0, 0xFFFFFFFF);          // last in the chain
    mem_.ww(kConSeg, kConHdr + 4, 0x8013);              // char, stdin, stdout, special
    mem_.ww(kConSeg, kConHdr + 6, kConStrat);
    mem_.ww(kConSeg, kConHdr + 8, kConInt);
    const char* nm = "CON     ";
    for (int i = 0; i < 8; ++i) mem_.wb(kConSeg, kConHdr + 10 + i, nm[i]);

    mem_.wb(kConSeg, kConStrat + 0, 0xCD);              // INT kDevStrat
    mem_.wb(kConSeg, kConStrat + 1, kDevStrat);
    mem_.wb(kConSeg, kConStrat + 2, 0xCB);              // RETF
    mem_.wb(kConSeg, kConInt + 0, 0xCD);                // INT kDevInt
    mem_.wb(kConSeg, kConInt + 1, kDevInt);
    mem_.wb(kConSeg, kConInt + 2, 0xCB);
    mem_.wb(kConSeg, kRetOff, 0xF4);                    // HLT, never reached

    con_seg_ = kConSeg;
    con_hdr_ = kConHdr;
    publish_lol();
}

// The list-of-lists, as far as anything here needs it. It is built once and
// kept, not rebuilt per call: a FEP reads the CON pointer out of it during its
// own INIT, and AH=52h zeroing the block would take that away again.
void Dos::publish_lol() {
    mem_.ww(kLolSeg, 0x0E, blocks_.empty() ? 0xFFFF : blocks_.front().seg);  // LoL-2
    mem_.wd(kLolSeg, 0x14, 0xFFFFFFFF);                 // LoL+4: SFT chain, empty
    mem_.ww(kLolSeg, 0x1C, con_hdr_);                   // LoL+0Ch: the console
    mem_.ww(kLolSeg, 0x1E, con_seg_);
}

// Run a far routine in the guest and come back.
//
// The CPU is an interpreter, so this is just "push a return address it cannot
// reach by accident, jump, and step until it gets there". Everything is saved
// and put back, because this is called from inside the guest's own INT 21h --
// the console read that the FEP is in the middle of answering.
bool Dos::call_far(uint16_t seg, uint16_t off, uint16_t es, uint16_t bx) {
    uint16_t r[8], rhi[8], s[6];
    for (int i = 0; i < 8; ++i) { r[i] = cpu_.r[i]; rhi[i] = cpu_.rhi[i]; }
    for (int i = 0; i < 6; ++i) s[i] = cpu_.sreg[i];
    const auto ip = cpu_.ip;
    const auto flags = cpu_.flags;

    cpu_.set_seg(ES, es);
    cpu_.r[BX] = bx;
    cpu_.set_seg(SS, drv_work_);
    cpu_.r[SP] = 0x0F00;                                // a stack of our own
    cpu_.r[SP] -= 2; mem_.ww(drv_work_, cpu_.r[SP], kConSeg);
    cpu_.r[SP] -= 2; mem_.ww(drv_work_, cpu_.r[SP], kRetOff);
    cpu_.set_seg(CS, seg);
    cpu_.ip = off;

    // A console read blocks until there is a character, so the driver spins on
    // the keyboard here -- and the script that is supposed to type is outside
    // this loop. Letting it act every so often is what makes the two parts of
    // the same clock agree: instructions are what the script counts, and these
    // are instructions.
    bool ok = false;
    for (long i = 0; i < 200000000L; ++i) {
        if (cpu_.sreg[CS] == kConSeg && (cpu_.ip & 0xFFFF) == kRetOff) { ok = true; break; }
        if (cpu_.halted) break;
        if (pump_script && !(i & 0x3FF)) pump_script();
        cpu_.step();
    }

    for (int i = 0; i < 8; ++i) { cpu_.r[i] = r[i]; cpu_.rhi[i] = rhi[i]; }
    for (int i = 0; i < 6; ++i) cpu_.set_seg(i, s[i]);
    cpu_.ip = ip;
    cpu_.flags = flags;
    return ok;
}

// One request to whatever is CON now. `pkt` is filled in by the caller at
// drv_work_:0; both entry points are called, as DOS calls them.
bool Dos::con_request() {
    if (!con_seg_ || !drv_work_) return false;
    const uint16_t strat = mem_.rw(con_seg_, con_hdr_ + 6);
    const uint16_t intr = mem_.rw(con_seg_, con_hdr_ + 8);
    if (!call_far(con_seg_, strat, drv_work_, 0)) return false;
    return call_far(con_seg_, intr, drv_work_, 0);
}

// One byte from the CON driver. The packet goes at the front of the work block
// and the byte comes back just above the argument text.
int Dos::con_read() {
    const uint16_t buf = 0x0200;
    for (int i = 0; i < 22; ++i) mem_.wb(drv_work_, static_cast<uint16_t>(i), 0);
    mem_.wb(drv_work_, 0, 22);
    mem_.wb(drv_work_, 2, 4);                            // READ
    mem_.wd(drv_work_, 14, (static_cast<uint32_t>(drv_work_) << 16) | buf);
    mem_.ww(drv_work_, 18, 1);
    if (!con_request()) return -1;
    const uint16_t got = mem_.rw(drv_work_, 18);
    const int c = got ? mem_.rb(drv_work_, buf) : -1;
    static const bool tr = getenv("DOSEMU_CON_TRACE") != nullptr;
    if (tr) std::fprintf(stderr, "[con] read -> %d (%02X) count=%u status=%04X\n",
                         c, c & 0xFF, got, mem_.rw(drv_work_, 3));
    return c;
}

// Non-destructive read: DOS's "is a character waiting?" (AH=0Bh) asks the driver,
// and a FEP answers no while it is in the middle of a conversion -- the keys are
// inside it and nothing has been confirmed yet.
bool Dos::con_ready() {
    for (int i = 0; i < 22; ++i) mem_.wb(drv_work_, static_cast<uint16_t>(i), 0);
    mem_.wb(drv_work_, 0, 22);
    mem_.wb(drv_work_, 2, 5);                            // non-destructive read
    if (!con_request()) return false;
    const uint16_t st = mem_.rw(drv_work_, 3);
    static const bool tr = getenv("DOSEMU_CON_TRACE") != nullptr;
    if (tr) std::fprintf(stderr, "[con] ready? status=%04X char=%02X\n", st, mem_.rb(drv_work_, 13));
    return (st & 0x0200) == 0;                           // busy bit clear: something is there
}

// The built-in CON's two entry points, reached through INT kDevStrat/kDevInt.
// STRATEGY only remembers where the packet is; INTERRUPT does the work.
bool Dos::device_int(bool strategy) {
    if (strategy) {
        req_seg_ = cpu_.sreg[ES];
        req_off_ = cpu_.r[BX];
        return true;
    }
    const uint16_t s = req_seg_, o = req_off_;
    const uint8_t cmd = mem_.rb(s, o + 2);
    uint16_t status = 0x0100;                           // done
    switch (cmd) {
        case 0:                                         // INIT -- nothing to do
            mem_.wb(s, o + 13, 0);
            mem_.wd(s, o + 14, (static_cast<uint32_t>(kConSeg) << 16) | kRetOff);
            break;
        case 4: {                                       // READ
            const uint16_t n = mem_.rw(s, o + 18);
            const uint32_t buf = mem_.rd(s, o + 14);
            uint16_t got = 0;
            while (got < n) {
                const int c = host_getch();
                if (c < 0) break;
                mem_.wb(static_cast<uint16_t>(buf >> 16),
                        static_cast<uint16_t>((buf & 0xFFFF) + got), static_cast<uint8_t>(c));
                ++got;
            }
            mem_.ww(s, o + 18, got);
            break;
        }
        case 5: {                                       // non-destructive read
            if (!keys_waiting()) { status = 0x0300; break; }   // done + busy: nothing there
            mem_.wb(s, o + 13, 0);
            break;
        }
        case 8: case 9: {                               // WRITE
            const uint16_t n = mem_.rw(s, o + 18);
            const uint32_t buf = mem_.rd(s, o + 14);
            for (uint16_t i = 0; i < n; ++i)
                out(1, static_cast<char>(mem_.rb(static_cast<uint16_t>(buf >> 16),
                                                 static_cast<uint16_t>((buf & 0xFFFF) + i))));
            break;
        }
        default: break;
    }
    mem_.ww(s, o + 3, status);
    return true;
}

// Load one .SYS and install it, as `DEVICE=` would.
bool Dos::load_device(const std::string& path, const std::string& args, std::string& err) {
    std::vector<uint8_t> f = read_file(path);
    if (f.empty()) { err = "cannot read " + path; return false; }

    // How much memory to give it: an EXE says what it needs, a flat image is
    // its own size. Either way the driver trims it at INIT.
    const bool mz = f.size() > 0x40 && f[0] == 'M' && f[1] == 'Z';
    auto rd16 = [&](size_t o) { return static_cast<uint16_t>(f[o] | (f[o + 1] << 8)); };
    uint32_t body = static_cast<uint32_t>(f.size());
    uint32_t hdr_bytes = 0;
    uint16_t extra = 0;
    if (mz) {
        const uint16_t bytes_last = rd16(2), pages = rd16(4), hdr_paras = rd16(8);
        hdr_bytes = static_cast<uint32_t>(hdr_paras) * 16;
        uint32_t image = static_cast<uint32_t>(pages) * 512;
        if (bytes_last) image = image - 512 + bytes_last;
        if (image > f.size()) image = static_cast<uint32_t>(f.size());
        body = image - hdr_bytes;
        extra = rd16(10);                                // minalloc
    }
    const uint32_t need = body + static_cast<uint32_t>(extra) * 16 + 256;
    const uint16_t paras = static_cast<uint16_t>((need + 15) / 16);
    const uint16_t seg = mem_alloc(paras);
    if (!seg) { err = "no room for " + path; return false; }

    mem_.write(Memory::phys(seg, 0), f.data() + hdr_bytes, body);
    if (mz) {                                            // relocate
        const uint16_t nreloc = rd16(6), reloc_off = rd16(24);
        for (uint16_t i = 0; i < nreloc; ++i) {
            const uint16_t ro = rd16(reloc_off + i * 4), rs = rd16(reloc_off + i * 4 + 2);
            mem_.ww(seg + rs, ro, static_cast<uint16_t>(mem_.rw(seg + rs, ro) + seg));
        }
    }

    // The CONFIG.SYS line, from just after `DEVICE=` to the end of the line.
    // 鳳 scans it for the first `.`, skips three characters and then looks for
    // the switches, so the extension and the CR LF both have to be there.
    std::string line = path;
    if (!args.empty()) { line += ' '; line += args; }
    line += "\r\n";
    const uint16_t arg_off = 0x0100;
    for (size_t i = 0; i < line.size(); ++i)
        mem_.wb(drv_work_, static_cast<uint16_t>(arg_off + i), static_cast<uint8_t>(line[i]));

    // Walk the headers: one .SYS can hold several devices, chained.
    uint16_t hdr = 0;
    int installed = 0;
    for (int guard = 0; guard < 16; ++guard) {
        const uint16_t strat = mem_.rw(seg, hdr + 6);
        const uint16_t intr = mem_.rw(seg, hdr + 8);

        for (int i = 0; i < 26; ++i) mem_.wb(drv_work_, static_cast<uint16_t>(i), 0);
        mem_.wb(drv_work_, 0, 26);                       // length
        mem_.wb(drv_work_, 2, 0);                        // INIT
        mem_.wd(drv_work_, 18, (static_cast<uint32_t>(drv_work_) << 16) | arg_off);
        mem_.wb(drv_work_, 22, 2);                       // drive C:, as DOS passes

        if (!call_far(seg, strat, drv_work_, 0) || !call_far(seg, intr, drv_work_, 0)) {
            err = "the driver in " + path + " did not come back";
            return false;
        }
        const uint16_t status = mem_.rw(drv_work_, 3);
        const uint32_t end = mem_.rd(drv_work_, 14);
        const uint32_t end_lin = (static_cast<uint32_t>(end >> 16) << 4) + (end & 0xFFFF);
        const uint32_t base_lin = static_cast<uint32_t>(seg) << 4;
        const uint16_t keep = end_lin > base_lin
                            ? static_cast<uint16_t>((end_lin - base_lin + 15) / 16) : 0;

        // The header is read *after* INIT, because until then it may not be the
        // driver's own. OTRI.SYS is DIET-compressed: what sits at offset 0 on
        // disk is the decompressor's header, with the attribute word intact --
        // DOS needs that to install it at all -- but the name field overwritten
        // with the decompressor's code. INIT unpacks 鳳 over it, and only then
        // does the header say CON.
        Device d;
        d.seg = seg; d.hdr = hdr; d.attr = mem_.rw(seg, hdr + 4);
        for (int i = 0; i < 8; ++i) d.name[i] = static_cast<char>(mem_.rb(seg, hdr + 10 + i));
        d.name[8] = 0;
        for (int i = 7; i >= 0 && d.name[i] == ' '; --i) d.name[i] = 0;

        if ((status & 0x8000) || keep == 0) {
            std::fprintf(stderr, "dosemu: %s: %s refused to install (status %04X)\n",
                         path.c_str(), d.name, status);
        } else {
            devices_.push_back(d);
            ++installed;
            // A driver that calls itself CON becomes the console, and the one it
            // replaces is the one it chained to during its own INIT. Bit 0 of
            // the attribute says the same thing in the way DOS actually acts on
            // it -- "this is the standard input device" is what re-points CON,
            // and a driver whose name got lost on the way still claims it.
            if ((d.attr & 0x8000) && (std::strcmp(d.name, "CON") == 0 || (d.attr & 1))) {
                con_seg_ = seg; con_hdr_ = hdr;
                con_driver_ = true;
                publish_lol();
            }
            std::fprintf(stderr, "dosemu: %s: installed %-8s at %04X:%04X, %u paragraphs\n",
                         path.c_str(), d.name, seg, hdr, keep);
        }

        const uint32_t next = mem_.rd(seg, hdr + 0);
        if (next == 0xFFFFFFFF || (next >> 16) != seg) break;
        hdr = static_cast<uint16_t>(next & 0xFFFF);
    }
    // A driver that refuses still keeps its memory. DOS hands it back, but DOS is
    // also entitled to assume the driver unhooked everything first, and wxpdosv
    // does not: its device-mode INIT installs its interrupt handlers and *then*
    // declines, because it expects to be run again as a program. Freeing the
    // block leaves those vectors pointing into memory the next program is about
    // to use -- which is a jump into whatever lands there.
    if (!installed) { err = path + ": nothing installed"; return false; }

    // Give back what the last INIT did not claim. Every header in one file
    // shares the image, so the end address of the last one is the whole.
    const uint32_t end = mem_.rd(drv_work_, 14);
    const uint32_t end_lin = (static_cast<uint32_t>(end >> 16) << 4) + (end & 0xFFFF);
    const uint32_t base_lin = static_cast<uint32_t>(seg) << 4;
    if (end_lin > base_lin) {
        const uint16_t keep = static_cast<uint16_t>((end_lin - base_lin + 15) / 16);
        if (keep < paras) mem_resize(seg, keep);
    }
    return true;
}

}  // namespace dosemu
