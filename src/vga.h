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

    // One byte per pixel, 0-15, row-major. Returns false in a text mode.
    bool snapshot(unsigned char* out) const;
    bool save_png(const std::string& path) const;

    int width() const { return width_; }
    int height() const { return height_; }

private:
    uint8_t plane_[kPlanes][kPlaneBytes] = {};
    uint8_t latch_[kPlanes] = {};
    uint8_t gc_[16] = {};
    uint8_t seq_[8] = {};
    uint8_t gc_index_ = 0, seq_index_ = 0;
    uint8_t status_ = 0;
    int width_ = 0, height_ = 0, stride_ = 0;
};

}  // namespace dosemu
