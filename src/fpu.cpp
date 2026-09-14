// The x87 subset a real DOS compiler needs.
//
// WHY THIS EXISTS. The escapes D8-DF used to be consumed as no-ops, on the reasoning
// that LSI C does its floating point in software. That held right up until DJGPP's
// gcc: cc1 executes 35 x87 instructions during startup and aborts 556K instructions
// later. A no-op FPU is the worst kind of stub — the program does not fault, it just
// computes with whatever was already in the register and carries on, so the failure
// surfaces somewhere unrelated with no hint of the cause.
//
// WHAT IT IS. Eight doubles as the register stack. Real hardware keeps 80-bit extended
// values internally; a double has 53 bits of mantissa against 64. Nothing we run can
// see the difference, and DJGPP's own emu387 makes the same trade. Memory formats are
// exact though: m32/m64/m80 and the 16/32/64-bit integer forms all convert properly,
// because those *are* observable — a `long double` written to memory and read back has
// to keep its bit pattern.
//
// Unknown encodings stay no-ops rather than faulting, the same as before. That keeps
// the change from turning a working guest into a crashing one; DOSEMU_FPU_TRACE in
// cpu.cpp lists what a program actually executes if something looks wrong.
#include "cpu.h"
#include <cmath>
#include <cstring>

namespace dosemu {

namespace {

// The status word packs the compare result into three scattered bits, plus TOP.
constexpr uint16_t C0 = 1u << 8, C1 = 1u << 9, C2 = 1u << 10, C3 = 1u << 14;

float  bits_to_f32(uint32_t b) { float f;  std::memcpy(&f, &b, 4); return f; }
uint32_t f32_to_bits(float f)  { uint32_t b; std::memcpy(&b, &f, 4); return b; }
double bits_to_f64(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
uint64_t f64_to_bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

}  // namespace

void Cpu::fpu_exec(uint8_t op, int reg, int mod, int rm, uint32_t addr) {
    auto st   = [&](int i) -> double& { return fst[(ftop + i) & 7]; };
    auto push = [&](double v) { ftop = (ftop - 1) & 7; fst[ftop] = v; };
    auto pop  = [&]() { ftop = (ftop + 1) & 7; };

    // Integer conversion follows the control word's rounding-control field. Not a detail:
    // a C cast to int has to truncate, and the way a compiler gets that is to set RC=11
    // around the FIST, so a store that always rounds to nearest makes `(long)0.6` come
    // out as 1. It is also how printf("%f") splits a value — extract the integer part,
    // subtract, repeat — so ignoring RC did not produce slightly-off numbers, it printed
    // 0.841471 as 1.000000 and 2.928968 as 3.000000, which looks nothing like a rounding
    // bug and everything like broken arithmetic.
    //
    //   RC = 00 nearest (even)   01 down (-inf)   10 up (+inf)   11 truncate (toward 0)
    auto round_rc = [&](double v) -> double {
        switch ((fcw >> 10) & 3) {
            case 1:  return std::floor(v);
            case 2:  return std::ceil(v);
            case 3:  return std::trunc(v);
            default: return std::nearbyint(v);   // nearbyint is round-half-to-even
        }
    };

    auto rd32 = [&] { return mem_.r32(addr); };
    auto rd64 = [&] { return static_cast<uint64_t>(mem_.r32(addr))
                           | (static_cast<uint64_t>(mem_.r32(addr + 4)) << 32); };
    auto wr64 = [&](uint64_t v) { mem_.w32(addr, static_cast<uint32_t>(v));
                                  mem_.w32(addr + 4, static_cast<uint32_t>(v >> 32)); };

    // 80-bit extended: 64-bit mantissa with an explicit integer bit, then sign+exponent.
    auto rd80 = [&]() -> double {
        const uint64_t m = rd64();
        const uint16_t se = mem_.r16(addr + 8);
        const int e = se & 0x7FFF;
        const double sign = (se & 0x8000) ? -1.0 : 1.0;
        if (e == 0 && m == 0) return sign * 0.0;
        return sign * std::ldexp(static_cast<double>(m), e - 16383 - 63);
    };
    auto wr80 = [&](double v) {
        uint16_t se = 0; uint64_t m = 0;
        if (v != 0 && std::isfinite(v)) {
            if (v < 0) { se = 0x8000; v = -v; }
            int e = 0;
            const double frac = std::frexp(v, &e);        // v = frac * 2^e, frac in [0.5,1)
            m = static_cast<uint64_t>(std::ldexp(frac, 64));
            se |= static_cast<uint16_t>((e - 1 + 16383) & 0x7FFF);
        } else if (v < 0) {
            se = 0x8000;
        }
        wr64(m);
        mem_.w16(addr + 8, se);
    };

    // FCOM/FUCOM report through C3/C2/C0 — the same three bits SAHF later tests.
    auto compare = [&](double a, double b) {
        fsw &= ~(C0 | C2 | C3);
        if (std::isnan(a) || std::isnan(b)) fsw |= C0 | C2 | C3;   // unordered
        else if (a > b)                     { /* all clear */ }
        else if (a < b)                     fsw |= C0;
        else                                fsw |= C3;
    };
    // TOP lives in bits 11-13 of the status word and has to be current when it is read.
    auto status = [&]() -> uint16_t {
        return static_cast<uint16_t>((fsw & ~0x3800u) | ((ftop & 7) << 11));
    };
    // The arithmetic group, shared by D8/DC (float/double memory) and the register forms.
    // `rev` is the R in FSUBR/FDIVR: same operands, subtraction the other way round.
    auto arith = [&](int which, double& dst, double src, bool rev) {
        switch (which) {
            case 0: dst = dst + src; break;                       // FADD
            case 1: dst = dst * src; break;                       // FMUL
            case 4: dst = rev ? src - dst : dst - src; break;     // FSUB / FSUBR
            case 5: dst = rev ? dst - src : src - dst; break;
            case 6: dst = rev ? src / dst : dst / src; break;     // FDIV / FDIVR
            case 7: dst = rev ? dst / src : src / dst; break;
        }
    };

    if (mod != 3) {
        switch (op) {
            case 0xD8: {                                          // arithmetic with m32
                const double v = bits_to_f32(rd32());
                if (reg == 2 || reg == 3) { compare(st(0), v); if (reg == 3) pop(); }
                else arith(reg, st(0), v, false);
                break;
            }
            case 0xDC: {                                          // arithmetic with m64
                const double v = bits_to_f64(rd64());
                if (reg == 2 || reg == 3) { compare(st(0), v); if (reg == 3) pop(); }
                else arith(reg, st(0), v, false);
                break;
            }
            case 0xD9:
                switch (reg) {
                    case 0: push(bits_to_f32(rd32())); break;                  // FLD m32
                    case 2: mem_.w32(addr, f32_to_bits(static_cast<float>(st(0)))); break;      // FST m32
                    case 3: mem_.w32(addr, f32_to_bits(static_cast<float>(st(0)))); pop(); break;// FSTP m32
                    case 5: fcw = mem_.r16(addr); break;                       // FLDCW
                    case 7: mem_.w16(addr, fcw); break;                        // FNSTCW
                }
                break;
            case 0xDD:
                switch (reg) {
                    case 0: push(bits_to_f64(rd64())); break;                  // FLD m64
                    case 2: wr64(f64_to_bits(st(0))); break;                   // FST m64
                    case 3: wr64(f64_to_bits(st(0))); pop(); break;            // FSTP m64
                    case 7: mem_.w16(addr, status()); break;                   // FNSTSW m16
                }
                break;
            case 0xDB:
                switch (reg) {
                    case 0: push(static_cast<double>(static_cast<int32_t>(rd32()))); break;   // FILD m32
                    case 2: mem_.w32(addr, static_cast<uint32_t>(static_cast<int32_t>(round_rc(st(0))))); break;
                    case 3: mem_.w32(addr, static_cast<uint32_t>(static_cast<int32_t>(round_rc(st(0))))); pop(); break;
                    case 5: push(rd80()); break;                               // FLD m80
                    case 7: wr80(st(0)); pop(); break;                         // FSTP m80
                }
                break;
            case 0xDF:
                switch (reg) {
                    case 0: push(static_cast<double>(static_cast<int16_t>(mem_.r16(addr)))); break;   // FILD m16
                    case 2: mem_.w16(addr, static_cast<uint16_t>(static_cast<int16_t>(round_rc(st(0))))); break;
                    case 3: mem_.w16(addr, static_cast<uint16_t>(static_cast<int16_t>(round_rc(st(0))))); pop(); break;
                    case 5: push(static_cast<double>(static_cast<int64_t>(rd64()))); break;           // FILD m64
                    case 7: wr64(static_cast<uint64_t>(static_cast<int64_t>(round_rc(st(0))))); pop(); break;
                }
                break;
            case 0xDA: {                                          // arithmetic with i32
                const double v = static_cast<double>(static_cast<int32_t>(rd32()));
                if (reg == 2 || reg == 3) { compare(st(0), v); if (reg == 3) pop(); }
                else arith(reg, st(0), v, false);
                break;
            }
            case 0xDE: {                                          // arithmetic with i16
                const double v = static_cast<double>(static_cast<int16_t>(mem_.r16(addr)));
                if (reg == 2 || reg == 3) { compare(st(0), v); if (reg == 3) pop(); }
                else arith(reg, st(0), v, false);
                break;
            }
        }
        return;
    }

    // ---- mod == 3: register forms -------------------------------------------
    switch (op) {
        case 0xD8:                                                // ST(0) op= ST(i)
            if (reg == 2 || reg == 3) { compare(st(0), st(rm)); if (reg == 3) pop(); }
            else arith(reg, st(0), st(rm), false);
            break;
        // FUCOMPP (DA E9): compare ST(0) with ST(1) and pop both. The unordered form of
        // FCOMPP, and the one a C library reaches for because it must not fault on a NaN.
        // Leaving it out left the compare flags holding the *previous* comparison, so
        // printf's digit loop took the wrong branch and emitted a fraction of all zeros:
        // 0.666667 printed as 0.000000 while (long)(q*1e6) came out at exactly 666666.
        // Arithmetic that is provably right and formatting that is wrong is a strange
        // place to end up, and it is what a missing compare looks like.
        case 0xDA:
            if (reg == 5 && rm == 1) { compare(st(0), st(1)); pop(); pop(); }
            break;
        case 0xDC:                                                // ST(i) op= ST(0), reversed
            if (reg == 2 || reg == 3) { compare(st(rm), st(0)); }
            else arith(reg, st(rm), st(0), true);
            break;
        case 0xDE:                                                // ...and pop
            if (reg == 3) { if (rm == 1) { compare(st(0), st(1)); pop(); pop(); } }  // FCOMPP
            else { arith(reg, st(rm), st(0), true); pop(); }
            break;
        case 0xD9:
            switch (reg) {
                case 0: { const double v = st(rm); push(v); break; }            // FLD ST(i)
                case 1: std::swap(st(0), st(rm)); break;                        // FXCH
                case 2: break;                                                  // FNOP
                case 4:
                    if (rm == 0) st(0) = -st(0);                                // FCHS
                    else if (rm == 1) st(0) = std::fabs(st(0));                 // FABS
                    else if (rm == 4) compare(st(0), 0.0);                      // FTST
                    break;
                case 5:
                    switch (rm) {                                               // constants
                        case 0: push(1.0); break;                               // FLD1
                        case 1: push(3.321928094887362348); break;              // FLDL2T
                        case 2: push(1.442695040888963407); break;              // FLDL2E
                        case 3: push(3.141592653589793238); break;              // FLDPI
                        case 4: push(0.301029995663981195); break;              // FLDLG2
                        case 5: push(0.693147180559945309); break;              // FLDLN2
                        case 6: push(0.0); break;                               // FLDZ
                    }
                    break;
                case 7:
                    switch (rm) {
                        case 0: st(0) = std::fmod(st(0), st(1)); break;         // FPREM
                        case 2: st(0) = std::sqrt(st(0)); break;                // FSQRT
                        case 4: st(0) = round_rc(st(0)); break;           // FRNDINT
                        case 5: st(0) = std::ldexp(st(0), static_cast<int>(st(1))); break;  // FSCALE
                        case 6: st(0) = std::sin(st(0)); break;                 // FSIN
                        case 7: st(0) = std::cos(st(0)); break;                 // FCOS
                    }
                    break;
            }
            break;
        case 0xDB:
            if (reg == 4) {                                       // FNCLEX / FNINIT
                if (rm == 2) fsw = 0;
                else if (rm == 3) { fcw = 0x037F; fsw = 0; ftop = 0; for (double& v : fst) v = 0; }
            }
            break;
        case 0xDD:
            switch (reg) {
                case 0: break;                                                  // FFREE
                case 2: st(rm) = st(0); break;                                  // FST ST(i)
                case 3: st(rm) = st(0); pop(); break;                           // FSTP ST(i)
                case 4: compare(st(0), st(rm)); break;                          // FUCOM
                case 5: compare(st(0), st(rm)); pop(); break;                   // FUCOMP
            }
            break;
        case 0xDF:
            if (reg == 4 && rm == 0) r[AX] = status();            // FNSTSW AX
            break;
    }
}

}  // namespace dosemu
