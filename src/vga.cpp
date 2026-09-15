#include "vga.h"
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <vector>

namespace dosemu {

// The sixteen colours mode 12h starts with -- the EGA default the BIOS loads.
static const unsigned char kEgaRgb[16][3] = {
    {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
    {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
    { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
    {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255},
};

bool Vga::io_out(uint16_t port, uint8_t v) {
    switch (port) {
        case 0x3C4: seq_index_ = v & 7; return true;
        case 0x3C5: seq_[seq_index_] = v; return true;
        case 0x3CE: gc_index_ = v & 0x0F; return true;
        case 0x3CF: gc_[gc_index_] = v; return true;
        default: return false;
    }
}

bool Vga::io_in(uint16_t port, uint8_t& v) {
    switch (port) {
        case 0x3C5: v = seq_[seq_index_]; return true;
        case 0x3CF: v = gc_[gc_index_]; return true;
        // Input status 1. Bit 3 is vertical retrace and bit 0 the display
        // enable; a program that waits for one of them has to see it change or
        // it waits for ever, so toggle both on every read.
        case 0x3BA: case 0x3DA: status_ ^= 0x09; v = status_; return true;
        default: return false;
    }
}

// What the BIOS leaves behind after a mode set: the attribute palette is the
// identity and the DAC holds the sixteen EGA colours, so a guest that never
// touches either draws in the colours everyone expects.
void Vga::reset_palette() {
    for (int i = 0; i < 16; ++i) {
        pal_[i] = static_cast<uint8_t>(i);
        for (int c = 0; c < 3; ++c)
            dac_[i][c] = static_cast<uint8_t>(kEgaRgb[i][c] * 63 / 255);
    }
}

void Vga::set_mode(uint8_t mode, int width, int height, int stride) {
    (void)mode;
    width_ = width;
    height_ = height;
    stride_ = stride;
    std::memset(plane_, 0, sizeof plane_);
    std::memset(latch_, 0, sizeof latch_);
    gc_[BIT_MASK] = 0xFF;
    seq_[2] = 0x0F;                       // plane write mask: all four
    reset_palette();
}

uint8_t Vga::read(uint32_t lin) {
    const uint32_t off = (lin - kBase) & (kPlaneBytes - 1);
    for (int p = 0; p < kPlanes; ++p) latch_[p] = plane_[p][off];
    return latch_[gc_[READ_MAP] & 3];     // read mode 0; nothing here uses mode 1
}

void Vga::write(uint32_t lin, uint8_t data) {
    const uint32_t off = (lin - kBase) & (kPlaneBytes - 1);
    const uint8_t mode = gc_[MODE] & 3;
    const uint8_t rot = gc_[DATA_ROTATE] & 7;
    const uint8_t op = (gc_[DATA_ROTATE] >> 3) & 3;
    const uint8_t enable = gc_[ENABLE_SR] & 0x0F;
    const uint8_t setres = gc_[SET_RESET];
    const uint8_t wmask = seq_[2] & 0x0F;
    uint8_t bits = gc_[BIT_MASK];

    // The read half of a read-modify-write is what loads the latches, and the
    // guest relies on having done it; but a bare write has to work too, so the
    // latches are whatever the last read left.
    if (rot && mode != 2) data = static_cast<uint8_t>((data >> rot) | (data << (8 - rot)));
    if (mode == 3) bits = static_cast<uint8_t>(bits & data);

    for (int p = 0; p < kPlanes; ++p) {
        if (!(wmask & (1 << p))) continue;
        uint8_t src;
        if (mode == 1) { plane_[p][off] = latch_[p]; continue; }
        else if (mode == 2) src = (data & (1 << p)) ? 0xFF : 0x00;
        else if (mode == 3 || (enable & (1 << p))) src = (setres & (1 << p)) ? 0xFF : 0x00;
        else src = data;

        uint8_t val;
        switch (op) {
            case 1: val = static_cast<uint8_t>(src & latch_[p]); break;
            case 2: val = static_cast<uint8_t>(src | latch_[p]); break;
            case 3: val = static_cast<uint8_t>(src ^ latch_[p]); break;
            default: val = src; break;
        }
        plane_[p][off] = static_cast<uint8_t>((val & bits) | (latch_[p] & ~bits));
    }
}

// The host's own pixel, bypassing the graphics controller: the four planes get
// one bit each of the colour, at the bit the x position names.
void Vga::put_pixel(int x, int y, uint8_t colour) {
    if (width_ <= 0 || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const int off = y * stride_ + (x >> 3);
    const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
    for (int p = 0; p < kPlanes; ++p) {
        if (colour & (1u << p)) plane_[p][off] |= bit;
        else                    plane_[p][off] = static_cast<uint8_t>(plane_[p][off] & ~bit);
    }
}

uint8_t Vga::get_pixel(int x, int y) const {
    if (width_ <= 0 || x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    const int off = y * stride_ + (x >> 3);
    const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
    uint8_t c = 0;
    for (int p = 0; p < kPlanes; ++p)
        if (plane_[p][off] & bit) c |= static_cast<uint8_t>(1u << p);
    return c;
}

bool Vga::snapshot(unsigned char* out) const {
    // DOSEMU_VRAM_MAP=1: how much of each plane is set inside the visible page and
    // how much above it. "The screen is blank, so nothing was drawn" is a guess, and
    // the wrong one if the guest drew to a second page and never flipped: 64 KB per
    // plane holds two 640x480 pages, and mode 12h shows the first 38,400 bytes.
    // This says which of the two it is in one line.
    if (getenv("DOSEMU_VRAM_MAP")) {
        const int page = width_ > 0 ? stride_ * height_ : kPlaneBytes;
        for (int p = 0; p < kPlanes; ++p) {
            long lo = 0, hi = 0;
            for (int i = 0; i < kPlaneBytes; ++i)
                (i < page ? lo : hi) += plane_[p][i] ? 1 : 0;
            std::fprintf(stderr, "[vram] plane %d: %ld bytes set in the visible page, %ld above\n",
                         p, lo, hi);
        }
    }

    if (width_ <= 0 || height_ <= 0) return false;
    for (int y = 0; y < height_; ++y) {
        const int row = y * stride_;
        for (int xb = 0; xb < stride_; ++xb) {
            for (int bit = 7; bit >= 0; --bit) {
                const uint8_t m = static_cast<uint8_t>(1u << bit);
                *out++ = static_cast<unsigned char>(
                    ((plane_[0][row + xb] & m) ? 1 : 0) |
                    ((plane_[1][row + xb] & m) ? 2 : 0) |
                    ((plane_[2][row + xb] & m) ? 4 : 0) |
                    ((plane_[3][row + xb] & m) ? 8 : 0));
            }
        }
    }
    return true;
}

// --- a minimal indexed PNG -------------------------------------------------
//
// Just enough of zlib to be valid: CRC-32, Adler-32 and stored deflate blocks.
// The files are bigger than they need to be and that is fine; they exist to be
// looked at and diffed against the port's own screenshots.
static uint32_t crc32x(const unsigned char* b, size_t n, uint32_t crc = 0) {
    static uint32_t tab[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        ready = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = tab[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void put32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(static_cast<unsigned char>(x >> 24));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x));
}

static void chunk(std::FILE* f, const char* tag, const unsigned char* d, uint32_t n) {
    std::vector<unsigned char> head;
    put32(head, n);
    std::fwrite(head.data(), 1, 4, f);
    std::vector<unsigned char> body(tag, tag + 4);
    body.insert(body.end(), d, d + n);
    std::fwrite(body.data(), 1, body.size(), f);
    std::vector<unsigned char> crc;
    put32(crc, crc32x(body.data(), body.size()));
    std::fwrite(crc.data(), 1, 4, f);
}

bool Vga::save_raw(const std::string& path) const {
    if (width_ <= 0 || height_ <= 0) return false;
    std::vector<unsigned char> px(static_cast<size_t>(width_) * height_);
    if (!snapshot(px.data())) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (unsigned char p : px) {
        const uint8_t* d = dac_[pal_[p & 15]];
        const unsigned char rgba[4] = {dac8(d[0]), dac8(d[1]), dac8(d[2]), 255};
        std::fwrite(rgba, 1, 4, f);
    }
    std::fclose(f);
    return true;
}

bool Vga::save_png(const std::string& path) const {
    if (width_ <= 0 || height_ <= 0) return false;
    std::vector<unsigned char> px(static_cast<size_t>(width_) * height_);
    if (!snapshot(px.data())) return false;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

    std::vector<unsigned char> ihdr;
    put32(ihdr, static_cast<uint32_t>(width_));
    put32(ihdr, static_cast<uint32_t>(height_));
    ihdr.push_back(8); ihdr.push_back(3);           // 8 bits, palette
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr.data(), static_cast<uint32_t>(ihdr.size()));

    unsigned char plte[16 * 3];
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) plte[i * 3 + c] = dac8(dac_[pal_[i]][c]);
    chunk(f, "PLTE", plte, sizeof plte);

    std::vector<unsigned char> raw;
    raw.reserve(px.size() + height_);
    for (int y = 0; y < height_; ++y) {
        raw.push_back(0);                            // no filter
        raw.insert(raw.end(), px.begin() + static_cast<size_t>(y) * width_,
                   px.begin() + static_cast<size_t>(y + 1) * width_);
    }
    std::vector<unsigned char> z;
    z.push_back(0x78); z.push_back(0x01);            // zlib, no compression
    for (size_t at = 0; at < raw.size(); at += 65535) {
        const uint16_t n = static_cast<uint16_t>(std::min<size_t>(65535, raw.size() - at));
        z.push_back(at + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<unsigned char>(n));
        z.push_back(static_cast<unsigned char>(n >> 8));
        z.push_back(static_cast<unsigned char>(~n));
        z.push_back(static_cast<unsigned char>(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z.data(), static_cast<uint32_t>(z.size()));
    chunk(f, "IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

}  // namespace dosemu
