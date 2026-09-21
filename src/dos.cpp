// INT 21h / BIOS service dispatch.
#include "dos.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cctype>
#include <filesystem>

namespace dosemu {

// Where the DOS list-of-lists is handed out: just above the DPMI host's entry points,
// which own 0x00E0 through 0x00F7. Both are inside the memory arena and marked as DOS's
// own, so nothing allocates over them.

bool Dos::trace = getenv("DOSEMU_DOS_TRACE") != nullptr;

// See the comment on the declaration in src/dos.h.
uint64_t Dos::hundredths() const {
    static const uint64_t per = [] {
        const char* v = getenv("DOSEMU_CLOCK");
        return v ? strtoull(v, nullptr, 10) : 0ull;
    }();

    return per ? cpu_.insns / per : 0;
}

// The interrupt vector table pointed at nothing, because the emulator services INT
// through a callback and never consults it. That is fine until a program *reads* a
// vector and calls it — Watcom's W32RUN takes INT 21h with AH=35h and then far-calls
// what it got, which was 0000:0000, so it jumped to address zero and marched through
// low memory until it wandered into the DPMI entry point.
//
// So give every vector something real to point at: four bytes per interrupt at
// 0050:n*4, holding `INT n; IRET`. Executing that stub re-enters the same dispatch the
// CPU would have used, and the IRET then unwinds the `pushf; call far` frame the
// caller pushed. 256 * 4 bytes fits exactly in 0x0500-0x08FF, which is free — the IVT
// and BIOS data end at 0x0500, and the DPMI entry, the list-of-lists and the
// environment all live at 0x0E00 and above.
static constexpr uint16_t kIvtStubSeg = 0x0050;

// The DOS/V font stubs live in the ROM BIOS area, at F000:FF00. Low memory is
// not safe for them: at 0090:0000 they were gone by the time the guest used the
// far pointer, and it ran zeroes upward until it fell out of the segment. Two vectors above 0x80 are
// borrowed to get back here; a guest that installs its own handler there writes
// the vector first, and these are only ever reached through the far pointer
// INT 15h AX=5000h hands out.
static constexpr uint16_t kFontStubSeg = 0xF000;
static constexpr uint16_t kFontStubOff = 0xFF00;
static constexpr uint8_t kFontIntAnk = 0xEE;
static constexpr uint8_t kFontIntKanji = 0xEF;

// ...but only for the vectors a PC actually has. Above 0x7F the table is where drivers
// and extenders put *themselves*, and they find room by scanning for entries that are
// not in use. DOS/4GW walks 0x2C8-0x2FA (vectors 0xB2-0xBE) looking for a repeated
// value; with all 256 stubbed, every entry there was distinct and it scanned for ever.
// Zero is what DOS leaves in that range and it is the honest answer to "is anything
// hooked here?" — a program that installs one writes the vector first anyway.
void Dos::install_ivt_stubs() {
    for (int n = 0x80; n < 256; ++n) mem_.w32(n * 4, 0);
    for (int n = 0; n < 0x80; ++n) {
        const uint16_t off = static_cast<uint16_t>(n * 4);
        mem_.wb(kIvtStubSeg, off + 0, 0xCD);                 // INT n
        mem_.wb(kIvtStubSeg, off + 1, static_cast<uint8_t>(n));
        mem_.wb(kIvtStubSeg, off + 2, 0xCF);                 // IRET
        mem_.wb(kIvtStubSeg, off + 3, 0x90);                 // pad, so n*4 indexes cleanly
        mem_.w16(n * 4, off);
        mem_.w16(n * 4 + 2, kIvtStubSeg);
    }
}

// One timestamp for every file. Not a real mtime, but the *same* answer from
// FindFirst and from get-file-time, so a program that cross-checks them agrees.
static constexpr uint16_t kFixedDate = ((1993 - 1980) << 9) | (8 << 5) | 19;
static constexpr uint16_t kFixedTime = (12 << 11);

bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&, const std::string&, uint16_t);
std::vector<uint8_t> make_default_env(const std::string& dos_name);

uint16_t Dos::alloc_env(const std::string& dos_name) {
    const std::vector<uint8_t> b = make_default_env(dos_name);
    const uint16_t seg = mem_alloc(static_cast<uint16_t>((b.size() + 15) / 16));
    if (!seg) return 0;
    for (size_t i = 0; i < b.size(); ++i) mem_.wb(seg, static_cast<uint16_t>(i), b[i]);
    env_seg = seg;
    return seg;
}

// The PSP fields that depend on who launched the program and how much memory it owns.
// build_psp() in the loader cannot fill these: it knows neither.
//
//   0x02  segment just past this program's memory block. The same ceiling AH=4Ah
//         reports, so the two agree about how much there is.
//   0x16  the parent's PSP. A loader stub that has to find the program that launched
//         it walks here; leaving it zero sends it to segment 0.
//   0x50  the classic `INT 21h; RETF` call gate, which some runtimes far-call instead
//         of issuing INT 21h themselves.
// The BIOS data area at 0040:0000. Programs read it directly rather than asking
// the BIOS, so leaving it zero is not neutral: JW_CAD takes the screen's byte
// width from 0040:004A and the character cell height from 0040:0085, then
// divides by them, and stops with "run-time error R6003 - integer divide by 0"
// before it draws anything.
void Dos::install_bios_data() {
    mem_.ww(0x40, 0x0010, 0x0021);   // equipment list: 80x25 colour, one FDD
    mem_.ww(0x40, 0x0013, 640);      // KB of conventional memory
    mem_.wb (0x40, 0x0049, 0x03);     // current video mode
    mem_.ww(0x40, 0x004A, 80);       // columns -- the screen's width in bytes
    mem_.ww(0x40, 0x004C, 80 * 25 * 2);
    mem_.ww(0x40, 0x0063, 0x03D4);   // CRTC port
    mem_.wb (0x40, 0x0084, 25 - 1);   // rows, less one
    mem_.ww(0x40, 0x0085, 16);       // scan lines per character
    mem_.wb (0x40, 0x0087, 0x60);     // EGA/VGA info
    mem_.wb (0x40, 0x0088, 0x09);
    mem_.wb (0x40, 0x0089, 0x01);     // VGA flags
    mem_.ww(0x40, 0x001A, 0x001E);   // keyboard buffer: head,
    mem_.ww(0x40, 0x001C, 0x001E);   // tail (empty),
    mem_.ww(0x40, 0x0080, 0x001E);   // and where it starts
    mem_.ww(0x40, 0x0082, 0x003E);   // ...and ends
}

// The BIOS keyboard buffer: a sixteen-word ring at 0040:001E, with the head and
// tail at 0040:001A and 0040:001C.
//
// Keys go *here*, not only into a queue of our own, because that is where the
// machine keeps them and resident software knows it. WXP for J-3100 reads the
// ring directly rather than calling INT 16h -- a FEP is answering a console read
// at the time, so calling the BIOS it has hooked would be calling itself -- and
// with an empty ring it returned a null byte for every key pressed.
bool Dos::kbd_push(uint16_t key) {
    const uint16_t start = mem_.rw(0x40, 0x0080), end = mem_.rw(0x40, 0x0082);
    const uint16_t tail = mem_.rw(0x40, 0x001C);
    uint16_t next = static_cast<uint16_t>(tail + 2);
    if (next >= end) next = start;
    if (next == mem_.rw(0x40, 0x001A)) return false;      // full: the beep case
    mem_.ww(0x40, tail, key);
    mem_.ww(0x40, 0x001C, next);
    return true;
}

// The next key, or -1. `take` false only looks.
int Dos::kbd_pop(bool take) {
    const uint16_t head = mem_.rw(0x40, 0x001A);
    if (head == mem_.rw(0x40, 0x001C)) return -1;         // empty
    const uint16_t key = mem_.rw(0x40, head);
    if (take) {
        const uint16_t start = mem_.rw(0x40, 0x0080), end = mem_.rw(0x40, 0x0082);
        uint16_t next = static_cast<uint16_t>(head + 2);
        if (next >= end) next = start;
        mem_.ww(0x40, 0x001A, next);
    }
    return key;
}

void Dos::init_psp(uint16_t psp, uint16_t parent, const std::string& path) {
    prog_path = path;
    env_seg = mem_.rw(psp, 0x2C);   // still a segment now; the DPMI switch may rewrite it
    // A program DOS loads owns all of memory from its PSP up, and shrinks its block to
    // what it needs. A child's block was already carved out by exec(), so this only
    // claims it for the top-level program — but that claim is what makes the first
    // AH=4Ah a *shrink* rather than an allocation out of thin air.
    uint16_t top = heap_end_;
    const size_t bi = mem_find(psp);
    if (bi != blocks_.size() && blocks_[bi].used) {
        blocks_[bi].owner = psp;
        top = static_cast<uint16_t>(psp + blocks_[bi].paras);
    } else {
        mem_own(psp, static_cast<uint16_t>(heap_end_ - psp));
        const size_t bj = mem_find(psp);
        if (bj != blocks_.size()) { blocks_[bj].owner = psp; top = static_cast<uint16_t>(psp + blocks_[bj].paras); }
        mem_publish();
    }
    mem_.ww(psp, 0x02, top);
    mem_.ww(psp, 0x16, parent);
    mem_.wb(psp, 0x50, 0xCD); mem_.wb(psp, 0x51, 0x21); mem_.wb(psp, 0x52, 0xCB);
}

// Build a child's environment: a copy of the parent's strings, then the child's own
// full path, in a block of its own.
//
// There used to be exactly one environment block, at a fixed segment, rebuilt for
// whoever loaded last. That is fine while only one program runs at a time and nobody
// looks back — but a loader stub EXECs a helper and expects to still be findable, and
// the shared block meant loading the helper *overwrote the parent's own program name*
// with the helper's. DOS gives every program its own block; so do we now.
// A program's own full path lives after its environment strings: a double NUL, a word
// count of 1, then the ASCIIZ path. DOS 3.0+ writes it at load time, and it writes it
// into the block the caller supplied as much as into one it copied — a program has to be
// able to find itself whoever built its environment.
//
// Skipping that for a caller-supplied block is what broke `wcl hello.c`. wcl copies its
// strings into a new block and stops there, count = 0 and no name; wcc's 32-bit stub then
// looks up its own path to record it as `$=<path>` for W32RUN to load, finds nothing, and
// writes `$=`. W32RUN duly EXECs the empty string: `Can't open ''; rc=2`. Nothing in that
// chain reports a missing path — each step faithfully passes on the emptiness.
//
// In place if the block has room, otherwise a copy; and always a copy when the caller
// handed us its *own* environment, because stamping that would overwrite the parent's
// name with the child's and take away the parent's ability to find itself.
uint16_t Dos::stamp_env_path(uint16_t env, const std::string& path) {
    uint16_t end = 0;                                   // first byte past the double NUL
    for (uint16_t i = 0; i < 8192; ++i)
        if (!mem_.rb(env, i) && !mem_.rb(env, i + 1)) { end = static_cast<uint16_t>(i + 2); break; }
    const size_t need = static_cast<size_t>(end) + 2 + path.size() + 1;
    const size_t bi = mem_find(env);
    const size_t have = (bi != blocks_.size()) ? static_cast<size_t>(blocks_[bi].paras) * 16 : 0;
    if (env == env_seg || need > have) {
        if (trace) std::fprintf(stderr, "[env] supplied block %04X %s; copying\n", env,
                                env == env_seg ? "is the parent's own" : "has no room for the path");
        return make_child_env(env, path);
    }
    mem_.ww(env, end, 1);
    for (size_t i = 0; i < path.size(); ++i)
        mem_.wb(env, static_cast<uint16_t>(end + 2 + i), static_cast<uint8_t>(path[i]));
    mem_.wb(env, static_cast<uint16_t>(end + 2 + path.size()), 0);
    if (trace) std::fprintf(stderr, "[env] supplied block %04X stamped with \"%s\"\n", env, path.c_str());
    return env;
}

// The child's environment when the caller asked to inherit: the parent's strings, then
// the child's own path as described above.
uint16_t Dos::make_child_env(uint16_t parent_env, const std::string& parent_path) {
    // Copied *verbatim*, trailing program name included — the child does NOT get its
    // own name here. That is the DOS behaviour, and it is deliberate on the guest's
    // side too: the name after the strings is appended by whoever built the block (the
    // shell, for a program it launched), not by the EXEC primitive. A program that
    // EXECs a child hands down its own block, so the child sees the *parent's* path.
    //
    // It reads like a quirk until you see what it is for. Watcom's 32-bit stub EXECs
    // W32RUN with no arguments identifying itself — the parameter block carries env 0,
    // its own PSP command tail and its two FCBs, and nothing else (disassembled at
    // 0110:0686). The only way W32RUN can learn which executable to load is to read
    // this name. DJGPP's crt1.c has a stack of fallbacks for argv[0] for the same
    // reason: it is not reliably your own name.
    std::vector<uint8_t> b;
    for (uint16_t i = 0; i < 8192; ++i) {          // strings, up to the double NUL
        const uint8_t c = mem_.rb(parent_env, i);
        b.push_back(c);
        if (!c && !mem_.rb(parent_env, i + 1)) break;
    }
    b.push_back(0);                                 // end of strings
    const uint16_t after = static_cast<uint16_t>(b.size());
    // The trailing name is the child's own path, regenerated rather than copied. It has
    // to be regenerated: a program that appends a variable to its environment writes
    // over the end-of-strings NUL, and the count word and name that followed go with
    // it. Watcom's stub does exactly that, adding `$=<its own path>`, so by the time
    // the block reaches the child there is no name left to copy -- while the `$`
    // variable, which is how the loader learns what to load, must survive. Copying the
    // strings and rebuilding the name is what keeps both.
    const uint16_t count = 1;
    b.push_back(1); b.push_back(0);
    for (char c : parent_path) b.push_back(static_cast<uint8_t>(c));
    b.push_back(0);

    if (trace) {
        std::fprintf(stderr, "[env] child env %u bytes, trailing name \"", (unsigned)b.size());
        for (size_t i = after + 2; i < b.size() && b[i]; ++i) std::fputc(b[i], stderr);
        std::fprintf(stderr, "\" (count=%u) from %04X, strings:", count, parent_env);
        for (size_t i = 0; i + 1 < after; ++i)
            std::fputc(b[i] ? static_cast<char>(b[i]) : ' ', stderr);
        std::fputc('\n', stderr);
    }
    const uint16_t paras = static_cast<uint16_t>((b.size() + 15) / 16);
    const uint16_t seg = mem_alloc_at(paras, true);   // below the child, not at the bottom of memory
    if (trace) std::fprintf(stderr, "[env] child env at %04X (%u paras)\n", seg, paras);
    if (!seg) return parent_env;                            // no room: share, as before
    for (size_t i = 0; i < b.size(); ++i) mem_.wb(seg, static_cast<uint16_t>(i), b[i]);
    return seg;
}

// ---- conventional memory ---------------------------------------------------------
// Blocks in address order covering the whole arena, every one either owned by somebody
// or free, each with its MCB in the paragraph below the segment the guest is given. See
// the note on blocks_ for why the bump pointer had to go, and why the chain is real.

size_t Dos::mem_find(uint16_t seg) const {
    for (size_t i = 0; i < blocks_.size(); ++i) if (blocks_[i].seg + 1 == seg) return i;
    return blocks_.size();
}

// Carve `paras` off the front of block `mcb`, leaving the remainder as a free block with
// an MCB of its own. The paragraph that MCB occupies comes out of the remainder, so a
// split of an N-paragraph block into P and R has P + R = N - 1: memory spent on
// bookkeeping, exactly as DOS spends it.
void Dos::mem_split(uint16_t mcb, uint16_t paras) {
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].seg != mcb) continue;
        if (blocks_[i].paras < paras + 1) return;          // no room for a second MCB
        const Block tail{static_cast<uint16_t>(mcb + 1 + paras),
                         static_cast<uint16_t>(blocks_[i].paras - paras - 1), 0, false};
        blocks_[i].paras = paras;
        blocks_.insert(blocks_.begin() + i + 1, tail);
        return;
    }
}

// Claim [seg, seg+paras) for whoever is asking, splitting whatever free block contains
// it. Used for a program's own block at load and for where an overlay lands.
void Dos::mem_own(uint16_t seg, uint16_t paras) {
    if (!paras) return;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const uint32_t lo = blocks_[i].seg + 1, hi = lo + blocks_[i].paras;
        if (seg < lo || seg >= hi) continue;
        if (blocks_[i].used) return;                        // already owned; leave it alone
        if (seg > lo) {                                     // keep the gap below as free
            mem_split(blocks_[i].seg, static_cast<uint16_t>(seg - 1 - blocks_[i].seg - 1));
            ++i;
            if (i >= blocks_.size() || blocks_[i].seg + 1 != seg) return;
        }
        if (blocks_[i].paras > paras) mem_split(blocks_[i].seg, paras);
        blocks_[i].used = true; blocks_[i].owner = psp_seg;
        mem_coalesce();
        mem_publish();
        return;
    }
}

// Only free neighbours merge, and the merged block absorbs the paragraph the second
// MCB used to occupy. Two adjacent *used* blocks are two separate allocations and the
// boundary between them is the whole point: merging them loses the one thing AH=49h and
// AH=4Ah need, which is where each block begins.
void Dos::mem_coalesce() {
    for (size_t i = 1; i < blocks_.size();) {
        const Block& p = blocks_[i - 1];
        if (!blocks_[i].used && !p.used && p.seg + 1 + p.paras == blocks_[i].seg) {
            blocks_[i - 1].paras = static_cast<uint16_t>(p.paras + 1 + blocks_[i].paras);
            blocks_.erase(blocks_.begin() + i);
        } else ++i;
    }
}

uint16_t Dos::mem_largest() const {
    uint16_t best = 0;
    for (const Block& b : blocks_) if (!b.used && b.paras > best) best = b.paras;
    return best;
}

uint16_t Dos::mem_alloc(uint16_t paras) { return mem_alloc_at(paras, false); }

// `biggest` carves out of the largest free block instead of the first one that fits, so
// the caller lands immediately below whatever takes the rest of that block. That is where
// DOS puts a child's environment — directly under the PSP — and the difference is not
// cosmetic. First fit put every environment at the bottom of the arena, and freeing them
// as children exited left one-paragraph holes down among the interrupt stubs. DOS/4GW
// sizes itself by allocating one paragraph and growing it to the maximum, repeatedly, so
// it collected those holes: it came away with a 928-byte block at 0x00A5 as its
// conventional-memory transfer buffer, wrote DOS call arguments past the end of it, and
// died with its error message unreadable. Keeping the arena's low end whole makes the
// nested case look like the standalone one, which always worked.
uint16_t Dos::mem_alloc_at(uint16_t paras, bool biggest) {
    if (!paras) paras = 1;
    size_t pick = blocks_.size();
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].used || blocks_[i].paras < paras) continue;
        if (!biggest) { pick = i; break; }
        if (pick == blocks_.size() || blocks_[i].paras > blocks_[pick].paras) pick = i;
    }
    if (pick == blocks_.size()) return 0;
    const uint16_t seg = static_cast<uint16_t>(blocks_[pick].seg + 1);
    if (blocks_[pick].paras > paras) mem_split(blocks_[pick].seg, paras);
    blocks_[pick].used = true; blocks_[pick].owner = psp_seg;
    mem_publish();
    return seg;
}

bool Dos::mem_free(uint16_t seg) {
    const size_t i = mem_find(seg);
    if (i == blocks_.size() || !blocks_[i].used) return false;
    blocks_[i].used = false;
    mem_coalesce();
    mem_publish();
    return true;
}

bool Dos::mem_resize(uint16_t seg, uint16_t paras) {
    size_t i = mem_find(seg);
    if (i == blocks_.size()) {
        // A block the guest asserts ownership of without having asked us — a stub that
        // relocated itself inside its own block, most often. Taking the assertion at
        // face value is closer to DOS than refusing it.
        mem_own(seg, 1);
        i = mem_find(seg);
        if (i == blocks_.size()) return false;
    }
    const uint16_t have = blocks_[i].paras;
    if (paras <= have) {                                    // shrink: the tail comes back
        if (paras < have) { mem_split(blocks_[i].seg, paras); mem_coalesce(); mem_publish(); }
        return true;
    }
    // Grow, only into free space immediately above, and by *extending this block* rather
    // than marking the range used. Marking left the new paragraphs as a second used
    // block butted against the first, so the next grow found its own tail in the way:
    // LSI C's driver creeps its block up one paragraph at a time and got "out of memory"
    // after exactly two. Each absorbed neighbour also gives back its MCB paragraph.
    uint32_t avail = have;
    size_t k = i + 1;
    for (; k < blocks_.size() && !blocks_[k].used; ++k) avail += 1u + blocks_[k].paras;
    if (avail < paras) return false;
    blocks_[i].paras = static_cast<uint16_t>(avail);
    blocks_.erase(blocks_.begin() + i + 1, blocks_.begin() + k);
    if (blocks_[i].paras > paras) mem_split(blocks_[i].seg, paras);
    mem_coalesce();
    mem_publish();
    return true;
}

// The chain DOS/4GW walks: 16 bytes per block at the paragraph below it — 'M' for a link
// and 'Z' for the last, the owning PSP (0 when free), the size in paragraphs, and the
// program name DOS 4 added. The first MCB's segment is published at LoL-2 by AH=52h.
//
// A *partial* chain is worse than none: one fake MCB for the environment block alone was
// tried early on and left FreeCOM answering "Bad command or filename" to everything,
// because a walker that finds a signature follows it to the end. Every block gets one or
// no block does.
void Dos::mem_publish() {
    static unsigned long long calls = 0;
    if (++calls % 100000 == 0)
        std::fprintf(stderr, "[dbg] mem_publish %llu calls, %u blocks\n", calls,
                     static_cast<unsigned>(blocks_.size()));
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        mem_.wb(b.seg, 0, i + 1 == blocks_.size() ? 'Z' : 'M');
        mem_.ww(b.seg, 1, b.used ? b.owner : 0);
        mem_.ww(b.seg, 3, b.paras);
        for (int j = 5; j < 8; ++j) mem_.wb(b.seg, j, 0);
        for (int j = 0; j < 8; ++j) mem_.wb(b.seg, 8 + j, 0);
    }
}

void Dos::mem_dump(const char* why) const {
    std::fprintf(stderr, "[mem] %s:", why);
    for (const Block& b : blocks_)
        std::fprintf(stderr, " %04X+%04X%s", b.seg + 1, b.paras, b.used ? "*" : "");
    std::fputc('\n', stderr);
}
void Dos::terminate(int code) {
    if (exec_depth_ > 0) { child_exited_ = true; child_code_ = code; return; }  // end the child only
    cpu_.exit_code = code;
    cpu_.halted = true;
}

// AH=4Bh AL=03 — load an overlay. The image goes where the caller says, relocated by the
// factor it supplies, with no PSP and no transfer of control: the caller has already
// sized its own memory block and will far-call in itself. This is how a DOS extender's
// stub brings in the extender. Watcom's `wlink` is stubbed for DOS/4GW and loads
// `dos4gw.exe` exactly this way, so without it the linker cannot start at all — and
// "load and execute" is not a substitute, because a second PSP is the one thing the
// stub does not want.
bool Dos::load_overlay(const std::string& name, uint16_t pb_seg, uint16_t pb_off) {
    std::string host = files_.host_path(name);
    std::FILE* fp = std::fopen(host.c_str(), "rb");
    if (!fp) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> f(n > 0 ? n : 0);
    if (n > 0 && std::fread(f.data(), 1, n, fp) != static_cast<size_t>(n)) f.clear();
    std::fclose(fp);
    if (f.empty()) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }

    const uint16_t load_seg = mem_.rw(pb_seg, pb_off);          // where it goes
    const uint16_t factor   = mem_.rw(pb_seg, pb_off + 2);      // what to add to each fixup
    auto rd16 = [&](size_t o) {
        return static_cast<uint16_t>(o + 1 < f.size() ? (f[o] | (f[o + 1] << 8)) : 0);
    };

    uint32_t loaded = 0;
    if (f.size() >= 2 && f[0] == 'M' && f[1] == 'Z') {
        const uint16_t bytes_last = rd16(2), pages = rd16(4), nreloc = rd16(6);
        const uint16_t hdr_paras = rd16(8), reloc_off = rd16(24);
        const uint32_t hdr_bytes = static_cast<uint32_t>(hdr_paras) * 16;
        uint32_t image_bytes = static_cast<uint32_t>(pages) * 512;
        if (bytes_last) image_bytes = image_bytes - 512 + bytes_last;
        if (image_bytes > f.size()) image_bytes = static_cast<uint32_t>(f.size());
        if (hdr_bytes > image_bytes) { cpu_.flags |= CF; cpu_.r[AX] = 11; return true; }
        loaded = image_bytes - hdr_bytes;
        mem_.write(Memory::phys(load_seg, 0), f.data() + hdr_bytes, loaded);
        for (uint16_t i = 0; i < nreloc; ++i) {
            const uint16_t ro = rd16(reloc_off + i * 4);
            const uint16_t rs = rd16(reloc_off + i * 4 + 2);
            mem_.ww(load_seg + rs, ro, static_cast<uint16_t>(mem_.rw(load_seg + rs, ro) + factor));
        }
    } else {
        loaded = static_cast<uint32_t>(f.size());
        mem_.write(Memory::phys(load_seg, 0), f.data(), loaded);
    }
    // Where an overlay lands is memory the caller owns; record that, so a later AH=48h
    // does not hand out the middle of the extender we just put there.
    mem_own(load_seg, static_cast<uint16_t>((loaded + 15) / 16));
    if (trace) std::fprintf(stderr, "[overlay] %s -> %04X:0000, %u bytes, factor %04X\n",
                            name.c_str(), load_seg, loaded, factor);
    cpu_.flags &= ~CF;
    return true;
}

// INT 21h AH=4Bh AL=0: load and run a child program to completion, then return to
// the parent. The child runs on the same CPU through a nested step loop; the
// parent's registers, PSP and heap mark are saved and restored around it.
bool Dos::exec(const std::string& name, uint16_t pb_seg, uint16_t pb_off) {
    std::string host = files_.host_path(name);
    std::FILE* fp = std::fopen(host.c_str(), "rb");
    if (!fp) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }   // file not found
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> file(n > 0 ? n : 0);
    if (n > 0) { if (std::fread(file.data(), 1, n, fp) != (size_t)n) file.clear(); }
    std::fclose(fp);
    if (file.empty()) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }

    // Parameter block: [0]=env seg, [2..5]=cmdline far ptr (offset,segment).
    uint16_t cmd_off = mem_.rw(pb_seg, pb_off + 2);
    uint16_t cmd_seg = mem_.rw(pb_seg, pb_off + 4);
    uint8_t taillen = mem_.rb(cmd_seg, cmd_off);
    std::string tail;
    for (uint8_t i = 0; i < taillen; ++i) { char c = static_cast<char>(mem_.rb(cmd_seg, cmd_off + 1 + i)); if (c == '\r') break; tail += c; }

    // Save the parent's whole context. Cpu::save() rather than a hand-rolled copy of
    // r[] and sreg[]: once the parent can be a protected-mode program, cr0, the
    // descriptor cache, cs_d/ss_d and ip_mask decide how those registers are even
    // read. A child that switches to protected mode and exits would otherwise leave
    // the parent running with the child's addressing.
    const Cpu::State parent = cpu_.save();
    uint16_t spsp = psp_seg, senv = env_seg;
    const std::vector<Block> sblocks = blocks_;   // the child's allocations unwind with it
    const std::string sname = prog_path;
    // The DTA and the directory search running through it belong to the *process*: on DOS
    // the search state lives in the 21 reserved bytes of the DTA, so a child cannot
    // disturb its parent's FindFirst/FindNext no matter what it searches for. Ours is one
    // shared vector, and leaving it shared cost `wcl hello.c`: wcl expands its file
    // arguments with FindFirst, runs wcc, and calls FindNext — and got the last thing the
    // child had looked for, which is how it came to try and compile `_COMDEF.H`.
    const uint16_t sdta_seg = dta_seg_, sdta_off = dta_off_;
    const std::vector<Found> sfind = find_;
    const size_t sfind_pos = find_pos_;

    // The parameter block's environment word: a segment to use as is, or 0 meaning
    // "inherit". DOS inherits by *copying* the parent's strings into a new block and
    // appending the child's own path, which is where a C runtime finds argv[0].
    //
    // Allocated *before* the child's own block, and that order is the point. DOS builds
    // the environment first, so it sits below the PSP and outside the arena the child
    // owns; we used to put it just above, which reads correctly and is still wrong. The
    // moment a child asks `AH=4Ah` to grow into all the memory it has been told it owns
    // — W32RUN asks for 0x6F00 paragraphs immediately — it zeroes its own environment
    // and then cannot find the variable that says what to load. It looked like our
    // environment block was empty; it was overwritten by its reader.
    uint16_t child_env = mem_.rw(pb_seg, pb_off);
    if (!child_env) child_env = make_child_env(senv, name);
    else               child_env = stamp_env_path(child_env, name);

    // The child gets everything that is free, which is what DOS does: it hands the
    // program the largest block and lets it shrink to what it needs. A fixed 96 KiB was
    // enough for LSI C's passes and nothing else.
    const uint16_t child_paras = mem_largest();
    const uint16_t child_psp = mem_alloc(child_paras);
    if (!child_psp) { cpu_.flags |= CF; cpu_.r[AX] = 8; return true; }

    std::string err;
    if (!load_program(file, cpu_, child_psp, tail, err, name, child_env)) {
        cpu_.restore(parent); psp_seg = spsp; blocks_ = sblocks; env_seg = senv; prog_path = sname;
        dta_seg_ = sdta_seg; dta_off_ = sdta_off; find_ = sfind; find_pos_ = sfind_pos;
        cpu_.flags |= CF; cpu_.r[AX] = 2; return true;
    }
    init_psp(child_psp, spsp, name);  // the child's parent is us
    psp_seg = child_psp;
    dta_seg_ = child_psp; dta_off_ = 0x80;   // a program starts with its own PSP:80h as DTA

    // Run the child to completion.
    bool saved_exited = child_exited_; child_exited_ = false;
    ++exec_depth_;
    while (!child_exited_ && !cpu_.halted) cpu_.step();
    --exec_depth_;
    int code = child_code_;
    child_exited_ = saved_exited;

    // Restore the parent and hand it the child's exit code (via AH=4Dh).
    cpu_.restore(parent); psp_seg = spsp; blocks_ = sblocks; env_seg = senv; prog_path = sname;
    dta_seg_ = sdta_seg; dta_off_ = sdta_off; find_ = sfind; find_pos_ = sfind_pos;
    last_child_code_ = code;
    cpu_.flags &= ~CF; cpu_.r[AX] = 0;
    return true;
}

// DOS 8.3 wildcard match (case-insensitive) over "NAME.EXT".
static bool wildmatch(const std::string& pat, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    auto up = [](char c){ return (char)std::toupper((unsigned char)c); };
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || up(pat[p]) == up(name[n]))) { ++p; ++n; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
        else if (star != std::string::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// DOS 8.3 match: the name and extension are matched separately, so "*.*" (and a
// bare "*") match names with no extension too, which a plain glob would not.
static bool dos83match(const std::string& pat, const std::string& name) {
    auto split = [](const std::string& s, std::string& nm, std::string& ext) {
        size_t d = s.find('.');
        if (d == std::string::npos) { nm = s; ext.clear(); }
        else { nm = s.substr(0, d); ext = s.substr(d + 1); }
    };
    std::string pn, pe, nn, ne;
    split(pat, pn, pe); split(name, nn, ne);
    if (pat.find('.') == std::string::npos) pe = "*";   // "FOO" / "*" also match any extension
    return wildmatch(pn, nn) && wildmatch(pe, ne);
}

bool Dos::find_first(const std::string& spec, uint16_t attr) {
    find_.clear(); find_pos_ = 0;
    std::string dir = spec, pat = "*";
    size_t s = spec.find_last_of("/\\:");
    if (s != std::string::npos) { pat = spec.substr(s + 1); dir = spec.substr(0, s + 1); }
    else { pat = spec; dir = ""; }
    if (pat.empty()) pat = "*";
    const uint16_t date = kFixedDate, time = kFixedTime;
    bool wantDirs = (attr & 0x10) != 0;
    // "." and ".." in a real (non-root) subdirectory when directories are requested
    if (wantDirs && files_.normalize(dir.empty() ? "." : dir) != "\\") {
        find_.push_back({".", 0, true, date, time});
        find_.push_back({"..", 0, true, date, time});
    }
    std::string host = files_.host_path(dir.empty() ? "." : dir);
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(host, ec)) {
        bool is_dir = e.is_directory(ec);
        if (is_dir && !wantDirs) continue;             // dirs only when asked for (attr & 0x10)
        std::string dosname = e.path().filename().string();
        for (char& c : dosname) c = (char)std::toupper((unsigned char)c);
        if (!dos83match(pat, dosname)) continue;
        Found f; f.name = dosname; f.is_dir = is_dir;
        f.size = is_dir ? 0 : (uint32_t)e.file_size(ec);
        f.date = date; f.time = time;
        find_.push_back(f);
    }
    return !find_.empty();
}

void Dos::write_dta_entry() {
    const Found& f = find_[find_pos_++];
    uint16_t s = dta_seg_, o = dta_off_;
    for (int i = 0; i < 21; ++i) mem_.wb(s, o + i, 0);           // reserved (search state)
    mem_.wb(s, o + 21, f.is_dir ? 0x10 : 0x20);                 // attribute
    mem_.ww(s, o + 22, f.time);
    mem_.ww(s, o + 24, f.date);
    mem_.w16(Memory::phys(s, o + 26), f.size & 0xFFFF);
    mem_.w16(Memory::phys(s, o + 28), f.size >> 16);
    std::string nm = f.name; if (nm.size() > 12) nm.resize(12);
    for (size_t i = 0; i < nm.size(); ++i) mem_.wb(s, o + 30 + i, nm[i]);
    mem_.wb(s, o + 30 + nm.size(), 0);
}

// A protected-mode client is entitled to issue INT 21h directly and have the host reflect
// it — that is in the DPMI spec, and DOS/4GW does it for everything. Our DOS layer reads
// its arguments as segment:offset, so a selector in DS or ES addresses a paragraph 16
// times too low: DOS/4GW's error message came out as two bytes of noise, which is the
// worst possible outcome, because the message was the thing that would have said what was
// wrong.
//
// So translate, for the duration of the call, the selectors the layer is about to read
// into the paragraphs they alias. A host can only do this for conventional memory —
// a base above 1 MiB has no paragraph — and that is not a limitation we invented: a
// client wanting DOS to read its buffer has to put the buffer where DOS can reach it.
struct SegAlias {
    Cpu& cpu; int idx; uint16_t sel; bool swapped = false;
    // `apply` is false for INT 31h, and that is not tidiness. The DPMI host reads its own
    // arguments through Cpu::lin() with the real selector, so it never needed the alias —
    // and, decisively, a host service may *transfer control*: 0301h/0302h set the machine
    // up for a real-mode procedure and return, expecting the guest to run next. Unwinding
    // an alias on the way out then puts a selector back into a segment register whose base
    // has just been set for real mode, leaving DS = 003F with base 0x1100. That is a
    // segment register disagreeing with its own cache, and it is what made the Watcom
    // linker print its error message out of the wrong address for a day.
    SegAlias(Cpu& c, int i, bool apply) : cpu(c), idx(i), sel(c.sreg[i]) {
        if (!apply || !cpu.pe()) return;
        const uint32_t base = cpu.sbase[idx];
        if (base >= 0x100000 || (base & 0xF)) return;
        cpu.sreg[idx] = static_cast<uint16_t>(base >> 4);
        swapped = true;
    }
    ~SegAlias() {
        // Only put the selector back if the handler did not deliberately set this
        // register itself (AH=35h and AH=52h both return through ES).
        if (swapped && cpu.sreg[idx] == static_cast<uint16_t>(cpu.sbase[idx] >> 4))
            cpu.sreg[idx] = sel;
    }
};

// Answering an interrupt whose result is in the flags, from inside the stub.
//
// A resident program that chains to a BIOS service -- `jmp far [old_int16]` -- has
// pushed nothing; the frame on the stack is the *caller's*, from its own INT, and
// the RET at the end of our stub is an IRET that pops those flags back. So a ZF
// set here would be thrown away between the answer and the asker. The real BIOS
// has the same problem and the same answer: write the flags into the saved copy.
// MNfer.sys chains INT 16h AH=11h this way and JW_CAD's key poll reads the result
// as "a key is waiting" for ever -- it never draws a thing.
struct StubFlags {
    Cpu& cpu; Memory& mem; bool in_stub;
    StubFlags(Cpu& c, Memory& m, bool s) : cpu(c), mem(m), in_stub(s) {}
    ~StubFlags() {
        if (in_stub) mem.ww(cpu.sreg[SS], static_cast<uint16_t>(cpu.r[SP] + 4),
                            static_cast<uint16_t>(cpu.flags));
    }
};

bool Dos::handle(uint8_t n) {
    // A protected-mode client that hooked this vector gets it first. Its handler may
    // service the call itself, or translate the arguments and pass it down through the
    // host's default handler; either way, going straight to the DOS layer would skip
    // work the client is relying on.
    if (cpu_.pe() && dpmi_.pm_int(n)) return true;
    // The same courtesy in real mode: a vector the guest re-pointed (AH=25h or a
    // direct IVT write) is the guest's own handler, and the CPU must dispatch through
    // the table exactly as hardware would. Turbo C++'s TCC is a VROOMM-overlaid
    // program — every inter-overlay call is `INT 3Fh` into the manager it installed,
    // and servicing that as a no-op falls through into the stub's *data*. The default
    // stubs at 0050:n*4 re-enter `INT n`, so when one of those is what is executing
    // (a handler chaining to the saved vector lands there), fall through to the
    // built-in layer instead of going around the table for ever. Returning false
    // hands the INT to the CPU's own IVT dispatch.
    if (!cpu_.pe() && cpu_.sreg[CS] != kIvtStubSeg) {
        const uint16_t vo = mem_.r16(n * 4), vs = mem_.r16(n * 4 + 2);
        if ((vs || vo) && (vs != kIvtStubSeg || vo != n * 4)) return false;
    }
    SegAlias ads(cpu_, DS, n != 0x31), aes(cpu_, ES, n != 0x31);
    StubFlags sf(cpu_, mem_, !cpu_.pe() && cpu_.sreg[CS] == kIvtStubSeg);
    switch (n) {
        // A CPU exception, not a service call. Nothing here can handle one, but
        // silently returning would leave the guest running on the garbage that caused
        // it -- so say so. Under a real DPMI host these become SIGFPE and friends.
        case 0x00: case 0x01: case 0x03: case 0x04: case 0x06: case 0x0C: case 0x0D:
            std::fprintf(stderr, "[cpu] exception INT %02Xh at %04X:%08X (unhandled)\n",
                         n, cpu_.sreg[CS], cpu_.ip);
            return true;
        case 0x20: terminate(0); return true;           // terminate program
        case 0x21: {
            // Nothing a guest can pass may take the emulator down. The DOS layer reaches the
            // host filesystem, and a filename made of machine code (which is what a guest
            // that has jumped into data passes) can throw out of <filesystem> rather than
            // simply failing. A DOS call that goes wrong returns an error; it does not exit.
            // CpuError is deliberately not caught here: that is the emulator's own report of
            // an instruction it cannot run, and it belongs to main().
            try {
                if (!trace) return int21();
            const uint16_t ax = cpu_.r[AX], bx = cpu_.r[BX], cx = cpu_.r[CX], dx = cpu_.r[DX];
            const uint16_t es = cpu_.sreg[ES], ds0 = cpu_.sreg[DS];   // captured at entry: a
            // handler may legitimately change them, and printing the value afterwards made a
            // reflected call look as if it had read from the wrong segment when it had not.
            // For a protected-mode caller the 16-bit pair says nothing: what the layer
            // will actually read is the descriptor base plus the *32-bit* offset, and
            // whether those two agree is the whole question.
            const bool pm = cpu_.pe();
            const uint32_t edx = cpu_.gd(DX), dsb = cpu_.sbase[DS];
            // The calls that carry a string say far more than their registers do.
            const uint8_t ah_ = ax >> 8;
            if (ah_ == 0x3D || ah_ == 0x3C || ah_ == 0x41 || ah_ == 0x43 || ah_ == 0x4E ||
                ah_ == 0x4B || ah_ == 0x60 || ah_ == 0x39 || ah_ == 0x3B || ah_ == 0x5B) {
                std::fprintf(stderr, "[int21]   path \"%s\"", read_asciiz(cpu_.sreg[DS], dx).c_str());
                if (ah_ == 0x4B) {                       // ...and EXEC's command tail
                    const uint16_t co = mem_.rw(cpu_.sreg[ES], bx + 2);
                    const uint16_t cs = mem_.rw(cpu_.sreg[ES], bx + 4);
                    const uint8_t n = mem_.rb(cs, co);
                    std::fprintf(stderr, "  tail(%u) \"", n);
                    for (uint8_t i = 0; i < n && i < 90; ++i)
                        std::fputc(mem_.rb(cs, static_cast<uint16_t>(co + 1 + i)), stderr);
                    std::fprintf(stderr, "\"  env=%04X", mem_.rw(cpu_.sreg[ES], bx));
                }
                std::fputc('\n', stderr);
            }
            const bool r = int21();
            std::fprintf(stderr,
                "[int21]%llu ah=%02X al=%02X bx=%04X cx=%04X ds:dx=%04X:%04X es=%04X -> %s ax=%04X bx=%04X"
                "  at %04X:%08X\n",
                (unsigned long long)cpu_.insns, ax >> 8, ax & 0xFF, bx, cx, ds0, dx, es,
                (cpu_.flags & CF) ? "CF" : "ok", cpu_.r[AX], cpu_.r[BX],
                cpu_.sreg[CS], cpu_.ip);
            if (pm) std::fprintf(stderr, "[int21]   pm: ds base=%08X edx=%08X -> %08X\n",
                                 dsb, edx, dsb + edx);
            return r;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[dos] INT 21h AH=%02Xh failed: %s\n", cpu_.r[AX] >> 8, e.what());
                cpu_.flags |= CF; cpu_.r[AX] = 2;            // file not found, the DOS answer
                return true;
            }
        }
        case 0x16: {                                     // BIOS keyboard services
            // Who takes a key, and who only looks. With a FEP installed there
            // are two readers of the same ring -- the application and the
            // driver sitting in front of it -- and when a keystroke disappears
            // the only useful question is which of them had it last.
            static const bool kbd_trace = getenv("DOSEMU_KBD_TRACE") != nullptr;
            uint8_t ah = cpu_.r[AX] >> 8;
            if (trace) std::fprintf(stderr, "[int16] ah=%02X at %04X:%08X\n",
                                    ah, cpu_.sreg[CS], cpu_.ip);
            if (ah == 0x00 || ah == 0x10) {              // read a key -> AL=ascii, AH=scancode
                if (ime_.on()) {                         // our own FEP is holding the keyboard
                    const int c = host_getch();
                    cpu_.r[AX] = static_cast<uint16_t>(c < 0 ? 0 : ((c ? 0x1C : 0) << 8) | (c & 0xFF));
                    return true;
                }
                const int k = kbd_pop(true);
                if (kbd_trace) std::fprintf(stderr, "[kbd] ah=%02X take=%d\n", ah, k);
                if (k >= 0) { cpu_.r[AX] = static_cast<uint16_t>(k); return true; }
                // host_getch, not getch: this is the BIOS, *below* whatever holds
                // CON. A FEP reads the keyboard here while it is answering the
                // console read above it, and going back through CON would be the
                // driver calling itself.
                int c = host_getch(); if (c < 0) c = 0x1A;
                cpu_.r[AX] = (static_cast<uint16_t>(c ? 0x1C : 0) << 8) | (c & 0xFF);
            } else if (ah == 0x01 || ah == 0x11) {       // key available? ZF=1 means no
                pump_ime();
                if (ime_.on()) {
                    if (ime_.has_out() || pushback_ >= 0) { cpu_.flags &= ~ZF; cpu_.r[AX] = 0x1C0D; }
                    else cpu_.flags |= ZF;
                    return true;
                }
                const int k = kbd_pop(false);
                if (kbd_trace) std::fprintf(stderr, "[kbd] ah=%02X peek=%d from %04X:%04X\n",
                                            ah, k, cpu_.sreg[CS], (uint16_t)cpu_.ip);
                if (k >= 0) { cpu_.flags &= ~ZF; cpu_.r[AX] = static_cast<uint16_t>(k); return true; }
                // This used to answer "yes, Enter is waiting" unconditionally, which is
                // the same shape of lie as every other bug on this project: a caller that
                // believes it then reads the key, and the read blocks on a stdin nobody is
                // typing into. DOS/4GW polls the keyboard during startup and hung there
                // with no output at all — the hardest kind of hang to find, because a
                // process asleep in a read looks identical to a process wedged in a loop.
                //
                // Nobody can answer this truthfully without a way to ask whether input is
                // waiting, so that is a hook the host supplies; unset, the answer is no.
                // The DOS input calls (AH=01/07/08/0Ah) are unaffected — they block, which
                // is what they are for.
                const bool ready = input_ready && input_ready();
                if (ready) { cpu_.flags &= ~ZF; cpu_.r[AX] = 0x1C0D; }
                else       { cpu_.flags |= ZF; }
            } else if (ah == 0x02 || ah == 0x12) {       // shift status, plain and extended
                // JW_CAD asks for this between every pair of mouse polls: the
                // modifier held while the button goes down is what turns a press
                // into 読取 with [SHIFT], [GRPH] or [CTRL], and 複写's 「基準 位置」
                // refuses a press that arrives with one held. AH=12h used to fall
                // through untouched, which handed the guest whatever its own int86
                // buffer held -- Ctrl, as it happened, on every single press.
                cpu_.sb(AX, kbd_flags_);
                if (ah == 0x12) cpu_.r[AX] = static_cast<uint16_t>(
                    (static_cast<uint16_t>(kbd_flags2_) << 8) | kbd_flags_);
            }
            return true;
        }
        case 0x2F:                                       // DOS multiplex
            if (dpmi_.int2f()) return true;               // AX=1687h: yes, DPMI is here
            if (cpu_.r[AX] == 0xAE00) cpu_.sb(AX, 0);     // no installable-command extension
            return true;
        case 0x31:                                       // DPMI services (protected mode)
            return dpmi_.int31();
        case 0x10:                                       // BIOS video
            return int10();
        case 0x15:                                       // BIOS misc / DOS-V fonts
            return int15();
        case 0x33:                                       // mouse driver
            return int33();
        case kFontIntAnk: return font_fetch(false);
        case kFontIntKanji: return font_fetch(true);
        case kDevStrat: return device_int(true);
        case kDevInt: return device_int(false);
        case 0x1A:                                       // BIOS time
            if ((cpu_.r[AX] >> 8) == 0) {
                // AH=0: the tick count since midnight, 18.2 a second.
                const uint64_t t = hundredths() * 182 / 1000;

                cpu_.r[CX] = static_cast<uint16_t>((t >> 16) & 0xFFFF);
                cpu_.r[DX] = static_cast<uint16_t>(t & 0xFFFF);
                cpu_.sb(AX, 0);                          // no midnight yet
            }
            return true;
        default:
            return int21_default(n);
    }
}

// The DOS/V font interface.
//
// A DOS/V program does not carry glyphs; it asks the display driver for the
// address of a routine that copies one, and calls that. JW_CAD does it twice
// at startup -- 16x16 for kanji and 8x16 for the single-byte set -- and then
// calls back with ES:SI pointing at a buffer and CX holding the character
// code. Without this the pointers it stores are nothing, calling through them
// runs into whatever is at that address, and the Microsoft C runtime stops
// with "R6003 - integer divide by 0" the moment the first character is drawn.
//
// What it gets back is three bytes of real code at a fixed address: an INT
// that lands here, and a RETF. The fetch itself is done in C++ against a
// FONTX2 file, which is the format DOS/V fonts come in.
void Dos::install_font_stubs() {
    mem_.wb(kFontStubSeg, kFontStubOff + 0, 0xCD);
    mem_.wb(kFontStubSeg, kFontStubOff + 1, kFontIntAnk);
    mem_.wb(kFontStubSeg, kFontStubOff + 2, 0xCB);            // RETF
    mem_.wb(kFontStubSeg, kFontStubOff + 4, 0xCD);
    mem_.wb(kFontStubSeg, kFontStubOff + 5, kFontIntKanji);
    mem_.wb(kFontStubSeg, kFontStubOff + 6, 0xCB);
}

bool Dos::load_fontx(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 18) { out.resize((size_t)n); if (std::fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear(); }
    std::fclose(f);
    return out.size() > 18 && std::memcmp(out.data(), "FONTX2", 6) == 0;
}

// FONTX2: a 17-byte header, then either 256 images in code order (single byte)
// or a table of code blocks followed by the images of just the codes present.
//
// The guest asks for a glyph through the font interrupt below; the FEP's own
// window (src/ime.cpp) draws with the same fonts and the same lookup, which is
// why this is a free function and not part of that.
const uint8_t* fontx_glyph(const std::vector<uint8_t>& f, uint16_t code, int& w, int& h) {
    w = h = 0;
    if (f.size() < 18) return nullptr;
    w = f[14]; h = f[15];
    const long size = (w + 7) / 8 * h;
    long at = -1;
    if (!f[16]) {
        if (code < 256) at = 17 + code * size;
    } else {
        const int nb = f[17];
        long before = 0;
        for (int i = 0; i < nb; ++i) {
            const uint16_t lo = (uint16_t)(f[18 + i * 4] | (f[19 + i * 4] << 8));
            const uint16_t hi = (uint16_t)(f[20 + i * 4] | (f[21 + i * 4] << 8));
            if (code >= lo && code <= hi) { at = 18 + 4L * nb + (before + code - lo) * size; break; }
            before += hi - lo + 1;
        }
    }
    if (at < 0 || at + size > (long)f.size()) return nullptr;
    return f.data() + at;
}

bool Dos::font_fetch(bool dbcs) {
    static const bool tr = getenv("DOSEMU_FONT_TRACE") != nullptr;
    if (tr) std::fprintf(stderr, "[font]%s %04X -> %04X:%04X\n", dbcs ? "16" : " 8",
                         cpu_.r[CX], cpu_.sreg[ES], cpu_.r[SI]);
    const std::vector<uint8_t>& f = dbcs ? font_kanji_ : font_ank_;
    int w = 0, h = 0;
    const uint8_t* g = fontx_glyph(f, cpu_.r[CX], w, h);
    const long size = (w + 7) / 8 * h;
    if (!g) {
        // No glyph for that code. A real DOS/V driver would hand one back; ours
        // has only what the FONTX2 file holds, and the Shinonome fonts carry no
        // NEC row 13 -- the circled digits JW_CAD writes along its top line.
        // Give back a blank rather than leaving whatever the caller's buffer
        // held from the last character: stale bytes put a different wrong glyph
        // on the screen every time, and then a port that draws nothing there
        // looks wrong for a reason that has nothing to do with the port.
        for (long i = 0; i < size; ++i)
            mem_.wb(cpu_.sreg[ES], (uint16_t)(cpu_.r[SI] + i), 0);
        return true;
    }
    for (long i = 0; i < size; ++i)
        mem_.wb(cpu_.sreg[ES], (uint16_t)(cpu_.r[SI] + i), g[i]);
    return true;
}

// The mouse.
//
// JW_CAD does not ask whether a mouse is there -- it requires one. Overlay 8
// calls INT 33h function 0 (reset), and if AX comes back zero it restores the
// text mode, prints "mouse driver not installed" and exits(1). That is where
// every run ended before this: 6.6 million instructions of loading overlays
// and reading its configuration, and then out, with nothing on the screen.
//
// So the driver is always installed. What it reports is a mouse sitting wherever
// the host last put it with mouse_move()/mouse_button() -- nowhere, for a run
// that only wants the opening screen, and wherever a script says for one that
// drives the program.
//
// Positions here are the driver's "virtual" coordinates. In mode 12h those are
// the 640x480 pixels one for one, and JW_CAD sets its own range (functions 7
// and 8) before it ever reads a position, so the range this starts with only
// has to be sane, not authentic.
// The modifier keys the guest can ask about at any moment. Kept here *and* in
// the BIOS data area, because a program is free to read either: INT 16h is the
// documented way and 0040:0017 is the one every DOS program eventually pokes at.
void Dos::set_mods(uint8_t flags) {
    kbd_flags_ = flags;
    // The extended byte's Ctrl and Alt bits are the "right-hand" ones; with no
    // way to tell the two sides apart, report neither and leave the locks alone.
    kbd_flags2_ = 0;
    mem_.wb(0x0040, 0x0017, kbd_flags_);
    mem_.wb(0x0040, 0x0018, kbd_flags2_);
}

void Dos::mouse_move(int16_t x, int16_t y) {
    if (x < mouse_.min_x) x = mouse_.min_x;
    if (x > mouse_.max_x) x = mouse_.max_x;
    if (y < mouse_.min_y) y = mouse_.min_y;
    if (y > mouse_.max_y) y = mouse_.max_y;
    mouse_.dx = static_cast<int16_t>(mouse_.dx + (x - mouse_.x));
    mouse_.dy = static_cast<int16_t>(mouse_.dy + (y - mouse_.y));
    mouse_.x = x; mouse_.y = y;
}

// A press and a release are counted, not just recorded: JW_CAD reads function 5
// (and 6) and acts on the *count*, so a click that happens between two polls
// still has to be seen. The counters are cleared by the read, as a real driver
// clears them.
void Dos::mouse_button(int button, bool down) {
    if (button < 0 || button > 2) return;
    const uint16_t bit = static_cast<uint16_t>(1u << button);
    if (down == ((mouse_.buttons & bit) != 0)) return;
    if (down) {
        mouse_.buttons |= bit;
        ++mouse_.press[button].count;
        mouse_.press[button].x = mouse_.x; mouse_.press[button].y = mouse_.y;
    } else {
        mouse_.buttons = static_cast<uint16_t>(mouse_.buttons & ~bit);
        ++mouse_.release[button].count;
        mouse_.release[button].x = mouse_.x; mouse_.release[button].y = mouse_.y;
    }
}

// One byte of a scripted keystroke, or -1 when the queue is empty. DOS reads bytes,
// the BIOS reads whole keys: a key with no ASCII (a function key, an arrow) is handed
// to a DOS read as 0x00 followed by its scan code.
int Dos::next_key_byte() {
    if (pushback_ >= 0) { const int k = pushback_; pushback_ = -1; return k; }
    if (pending_scan_ >= 0) { const int sc = pending_scan_; pending_scan_ = -1; return sc; }
    const int got = kbd_pop(true);
    if (got < 0) return -1;
    const uint16_t k = static_cast<uint16_t>(got);
    if ((k & 0xFF) == 0) { pending_scan_ = k >> 8; return 0; }
    return k & 0xFF;
}

bool Dos::int33() {
    const uint16_t fn = cpu_.r[AX];
    static const bool tr = getenv("DOSEMU_MOUSE_TRACE") != nullptr;
    if (tr) std::fprintf(stderr, "[mouse] fn=%04X bx=%04X cx=%04X dx=%04X at %04X:%04X\n",
                         fn, cpu_.r[BX], cpu_.r[CX], cpu_.r[DX],
                         cpu_.sreg[CS], static_cast<uint16_t>(cpu_.ip));
    auto clamp = [](int16_t v, int16_t lo, int16_t hi) {
        return static_cast<int16_t>(v < lo ? lo : v > hi ? hi : v);
    };
    switch (fn) {
        case 0x0000:                                      // reset and read status
        case 0x0021:                                      // software reset
            mouse_.buttons = 0;
            mouse_.show = -1;
            mouse_.dx = mouse_.dy = 0;
            for (int b = 0; b < 3; ++b) { mouse_.press[b] = {}; mouse_.release[b] = {}; }
            mouse_.x = static_cast<int16_t>((mouse_.min_x + mouse_.max_x) / 2);
            mouse_.y = static_cast<int16_t>((mouse_.min_y + mouse_.max_y) / 2);
            cpu_.r[AX] = 0xFFFF;                          // installed -- the whole point
            cpu_.r[BX] = 2;                               // two buttons
            return true;
        case 0x0001: ++mouse_.show; return true;          // show cursor
        case 0x0002: --mouse_.show; return true;          // hide cursor
        case 0x0003:                                      // position and buttons
            cpu_.r[BX] = mouse_.buttons;
            cpu_.r[CX] = static_cast<uint16_t>(mouse_.x);
            cpu_.r[DX] = static_cast<uint16_t>(mouse_.y);
            return true;
        case 0x0004:                                      // set position
            mouse_.x = clamp(static_cast<int16_t>(cpu_.r[CX]), mouse_.min_x, mouse_.max_x);
            mouse_.y = clamp(static_cast<int16_t>(cpu_.r[DX]), mouse_.min_y, mouse_.max_y);
            return true;
        case 0x0005:                                      // button press info
        case 0x0006: {                                    // button release info
            const int b = cpu_.r[BX] & 3;
            auto& st = fn == 0x0005 ? mouse_.press[b] : mouse_.release[b];
            cpu_.r[AX] = mouse_.buttons;
            cpu_.r[BX] = st.count;
            cpu_.r[CX] = static_cast<uint16_t>(st.x);
            cpu_.r[DX] = static_cast<uint16_t>(st.y);
            st.count = 0;                                 // reading clears the count
            return true;
        }
        case 0x0007:                                      // horizontal range
            mouse_.min_x = static_cast<int16_t>(cpu_.r[CX]);
            mouse_.max_x = static_cast<int16_t>(cpu_.r[DX]);
            if (mouse_.min_x > mouse_.max_x) std::swap(mouse_.min_x, mouse_.max_x);
            mouse_.x = clamp(mouse_.x, mouse_.min_x, mouse_.max_x);
            return true;
        case 0x0008:                                      // vertical range
            mouse_.min_y = static_cast<int16_t>(cpu_.r[CX]);
            mouse_.max_y = static_cast<int16_t>(cpu_.r[DX]);
            if (mouse_.min_y > mouse_.max_y) std::swap(mouse_.min_y, mouse_.max_y);
            mouse_.y = clamp(mouse_.y, mouse_.min_y, mouse_.max_y);
            return true;
        case 0x000B:                                      // motion counters, and clear
            cpu_.r[CX] = static_cast<uint16_t>(mouse_.dx);
            cpu_.r[DX] = static_cast<uint16_t>(mouse_.dy);
            mouse_.dx = mouse_.dy = 0;
            return true;
        case 0x000C:                                      // install event handler
            mouse_.handler_mask = cpu_.r[CX];
            mouse_.handler_seg = cpu_.sreg[ES];
            mouse_.handler_off = cpu_.r[DX];
            return true;
        default:
            // Cursor shape (09h/0Ah), exclusion area (10h), sensitivity (0Fh/1Ah/1Bh)
            // and the rest: accepted and ignored. Nothing here changes what ends up on
            // the screen, because the cursor is not drawn by the guest.
            return true;
    }
}

bool Dos::int15() {
    if (cpu_.r[AX] == 0x5000) {                               // get font read routine
        // Written here, not once at startup: something between the constructor
        // and the guest's first call clears this part of low memory, and the
        // three bytes were gone by the time the far pointer was used. The guest
        // then ran zeroes from 0090:0004 upwards until it fell out of the
        // segment -- which is what "CS=000F executing nothing" turned out to be.
        install_font_stubs();
        // DX is the cell: DH wide, DL tall. 16x16 is the kanji set, anything
        // else the single-byte one.
        const bool dbcs = (cpu_.r[DX] >> 8) == 16;
        // set_seg, not a bare write to sreg: the cached base is kept even in
        // real mode, and a segment register that disagrees with its own cache
        // sends the next far call somewhere else entirely.
        cpu_.set_seg(ES, kFontStubSeg);
        cpu_.r[BX] = static_cast<uint16_t>(kFontStubOff + (dbcs ? 4 : 0));
        cpu_.flags &= ~CF;
        return true;
    }
    cpu_.flags |= CF;                                         // anything else: unsupported
    return true;
}

// The video BIOS, as far as a graphics program needs it.
//
// "Accept and ignore" is not enough once the guest draws: JW_CAD asks for the
// current mode (AH=0Fh), keeps it to restore on the way out, sets its own
// (AH=00h), and then reads the screen's geometry straight out of the BIOS data
// area -- the byte width from 0040:004A and the cell height from 0040:0085 --
// and divides by them. With nothing behind the call it divided by zero and the
// Microsoft C runtime stopped with "R6003".
bool Dos::int10() {
    static const bool cursor_trace = getenv("DOSEMU_TEXT_TRACE") != nullptr;
    const uint8_t ah = cpu_.r[AX] >> 8, al = cpu_.r[AX] & 0xFF;
    static const bool io_trace = getenv("DOSEMU_IO_TRACE") != nullptr;    // with the port writes
    if (io_trace) std::fprintf(stderr, "[int10] ax=%04X bx=%04X cx=%04X dx=%04X es=%04X\n",
                               cpu_.r[AX], cpu_.r[BX], cpu_.r[CX], cpu_.r[DX], cpu_.sreg[ES]);
    switch (ah) {
        case 0x00: {                                     // set video mode
            static const struct { uint8_t mode, cols, rows, cell; uint16_t width; }
                kModes[] = {
                    {0x03, 80, 25, 16, 0}, {0x12, 80, 30, 16, 640},
                    {0x13, 40, 25,  8, 320}, {0x6A, 100, 37, 16, 800},
                };
            uint8_t mode = al & 0x7F;
            uint8_t cols = 80, rows = 25, cell = 16;
            uint16_t m_width = 0;
            for (const auto& m : kModes)
                if (m.mode == mode) { cols = m.cols; rows = m.rows; cell = m.cell; m_width = m.width; }
            video_mode_ = mode;
            // The graphics modes get planes behind A000:0000; a text mode
            // leaves them alone, which is what makes snapshot() say "no".
            vga.set_mode(mode, m_width, rows * cell, m_width / 8);
            mem_.wb(0x40, 0x0049, mode);
            mem_.ww(0x40, 0x004A, cols);
            mem_.wb(0x40, 0x0084, static_cast<uint8_t>(rows - 1));
            text_split_ = false;                         // a mode set undoes the split
            mem_.ww(0x40, 0x0085, cell);
            return true;
        }
        case 0x0F:                                       // get video mode
            cpu_.r[AX] = static_cast<uint16_t>(
                (mem_.rb(0x40, 0x004A) << 8) | video_mode_);
            cpu_.r[BX] = static_cast<uint16_t>(cpu_.r[BX] & 0x00FF);  // page 0
            return true;
        case 0x09: case 0x0A: case 0x0E: case 0x13:      // text output, drawn as glyphs
            return int10_text(ah);
        // The DOS/V system line. There is no row 31 on a 480-line screen, so a
        // FEP that simply wrote below the last one would write off the bottom
        // -- 鳳 puts its 「鳳」 mark at [0040:0084]+1 and its input line there
        // too. The row exists because the driver makes it: AX=1D00h with BX=1
        // takes the last row away from the application and keeps it for the
        // system, which is what the FEP asks for before it writes anything.
        case 0x1D: {
            const uint8_t al = cpu_.r[AX] & 0xFF;
            if (al == 0x00) {
                const bool want = cpu_.r[BX] != 0;
                if (want != text_split_) {
                    const uint8_t last = mem_.rb(0x40, 0x0084);
                    mem_.wb(0x40, 0x0084, static_cast<uint8_t>(want ? last - 1 : last + 1));
                    text_split_ = want;
                }
            } else if (al == 0x02) {
                cpu_.r[BX] = text_split_ ? 1 : 0;
            }
            return true;
        }
        // The cursor. A graphics program has nothing to show for it, but it
        // still keeps one, and a FEP doing inline input asks where it is to
        // decide which row to put its window on. These two lines are the answer
        // to "why did the conversion window land *there*".
        case 0x02:                                       // set cursor position
            if (cursor_trace) std::fprintf(stderr, "[text] cursor <- %04X by %04X:%04X\n",
                                           cpu_.r[DX], cpu_.sreg[CS], (uint16_t)cpu_.ip);
            cursor_ = cpu_.r[DX];
            return true;
        case 0x03:                                       // get cursor position
            if (cursor_trace) std::fprintf(stderr, "[text] cursor -> %04X by %04X:%04X\n",
                                           cursor_, cpu_.sreg[CS], (uint16_t)cpu_.ip);
            cpu_.r[DX] = cursor_;
            cpu_.r[CX] = 0x0607;
            return true;
        // The palette. Not decoration: JW_CAD reads JW_PAL.DAT and installs its own
        // sixteen colours here, and they are not the EGA defaults -- index 4 is green
        // where the default is red, 6 is yellow where the default is brown, 7 is white
        // where the default is light grey. A screenshot painted with the defaults is
        // the wrong picture, and the whole point of the screenshots is to compare them
        // against the port's.
        case 0x10:
            switch (al) {
                case 0x00:                               // set one attribute-palette register
                    vga.set_pal(cpu_.r[BX] & 0xFF, cpu_.r[BX] >> 8);
                    return true;
                case 0x02: {                             // set all sixteen, from ES:DX
                    for (int i = 0; i < 16; ++i)
                        vga.set_pal(static_cast<uint8_t>(i),
                                    mem_.rb(cpu_.sreg[ES], static_cast<uint16_t>(cpu_.r[DX] + i)));
                    return true;                         // byte 16 is the overscan; nothing shows it
                }
                case 0x07:                               // read one attribute-palette register
                    cpu_.r[BX] = static_cast<uint16_t>((cpu_.r[BX] & 0xFF) |
                                 (vga.get_pal(cpu_.r[BX] & 0xFF) << 8));
                    return true;
                case 0x10:                               // set one DAC entry: DH red, CH green, CL blue
                    vga.set_dac(static_cast<uint8_t>(cpu_.r[BX]),
                                static_cast<uint8_t>(cpu_.r[DX] >> 8),
                                static_cast<uint8_t>(cpu_.r[CX] >> 8),
                                static_cast<uint8_t>(cpu_.r[CX] & 0xFF));
                    return true;
                case 0x12: {                             // set a block of DAC entries from ES:DX
                    uint16_t at = cpu_.r[DX];
                    for (uint16_t i = 0; i < cpu_.r[CX]; ++i, at = static_cast<uint16_t>(at + 3))
                        vga.set_dac(static_cast<uint8_t>(cpu_.r[BX] + i),
                                    mem_.rb(cpu_.sreg[ES], at),
                                    mem_.rb(cpu_.sreg[ES], static_cast<uint16_t>(at + 1)),
                                    mem_.rb(cpu_.sreg[ES], static_cast<uint16_t>(at + 2)));
                    return true;
                }
                case 0x15: {                             // read one DAC entry
                    uint8_t r = 0, g = 0, b = 0;
                    vga.get_dac(static_cast<uint8_t>(cpu_.r[BX]), r, g, b);
                    cpu_.r[DX] = static_cast<uint16_t>((r << 8) | (cpu_.r[DX] & 0xFF));
                    cpu_.r[CX] = static_cast<uint16_t>((g << 8) | b);
                    return true;
                }
                case 0x17: {                             // read a block of DAC entries to ES:DX
                    uint16_t at = cpu_.r[DX];
                    for (uint16_t i = 0; i < cpu_.r[CX]; ++i, at = static_cast<uint16_t>(at + 3)) {
                        uint8_t r = 0, g = 0, b = 0;
                        vga.get_dac(static_cast<uint8_t>(cpu_.r[BX] + i), r, g, b);
                        mem_.wb(cpu_.sreg[ES], at, r);
                        mem_.wb(cpu_.sreg[ES], static_cast<uint16_t>(at + 1), g);
                        mem_.wb(cpu_.sreg[ES], static_cast<uint16_t>(at + 2), b);
                    }
                    return true;
                }
                default: return true;                    // overscan, blink, paging: nothing to show
            }
        case 0x12:                                       // EGA/VGA configuration
            if ((cpu_.r[BX] & 0xFF) == 0x10) {           // BL=10h: get info
                cpu_.r[BX] = 0x0003;                     // colour, 256 KB
                cpu_.r[CX] = 0x0009;
            }
            return true;
        default:
            // AH=01h cursor shape and the rest change nothing the guest can read back.
            return true;
    }
}

bool Dos::int21_default(uint8_t) { return true; }   // unknown INTs: no-op for now

bool Dos::int21() {
    // DOS clears the carry flag on the way out of a call that worked, and the handlers
    // below only ever *set* it on failure — so several of them were leaving whatever CF
    // the previous call had left behind. Watcom's loader installs its interrupt
    // handlers with AH=25h and checks CF afterwards; it read a stale flag and concluded
    // the install had failed. Clear it once, here, and let a failure say so.
    cpu_.flags &= ~CF;
    uint8_t ah = cpu_.r[AX] >> 8;
    switch (ah) {
        case 0x00: terminate(0); return true;                       // terminate
        case 0x01: {                                                // read char from stdin, with echo -> AL
            int c = getch(); if (c < 0) c = 0x1A;                    // Ctrl-Z at EOF
            out(1, static_cast<char>(c)); cpu_.sb(AX, static_cast<uint8_t>(c)); return true;
        }
        case 0x02: out(1, cpu_.r[DX] & 0xFF); return true;          // output char in DL
        case 0x07:                                                  // read char, no echo, no Ctrl-C check
        case 0x08: {                                                // read char, no echo
            int c = getch(); cpu_.sb(AX, static_cast<uint8_t>(c < 0 ? 0x1A : c)); return true;
        }
        case 0x0A: {                                                // buffered line input at DS:DX
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            uint8_t maxlen = mem_.rb(seg, off);
            uint8_t len = 0;
            for (;;) {
                int c = getch();
                if (c < 0 || c == '\r') { out(1, '\r'); out(1, '\n'); break; }
                if (c == 0x08) { if (len) { --len; out(1, '\b'); out(1, ' '); out(1, '\b'); } continue; }  // backspace
                if (len + 1 >= maxlen) continue;
                mem_.wb(seg, off + 2 + len, static_cast<uint8_t>(c)); ++len; out(1, static_cast<char>(c));
            }
            mem_.wb(seg, off + 2 + len, 0x0D);
            mem_.wb(seg, off + 1, len);
            return true;
        }
        case 0x04: return true;                                     // output to aux — ignore
        case 0x05: return true;                                     // output to printer — ignore
        case 0x06:                                                  // direct console I/O
            if ((cpu_.r[DX] & 0xFF) != 0xFF) { out(1, cpu_.r[DX] & 0xFF); }
            else { int c = getch(); if (c < 0) { cpu_.sb(AX, 0); cpu_.flags |= ZF; } else { cpu_.sb(AX, (uint8_t)c); cpu_.flags &= ~ZF; } }
            return true;
        case 0x09: {                                                // output '$'-terminated string at DS:DX
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            for (int i = 0; i < 65535; ++i) {
                char c = static_cast<char>(mem_.rb(seg, off + i));
                if (c == '$') break;
                out(1, c);
            }
            cpu_.sb(AX, '$');
            return true;
        }
        // Check input status. "A character is waiting" was a constant 0xFF here, and it
        // is the same lie INT 16h AH=01h was careful not to tell: JW_CAD's key poll asks
        // this first and only reads (AH=08h) when the answer is yes, so a permanent yes
        // made it read for ever. On a run with nothing typed into it the screen froze
        // after the first 7 million instructions and never changed again. The honest
        // answer comes from the same hook INT 16h uses; unset, it is no.
        // Is a character waiting? DOS answers this by asking CON, and when the
        // answer is no it raises the idle interrupt on the way out -- which is
        // where a resident program does the work it could not do inside the
        // keyboard interrupt. JW_CAD asks this before every read, so this is
        // the only place a FEP under it ever gets to think.
        case 0x0B:
            if (con_driver_) {
                const bool ready = con_ready();
                if (!ready) dos_idle();
                cpu_.sb(AX, ready ? 0xFF : 0x00);
                return true;
            }
            cpu_.sb(AX, keys_waiting() || (input_ready && input_ready()) ? 0xFF : 0x00);
            return true;
        // The InDOS flag. Every resident thing asks for it -- it is how a TSR
        // knows whether DOS is busy -- and WXP asks during its own INIT. One
        // byte in the work block, always zero, which is the truth here: nothing
        // pops out of DOS to run in the background.
        case 0x34:
            cpu_.set_seg(ES, drv_work_); cpu_.r[BX] = 0x0300;
            mem_.wb(drv_work_, 0x0300, 0);
            return true;
        // Terminate and stay resident. The block shrinks to what DX says and
        // keeps its owner, so nothing hands it out again; then the program ends
        // like any other. This is how a FEP's companion installs itself --
        // wxpdosv hooks the J-3100 BIOS calls WXP makes and then stays.
        case 0x31: {
            const uint16_t keep = cpu_.r[DX] ? cpu_.r[DX] : 0x11;
            mem_resize(psp_seg, keep);
            std::fprintf(stderr, "dosemu: %s stayed resident: %04X, %u paragraphs\n",
                         prog_path.c_str(), psp_seg, keep);
            terminate(cpu_.r[AX] & 0xFF);
            return true;
        }
        case 0x0C: return true;                                     // flush + input — ignore
        // Reset drive: flush DOS's write buffers. We write through, so there is nothing
        // to flush and "done" is the truthful answer — but it has to be an *answer*.
        // DOS/4GW issues it before loading and treats a refusal as a failure.
        case 0x0D: cpu_.flags &= ~CF; return true;
        case 0x0E: cpu_.sb(AX, 3); return true;                     // select drive -> report 3 drives
        case 0x19: cpu_.sb(AX, 0); return true;                     // get current drive -> A:
        case 0x1A: dta_seg_ = cpu_.sreg[DS]; dta_off_ = cpu_.r[DX]; return true;   // set DTA
        case 0x2F: cpu_.set_seg(ES, dta_seg_); cpu_.r[BX] = dta_off_; return true;   // get DTA -> ES:BX
        case 0x4E:                                                  // find first (DS:DX spec, CX attr)
            if (cpu_.r[CX] == 0x08) { cpu_.flags |= CF; cpu_.r[AX] = 18; return true; }  // no volume label
            if (find_first(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[CX])) { write_dta_entry(); cpu_.flags &= ~CF; }
            else { cpu_.flags |= CF; cpu_.r[AX] = 18; }             // no more files
            return true;
        case 0x4F:                                                  // find next
            if (find_pos_ < find_.size()) { write_dta_entry(); cpu_.flags &= ~CF; }
            else { cpu_.flags |= CF; cpu_.r[AX] = 18; }
            return true;
        case 0x36:                                                  // get free disk space (DL drive)
            cpu_.r[AX] = 8;        // sectors per cluster
            cpu_.r[CX] = 512;      // bytes per sector
            cpu_.r[BX] = 0xFFFF;   // free clusters
            cpu_.r[DX] = 0xFFFF;   // total clusters
            return true;
        case 0x69: cpu_.flags &= ~CF; return true;                  // get/set disk serial: accept
        case 0x73: cpu_.flags |= CF; return true;                   // FAT32 free space: unsupported -> use 36h
        case 0x47: {                                                // get current directory (DL drive) -> DS:SI
            std::string c = files_.cwd();                            // DOS wants it without the leading backslash
            if (!c.empty() && c[0] == '\\') c = c.substr(1);
            for (size_t i = 0; i < c.size(); ++i) mem_.wb(cpu_.sreg[DS], cpu_.r[SI] + i, c[i]);
            mem_.wb(cpu_.sreg[DS], cpu_.r[SI] + c.size(), 0);
            cpu_.r[AX] = 0x0100; cpu_.flags &= ~CF; return true;
        }
        case 0x38: {                                                // get country info (DS:DX 34-byte buffer)
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            for (int i = 0; i < 34; ++i) mem_.wb(seg, off + i, 0);
            mem_.ww(seg, off + 0, 0);        // date format: 0 = USA (m d y)
            mem_.wb(seg, off + 2, '$');       // currency symbol
            mem_.wb(seg, off + 7, ',');       // thousands separator
            mem_.wb(seg, off + 9, '.');       // decimal separator
            mem_.wb(seg, off + 11, '-');      // date separator
            mem_.wb(seg, off + 13, ':');      // time separator
            mem_.wb(seg, off + 17, 0);        // time format: 12-hour
            // case-map: a far routine that uppercases AL. Plant it in scratch and
            // point the country block at it (offset 18 = far pointer).
            const uint16_t cseg = 0x0090, coff = 0x0100;
            const uint8_t cm[] = { 0x3C,0x61, 0x72,0x06, 0x3C,0x7A, 0x77,0x02, 0x2C,0x20, 0xCB };  // cmp/jb/cmp/ja/sub/retf
            for (size_t i = 0; i < sizeof cm; ++i) mem_.wb(cseg, coff + i, cm[i]);
            mem_.ww(seg, off + 18, coff); mem_.ww(seg, off + 20, cseg);
            mem_.wb(seg, off + 22, ',');      // data-list separator
            cpu_.r[BX] = 1;                   // country code = USA
            cpu_.flags &= ~CF; return true;
        }
        case 0x58:                                                  // get/set allocation strategy / UMB link
            switch (cpu_.r[AX] & 0xFF) {
                case 0: cpu_.r[AX] = 0; break;        // get strategy -> first fit
                case 2: cpu_.r[AX] = 0; break;        // get UMB link -> not linked
                default: break;                       // set strategy / set UMB link: accept
            }
            cpu_.flags &= ~CF; return true;
        case 0x65:                                                  // get extended country info: report "not
            cpu_.r[AX] = 1; cpu_.flags |= CF; return true;          // supported" so callers (FreeCOM) keep
                                                                    // their own sane NLS fallback (valid
                                                                    // filename chars etc.) instead of our blank.
        case 0x71:                                                  // Windows long-filename API: not supported
            cpu_.r[AX] = 0x7100; cpu_.flags |= CF; return true;     // the standard "no LFN" answer
        case 0x5D:                                                  // server/network functions, and 5D06h:
            // "get address of the swappable data area". DJGPP's fstat asks for the SDA to
            // read a file's open mode out of DOS's internals. There is no SDA here and
            // inventing one would be the FreeCOM mistake again, so say so honestly: every
            // caller that asks has a fallback, and DJGPP's is the ordinary DOS calls.
            cpu_.r[AX] = 1; cpu_.flags |= CF; return true;          // invalid function
        case 0x66:                                                  // get/set global code page
            // wlink's exit path asks. We have one code page and no way to change it, and a
            // program that is told "unsupported" carries on; one that is told "success" and
            // handed a zero would believe the code page is 0.
            cpu_.r[AX] = 1; cpu_.flags |= CF; return true;          // invalid function
        case 0x33: cpu_.sb(DX, 0); cpu_.flags &= ~CF; return true;  // get/set Ctrl-Break flag -> off
        case 0x3B:                                                  // chdir (DS:DX)
            if (files_.chdir(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]))) cpu_.flags &= ~CF;
            else { cpu_.flags |= CF; cpu_.r[AX] = 3; }
            return true;
        case 0x25: {                                                // set interrupt vector (AL=n) DS:DX
            uint8_t v = cpu_.r[AX] & 0xFF;
            mem_.w16(v * 4, cpu_.r[DX]);
            mem_.w16(v * 4 + 2, cpu_.sreg[DS]);
            cpu_.flags &= ~CF; return true;
        }
        case 0x30:                                                  // get DOS version: AL=major, AH=minor
            cpu_.r[AX] = (30 << 8) | 3;   // 3.30
            cpu_.r[BX] = 0; cpu_.r[CX] = 0;
            return true;
        case 0x35: {                                                // get interrupt vector (AL=n) -> ES:BX
            uint8_t v = cpu_.r[AX] & 0xFF;
            cpu_.r[BX] = mem_.r16(v * 4);
            cpu_.set_seg(ES, mem_.r16(v * 4 + 2));
            return true;
        }
        case 0x37:                                                  // get/set switch character
            if ((cpu_.r[AX] & 0xFF) == 0) { cpu_.sb(AX, 0); cpu_.r[DX] = (cpu_.r[DX] & 0xFF00) | '/'; }
            else cpu_.sb(AX, 0);
            return true;
        // Get the DOS "list of lists" (undocumented, AH=52h) -> ES:BX. Pointing at a
        // zeroed scratch area is not a safe non-answer: DJGPP's fstat() deliberately
        // does not check CF here ("Ralph Brown's Interrupt List doesn't say FLAGS are
        // set"), takes ES:BX as given, and walks the System File Table chain from
        // LoL+4 until a next-pointer of 0xFFFF. Zeros mean it never terminates — gcc's
        // cc1 spun there forever, 20 million instructions a second going nowhere.
        //
        // So the structure has to be well-formed and *empty*: an SFT list pointer of
        // 0xFFFF:0xFFFF. get_sft_entry() exits its loop at once and returns -2, which
        // fstat reads as "SFT unavailable" and falls back to ordinary DOS calls. That
        // is an answer we can give truthfully; inventing SFT contents would be the
        // same "success but blank" mistake as the FreeCOM NLS lesson below.
        //
        // LoL-2 is the other half of that: the first MCB's segment, which is how a
        // program walks the memory arena. It used to say 0xFFFF ("none") because there
        // was no arena to walk; now there is one, and DOS/4GW plans its layout from it.
        // Rebuilt, not wiped: LoL+0Ch is the pointer to the current CON, and a
        // FEP reads it during its own INIT to find the console it is replacing.
        // Zeroing the block here would hand it a null pointer to chain to.
        case 0x52:
            publish_lol();
            cpu_.set_seg(ES, kLolSeg); cpu_.r[BX] = 0x0010;
            return true;
        case 0x29: {                                                // parse filename (DS:SI) into an FCB (ES:DI)
            uint16_t sseg = cpu_.sreg[DS], si = cpu_.r[SI];
            uint16_t eseg = cpu_.sreg[ES], di = cpu_.r[DI];
            while (mem_.rb(sseg, si) == ' ') ++si;                   // skip leading blanks
            uint8_t drive = 0;
            if (mem_.rb(sseg, si + 1) == ':') { drive = (std::toupper(mem_.rb(sseg, si)) - 'A') + 1; si += 2; }
            mem_.wb(eseg, di, drive);
            auto fill = [&](int at, int width, bool ext) {
                for (int i = 0; i < width; ++i) mem_.wb(eseg, di + at + i, ' ');
                int i = 0;
                while (i < width) { char c = static_cast<char>(mem_.rb(sseg, si)); if (!c || c == ' ' || c == '\r' || (!ext && c == '.') || c == '/' ) break; if (c=='.') break; mem_.wb(eseg, di + at + i, std::toupper(c)); ++si; ++i; }
                while (mem_.rb(sseg, si) && mem_.rb(sseg, si) != ' ' && mem_.rb(sseg, si) != '.' && mem_.rb(sseg, si) != '\r' && !ext) ++si;
            };
            fill(1, 8, false);
            if (mem_.rb(sseg, si) == '.') { ++si; fill(9, 3, true); }
            else { for (int i = 0; i < 3; ++i) mem_.wb(eseg, di + 9 + i, ' '); }
            cpu_.sb(AX, 0); cpu_.r[SI] = si;
            return true;
        }
        case 0x2A:                                                  // get date -> CX=year DH=month DL=day AL=dow
            cpu_.r[CX] = 1993; cpu_.r[DX] = (8 << 8) | 19; cpu_.sb(AX, 4); return true;
        case 0x2B: cpu_.sb(AX, 0); cpu_.flags &= ~CF; return true;      // set date -> accept
        case 0x2C: {                                                // get time -> CH=hr CL=min DH=sec DL=1/100
            const uint64_t h = hundredths();

            cpu_.r[CX] = static_cast<uint16_t>(((12 + h / 360000) % 24) << 8
                                               | (h / 6000) % 60);
            cpu_.r[DX] = static_cast<uint16_t>(((h / 100) % 60) << 8 | h % 100);
            return true;
        }
        case 0x43: {                                                // get/set file attributes (AL=0 get, 1 set)
            if ((cpu_.r[AX] & 0xFF) == 0) {                          // get: also how a program tests existence
                if (files_.exists(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]))) { cpu_.r[CX] = 0x20; cpu_.flags &= ~CF; }
                else { cpu_.flags |= CF; cpu_.r[AX] = 2; }
            } else cpu_.flags &= ~CF;                                // set: accept
            return true;
        }
        case 0x39: {                                                // create directory (DS:DX name)
            std::error_code ec;
            bool ok = std::filesystem::create_directory(files_.host_path(read_asciiz(cpu_.sreg[DS], cpu_.r[DX])), ec);
            if (ok && !ec) cpu_.flags &= ~CF; else { cpu_.flags |= CF; cpu_.r[AX] = 3; }
            return true;
        }
        case 0x3A: {                                                // remove directory (DS:DX name)
            std::error_code ec;
            bool ok = std::filesystem::remove(files_.host_path(read_asciiz(cpu_.sreg[DS], cpu_.r[DX])), ec);
            if (ok && !ec) cpu_.flags &= ~CF; else { cpu_.flags |= CF; cpu_.r[AX] = 5; }
            return true;
        }
        case 0x3C: {                                                // create file (CX attr, DS:DX name) -> handle
            int h = files_.create(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[CX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        // Create a *new* file: like 3Ch but it must not already exist. This is how a
        // program claims a unique temporary name without a race, and gcc's driver uses
        // it for every intermediate file — without it, `Cannot create temporary file`
        // and the compile stops before the assembler ever runs.
        case 0x5B: {
            const std::string p = read_asciiz(cpu_.sreg[DS], cpu_.r[DX]);
            if (files_.exists(p)) { cpu_.flags |= CF; cpu_.r[AX] = 80; return true; }  // already exists
            int h = files_.create(p, cpu_.r[CX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3D: {                                                // open file (AL access, DS:DX name) -> handle
            int h = files_.open(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[AX] & 0x03);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3E: {                                                // close handle
            int r = files_.close(cpu_.r[BX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x3F: {                                                // read (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (files_.is_console(h, cfd)) {                        // console read: a line, CR/LF terminated
                uint16_t k = 0;
                while (k < cnt) {
                    int c = getch();
                    if (c < 0) break;                               // EOF
                    if (c == '\r') { out(1,'\r'); mem_.wb(seg, off + k++, '\r'); if (k < cnt) { out(1,'\n'); mem_.wb(seg, off + k++, '\n'); } break; }
                    out(1, static_cast<char>(c)); mem_.wb(seg, off + k++, static_cast<uint8_t>(c));
                }
                cpu_.r[AX] = k; cpu_.flags &= ~CF; return true;
            }
            std::vector<uint8_t> buf(cnt);
            int n = files_.read(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            // The transfer address is physical, not DS:DX with the offset wrapping:
            // a buffer that starts near the top of its segment simply runs on into the
            // next paragraph. JW_CAD hands DOS 0x4000 bytes at 4612:CFA2 when it saves a
            // big drawing, and 12,382 bytes in that crosses 64 KiB — with a 16-bit offset
            // the rest folded back to 4612:0000 and the file came out with a slab of the
            // wrong memory in the middle of the entity array.
            { uint32_t a = Memory::phys(seg, off);
              for (int i = 0; i < n; ++i) mem_.w8((a + i) & 0xFFFFF, buf[i]); }
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x40: {                                                // write (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (!cnt) {                                             // CX=0 truncates at the
                int r = files_.truncate_here(h);                    // current position: DOS
                if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; }   // has no other way to
                else { cpu_.flags &= ~CF; cpu_.r[AX] = 0; }         // shorten a file
                return true;
            }
            if (files_.is_console(h, cfd)) {
                for (uint16_t i = 0; i < cnt; ++i) out(cfd == 2 ? 2 : 1, static_cast<char>(mem_.rb(seg, off + i)));
                cpu_.r[AX] = cnt; cpu_.flags &= ~CF; return true;
            }
            std::vector<uint8_t> buf(cnt);
            { uint32_t a = Memory::phys(seg, off);          // physical, see the read above
              for (uint16_t i = 0; i < cnt; ++i) buf[i] = mem_.r8((a + i) & 0xFFFFF); }
            int n = files_.write(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x41: {                                                // delete file (DS:DX name)
            int r = files_.remove(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]));
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x42: {                                                // lseek (AL method, BX handle, CX:DX offset) -> DX:AX
            long off = (static_cast<long>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            long pos = files_.seek(cpu_.r[BX], off, cpu_.r[AX] & 0xFF);
            if (pos < 0) { cpu_.flags |= CF; cpu_.r[AX] = 6; return true; }
            cpu_.r[AX] = pos & 0xFFFF; cpu_.r[DX] = (pos >> 16) & 0xFFFF; cpu_.flags &= ~CF;
            return true;
        }
        case 0x45: {                                                // dup handle
            int h = files_.dup(cpu_.r[BX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x46: {                                                // dup2 (force BX onto CX)
            int r = files_.dup2(cpu_.r[BX], cpu_.r[CX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x44:                                                  // IOCTL: report 0/1/2 as console devices
            if ((cpu_.r[AX] & 0xFF) == 0 && cpu_.r[BX] <= 2) {
                cpu_.r[DX] = 0x0080 | (cpu_.r[BX] == 0 ? 0x01 : 0x02) | 0x0010;  // char device, stdin/stdout, is-con
                cpu_.flags &= ~CF; return true;
            }
            cpu_.flags &= ~CF; cpu_.r[DX] = 0; return true;
        case 0x48: {                                                // allocate memory (BX paragraphs)
            const uint16_t seg = mem_alloc(cpu_.r[BX]);
            if (!seg) {                                             // not enough: report largest free
                if (trace) { std::fprintf(stderr, "[mem] AH=48h wants %04X paras, largest free %04X\n",
                                          cpu_.r[BX], mem_largest()); mem_dump("arena"); }
                cpu_.flags |= CF; cpu_.r[AX] = 8;                   // INSUFFICIENT MEMORY
                cpu_.r[BX] = mem_largest();
                return true;
            }
            cpu_.r[AX] = seg;
            cpu_.flags &= ~CF; return true;
        }
        case 0x49:                                                  // free memory (ES = block)
            if (!mem_free(cpu_.sreg[ES])) { cpu_.flags |= CF; cpu_.r[AX] = 9; return true; }
            cpu_.flags &= ~CF; return true;
        // Get/set the current PSP. Nothing 16-bit here ever asked, because a .COM or
        // .EXE is handed DS=ES=PSP at entry and just keeps it. A DOS extender cannot:
        // it reloads the segment registers to switch modes, so it asks — and until
        // this existed the call fell through to the no-op default, which returns
        // success with BX untouched. The stub then believed the PSP was wherever BX
        // happened to point and read an empty command tail from it, which is why
        // every DJGPP program started with argc == 0.
        // Get/set a file's date and time by handle. This is fstat()'s *trusted* source:
        // having decided the SFT is unavailable it falls back to
        //   if (_getftime(fhandle, &t) == 0 && __filelength(fhandle) != -1) ...
        // and if that fails too it has nothing left and gives up. The same fixed
        // timestamp find_first() reports, so the two agree about a file.
        case 0x57:
            if ((cpu_.r[AX] & 0xFF) == 0) {                          // get
                cpu_.r[CX] = kFixedTime; cpu_.r[DX] = kFixedDate;
            }
            cpu_.flags &= ~CF; return true;                          // set: accept
        // Rename (DS:DX old -> ES:DI new). The last step of every compile: the driver
        // builds `hello.000` and renames it into place, so without this gcc does all
        // the work and then throws it away with `rename ... failed`.
        case 0x56: {
            const std::string from = files_.host_path(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]));
            const std::string to   = files_.host_path(read_asciiz(cpu_.sreg[ES], cpu_.r[DI]));
            std::error_code ec;
            std::filesystem::rename(from, to, ec);
            if (ec) { cpu_.flags |= CF; cpu_.r[AX] = 2; } else cpu_.flags &= ~CF;
            return true;
        }
        // Get the double-byte character set lead-byte table. An *empty* table is the
        // truthful answer here — this is not a Japanese DOS, so there are no lead
        // bytes — and it is a real answer rather than an error, which matters because
        // Watcom's extender stub asks before it will start.
        case 0x63:
            for (int i = 0; i < 4; ++i) mem_.wb(kLolSeg, 0x18 + i, 0);   // 0000 terminator
            cpu_.set_seg(DS, kLolSeg); cpu_.r[SI] = 0x18;
            cpu_.sb(AX, 0); cpu_.flags &= ~CF; return true;
        case 0x51: case 0x62: cpu_.r[BX] = psp_seg; return true;    // get PSP -> BX
        case 0x50: psp_seg = cpu_.r[BX]; return true;               // set PSP
        // TRUENAME (DS:SI -> ES:DI). gcc canonicalises every path it touches, so this
        // is called constantly; without it the driver cannot tell two spellings of the
        // same file apart. normalize() already does the work for the rest of the layer.
        case 0x60: {
            const std::string in = read_asciiz(cpu_.sreg[DS], cpu_.r[SI]);
            std::string out = "A:" + files_.normalize(in);
            if (out.size() > 127) out.resize(127);
            for (size_t i = 0; i < out.size(); ++i)
                mem_.wb(cpu_.sreg[ES], static_cast<uint16_t>(cpu_.r[DI] + i),
                        static_cast<uint8_t>(toupper((unsigned char)out[i])));
            mem_.wb(cpu_.sreg[ES], static_cast<uint16_t>(cpu_.r[DI] + out.size()), 0);
            cpu_.flags &= ~CF; return true;
        }
        case 0x68: case 0x6A: cpu_.flags &= ~CF; return true;       // commit file: we do not buffer
        case 0x4A: {                                                // resize block (ES:block, BX paragraphs)
            // Saying yes to everything is not the safe answer. The standard way to ask
            // "how much memory can I have?" is to request 0xFFFF paragraphs and read
            // the real maximum out of BX when it fails — Watcom's W32RUN does exactly
            // that. Granting a 1 MiB block it cannot possibly have left it convinced
            // it owned memory that was not there, and it gave up with
            // `Fatal error allocating DOS memory`. Report the truth instead.
            const uint16_t blk = cpu_.sreg[ES];
            if (!mem_resize(blk, cpu_.r[BX])) {
                uint16_t got = 0;                                   // what it could have had
                const size_t i = mem_find(blk);
                if (i != blocks_.size()) {
                    got = blocks_[i].paras;
                    for (size_t j = i + 1; j < blocks_.size() && !blocks_[j].used; ++j)
                        got = static_cast<uint16_t>(got + 1 + blocks_[j].paras);
                }
                if (trace) { std::fprintf(stderr, "[mem] AH=4Ah %04X wants %04X paras, can have %04X\n",
                                          blk, cpu_.r[BX], got); mem_dump("arena"); }
                cpu_.flags |= CF; cpu_.r[AX] = 8;                   // INSUFFICIENT MEMORY
                cpu_.r[BX] = got;                                   // ...but this much is free
                return true;
            }
            cpu_.flags &= ~CF; return true;
        }
        case 0x4B: {                                                // load & execute (AL=0) / load overlay (AL=3)
            const uint8_t sub = cpu_.r[AX] & 0xFF;
            const std::string path = read_asciiz(cpu_.sreg[DS], cpu_.r[DX]);
            if (sub == 0x00) return exec(path, cpu_.sreg[ES], cpu_.r[BX]);
            if (sub == 0x03) return load_overlay(path, cpu_.sreg[ES], cpu_.r[BX]);
            cpu_.flags |= CF; cpu_.r[AX] = 1; return true;          // AL=1 (load, no execute) unsupported
        }
        case 0x4C: terminate(cpu_.r[AX] & 0xFF); return true;       // terminate with return code
        case 0x4D: cpu_.r[AX] = last_child_code_ & 0xFF; return true;  // get child return code
        default:
            std::fprintf(stderr, "[dos] unimplemented INT 21h AH=%02Xh at %04X:%04X\n",
                         ah, cpu_.sreg[CS], cpu_.ip);
            cpu_.flags |= CF;   // signal error to the guest
            return true;
    }
}

}  // namespace dosemu
