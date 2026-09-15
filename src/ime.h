// A Japanese front-end processor, on the host's side of the console.
//
// DOS/V applications do not convert Japanese themselves. JW_CAD reads one byte
// at a time from DOS (INT 21h AH=07h) and draws whatever comes back; if a byte
// is a Shift-JIS lead byte it reads the next one and draws a kanji. Everything
// in between -- romaji to kana, kana to kanji, the window the candidates appear
// in -- belongs to a FEP, a resident program that hooks the console. None ships
// with JW_CAD (its manual says to install one), and the free ones that survive
// are single-kanji multi-shift oddities, so there is nothing to run and nothing
// to copy: this is the FEP.
//
// It lives on the host side for the same reason the DOS/V font interface does.
// A FEP is, from the application's point of view, part of the machine: it never
// calls it, never sees its window, and cannot tell it from DOS. Writing it as a
// resident 8086 program would be a faithful *shape* and a much worse tool --
// the conversion logic has to be readable and testable, and it is wanted again
// in the C port, which has no 8086 in it at all.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dosemu {

class Vga;

class Ime {
public:
    // The screen to draw the window on, and the two fonts to draw it with --
    // the same FONTX2 files the guest reads its glyphs from, so the window is
    // in the same typeface as the application's own text.
    void attach(Vga* v, const std::vector<uint8_t>* ank, const std::vector<uint8_t>* kanji) {
        vga_ = v; ank_ = ank; kanji_ = kanji;
    }

    bool on() const { return on_; }
    void set_on(bool v);

    // Offer one byte from the keyboard. True means the FEP took it and the
    // guest must not see it -- it went into the conversion, and what the guest
    // eventually reads comes out of pop_out() instead.
    bool feed(int byte);

    bool has_out() const { return !out_.empty(); }
    int  pop_out();

    // Is there anything half-typed? A conversion in progress is why a console
    // read blocks even though keys have been pressed: the keys are here, inside
    // the FEP, and no confirmed byte has come of them yet.
    bool busy() const { return !raw_.empty() || !kana_.empty(); }

private:
    void draw();
    void erase();
    void confirm_kana();
    void put_kana(uint16_t code);
    void take_romaji();
    void cell(int x, int y, uint16_t code, uint8_t fg, uint8_t bg);
    int  text(int x, int y, const std::string& s, uint8_t fg, uint8_t bg);

    Vga* vga_ = nullptr;
    const std::vector<uint8_t>* ank_ = nullptr;
    const std::vector<uint8_t>* kanji_ = nullptr;

    bool on_ = false;
    std::string raw_;              // romaji typed but not yet a kana
    std::string kana_;             // the undecided string, Shift-JIS
    std::deque<uint8_t> out_;      // confirmed bytes, waiting for the guest
    std::vector<uint8_t> saved_;   // the screen under the window
    bool shown_ = false;
};

}  // namespace dosemu
