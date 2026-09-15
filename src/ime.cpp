#include "ime.h"

#include "dos.h"
#include "vga.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace dosemu {
namespace {

// Wapuro romaji. Longest match wins, so the table is sorted by length and the
// lookup walks it from the front.
struct Romaji { const char* r; uint16_t kana[2]; };
const Romaji kRomaji[] = {
#include "romaji.inc"
};

const uint16_t kSmallTu = 0x82C1;    // っ, what a doubled consonant becomes
const uint16_t kN       = 0x82F1;    // ん

bool vowel(char c) { return c=='a'||c=='i'||c=='u'||c=='e'||c=='o'; }

// The bar the window is drawn in: the bottom row of the screen, which is where
// a DOS/V FEP puts its own, and 16 pixels tall because that is the text line a
// DOS/V display driver draws.
const int kCell = 16;

}  // namespace

// EUC-JP to Shift-JIS, which is arithmetic and not a table: both are the same
// JIS X 0208 row and cell, packed differently. EUC puts row and cell in one
// byte each with the high bit set; Shift-JIS pairs the rows up, which is where
// the halving and the two ranges come from.
static bool euc_to_sjis(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) { out.push_back(in[i]); ++i; continue; }
        if (c == 0x8E || c == 0x8F || i + 1 >= in.size()) return false;   // half-width kana, JIS X 0212
        const int row = c - 0xA0, cell = static_cast<unsigned char>(in[i + 1]) - 0xA0;
        if (row < 1 || row > 94 || cell < 1 || cell > 94) return false;
        const int c1 = (row - 1) / 2 + (row < 63 ? 0x81 : 0xC1);
        const int c2 = (row & 1) ? cell + 0x3F + (cell >= 64 ? 1 : 0) : cell + 0x9E;
        out.push_back(static_cast<char>(c1));
        out.push_back(static_cast<char>(c2));
        i += 2;
    }
    return true;
}

// An SKK dictionary: one entry per line, `reading /candidate/candidate/`, with
// the okuri-ari entries first and the plain ones after a marker line. Only the
// plain ones are wanted here -- an okuri-ari reading ends in the first letter
// of its okurigana, which is a romaji letter, and this FEP has no okurigana
// handling to feed it to.
//
// The file is the one the SKK Development Team publishes, EUC-JP and GPL, kept
// in fep/ exactly as it comes.
void Ime::load_dict() {
    dict_tried_ = true;
    if (dict_path_.empty()) return;
    std::FILE* f = std::fopen(dict_path_.c_str(), "rb");
    if (!f) return;
    std::string line, sj, cand;
    bool plain = false;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == ';') {
            if (line.find("okuri-nasi") != std::string::npos) plain = true;
            continue;
        }
        if (!plain) continue;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) continue;
        const std::string reading = line.substr(0, sp);
        if (!euc_to_sjis(reading, sj)) continue;
        // Readings that are not all kana (abbreviations, ascii) are not
        // reachable from this keyboard.
        bool kana = !sj.empty();
        for (size_t i = 0; i < sj.size(); i += 2) {
            const unsigned char a = static_cast<unsigned char>(sj[i]);
            if (a != 0x82 || i + 1 >= sj.size()) { kana = false; break; }
        }
        if (!kana) continue;

        std::vector<std::string>& v = dict_[sj];
        size_t i = sp + 1;
        while (i < line.size()) {
            if (line[i] != '/') { ++i; continue; }
            const size_t end = line.find('/', i + 1);
            if (end == std::string::npos) break;
            std::string c = line.substr(i + 1, end - i - 1);
            const size_t semi = c.find(';');                  // an annotation
            if (semi != std::string::npos) c.erase(semi);
            i = end;
            if (c.empty() || c[0] == '[') continue;           // an okurigana block
            if (!euc_to_sjis(c, cand)) continue;
            v.push_back(cand);
        }
        if (v.empty()) dict_.erase(sj);
    }
    std::fclose(f);
}

// Cut the undecided kana into pieces, longest first, and look each one up.
//
// This is the simple thing and it is honest about being simple: take the
// longest prefix the dictionary knows, make it a piece, and start again on what
// is left. A real FEP weighs the whole sentence; this one does not, and where
// it guesses wrong the space bar walks the candidates of the piece it got.
void Ime::convert() {
    if (!dict_tried_) load_dict();
    segs_.clear();
    cur_ = 0;
    size_t at = 0;
    while (at < kana_.size()) {
        size_t take = 0;
        std::vector<std::string> found;
        for (size_t n = kana_.size() - at; n >= 2; n -= 2) {
            auto it = dict_.find(kana_.substr(at, n));
            if (it != dict_.end()) { take = n; found = it->second; break; }
        }
        Seg seg;
        if (take) { seg.kana = kana_.substr(at, take); seg.cand = found; at += take; }
        else      { seg.kana = kana_.substr(at, 2); at += 2; }     // one kana, as itself
        segs_.push_back(seg);
    }
    // A piece with candidates is where the space bar should start.
    for (size_t i = 0; i < segs_.size(); ++i)
        if (!segs_[i].cand.empty()) { cur_ = i; break; }
}

void Ime::set_on(bool v) {
    if (on_ == v) return;
    on_ = v;
    if (!on_) { confirm_kana(); erase(); }
    else draw();
}

void Ime::put_kana(uint16_t code) {
    if (!code) return;
    kana_.push_back(static_cast<char>(code >> 8));
    kana_.push_back(static_cast<char>(code & 0xFF));
}

// Turn as much of the romaji buffer into kana as it will give up.
//
// Three rules beyond the table: a consonant typed twice is っ and the second
// one stays; an `n` that is not going to become `na`..`no` or `nn` is ん; and
// anything that can never begin an entry is passed through as itself.
void Ime::take_romaji() {
    for (;;) {
        if (raw_.empty()) return;

        // A doubled consonant, before the table sees it.
        if (raw_.size() >= 2 && raw_[0] == raw_[1] && !vowel(raw_[0]) && raw_[0] != 'n') {
            put_kana(kSmallTu);
            raw_.erase(0, 1);
            continue;
        }
        // `n` alone in front of a consonant is ん.
        if (raw_.size() >= 2 && raw_[0] == 'n' && !vowel(raw_[1]) && raw_[1] != 'n'
            && raw_[1] != 'y') {
            put_kana(kN);
            raw_.erase(0, 1);
            continue;
        }

        const Romaji* hit = nullptr;
        bool prefix = false;                  // something longer could still match
        for (const Romaji& e : kRomaji) {
            const size_t n = std::strlen(e.r);
            if (n <= raw_.size()) {
                if (!hit && raw_.compare(0, n, e.r) == 0) hit = &e;
            } else if (std::strncmp(e.r, raw_.c_str(), raw_.size()) == 0) {
                prefix = true;
            }
        }
        if (hit) {
            put_kana(hit->kana[0]);
            put_kana(hit->kana[1]);
            raw_.erase(0, std::strlen(hit->r));
            continue;
        }
        if (prefix) return;                   // keep waiting for more letters
        // Nothing can come of the first letter: it is its own character.
        kana_.push_back(raw_[0]);
        raw_.erase(0, 1);
    }
}

void Ime::confirm_kana() {
    if (!segs_.empty()) {                     // converted: the chosen candidates
        for (const Seg& g : segs_)
            for (unsigned char c : g.text()) out_.push_back(c);
        segs_.clear();
        kana_.clear();
        raw_.clear();
        return;
    }
    if (!raw_.empty()) {                      // a trailing `n` is ん
        if (raw_ == "n") { put_kana(kN); raw_.clear(); }
        else take_romaji();
    }
    for (unsigned char c : kana_) out_.push_back(c);
    for (unsigned char c : raw_) out_.push_back(c);
    kana_.clear();
    raw_.clear();
}

bool Ime::feed(int byte) {
    if (!on_ || byte < 0) return false;
    const int c = byte & 0xFF;

    if (c == 0x0D) {                          // Enter confirms what is there
        if (!busy()) return false;            // ...and is the guest's otherwise
        confirm_kana();
        erase();
        return true;
    }
    if (c == 0x1B) {                          // Escape: back to kana, then away
        if (!busy()) return false;
        if (!segs_.empty()) { segs_.clear(); draw(); return true; }
        kana_.clear(); raw_.clear();
        erase();
        return true;
    }
    if (c == 0x08) {                          // Backspace: a letter, else a kana
        if (!busy()) return false;
        if (!segs_.empty()) { segs_.clear(); draw(); return true; }   // undo the conversion
        if (!raw_.empty()) raw_.erase(raw_.size() - 1);
        else if (kana_.size() >= 2 &&
                 static_cast<unsigned char>(kana_[kana_.size() - 2]) >= 0x81)
            kana_.erase(kana_.size() - 2);
        else kana_.erase(kana_.size() - 1);
        draw();
        return true;
    }
    if (c == ' ') {                           // the conversion key
        if (!busy()) return false;            // ...and a plain space when idle
        if (!raw_.empty()) {                  // a trailing `n` first
            if (raw_ == "n") { put_kana(kN); raw_.clear(); } else take_romaji();
        }
        if (segs_.empty()) convert();
        else if (!segs_[cur_].cand.empty())   // again: the next candidate
            segs_[cur_].pick = (segs_[cur_].pick + 1) % segs_[cur_].cand.size();
        draw();
        return true;
    }
    if (c == 0x09) {                          // Tab walks the pieces
        if (segs_.empty()) return false;
        cur_ = (cur_ + 1) % segs_.size();
        draw();
        return true;
    }
    if (c < 0x20) return false;               // control keys are the guest's

    if (!segs_.empty()) confirm_kana();       // typing on: settle what is converted
    raw_.push_back(static_cast<char>(c));
    take_romaji();
    draw();
    return true;
}

int Ime::pop_out() {
    if (out_.empty()) return -1;
    const int c = out_.front();
    out_.pop_front();
    return c;
}

// One glyph, from the same FONTX2 files the guest draws with.
void Ime::cell(int x, int y, uint16_t code, uint8_t fg, uint8_t bg) {
    if (!vga_) return;
    const bool dbcs = code >= 0x100;
    const std::vector<uint8_t>* f = dbcs ? kanji_ : ank_;
    int w = dbcs ? 16 : 8, h = kCell;
    const uint8_t* g = f ? fontx_glyph(*f, code, w, h) : nullptr;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) {
            const bool ink = g && (g[row * stride + (col >> 3)] & (0x80 >> (col & 7)));
            vga_->put_pixel(x + col, y + row, ink ? fg : bg);
        }
}

// A Shift-JIS string, one cell at a time. Returns where it ended.
int Ime::text(int x, int y, const std::string& s, uint8_t fg, uint8_t bg) {
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        if (lead && i + 1 < s.size()) {
            cell(x, y, static_cast<uint16_t>((c << 8) | static_cast<unsigned char>(s[i + 1])),
                 fg, bg);
            x += 16;
            i += 2;
        } else {
            cell(x, y, c, fg, bg);
            x += 8;
            i += 1;
        }
    }
    return x;
}

void Ime::draw() {
    if (!vga_ || !vga_->graphics()) return;
    const int w = vga_->width(), y0 = vga_->height() - kCell;
    if (!shown_) {                            // remember what the window covers
        saved_.resize(static_cast<size_t>(w) * kCell);
        for (int y = 0; y < kCell; ++y)
            for (int x = 0; x < w; ++x)
                saved_[static_cast<size_t>(y) * w + x] =
                    vga_->get_pixel(x, y0 + y);
        shown_ = true;
    }
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < w; ++x) vga_->put_pixel(x, y0 + y, 0);

    // `[かな]` and then the undecided string, underlined the way a DOS/V FEP
    // underlines what has not been settled yet.
    std::string tag;
    tag.push_back('\x81'); tag.push_back('\x79');          // 「
    tag.push_back('\x82'); tag.push_back('\xa9');          // か
    tag.push_back('\x82'); tag.push_back('\xc8');          // な
    tag.push_back('\x81'); tag.push_back('\x7a');          // 」
    int x = text(0, y0, tag, 14, 0);
    x += 8;
    const int from = x;
    if (segs_.empty()) {
        x = text(x, y0, kana_, 15, 0);
    } else {
        // The piece the space bar is on is the one drawn in reverse, which is
        // how a DOS/V FEP shows which 文節 it is working on.
        for (size_t i = 0; i < segs_.size(); ++i) {
            const bool now = i == cur_;
            x = text(x, y0, segs_[i].text(), now ? 0 : 15, now ? 15 : 0);
        }
    }
    x = text(x, y0, raw_, 15, 0);
    for (int i = from; i < x; ++i) vga_->put_pixel(i, y0 + kCell - 1, 15);
}

void Ime::erase() {
    if (!shown_ || !vga_) return;
    const int w = vga_->width(), y0 = vga_->height() - kCell;
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < w; ++x)
            vga_->put_pixel(x, y0 + y, saved_[static_cast<size_t>(y) * w + x]);
    shown_ = false;
}

}  // namespace dosemu
