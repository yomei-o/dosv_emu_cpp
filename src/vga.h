// The VGA, as a DOS/V graphics program uses it.
//
// Mode 12h is 640x480 in sixteen colours, and those colours are not bytes in
// memory: the screen is four one-bit planes that all answer to the same
// addresses at A000:0000, and which plane a write reaches is decided by the
// graphics controller at ports 0x3CE/0x3CF. So `mov es:[di], al` means nothing
// on its own -- the same instruction paints a colour, ORs a mask or stamps a
// glyph depending on registers set several instructions earlier.
//
// JW_CAD uses three of the four write modes:
//
//   mode 0  lines      colour in set/reset, the byte written is discarded
//   mode 2  arc pixels the low four bits of the byte are the colour
//   mode 3  glyphs     the byte becomes the bit mask; colour from set/reset
//
// This is written from the hardware's behaviour rather than shared with the
// port in jwcad_dos_wasm, on purpose. The whole point of running the original
// here is to check the port against it, and two implementations of the same
// spec catch each other's mistakes in a way one shared implementation cannot.
#pragma once
#include <cstdint>
#include <string>

namespace dosemu {

class Vga {
public:
    static constexpr uint32_t kBase = 0xA0000;
    static constexpr uint32_t kEnd  = 0xB0000;
    static constexpr int kPlanes = 4;
    static constexpr int kPlaneBytes = 0x10000;

    // Graphics controller registers, by index.
    enum { SET_RESET = 0, ENABLE_SR = 1, DATA_ROTATE = 3, READ_MAP = 4,
           MODE = 5, MISC = 6, BIT_MASK = 8 };

    bool io_out(uint16_t port, uint8_t v);
    bool io_in(uint16_t port, uint8_t& v);

    void write(uint32_t lin, uint8_t v);
    uint8_t read(uint32_t lin);

    // Set by INT 10h AH=00h so a screenshot knows the geometry.
    void set_mode(uint8_t mode, int width, int height, int stride);
    bool graphics() const { return width_ > 0; }

    // One pixel, straight into the four planes.
    //
    // This is not the guest's path -- Vga::write() is, through the graphics
    // controller. It is the *host's*: a DOS/V FEP draws its conversion window
    // through the display driver, which is host code as far as the application
    // is concerned, and our FEP is host code too. Reading a pixel back is for
    // putting the screen the way it was when the window closes.
    void put_pixel(int x, int y, uint8_t colour);
    uint8_t get_pixel(int x, int y) const;

    // One byte per pixel, 0-15, row-major. Returns false in a text mode.
    bool snapshot(unsigned char* out) const;
    bool save_png(const std::string& path) const;
    // The same picture as four bytes per pixel, R G B A, row by row -- byte for byte
    // what jwcad_dos_wasm's jw_view_rgba() produces. That is the point: the two
    // screens are compared with cmp(1), so the format has to be the port's, not a
    // convenient one.
    bool save_raw(const std::string& path) const;
    // The same RGBA, straight into a caller's buffer of width*height*4 bytes.
    // The browser front end (src/main_wasm.cpp) blits it into a canvas every
    // frame, and writing it to a file first would be silly there.
    bool rgba(unsigned char* out) const;

    int width() const { return width_; }
    int height() const { return height_; }

    // The colours. A pixel's four planes give an index 0-15; the attribute
    // controller's palette turns that into a DAC entry, and the DAC holds the
    // actual red/green/blue, six bits each. Both are guest state: JW_CAD reads
    // JW_PAL.DAT and installs all sixteen through INT 10h (AX=1010h per entry,
    // AX=1000h to make the attribute palette the identity), and its colours are
    // not the EGA defaults -- 4 is green where the default is red, 7 is white
    // where the default is light grey. A screenshot painted with the defaults
    // is the wrong picture, and comparing it against the port proves nothing.
    void set_pal(uint8_t reg, uint8_t v) { if (reg < 16) pal_[reg] = v & 0x3F; }
    void set_dac(uint8_t reg, uint8_t r, uint8_t g, uint8_t b) {
        dac_[reg][0] = r & 0x3F; dac_[reg][1] = g & 0x3F; dac_[reg][2] = b & 0x3F;
    }
    void get_dac(uint8_t reg, uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = dac_[reg][0]; g = dac_[reg][1]; b = dac_[reg][2];
    }
    uint8_t get_pal(uint8_t reg) const { return reg < 16 ? pal_[reg] : 0; }
    // 6-bit DAC value to 8-bit, by replicating the top bits: 0x3F -> 0xFF and
    // 0x2A -> 0xAA exactly. The port has to use the same rule or the two
    // screenshots differ in colour while agreeing on every pixel.
    static uint8_t dac8(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

private:
    uint8_t plane_[kPlanes][kPlaneBytes] = {};
    uint8_t latch_[kPlanes] = {};
    uint8_t gc_[16] = {};
    uint8_t seq_[8] = {};
    uint8_t gc_index_ = 0, seq_index_ = 0;
    uint8_t status_ = 0;
    int width_ = 0, height_ = 0, stride_ = 0;
    uint8_t pal_[16] = {};          // attribute controller: index -> DAC entry
    uint8_t dac_[256][3] = {};      // DAC: six bits per channel
    void reset_palette();
};

}  // namespace dosemu
