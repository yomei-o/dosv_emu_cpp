// The DOS personality: it services INT 21h (and a few BIOS interrupts) so a real
// DOS program runs with no DOS underneath. This first cut covers console output,
// program termination, the DOS version query, and the interrupt-vector calls; file
// I/O, memory allocation and EXEC come next.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include "cpu.h"
#include "memory.h"
#include "files.h"
#include "dpmi.h"
#include "vga.h"

namespace dosemu {

class Dos {
public:
    Dos(Cpu& cpu, Memory& mem, std::string root = ".")
        : cpu_(cpu), mem_(mem), files_(std::move(root)), dpmi_(cpu, mem) {
        cpu_.on_int = [this](uint8_t n) { return handle(n); };
        // INT 31h function 0300h and DOS calls made from protected mode both need to
        // land back in this handler, so the DPMI host reflects through it.
        dpmi_.real_int = [this](uint8_t n) { return handle(n); };
        install_ivt_stubs();
        install_bios_data();
        // install_font_stubs() is deliberately not called here; INT 15h does it.
        mem_.mmio_lo = Vga::kBase;
        mem_.mmio_hi = Vga::kEnd;
        mem_.mmio_r = [this](uint32_t a, uint8_t& v) {
            if (!vga.graphics()) return false;
            v = vga.read(a);
            return true;
        };
        mem_.mmio_w = [this](uint32_t a, uint8_t v) {
            if (!vga.graphics()) return false;
            vga.write(a, v);
            return true;
        };
        cpu_.io_out_hook = [this](uint16_t p, uint8_t v) { return vga.io_out(p, v); };
        cpu_.io_in_hook = [this](uint16_t p, uint8_t& v) { return vga.io_in(p, v); };
        dpmi_.get_psp = [this] { return psp_seg; };
        blocks_.push_back({kArenaMcb, static_cast<uint16_t>(heap_end_ - kArenaMcb - 1), 0, false});
        // The DPMI mode-switch entry and the list-of-lists are inside the arena and are
        // not free. Marked as DOS's own (owner 8, the convention), so nothing hands them
        // out and a guest walking the chain sees them for what they are.
        mem_own(kDpmiEntrySeg, 26);
        for (Block& b : blocks_) if (b.seg + 1 == kDpmiEntrySeg) b.owner = 8;
        mem_publish();
        dpmi_.alloc_dos = [this](uint16_t paras) { return mem_alloc(paras); };
    }

    // Guest output (fd 1/2) goes here; defaults to stdout/stderr.
    std::function<void(int fd, const char* data, size_t len)> output;
    // Guest input (INT 21h AH=01/07/08/0A). Returns the next byte, or -1 at EOF.
    // A '\n' is treated as the DOS Enter key ('\r').
    std::function<int()> input;
    // Whether a keystroke is waiting, for BIOS INT 16h AH=01h. Unset means "no", which is
    // the truthful answer for a non-interactive run and the only safe default: claiming a
    // key is ready sends the caller into a blocking read.
    std::function<bool()> input_ready;

    uint16_t psp_seg = 0;   // set by the loader
    // The current program's environment *segment*. Kept here rather than read back
    // from PSP:2Ch, because the DPMI mode switch rewrites that field into a selector
    // for the protected-mode client — after which it is no longer a segment and the
    // DOS layer cannot use it to build a child's environment.
    uint16_t env_seg = 0;
    // The DOS path of the running program, as it appears after the environment
    // strings. A child inherits *this*, not its own name — see make_child_env().
    std::string prog_path;

    // Fill in the PSP fields the loader cannot know: who the parent is, and where this
    // program's memory block ends. Call after load_program(). A program that only ever
    // reads its command tail never looks at these; one that has to find the program
    // that launched it does, and Watcom's W32RUN is exactly that.
    void init_psp(uint16_t psp, uint16_t parent, const std::string& path);

    // Allocate and fill the top-level program's environment block, before load_program()
    // (which needs the segment for PSP:2Ch). Out of the arena, like every other block,
    // so it has an MCB: FreeCOM validates the block it is handed against one, and a
    // block outside the chain is exactly the half-truth it catches.
    uint16_t alloc_env(const std::string& dos_name);

private:
    uint16_t make_child_env(uint16_t parent_env, const std::string& child_name);
    uint16_t stamp_env_path(uint16_t env, const std::string& path);
    bool load_overlay(const std::string& name, uint16_t pb_seg, uint16_t pb_off);
public:
    DosFiles& files() { return files_; }

    bool handle(uint8_t n);

    // DOSEMU_DOS_TRACE=1 logs every INT 21h call and what it answered. The DPMI trace
    // only sees calls a protected-mode client reflects; a real-mode extender stub does
    // its DOS work directly, and this is the only way to watch that.
    static bool trace;

private:
    Cpu& cpu_;
    Memory& mem_;
    DosFiles files_;
    Dpmi dpmi_;

    std::string read_asciiz(uint16_t seg, uint16_t off) {
        std::string s;
        for (int i = 0; i < 128; ++i) { char c = static_cast<char>(mem_.rb(seg, off + i)); if (!c) break; s += c; }
        return s;
    }

    // Conventional memory, as a list of blocks in address order.
    //
    // This was a bump pointer, which is enough for a program that starts, mallocs and
    // exits. It is not enough for a DOS extender, because the way one sizes itself is to
    // ask for *everything* and then release what it does not need — and a bump pointer
    // cannot express releasing. Worse, the rule that kept it from handing out memory a
    // program already owned ("if the block is above the mark, move the mark past it")
    // destroyed the whole arena the moment a stub relocated itself high and resized
    // there: wlink's does, and every later allocation failed with "insufficient memory"
    // while 600 KiB sat unused. So: blocks, first fit, and a free that frees.
    // `seg` is the block's **MCB** paragraph; the guest sees seg+1 and owns `paras`
    // paragraphs from there, so a block of N costs N+1. That accounting is not
    // bookkeeping for its own sake: DOS/4GW asks AH=52h for the list-of-lists and then
    // walks the MCB chain out of it to plan its layout, and a chain has to live
    // somewhere real. mem_publish() writes it into guest memory after every change.
    struct Block { uint16_t seg, paras, owner; bool used; };
    std::vector<Block> blocks_;                    // contiguous, ordered, covers the arena
    // The arena starts just above the IVT stubs (which end at 0x008F) rather than one
    // paragraph under the first PSP, because the environment block has to be inside it.
    static constexpr uint16_t kArenaMcb = 0x0090;
    static constexpr uint16_t kDpmiEntrySeg = 0x00E0;   // ...through F7, then the LoL at F8
    uint16_t heap_end_ = 0x9F00;                   // just below the video/BIOS area (~640 KiB)

    void     mem_split(uint16_t mcb, uint16_t paras);  // carve `paras` off the front
    void     mem_own(uint16_t seg, uint16_t paras);    // claim a range (a program's block)
    uint16_t mem_alloc(uint16_t paras);                // guest segment, or 0 if it does not fit
    uint16_t mem_alloc_at(uint16_t paras, bool biggest);
    bool     mem_free(uint16_t seg);
    bool     mem_resize(uint16_t seg, uint16_t paras);
    uint16_t mem_largest() const;
    void     mem_coalesce();
    void     mem_publish();                            // write the MCB chain into memory
    void     mem_dump(const char* why) const;
    size_t   mem_find(uint16_t seg) const;             // index of the block the guest calls `seg`

    // EXEC (AH=4Bh): a child runs on the same CPU via a nested loop. terminate()
    // ends the whole process at depth 0 but only the child at depth > 0.
    int  exec_depth_ = 0;
    bool child_exited_ = false;
    int  child_code_ = 0;
    int  last_child_code_ = 0;
    bool exec(const std::string& name, uint16_t pb_seg, uint16_t pb_off);

    // Disk Transfer Area + FindFirst/FindNext state (for DIR and friends).
    uint16_t dta_seg_ = 0, dta_off_ = 0x80;
    struct Found { std::string name; uint32_t size; bool is_dir; uint16_t date, time; };
    std::vector<Found> find_;
    size_t find_pos_ = 0;
    bool find_first(const std::string& spec, uint16_t attr);
    void write_dta_entry();

    void out(int fd, char c) { char b = c; if (output) output(fd, &b, 1); }
    // A scripted key comes first; stdin is the fallback. An extended key (no ASCII)
    // reaches a DOS read as two bytes, 0x00 then the scan code -- which is how a
    // program tells F1 from the letter it would otherwise look like.
    int  getch() {
        const int k = next_key_byte();
        if (k >= 0) return k;
        int c = input ? input() : -1; return c == '\n' ? '\r' : c;   // -1 at EOF
    }
    void install_ivt_stubs();
    void install_bios_data();
    bool int10();
    bool int15();
    bool int33();
public:
    Vga vga;
    bool set_font(const std::string& path, bool dbcs) {
        return load_fontx(path, dbcs ? font_kanji_ : font_ank_);
    }
    // The mouse, as INT 33h presents it.
    //
    // JW_CAD will not start without one: overlay 8 calls function 0 (reset) and
    // exits with "mouse driver not installed" if AX comes back zero. So the
    // driver is always here, and what it reports is whatever the host has fed
    // into the queue below. With an empty queue it is a mouse that is simply not
    // being moved -- which is the right answer for a run that only wants the
    // opening screen.
    void mouse_move(int16_t x, int16_t y);
    void mouse_button(int button, bool down);

    // Keystrokes fed from a script, as the BIOS presents them: scan code in the
    // high byte, ASCII in the low one (0 for the keys that have none). They are
    // read before the `input` callback, so a scripted run needs no stdin at all.
    void push_key(uint16_t k) { keys_.push_back(k); }
    bool keys_waiting() const { return !keys_.empty() || pending_scan_ >= 0; }

private:
    std::deque<uint16_t> keys_;
    int pending_scan_ = -1;     // DOS hands an extended key over as 0x00 then the scan code
    int next_key_byte();
    struct {
        int16_t x = 320, y = 240;            // driver coordinates (virtual, = pixels in 12h)
        int16_t min_x = 0, max_x = 639, min_y = 0, max_y = 479;
        uint16_t buttons = 0;                // bit 0 left, 1 right, 2 middle
        int16_t show = -1;                   // >= 0: cursor visible (function 1/2 counter)
        int16_t dx = 0, dy = 0;              // motion counters, cleared by function 0Bh
        struct { uint16_t count = 0; int16_t x = 0, y = 0; } press[3], release[3];
        uint16_t handler_mask = 0;
        uint16_t handler_seg = 0, handler_off = 0;
    } mouse_;

    bool font_fetch(bool dbcs);
    void install_font_stubs();
    bool load_fontx(const std::string& path, std::vector<uint8_t>& out);
    std::vector<uint8_t> font_ank_, font_kanji_;
    uint8_t video_mode_ = 0x03;
    uint16_t cursor_ = 0;
    bool int21();
    bool int21_default(uint8_t n);
    void terminate(int code);
};

}  // namespace dosemu
