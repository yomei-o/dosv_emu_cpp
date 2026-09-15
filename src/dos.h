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
#include "ime.h"

namespace dosemu {

// The two interrupts the built-in CON's entry points are made of, and where the
// DOS list-of-lists is handed out. A device driver is called through far
// pointers it reads out of a header, so the header needs real addresses in it;
// these are the addresses.
static constexpr uint8_t kDevStrat = 0xEC;
static constexpr uint8_t kDevInt = 0xED;
static constexpr uint16_t kLolSeg = 0x00F8;

// One glyph out of a FONTX2 file: the first byte of its bitmap, with the cell
// size. Rows are (w+7)/8 bytes, most significant bit leftmost.
const uint8_t* fontx_glyph(const std::vector<uint8_t>& f, uint16_t code, int& w, int& h);

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
        ime_.attach(&vga, &font_ank_, &font_kanji_);
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
        // Four kilobytes for talking to device drivers: the request packet, the
        // text of the CONFIG.SYS line, and a stack to call them on. After the
        // arena exists, not before -- an allocation out of an empty arena
        // returns segment zero, and then every driver call writes its stack
        // over the interrupt table.
        drv_work_ = mem_alloc(0x100);
        install_builtin_con();
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
    // A console read. With a device driver holding CON -- a Japanese FEP is one
    // -- the byte comes from *it*, which is the whole point: the conversion
    // happens inside what the application sees as INT 21h AH=07h. Without one,
    // the built-in console reads the keyboard directly.
    int  getch() { return con_driver_ ? con_read() : host_getch(); }

    // A console read, with our own FEP in the way.
    //
    // While the FEP is on, keys belong to it: it takes them, runs the
    // conversion, and only what has been confirmed comes out here. That is why
    // the loop can spin -- a DOS read *blocks* until there is a character, and
    // a FEP is exactly the thing that makes it block for a while. With nothing
    // left to read (a script that has run out, EOF on stdin) the loop stops and
    // the read fails, which is what it did before there was a FEP.
    int  host_getch() {
        for (;;) {
            if (ime_.has_out()) return ime_.pop_out();
            const int k = raw_key();
            if (k < 0) return -1;
            if (!ime_.feed(k)) return k;
        }
    }
    // One key byte, from a script or from stdin, before the FEP sees it.
    int  raw_key() {
        const int k = next_key_byte();
        if (k >= 0) return k;
        int c = input ? input() : -1; return c == '\n' ? '\r' : c;   // -1 at EOF
    }
    // Let the FEP have whatever has been typed, so that a program asking "is a
    // key ready?" gets the truth. JW_CAD asks that first and only reads when
    // the answer is yes, so without this the conversion would never run: the
    // keys would sit in the queue making the answer yes, and every read would
    // hand them to the FEP and find nothing to return.
    void pump_ime() {
        if (!ime_.on()) return;
        while (!ime_.has_out()) {
            const int k = next_key_byte();     // scripted keys only: stdin blocks
            if (k < 0) break;
            if (!ime_.feed(k)) { pushback_ = k; break; }
        }
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
    void push_key(uint16_t k) { kbd_push(k); }
    bool kbd_push(uint16_t key);
    int  kbd_pop(bool take);

    // Device drivers, as CONFIG.SYS loads them. A DOS/V Japanese FEP is one of
    // these -- a character device that takes over the name CON -- so this is
    // what it takes to run a real one. See src/device.cpp.
    struct Device { uint16_t seg, hdr, attr; char name[9]; };
    bool load_device(const std::string& path, const std::string& args, std::string& err);
    const std::vector<Device>& devices() const { return devices_; }
    bool device_int(bool strategy);       // the built-in CON's two entry points
    // Where the program can be loaded: above whatever the drivers took, which is
    // the order a real DOS boots in -- CONFIG.SYS first, then the program.
    uint16_t next_psp() const {
        if (blocks_.empty()) return 0x0100;
        const Block& b = blocks_.back();
        return b.used ? 0x0100 : static_cast<uint16_t>(b.seg + 1);
    }

    // One byte of guest memory, for the script's `dump`.
    uint8_t peek(uint16_t seg, uint16_t off) const { return mem_.rb(seg, off); }
    // "Is there a character to read?" -- with the FEP on, only a confirmed one
    // counts; the half-typed ones are inside it.
    bool keys_waiting() {
        pump_ime();
        if (ime_.on()) return ime_.has_out() || pushback_ >= 0;
        return kbd_pop(false) >= 0 || pending_scan_ >= 0 || pushback_ >= 0;
    }
    Ime& ime() { return ime_; }

private:
    std::deque<uint16_t> keys_;
    void install_builtin_con();
    void publish_lol();
    bool call_far(uint16_t seg, uint16_t off, uint16_t es, uint16_t bx);
    bool con_request();
    int  con_read();                       // one byte, through the CON driver
    bool con_ready();                      // ...and "is one there?"
    bool con_driver_ = false;              // a loaded driver took the name CON
    std::vector<Device> devices_;
    uint16_t con_seg_ = 0, con_hdr_ = 0;   // whatever is CON now
    uint16_t req_seg_ = 0, req_off_ = 0;   // the packet the last STRATEGY was given
    uint16_t drv_work_ = 0;                // request packet, argument text and stack

    int pending_scan_ = -1;     // DOS hands an extended key over as 0x00 then the scan code
    int pushback_ = -1;         // a key the FEP looked at and did not want
    Ime ime_;
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
