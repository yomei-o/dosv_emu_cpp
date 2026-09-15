// The DOS/V text screen: BIOS text output drawn into the graphics screen.
//
// On a DOS/V machine there is no text mode. The display driver keeps a text
// buffer and *draws* every character into the VGA with the fonts loaded from
// disk, which is why the machine can show kanji at all. JW_CAD never uses any
// of it -- it draws its own menus -- but a FEP does: 鳳's DOS/V screen code is
// `mov ah,13h / int 10h` and nothing else, because a resident program cannot
// know where the application keeps its pixels and has to go through the BIOS.
//
// So this is the part of the display driver a FEP needs: put the characters on
// the screen, remember them, and put back what was underneath when the window
// closes. The FEP does the remembering itself -- AH=13h writes the window, and
// AH=13h writes the saved line back over it.
#include "dos.h"

#include "cpu.h"
#include "memory.h"
#include "vga.h"

#include <cstring>

namespace dosemu {

// The colours an attribute byte names, as the DOS/V display driver uses them:
// the low nibble is the foreground and the high nibble the background, both
// straight into the sixteen palette entries.
static void cell_colours(uint8_t attr, uint8_t& fg, uint8_t& bg) {
    fg = static_cast<uint8_t>(attr & 0x0F);
    bg = static_cast<uint8_t>((attr >> 4) & 0x07);
}

// What the screen holds, cell by cell: the byte in this cell and its attribute.
// A DOS/V display driver keeps this because the BIOS can be asked to read the
// screen back -- 鳳 saves the two lines it is about to write on and puts them
// back when the conversion ends, and with nothing remembered it would put back
// blanks. Only what came through the BIOS is in here; what a graphics program
// drew itself is, quite correctly, unknown.
uint16_t& Dos::text_at(int col, int row) {
    const int cols = mem_.rw(0x40, 0x004A) ? mem_.rw(0x40, 0x004A) : 80;
    const int rows = mem_.rb(0x40, 0x0084) + 2;      // +1 for the count, +1 for a split-off line
    static uint16_t nowhere;
    if (col < 0 || row < 0 || col >= cols || row >= rows) return nowhere = 0x0720;
    if (text_ram_.size() != static_cast<size_t>(cols * rows)) text_ram_.assign(cols * rows, 0x0720);
    return text_ram_[row * cols + col];
}

// One character cell at (col,row). A double-byte code takes two cells and is
// drawn from the 16x16 font; everything else comes from the 8x16 one.
void Dos::text_cell(int col, int row, uint16_t code, uint8_t attr) {
    if (code >= 0x100) {
        text_at(col, row) = static_cast<uint16_t>((attr << 8) | (code >> 8));
        text_at(col + 1, row) = static_cast<uint16_t>((attr << 8) | (code & 0xFF));
    } else {
        text_at(col, row) = static_cast<uint16_t>((attr << 8) | code);
    }
    if (!vga.graphics()) return;
    uint8_t fg, bg;
    cell_colours(attr, fg, bg);
    const bool dbcs = code >= 0x100;
    const std::vector<uint8_t>& f = dbcs ? font_kanji_ : font_ank_;
    int w = dbcs ? 16 : 8, h = 16;
    const uint8_t* g = fontx_glyph(f, code, w, h);
    const int stride = (w + 7) / 8;
    const int x0 = col * 8, y0 = row * 16;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool ink = g && (g[y * stride + (x >> 3)] & (0x80 >> (x & 7)));
            vga.put_pixel(x0 + x, y0 + y, ink ? fg : bg);
        }
}

// Where the cursor is, in the BIOS data area, so that a guest reading it
// directly sees the same thing AH=03h reports.
void Dos::text_cursor(int& col, int& row) const {
    col = mem_.rb(0x40, 0x0050);
    row = mem_.rb(0x40, 0x0051);
}
void Dos::text_set_cursor(int col, int row) {
    mem_.wb(0x40, 0x0050, static_cast<uint8_t>(col));
    mem_.wb(0x40, 0x0051, static_cast<uint8_t>(row));
}

// A run of characters starting at (col,row), the way AH=13h and AH=09h put
// them: no control-code handling, no wrapping past the last row.
//
// The Shift-JIS pairing is done here rather than by the caller because the
// caller is a BIOS function that was handed a length in *bytes*: a FEP writes
// its window as one string of mixed kanji and ASCII and expects the driver to
// work out which is which.
int Dos::text_write(int col, int row, const uint8_t* s, int n, const uint8_t* attrs,
                    uint8_t attr) {
    const int cols = mem_.rw(0x40, 0x004A) ? mem_.rw(0x40, 0x004A) : 80;
    for (int i = 0; i < n; ) {
        const uint8_t c = s[i];
        const uint8_t a = attrs ? attrs[i] : attr;
        const bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        if (lead && i + 1 < n) {
            text_cell(col, row, static_cast<uint16_t>((c << 8) | s[i + 1]), a);
            col += 2;
            i += 2;
        } else {
            text_cell(col, row, c, a);
            col += 1;
            i += 1;
        }
        if (col >= cols) { col = 0; ++row; }
    }
    return col;
}

// INT 10h's text side. Only what a resident program actually calls: a FEP
// writes strings and moves the cursor, and reads the mode back to find out what
// it is drawing on.
bool Dos::int10_text(uint8_t ah) {
    // What a resident program puts on the screen, as it asked for it. A FEP's
    // window is the one thing here nobody can see being drawn -- it is gone
    // again by the time the next screenshot is taken -- so this is how to find
    // out whether it wrote nothing, or wrote it somewhere else.
    static const bool tr = getenv("DOSEMU_TEXT_TRACE") != nullptr;
    if (tr) std::fprintf(stderr, "[text] ah=%02X al=%02X bx=%04X cx=%04X dx=%04X es:bp=%04X:%04X\n",
                         ah, cpu_.r[AX] & 0xFF, cpu_.r[BX], cpu_.r[CX], cpu_.r[DX],
                         cpu_.sreg[ES], cpu_.r[BP]);
    const int cols = mem_.rw(0x40, 0x004A) ? mem_.rw(0x40, 0x004A) : 80;
    switch (ah) {
        case 0x09: case 0x0A: {                 // write char (and attribute) n times
            int col, row;
            text_cursor(col, row);
            const uint8_t attr = ah == 0x09 ? static_cast<uint8_t>(cpu_.r[BX] & 0xFF) : 0x07;
            const uint8_t ch = static_cast<uint8_t>(cpu_.r[AX] & 0xFF);
            for (uint16_t i = 0; i < (cpu_.r[CX] ? cpu_.r[CX] : 1); ++i) {
                text_cell(col, row, ch, attr);
                if (++col >= cols) { col = 0; ++row; }
            }
            return true;
        }
        case 0x0E: {                            // teletype: one character, cursor moves
            int col, row;
            text_cursor(col, row);
            const uint8_t ch = static_cast<uint8_t>(cpu_.r[AX] & 0xFF);
            if (ch == '\r') col = 0;
            else if (ch == '\n') ++row;
            else if (ch == 0x08) { if (col) --col; }
            else {
                text_cell(col, row, ch, 0x07);
                if (++col >= cols) { col = 0; ++row; }
            }
            text_set_cursor(col, row);
            return true;
        }
        // A string at ES:BP, CX cells, at the place DH/DL names, attribute in BL.
        // AL bit 1 says the string carries its own attribute after every
        // character and bit 0 whether the cursor follows -- and then DOS/V adds
        // two of its own, which is what a FEP here uses: bit 5 is the same as
        // bit 1 (鳳 writes 96h 07h 50h 07h for 「鳳」, a character and an
        // attribute apiece) and bit 4 turns the call round, filling ES:BP from
        // the screen instead. Saving a line and putting it back is the only way
        // a resident program can borrow one, so without the read half its window
        // never closes: it writes blanks over whatever was underneath.
        case 0x13: {
            const uint16_t al = cpu_.r[AX] & 0xFF;
            const uint16_t n = cpu_.r[CX];
            int col = cpu_.r[DX] & 0xFF, row = (cpu_.r[DX] >> 8) & 0xFF;
            std::vector<uint8_t> text, attrs;
            const uint16_t seg = cpu_.sreg[ES], off = cpu_.r[BP];
            const bool pairs = (al & 2) || (al & 0x20);
            if (al & 0x10) {                    // read the screen into ES:BP
                for (uint16_t i = 0; i < n; ++i) {
                    const uint16_t cell = text_at(col + i, row);
                    mem_.wb(seg, static_cast<uint16_t>(off + i * 2), static_cast<uint8_t>(cell & 0xFF));
                    mem_.wb(seg, static_cast<uint16_t>(off + i * 2 + 1), static_cast<uint8_t>(cell >> 8));
                }
                if (tr) std::fprintf(stderr, "[text]   read %u cells at %d,%d\n", n, col, row);
                return true;
            }
            if (pairs) {
                for (uint16_t i = 0; i < n; ++i) {
                    text.push_back(mem_.rb(seg, static_cast<uint16_t>(off + i * 2)));
                    attrs.push_back(mem_.rb(seg, static_cast<uint16_t>(off + i * 2 + 1)));
                }
            } else {
                for (uint16_t i = 0; i < n; ++i)
                    text.push_back(mem_.rb(seg, static_cast<uint16_t>(off + i)));
            }
            if (tr) {
                std::fprintf(stderr, "[text]   \"");
                for (uint8_t c : text) std::fprintf(stderr, "%c", c >= 0x20 ? c : '.');
                std::fprintf(stderr, "\" at %d,%d\n", col, row);
            }
            const int end = text_write(col, row, text.data(), static_cast<int>(text.size()),
                                       pairs ? attrs.data() : nullptr,
                                       static_cast<uint8_t>(cpu_.r[BX] & 0xFF));
            if (al & 1) text_set_cursor(end, row);
            return true;
        }
        default: return false;
    }
}

}  // namespace dosemu
