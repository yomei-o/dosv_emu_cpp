// Command-line front end: load a DOS .EXE/.COM and run it.
//   dosemu PROGRAM.EXE [guest args...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include "cpu.h"
#include "dos.h"
#include "memory.h"

namespace dosemu {
bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&, const std::string&, uint16_t);
}

static std::vector<uint8_t> read_file(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> v(n > 0 ? n : 0);
    if (n > 0 && std::fread(v.data(), 1, n, fp) != static_cast<size_t>(n)) v.clear();
    std::fclose(fp);
    return v;
}

// ---- the input script ------------------------------------------------------
//
// A graphics program with no window has no way to be driven, and no moment that
// is "now". `--after N --screenshot P` gives one picture; comparing a port
// against the original needs a *sequence* -- move here, click, press that, look
// -- replayed identically on both sides. So a run can be a script:
//
//     wait 13000000        # instructions, not seconds
//     mouse 320 240
//     click L
//     wait 2000000
//     key esc
//     type ABC
//     shot tmp/01.png
//
// Instructions are the clock on purpose. Wall time would put the two runs at
// different points on a busy machine, and there is nothing to synchronise on.
namespace {

// The BIOS word for a key: scan code in the high byte, ASCII in the low one.
// Only the scan codes a program actually looks at are here. JW_CAD reads
// ordinary keys as bytes through DOS and only ever inspects the scan code of
// the arrows and Home/End group (its jump table covers 0x47-0x53), so a rough
// table for the printable keys is enough and a wrong one would be worse.
uint16_t key_for_char(uint8_t c) {
    static const char* kRow[4] = {
        "1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./"
    };
    static const uint8_t kBase[4] = {0x02, 0x10, 0x1E, 0x2B};
    const uint8_t lower = static_cast<uint8_t>(std::tolower(c));
    for (int r = 0; r < 4; ++r)
        for (int i = 0; kRow[r][i]; ++i)
            if (static_cast<uint8_t>(kRow[r][i]) == lower)
                return static_cast<uint16_t>(((kBase[r] + i) << 8) | c);
    if (c == ' ') return 0x3920;
    if (c == '\r' || c == '\n') return 0x1C0D;
    return c;                                  // no scan code known; the ASCII still is
}

struct Step { std::string op, arg; long n = 0; int x = 0, y = 0; };

bool key_word(const std::string& name, uint16_t& out) {
    static const struct { const char* n; uint8_t scan, ascii; } kTable[] = {
        {"enter", 0x1C, 0x0D}, {"return", 0x1C, 0x0D}, {"esc", 0x01, 0x1B},
        {"tab", 0x0F, 0x09},   {"bs", 0x0E, 0x08},     {"backspace", 0x0E, 0x08},
        {"space", 0x39, 0x20},
        {"up", 0x48, 0}, {"down", 0x50, 0}, {"left", 0x4B, 0}, {"right", 0x4D, 0},
        {"home", 0x47, 0}, {"end", 0x4F, 0}, {"pgup", 0x49, 0}, {"pgdn", 0x51, 0},
        {"ins", 0x52, 0}, {"del", 0x53, 0},
        {"f1", 0x3B, 0}, {"f2", 0x3C, 0}, {"f3", 0x3D, 0}, {"f4", 0x3E, 0},
        {"f5", 0x3F, 0}, {"f6", 0x40, 0}, {"f7", 0x41, 0}, {"f8", 0x42, 0},
        {"f9", 0x43, 0}, {"f10", 0x44, 0}, {"f11", 0x85, 0}, {"f12", 0x86, 0},
    };
    for (const auto& k : kTable)
        if (name == k.n) { out = static_cast<uint16_t>((k.scan << 8) | k.ascii); return true; }
    if (name.size() == 1) { out = key_for_char(static_cast<uint8_t>(name[0])); return true; }
    if (name.size() == 3 && name[0] == 'x') {                // xHH: a raw byte
        out = static_cast<uint16_t>(std::strtoul(name.c_str() + 1, nullptr, 16) & 0xFF);
        return out != 0;
    }
    return false;
}

int button_index(const std::string& s) {
    if (s == "L" || s == "l" || s == "left") return 0;
    if (s == "R" || s == "r" || s == "right") return 1;
    if (s == "M" || s == "m" || s == "middle") return 2;
    return -1;
}

std::vector<Step> read_script(const char* path, std::string& err) {
    std::vector<Step> out;
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) { err = std::string("cannot read ") + path; return out; }
    char line[512];
    while (std::fgets(line, sizeof line, fp)) {
        std::string s(line);
        const size_t h = s.find('#');
        if (h != std::string::npos) s.erase(h);
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        s = s.substr(a);
        while (!s.empty() && std::strchr(" \t\r\n", s.back())) s.pop_back();
        const size_t sp = s.find_first_of(" \t");
        Step st;
        st.op = s.substr(0, sp);
        st.arg = sp == std::string::npos ? "" : s.substr(s.find_first_not_of(" \t", sp));
        if (st.op == "wait" || st.op == "run") st.n = std::strtol(st.arg.c_str(), nullptr, 10);
        else if (st.op == "mouse") {
            char* e = nullptr;
            st.x = static_cast<int>(std::strtol(st.arg.c_str(), &e, 10));
            st.y = static_cast<int>(std::strtol(e, nullptr, 10));
        }
        out.push_back(st);
    }
    std::fclose(fp);
    return out;
}

// Returns the exit code, or -1 if the script ran to the end without one.
int run_script(const std::vector<Step>& steps, dosemu::Cpu& cpu, dosemu::Dos& dos) {
    auto advance = [&](long n) {
        for (long i = 0; i < n && !cpu.halted; ++i) cpu.step();
    };
    for (const Step& st : steps) {
        if (cpu.halted && st.op != "shot") continue;    // a picture of the last screen still works
        if (st.op == "wait" || st.op == "run") advance(st.n);
        else if (st.op == "mouse") dos.mouse_move(static_cast<int16_t>(st.x),
                                                  static_cast<int16_t>(st.y));
        else if (st.op == "down" || st.op == "up" || st.op == "click") {
            const int b = button_index(st.arg);
            if (b < 0) { std::fprintf(stderr, "dosemu: script: unknown button '%s'\n", st.arg.c_str()); continue; }
            if (st.op != "up") dos.mouse_button(b, true);
            if (st.op != "down") dos.mouse_button(b, false);
        } else if (st.op == "key") {
            uint16_t k = 0;
            if (key_word(st.arg, k)) dos.push_key(k);
            else std::fprintf(stderr, "dosemu: script: unknown key '%s'\n", st.arg.c_str());
        } else if (st.op == "type") {
            for (char c : st.arg) dos.push_key(key_for_char(static_cast<uint8_t>(c)));
        } else if (st.op == "shot") {
            if (dos.vga.save_png(st.arg))
                std::fprintf(stderr, "dosemu: wrote %s  %dx%d  after %llu instructions\n",
                             st.arg.c_str(), dos.vga.width(), dos.vga.height(),
                             (unsigned long long)cpu.insns);
            else
                std::fprintf(stderr, "dosemu: no graphics screen to save (%s)\n", st.arg.c_str());
        } else if (st.op == "end") {
            break;
        } else {
            std::fprintf(stderr, "dosemu: script: unknown command '%s'\n", st.op.c_str());
        }
    }
    return cpu.halted ? cpu.exit_code : -1;
}

}  // namespace

int main(int argc, char** argv) {
    // dosemu [--root DIR] PROGRAM.EXE [guest args...]
    std::string root = ".";
    int a = 1;
    if (a < argc && std::string(argv[a]) == "--root" && a + 1 < argc) { root = argv[a + 1]; a += 2; }
    // DOS/V fonts, for INT 15h AX=5000h. A graphics program that draws text
    // asks the display driver for them and has none of its own.
    std::string font_ank, font_kanji, shot, script;
    uint64_t shot_after = 0;
    while (a + 1 < argc) {
        const std::string o = argv[a];
        if (o == "--font-ank") font_ank = argv[a + 1];
        else if (o == "--font-kanji") font_kanji = argv[a + 1];
        else if (o == "--screenshot") shot = argv[a + 1];
        else if (o == "--after") shot_after = strtoull(argv[a + 1], nullptr, 10);
        else if (o == "--script") script = argv[a + 1];
        else break;
        a += 2;
    }
    if (a >= argc) {
        std::fprintf(stderr, "usage: dosemu [--root DIR] [--font-ank F] [--font-kanji F]\n"
                             "              [--script F | --screenshot P --after N] PROGRAM.EXE [args...]\n");
        return 2;
    }

    std::vector<uint8_t> file = read_file(argv[a]);
    if (file.empty()) { std::fprintf(stderr, "dosemu: cannot read %s\n", argv[a]); return 1; }

    std::vector<Step> steps;
    if (!script.empty()) {
        std::string err;
        steps = read_script(script.c_str(), err);
        if (!err.empty()) { std::fprintf(stderr, "dosemu: %s\n", err.c_str()); return 1; }
    }

    std::string cmdline;
    for (int i = a + 1; i < argc; ++i) { cmdline += ' '; cmdline += argv[i]; }

    using namespace dosemu;
    Memory mem;
    Cpu cpu(mem);
    Dos dos(cpu, mem, root);
    if (!font_ank.empty() && !dos.set_font(font_ank, false))
        std::fprintf(stderr, "dosemu: %s is not a FONTX2 font\n", font_ank.c_str());
    if (!font_kanji.empty() && !dos.set_font(font_kanji, true))
        std::fprintf(stderr, "dosemu: %s is not a FONTX2 font\n", font_kanji.c_str());
    dos.output = [](int fd, const char* data, size_t len) {
        std::fwrite(data, 1, len, fd == 2 ? stderr : stdout);
        std::fflush(fd == 2 ? stderr : stdout);
    };
    dos.input = []() { int c = std::getchar(); return c == EOF ? -1 : c; };

    // The program's DOS path, as the guest will see it: A:\ plus its location under
    // --root. It has to be the real path, not just the base name — a program that lives
    // in a subdirectory finds its own installation by opening argv[0], and DJGPP's gcc
    // does exactly that. With the base name alone it reports `A:\GCC.EXE: can't open`.
    std::string dos_name;
    {
        auto slashes = [](std::string s) { for (char& c : s) if (c == '/') c = '\\'; return s; };
        std::string p = slashes(argv[a]), r = slashes(root);
        while (!r.empty() && r.back() == '\\') r.pop_back();
        std::string rel;
        if (!r.empty() && r != "." && p.size() > r.size() + 1 &&
            p.compare(0, r.size(), r) == 0 && p[r.size()] == '\\') {
            rel = p.substr(r.size() + 1);                       // inside the guest drive
        } else {
            auto s = p.find_last_of('\\');                      // elsewhere: base name only
            rel = (s == std::string::npos) ? p : p.substr(s + 1);
        }
        for (char& c : rel) c = static_cast<char>(std::toupper((unsigned char)c));
        dos_name = "A:\\" + rel;
    }

    std::string err;
    if (!load_program(file, cpu, 0x0100, cmdline, err, dos_name, dos.alloc_env(dos_name))) {
        std::fprintf(stderr, "dosemu: %s\n", err.c_str());
        return 1;
    }
    dos.psp_seg = 0x0100;
    dos.init_psp(0x0100, 0x0100, dos_name);   // no real parent; point at itself, as DOS does for the shell

    // A screenshot after a fixed number of instructions. The guest has no
    // window to close and no way to say "now" -- and for comparing screens
    // against a port of the same program, a fixed point in the run is what
    // makes the two comparable in the first place.
    try {
        if (!steps.empty()) {
            const int code = run_script(steps, cpu, dos);
            return code < 0 ? 0 : code;
        }
        if (shot.empty()) {
            cpu.run();
        } else {
            const uint64_t stop = shot_after ? shot_after : 400000000ull;
            while (!cpu.halted && cpu.insns < stop) cpu.step();
            if (dos.vga.save_png(shot))
                std::fprintf(stderr, "dosemu: wrote %s  %dx%d  after %llu instructions\n",
                             shot.c_str(), dos.vga.width(), dos.vga.height(),
                             (unsigned long long)cpu.insns);
            else
                std::fprintf(stderr, "dosemu: no graphics screen to save\n");
            return 0;
        }
    } catch (const CpuError& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "\ndosemu: %s  [%llu instructions]\n", e.what.c_str(),
                     static_cast<unsigned long long>(cpu.insns));
        return 1;
    }
    std::fflush(stdout);
    if (getenv("DOSEMU_OPHIST"))
        for (int i = 0; i < 512; ++i)
            if (cpu.ophist[i])
                std::fprintf(stderr, "[op] %s%02X %llu\n", i < 256 ? "" : "0F", i & 0xFF,
                             (unsigned long long)cpu.ophist[i]);
    if (getenv("DOSEMU_STATS"))
        std::fprintf(stderr, "[stats] %llu instructions, %llu x87 escapes no-opped\n",
                     (unsigned long long)cpu.insns, (unsigned long long)cpu.fpu_ops);
    return cpu.exit_code;
}
