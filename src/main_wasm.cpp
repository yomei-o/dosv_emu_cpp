// The browser front end: the same emulator, driven from a page instead of a
// script.
//
// There is no window and no terminal here, so the shape is different from
// src/main.cpp -- the page owns the clock. It asks for a slice of instructions
// per animation frame, takes the screen as RGBA, and pushes the mouse and the
// keyboard in. Everything below that (Cpu, Dos, Vga) is the same code the
// native build runs, which is the point: what the page shows is what the
// emulator does, not a second implementation of it.
//
// The guest's files are baked into the image at build time (tools/build_wasm.sh
// --embed-file), so the page needs no server and no upload: it opens JW_CADV.EXE
// out of the same orig/ directory the native runs use.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "cpu.h"
#include "dos.h"
#include "memory.h"

// The loader lives in src/loader.cpp and has no header of its own -- it sits
// in namespace dosemu, so the declaration has to as well.
namespace dosemu {
bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&,
                  std::string&, const std::string&, uint16_t);
}

using namespace dosemu;

namespace {

// One of everything, for the life of the page. They are pointers rather than
// objects because a reset has to put the guest back to the state it booted in,
// and the cleanest way to do that is to build the three again.
Memory* mem;
Cpu* cpu;
Dos* dos;

std::vector<unsigned char> frame;   // width*height*4, handed to the canvas
std::string console;                // what the guest wrote before it went graphic
std::string message;                // why it stopped, if it did
bool dead;

// Where the program loaded, so a page (or a later probe) can name addresses the
// way a disassembly does.
uint16_t load_seg;

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(static_cast<size_t>(n));
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

}  // namespace

extern "C" {

// Boot the guest. `drawing` is the .JWC to open, as the command line the
// program is given -- the same argument the native build takes.
EMSCRIPTEN_KEEPALIVE int de_boot(const char* drawing) {
    delete dos; delete cpu; delete mem;
    dos = nullptr; cpu = nullptr; mem = nullptr;
    console.clear();
    message.clear();
    dead = false;

    mem = new Memory();
    cpu = new Cpu(*mem);
    dos = new Dos(*cpu, *mem, "orig");
    dos->set_font("font/JWANK16.FNT", false);
    dos->set_font("font/JWKAN16.FNT", true);
    // The guest's own writes to stdout and stderr. JW_CAD says very little --
    // it goes graphic almost at once -- but when it refuses to start it says
    // why, and that sentence is the most useful thing the page can show.
    dos->output = [](int, const char* data, size_t len) {
        console.append(data, len);
        if (console.size() > 8000) console.erase(0, console.size() - 8000);
    };
    dos->input = []() { return -1; };

    std::vector<uint8_t> file = read_file("orig/JW_CADV.EXE");
    if (file.empty()) { message = "orig/JW_CADV.EXE is not in the image"; dead = true; return 0; }

    std::string cmdline;
    if (drawing && *drawing) { cmdline = " "; cmdline += drawing; }

    std::string err;
    const std::string dos_name = "A:\\JW_CADV.EXE";
    const uint16_t psp = dos->next_psp();
    if (!load_program(file, *cpu, psp, cmdline, err, dos_name, dos->alloc_env(dos_name))) {
        message = err;
        dead = true;
        return 0;
    }
    load_seg = static_cast<uint16_t>(psp + 0x10);
    dos->psp_seg = psp;
    dos->init_psp(psp, psp, dos_name);
    return 1;
}

// Run a slice. The page decides how big: enough that the guest gets on with
// it, small enough that the frame still lands. Returns the instruction count.
EMSCRIPTEN_KEEPALIVE double de_run(int slice) {
    if (!cpu || dead) return cpu ? static_cast<double>(cpu->insns) : 0.0;
    try {
        for (int i = 0; i < slice && !cpu->halted; ++i) cpu->step();
    } catch (const CpuError& e) {
        message = e.what;
        dead = true;
    }
    if (cpu->halted && message.empty()) message = "the program ended";
    return static_cast<double>(cpu->insns);
}

EMSCRIPTEN_KEEPALIVE int de_width(void)  { return dos ? dos->vga.width() : 0; }
EMSCRIPTEN_KEEPALIVE int de_height(void) { return dos ? dos->vga.height() : 0; }
EMSCRIPTEN_KEEPALIVE int de_graphics(void) { return dos && dos->vga.graphics(); }
EMSCRIPTEN_KEEPALIVE int de_dead(void) { return dead || (cpu && cpu->halted); }
EMSCRIPTEN_KEEPALIVE const char* de_message(void) { return message.c_str(); }
EMSCRIPTEN_KEEPALIVE const char* de_console(void) { return console.c_str(); }
EMSCRIPTEN_KEEPALIVE int de_load_seg(void) { return load_seg; }

// The screen, as RGBA. Null until the guest sets a graphics mode.
EMSCRIPTEN_KEEPALIVE unsigned char* de_frame(void) {
    if (!dos || !dos->vga.graphics()) return nullptr;
    const size_t want = static_cast<size_t>(dos->vga.width()) * dos->vga.height() * 4;
    if (frame.size() != want) frame.resize(want);
    if (!dos->vga.rgba(frame.data())) return nullptr;
    return frame.data();
}

EMSCRIPTEN_KEEPALIVE void de_mouse(int x, int y) {
    if (dos) dos->mouse_move(static_cast<int16_t>(x), static_cast<int16_t>(y));
}

EMSCRIPTEN_KEEPALIVE void de_button(int button, int down) {
    if (dos) dos->mouse_button(button, down != 0);
}

// A key, as the BIOS presents it: scan code in the high byte, ASCII in the low
// one. The page sends Japanese the same way a DOS/V front-end processor does --
// the two bytes of a Shift-JIS character as two ordinary keys.
EMSCRIPTEN_KEEPALIVE void de_key(int word) {
    if (dos) dos->push_key(static_cast<uint16_t>(word));
}

// Which modifier keys are held, as INT 16h AH=02h/12h reports them: bit 0
// right shift, 1 left shift, 2 Ctrl, 3 Alt.
EMSCRIPTEN_KEEPALIVE void de_mods(int flags) {
    if (dos) dos->set_mods(static_cast<uint8_t>(flags));
}

}  // extern "C"

int main(void) { return 0; }
