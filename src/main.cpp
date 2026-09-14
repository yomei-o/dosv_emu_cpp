// Command-line front end: load a DOS .EXE/.COM and run it.
//   dosemu PROGRAM.EXE [guest args...]
#include <cstdio>
#include <cstdlib>
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

int main(int argc, char** argv) {
    // dosemu [--root DIR] PROGRAM.EXE [guest args...]
    std::string root = ".";
    int a = 1;
    if (a < argc && std::string(argv[a]) == "--root" && a + 1 < argc) { root = argv[a + 1]; a += 2; }
    // DOS/V fonts, for INT 15h AX=5000h. A graphics program that draws text
    // asks the display driver for them and has none of its own.
    std::string font_ank, font_kanji;
    while (a + 1 < argc && (std::string(argv[a]) == "--font-ank" ||
                            std::string(argv[a]) == "--font-kanji")) {
        (std::string(argv[a]) == "--font-ank" ? font_ank : font_kanji) = argv[a + 1];
        a += 2;
    }
    if (a >= argc) { std::fprintf(stderr, "usage: dosemu [--root DIR] PROGRAM.EXE [args...]\n"); return 2; }

    std::vector<uint8_t> file = read_file(argv[a]);
    if (file.empty()) { std::fprintf(stderr, "dosemu: cannot read %s\n", argv[a]); return 1; }

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

    try {
        cpu.run();
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
