// 8086/80386 real-mode interpreter core. Covers the integer subset a DOS program
// and a C compiler emit — MOV, the ALU group, INC/DEC, stack, Jcc/JMP/CALL/RET/LOOP,
// string ops with REP, shifts/rotates, MUL/DIV, INT, LEA/LES/LDS, flag ops — plus
// the 80386 additions a DOS-extender stub uses before it enters protected mode:
// 32-bit operands/addresses via the 0x66/0x67 prefixes, FS/GS, the 0x0F two-byte
// opcodes (long Jcc, SETcc, MOVZX/MOVSX, IMUL, BT group, SHLD/SHRD, BSF/BSR), and
// the system instructions (MOV CRn, LGDT/LIDT, LMSW) that arm the mode switch.
//
// The 32-bit halves live in rhi[]; when the operand size is 16 (the real-mode
// default) every path here behaves exactly as the original 8086 core did. An
// unhandled opcode stops with its byte and address, the way a new guest is brought
// up. Not modelled yet: protected mode itself, and the x87 FPU (escapes are no-ops).
#include "cpu.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace dosemu {

bool Cpu::fpu_trace = getenv("DOSEMU_FPU_TRACE") != nullptr;
uint64_t Cpu::sample_every = [] { const char* s = getenv("DOSEMU_SAMPLE");
    return s ? strtoull(s, nullptr, 10) : 0ull; }();

uint64_t Cpu::trace_lo = [] { const char* s = getenv("DOSEMU_TRACE");
    return s ? strtoull(s, nullptr, 10) : 0ull; }();
uint64_t Cpu::trace_hi = [] { const char* s = getenv("DOSEMU_TRACE");
    const char* d = s ? strchr(s, '-') : nullptr;
    return d ? strtoull(d + 1, nullptr, 10) : 0ull; }();

uint32_t Cpu::watch_lo = [] { const char* s = getenv("DOSEMU_WATCH");
    return s ? (uint32_t)strtoul(s, nullptr, 16) : 0u; }();
uint32_t Cpu::watch_hi = [] { const char* s = getenv("DOSEMU_WATCH");
    const char* d = s ? strchr(s, '-') : nullptr;
    return d ? (uint32_t)strtoul(d + 1, nullptr, 16) : 0u; }();

void Cpu::trace_insn(uint8_t op) const {
    // The instruction bytes as the guest sees them, so a trace can be read without
    // guessing where the image was loaded. Disassembling a file at `header + EIP` is a
    // reliable way to produce plausible nonsense; this cannot be wrong about what ran.
    char by[32] = {0};
    for (int i = 0; i < 8; ++i)
        std::snprintf(by + i * 3, 4, "%02X ", mem_.r8(lin(CS, ip - 1 + i)));
    // The segment bases too: in protected mode an EIP means nothing without them, and
    // guessing a base is how a trace turns into a wrong answer that looks right.
    printf("[t]%llu %04X:%08X %s| csb=%08X dsb=%08X eax=%08X ecx=%08X ebx=%08X edx=%08X esp=%08X\n",
           (unsigned long long)insns, sreg[CS], ip - 1, by, sbase[CS], sbase[DS],
           gd(AX), gd(CX), gd(BX), gd(DX), gd(SP));
    (void)op;
}

void Cpu::watch_hit(uint32_t a, int s, uint32_t off) const {
    printf("[watch]%llu %08X  seg%d=%04X base=%08X off=%08X  at %04X:%08X\n",
           (unsigned long long)insns, a, s, sreg[s], sbase[s], off, sreg[CS], ip);
}

// A write reported by Memory rather than by lin(): the DOS layer, the DPMI host, or the
// loader. Distinguished in the output because "who wrote this" and "which segment did the
// guest use" are different questions and only one of them has an answer here.
void Cpu::mem_write_hit(uint32_t a) const {
    printf("[write]%llu %08X  by the host, at %04X:%08X\n",
           (unsigned long long)insns, a, sreg[CS], ip);
}

void Cpu::bad_sel(int i, uint16_t sel) const {
    static int n = 0;
    if (++n > 8) return;
    static const char* nm[] = {"ES","CS","SS","DS","FS","GS"};
    // Which table it came out of, and where that table is: a selector that is "not
    // present" because the descriptor is genuinely marked absent and one that reads as
    // absent because we looked in the wrong table are completely different bugs.
    printf("[gp] loaded %s with %s selector %04X (%s, gdt=%08X ldt=%08X) at %04X:%08X"
           " (a real CPU faults here)\n",
           nm[i], sel ? "a not-present" : "the null", sel, (sel & 4) ? "LDT" : "GDT",
           gdt_base, ldt_base, sreg[CS], ip);
}

// Port I/O. Only the A20 gate is modelled; everything else reads as zero and ignores
// writes, as before. Both historical ways of opening the gate are here because a
// program picks one and gives up if it does not take:
//
//   port 0x92 bit 1        the "fast A20" on anything after the PS/2
//   the keyboard controller  OUT 64h,D1h then OUT 60h,<output port>, bit 1 again --
//                          A20 hung off a spare pin of the 8042 because that was the
//                          chip with a line to spare, which is the whole reason this
//                          is a keyboard controller's job.
uint8_t Cpu::io_in(uint16_t port) {
    switch (port) {
        case 0x92: return a20 ? 0x02 : 0x00;
        case 0x64: return 0x00;      // 8042 status: both buffers empty, so polls exit
        case 0x60: return 0x00;
        default:   return 0x00;
    }
}

void Cpu::io_out(uint16_t port, uint8_t v) {
    static uint8_t kbd_cmd = 0;
    switch (port) {
        case 0x92: a20 = (v & 0x02) != 0; break;
        case 0x64:
            kbd_cmd = v;
            if (v == 0xDD) a20 = false;          // some BIOSes take the short way
            else if (v == 0xDF) a20 = true;
            break;
        case 0x60:
            if (kbd_cmd == 0xD1) { a20 = (v & 0x02) != 0; kbd_cmd = 0; }
            break;
        default: break;
    }
}

void Cpu::fail(const std::string& msg, uint8_t op) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (opcode 0x%02X) at %04X:%04X", msg.c_str(), op,
                  sreg[CS], static_cast<uint16_t>(ip - 1));
    throw CpuError{buf, sreg[CS], static_cast<uint16_t>(ip - 1)};
}

// ---- flag helpers ---------------------------------------------------------
static bool parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

namespace {
struct Modrm {
    uint8_t mod, reg, rm;
    bool is_reg;
    int seg_idx = DS;    // segment register index (ES..GS); only meaningful for memory
    uint32_t off;        // effective offset (16- or 32-bit)
};
}

// Decode context lives in the step() frame; pass what the helpers need.
struct Decode {
    int seg_ovr_idx = DS;        // segment override register index
    bool have_ovr = false;
    bool o32 = false;            // 0x66 seen: toggles operand size vs the CS default
    bool a32 = false;            // 0x67 seen: toggles address size vs the CS default
};

// The interpreter is one big method to keep the hot state in locals.
void Cpu::run() { while (!halted) step(); }

void Cpu::interrupt(uint8_t n) {
    if (n == 0x21 && getenv("DOSEMU_INTWATCH"))
        printf("[int]%llu INT 21 ax=%04X ds=%04X(%08X) pe=%d at %04X:%08X\n",
               (unsigned long long)insns, r[AX], sreg[DS], sbase[DS], pe() ? 1 : 0,
               sreg[CS], ip);
    // A protected-mode guest that loaded its own IDT means it: the handler is not
    // decoration. Watcom's X-32 extender runs under its own GDT/IDT and reflects DOS
    // calls itself, and the code around them assumes its own handler's register
    // discipline — `mov ebx,esp; int 21h; mov esp,ebx` only makes sense if what runs in
    // between is yours. Servicing that as DOS returned the vector's offset in BX, which
    // `mov esp,ebx` then made the stack pointer: a 32-bit program running on a stack at
    // 0x24, which is how its environment scan ended up reading two NULs and quitting.
    //
    // A vector the guest left un-hooked still falls through to DOS below, so DPMI
    // clients (which do not own the IDT — the host reflects for them) are untouched.
    if (pe() && idt_limit >= static_cast<uint32_t>(n) * 8 + 7) {
        const uint32_t g = idt_base + n * 8;
        const uint8_t attr = mem_.r8(g + 5);
        const uint8_t type = attr & 0x1F;
        const bool gate32 = (type == 0x0E || type == 0x0F);   // 386 interrupt/trap gate
        const bool gate16 = (type == 0x06 || type == 0x07);   // 286 interrupt/trap gate
        if ((attr & 0x80) && (gate32 || gate16)) {
            const uint16_t sel = mem_.r16(g + 2);
            const uint32_t off = gate32
                ? (mem_.r16(g) | (static_cast<uint32_t>(mem_.r16(g + 6)) << 16))
                : mem_.r16(g);
            // A gate into a more privileged segment switches to that ring's stack out
            // of the TSS and stacks the old SS:ESP below the usual three words. X-32
            // runs the 32-bit application at CPL 3 and its own handler at CPL 0, and
            // relies on both halves of that: the handler's frame has to live in *its*
            // data segment (it reloads SS from a fixed selector and expects to find the
            // frame still there), and the five-word frame is what its closing far return
            // consumes after shuffling it into EIP/CS/ESP/SS.
            const uint32_t cpl = sreg[CS] & 3, target = sel & 3;
            uint16_t old_ss = sreg[SS]; uint32_t old_sp = sp_get();
            if (target < cpl) {
                const uint32_t tss = read_desc(tr).base;
                const uint32_t nsp = mem_.r32(tss + 4 + target * 8);
                const uint16_t nss = mem_.r16(tss + 8 + target * 8);
                set_seg(SS, nss);
                sp_set(nsp);
                if (gate32) { push32(old_ss); push32(old_sp); }
                else        { push16(old_ss); push16(static_cast<uint16_t>(old_sp)); }
            }
            if (gate32) { push32(flags); push32(sreg[CS]); push32(ip); }
            else        { push16(flags); push16(sreg[CS]); push16(static_cast<uint16_t>(ip)); }
            if (type == 0x0E || type == 0x06) set_flag(IF, false);   // interrupt gate, not trap
            set_flag(TF, false);
            set_seg(CS, sel);
            jump(off);
            return;
        }
    }
    if (on_int && on_int(n)) return;   // serviced by DOS/BIOS layer
    // Fall back to the real interrupt vector table (rarely used by our guests).
    push16(flags);
    push16(sreg[CS]);
    push16(static_cast<uint16_t>(ip));
    set_flag(IF, false);
    set_flag(TF, false);
    uint16_t no = mem_.r16(n * 4);
    set_seg(CS, mem_.r16(n * 4 + 2));
    jump(no);
}

void Cpu::step() {
    ++insns;
    if (max_insns && insns > max_insns)
        throw CpuError{"instruction limit exceeded (runaway program?)", sreg[CS], static_cast<uint16_t>(ip)};
    // The DPMI host's own entry points: reaching one runs the host, not an instruction.
    if (hook_hi) {
        const uint32_t a = lin(CS, ip);
        if (a >= hook_lo && a < hook_hi && on_hook(a - hook_lo)) return;
    }
    if (sample_every && !sample_left--) {
        sample_left = sample_every;
        printf("[sample] %04X:%08X after %llu\n", sreg[CS], ip, (unsigned long long)insns);
    }
    Decode d;
    uint8_t op;

    // ---- prefixes ----
    uint8_t rep = 0;   // 0, 0xF2 (REPNE), 0xF3 (REP/REPE)
    for (;;) {
        op = fetch8();
        switch (op) {
            case 0x26: d.seg_ovr_idx = ES; d.have_ovr = true; continue;
            case 0x2E: d.seg_ovr_idx = CS; d.have_ovr = true; continue;
            case 0x36: d.seg_ovr_idx = SS; d.have_ovr = true; continue;
            case 0x3E: d.seg_ovr_idx = DS; d.have_ovr = true; continue;
            case 0x64: d.seg_ovr_idx = FS; d.have_ovr = true; continue;
            case 0x65: d.seg_ovr_idx = GS; d.have_ovr = true; continue;
            case 0x66: d.o32 = true; continue;   // operand-size prefix
            case 0x67: d.a32 = true; continue;   // address-size prefix
            case 0xF0: continue;                 // LOCK: no-op here
            case 0xF2: rep = 0xF2; continue;
            case 0xF3: rep = 0xF3; continue;
        }
        break;
    }
    // Effective sizes: the prefix toggles the code segment's default (16 in real mode
    // and 16-bit code segments, 32 in a 32-bit code segment).
    bool o32 = d.o32 ^ cs_d;
    bool a32 = d.a32 ^ cs_d;
    ++ophist[op];
    if (trace_hi && insns >= trace_lo && insns <= trace_hi) trace_insn(op);

    // ---- ModRM decode (16- or 32-bit addressing) ----
    auto decode_modrm = [&](Modrm& m) {
        uint8_t b = fetch8();
        m.mod = b >> 6; m.reg = (b >> 3) & 7; m.rm = b & 7;
        m.is_reg = (m.mod == 3);
        if (m.is_reg) return;
        if (a32) {
            // 32-bit addressing: 32-bit base/index regs, optional SIB, disp8/disp32.
            uint32_t base = 0; int seg = DS;
            if (m.rm == 4) {                       // SIB byte
                uint8_t sib = fetch8();
                int scale = sib >> 6, index = (sib >> 3) & 7, bs = sib & 7;
                if (index != 4) base += gd(index) << scale;
                if (bs == 5 && m.mod == 0) { base += fetch32(); }
                else { base += gd(bs); if (bs == 4 || bs == 5) seg = SS; }
            } else if (m.rm == 5 && m.mod == 0) {  // [disp32]
                base = fetch32();
            } else {
                base = gd(m.rm);
                if (m.rm == 5) seg = SS;           // EBP-relative → SS
            }
            if (m.mod == 1) base += static_cast<int32_t>(static_cast<int8_t>(fetch8()));
            else if (m.mod == 2) base += static_cast<int32_t>(fetch32());
            m.seg_idx = d.have_ovr ? d.seg_ovr_idx : seg;
            m.off = base;
        } else {
            uint16_t base = 0; int seg = DS;
            switch (m.rm) {
                case 0: base = r[BX] + r[SI]; break;
                case 1: base = r[BX] + r[DI]; break;
                case 2: base = r[BP] + r[SI]; seg = SS; break;
                case 3: base = r[BP] + r[DI]; seg = SS; break;
                case 4: base = r[SI]; break;
                case 5: base = r[DI]; break;
                case 6: if (m.mod == 0) { base = fetch16(); } else { base = r[BP]; seg = SS; } break;
                case 7: base = r[BX]; break;
            }
            if (m.mod == 1) base += static_cast<int16_t>(static_cast<int8_t>(fetch8()));
            else if (m.mod == 2) base += fetch16();
            m.seg_idx = d.have_ovr ? d.seg_ovr_idx : seg;
            m.off = base;
        }
    };

    auto pa = [&](const Modrm& m) -> uint32_t { return lin(m.seg_idx, m.off); };

    // byte operand read/write via a decoded ModRM
    auto r8m  = [&](const Modrm& m) -> uint8_t  { return m.is_reg ? gb(m.rm) : mem_.r8(pa(m)); };
    auto w8m  = [&](const Modrm& m, uint8_t v)  { if (m.is_reg) sb(m.rm, v); else mem_.w8(pa(m), v); };
    // width (16/32) register + operand read/write, selected by the operand-size prefix
    auto grw  = [&](int i) -> uint32_t { return o32 ? gd(i) : r[i]; };
    auto srw  = [&](int i, uint32_t v) { if (o32) sd(i, v); else r[i] = static_cast<uint16_t>(v); };
    auto rvm  = [&](const Modrm& m) -> uint32_t { if (m.is_reg) return grw(m.rm); return o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m)); };
    auto wvm  = [&](const Modrm& m, uint32_t v) { if (m.is_reg) srw(m.rm, v); else { if (o32) mem_.w32(pa(m), v); else mem_.w16(pa(m), static_cast<uint16_t>(v)); } };
    auto fetchImmV = [&]() -> uint32_t { return o32 ? fetch32() : fetch16(); };
    auto pushV = [&](uint32_t v) { if (o32) push32(v); else push16(static_cast<uint16_t>(v)); };
    auto popV  = [&]() -> uint32_t { return o32 ? pop32() : pop16(); };
    uint32_t vmask = o32 ? 0xFFFFFFFFu : 0xFFFFu;
    uint32_t vmsb  = o32 ? 0x80000000u : 0x8000u;
    int vbits = o32 ? 32 : 16;

    // ---- ALU with flags (op: 0=ADD 1=OR 2=ADC 3=SBB 4=AND 5=SUB 6=XOR 7=CMP) ----
    auto alu8 = [&](int aluop, uint8_t a, uint8_t b) -> uint8_t {
        uint32_t res; int carry = get_flag(CF) ? 1 : 0;
        switch (aluop) {
            case 0: res = a + b; break;
            case 2: res = a + b + carry; break;
            case 5: case 7: res = a - b; break;
            case 3: res = a - b - carry; break;
            case 1: res = a | b; break;
            case 4: res = a & b; break;
            case 6: res = a ^ b; break;
            default: res = 0;
        }
        uint8_t r8v = res & 0xFF;
        if (aluop == 1 || aluop == 4 || aluop == 6) { set_flag(CF, false); set_flag(OF, false); set_flag(AF, false); }
        else {
            bool sub = (aluop == 5 || aluop == 7 || aluop == 3);
            set_flag(CF, res & 0x100);
            set_flag(AF, (((a ^ b ^ r8v) & 0x10) != 0));
            bool of;
            if (sub) of = (((a ^ b) & (a ^ r8v)) & 0x80) != 0;
            else     of = ((~(a ^ b) & (a ^ r8v)) & 0x80) != 0;
            set_flag(OF, of);
        }
        set_flag(ZF, r8v == 0); set_flag(SF, r8v & 0x80); set_flag(PF, parity(r8v));
        return r8v;
    };
    // width-generic ALU (16- or 32-bit, per the operand-size prefix)
    auto aluv = [&](int aluop, uint32_t a, uint32_t b) -> uint32_t {
        uint64_t res; int carry = get_flag(CF) ? 1 : 0;
        switch (aluop) {
            case 0: res = static_cast<uint64_t>(a) + b; break;
            case 2: res = static_cast<uint64_t>(a) + b + carry; break;
            case 5: case 7: res = static_cast<uint64_t>(a) - b; break;
            case 3: res = static_cast<uint64_t>(a) - b - carry; break;
            case 1: res = a | b; break;
            case 4: res = a & b; break;
            case 6: res = a ^ b; break;
            default: res = 0;
        }
        uint32_t rv = static_cast<uint32_t>(res) & vmask;
        if (aluop == 1 || aluop == 4 || aluop == 6) { set_flag(CF, false); set_flag(OF, false); set_flag(AF, false); }
        else {
            bool sub = (aluop == 5 || aluop == 7 || aluop == 3);
            uint64_t cbit = o32 ? 0x100000000ull : 0x10000ull;
            set_flag(CF, (res & cbit) != 0);
            set_flag(AF, (((a ^ b ^ rv) & 0x10) != 0));
            bool of;
            if (sub) of = (((a ^ b) & (a ^ rv)) & vmsb) != 0;
            else     of = ((~(a ^ b) & (a ^ rv)) & vmsb) != 0;
            set_flag(OF, of);
        }
        set_flag(ZF, rv == 0); set_flag(SF, (rv & vmsb) != 0); set_flag(PF, parity(rv & 0xFF));
        return rv;
    };

    // condition-code evaluator shared by Jcc, SETcc and CMOVcc (cc = low nibble)
    auto cc = [&](int c) -> bool {
        switch (c & 0xF) {
            case 0x0: return get_flag(OF);
            case 0x1: return !get_flag(OF);
            case 0x2: return get_flag(CF);
            case 0x3: return !get_flag(CF);
            case 0x4: return get_flag(ZF);
            case 0x5: return !get_flag(ZF);
            case 0x6: return get_flag(CF) || get_flag(ZF);
            case 0x7: return !(get_flag(CF) || get_flag(ZF));
            case 0x8: return get_flag(SF);
            case 0x9: return !get_flag(SF);
            case 0xA: return get_flag(PF);
            case 0xB: return !get_flag(PF);
            case 0xC: return get_flag(SF) != get_flag(OF);
            case 0xD: return get_flag(SF) == get_flag(OF);
            case 0xE: return get_flag(ZF) || (get_flag(SF) != get_flag(OF));
            default:  return !get_flag(ZF) && (get_flag(SF) == get_flag(OF));
        }
    };

    // ---- 0x0F two-byte opcodes (80386) ----
    auto do_0f = [&]() {
        uint8_t op2 = fetch8();
        ++ophist[256 + op2];
        // long Jcc rel16/32
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t rel = o32 ? static_cast<int32_t>(fetch32()) : static_cast<int16_t>(fetch16());
            if (cc(op2 & 0xF)) jump(ip + rel);
            return;
        }
        // SETcc rm8
        if (op2 >= 0x90 && op2 <= 0x9F) { Modrm m; decode_modrm(m); w8m(m, cc(op2 & 0xF) ? 1 : 0); return; }
        // CMOVcc reg, rm (386 core doesn't have these, but harmless to support)
        if (op2 >= 0x40 && op2 <= 0x4F) { Modrm m; decode_modrm(m); uint32_t v = rvm(m); if (cc(op2 & 0xF)) srw(m.reg, v); return; }
        switch (op2) {
            case 0x00: { Modrm m; decode_modrm(m);   // group 6: SLDT/STR/LLDT/LTR/VERR/VERW
                switch (m.reg) {
                    case 0: wvm(m, ldtr); break;
                    case 1: wvm(m, tr); break;
                    case 2: ldtr = static_cast<uint16_t>(rvm(m)); break;
                    case 3: tr = static_cast<uint16_t>(rvm(m)); break;
                    default: break;                  // VERR/VERW: no-op
                } break; }
            case 0x01: { Modrm m; decode_modrm(m);   // group 7: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW
                // Only the four table instructions take memory; SMSW/LMSW take a
                // register, and for a register operand decode_modrm leaves seg_idx
                // unset — computing an address anyway indexed sbase[] out of bounds and
                // crashed. DOS/4GW is the first guest here to use the register form.
                const uint32_t a = m.is_reg ? 0 : pa(m);
                switch (m.reg) {
                    case 0: mem_.w16(a, gdt_limit); mem_.w32(a + 2, gdt_base); break;   // SGDT
                    case 1: mem_.w16(a, idt_limit); mem_.w32(a + 2, idt_base); break;   // SIDT
                    case 2: gdt_limit = mem_.r16(a); gdt_base = mem_.r32(a + 2) & (o32 ? 0xFFFFFFFF : 0xFFFFFF);
                            gdt_loaded = true; break;                                          // LGDT
                    case 3: idt_limit = mem_.r16(a); idt_base = mem_.r32(a + 2) & (o32 ? 0xFFFFFFFF : 0xFFFFFF); break;  // LIDT
                    case 4: wvm(m, cr[0] & 0xFFFF); break;                              // SMSW
                    case 6: cr[0] = (cr[0] & 0xFFFF0000u) | (rvm(m) & 0xFFFF); break;   // LMSW
                    default: break;
                } break; }
            case 0x06: cr[0] &= ~0x8u; break;        // CLTS (clear TS)
            case 0x08: case 0x09: break;             // INVD / WBINVD: no-op
            case 0x0B: fail("UD2", op2); break;
            // LAR / LSL: read a descriptor's access rights or limit. DJGPP's startup
            // uses LSL to discover how big the flat segment the extender gave it is,
            // so a 32-bit client hits these within a few instructions of its entry.
            // ZF reports whether the descriptor is *reachable* -- see desc_ok(); it is
            // not the present bit, and reading it as the present bit is what stopped the
            // Watcom linker.
            case 0x02: { Modrm m; decode_modrm(m);                                    // LAR r, rm16
                const uint16_t sel = static_cast<uint16_t>(rvm(m));
                const bool okd = desc_ok(sel);
                set_flag(ZF, okd);
                if (okd) { const Desc dsc = read_desc(sel);
                           srw(m.reg, (static_cast<uint32_t>(dsc.ar) << 8)
                                    | (static_cast<uint32_t>(dsc.hi & 0xF0) << 16)); }
                break; }
            case 0x03: { Modrm m; decode_modrm(m);                                    // LSL r, rm16
                const uint16_t sel = static_cast<uint16_t>(rvm(m));
                const bool okd = desc_ok(sel);
                set_flag(ZF, okd);
                if (okd) srw(m.reg, read_desc(sel).limit);
                break; }
            case 0x20: { uint8_t b = fetch8(); sd(b & 7, cr[(b >> 3) & 7]); break; }   // MOV r32, CRn
            case 0x21: { uint8_t b = fetch8(); sd(b & 7, dr[(b >> 3) & 7]); break; }   // MOV r32, DRn
            case 0x22: { uint8_t b = fetch8(); cr[(b >> 3) & 7] = gd(b & 7); break; }  // MOV CRn, r32
            case 0x23: { uint8_t b = fetch8(); dr[(b >> 3) & 7] = gd(b & 7); break; }  // MOV DRn, r32
            case 0x30: case 0x32: sd(AX, 0); sd(DX, 0); break;   // WRMSR / RDMSR: stub
            case 0x31: sd(AX, static_cast<uint32_t>(insns)); sd(DX, static_cast<uint32_t>(insns >> 32)); break;  // RDTSC
            case 0xA2:                              // CPUID (minimal 386/486-ish reply)
                if (gd(AX) == 0) { sd(AX, 1); sd(BX, 0x756E6547); sd(DX, 0x49656E69); sd(CX, 0x6C65746E); }  // "GenuineIntel"
                else { sd(AX, 0x0400); sd(BX, 0); sd(CX, 0); sd(DX, 0x00000011); }  // FPU + VME
                break;
            case 0xA0: pushV(sreg[FS]); break;
            case 0xA1: set_seg(FS, static_cast<uint16_t>(popV())); break;
            case 0xA8: pushV(sreg[GS]); break;
            case 0xA9: set_seg(GS, static_cast<uint16_t>(popV())); break;
            case 0xB2: { Modrm m; decode_modrm(m); srw(m.reg, o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m))); set_seg(SS, mem_.r16(pa(m) + (o32 ? 4 : 2))); break; }  // LSS
            case 0xB4: { Modrm m; decode_modrm(m); srw(m.reg, o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m))); set_seg(FS, mem_.r16(pa(m) + (o32 ? 4 : 2))); break; }  // LFS
            case 0xB5: { Modrm m; decode_modrm(m); srw(m.reg, o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m))); set_seg(GS, mem_.r16(pa(m) + (o32 ? 4 : 2))); break; }  // LGS
            case 0xB6: { Modrm m; decode_modrm(m); srw(m.reg, r8m(m)); break; }                       // MOVZX r, rm8
            case 0xB7: { Modrm m; decode_modrm(m); srw(m.reg, m.is_reg ? r[m.rm] : mem_.r16(pa(m))); break; }  // MOVZX r, rm16
            case 0xBE: { Modrm m; decode_modrm(m); srw(m.reg, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(r8m(m))))); break; }   // MOVSX r, rm8
            case 0xBF: { Modrm m; decode_modrm(m); uint16_t s = m.is_reg ? r[m.rm] : mem_.r16(pa(m)); srw(m.reg, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s)))); break; }  // MOVSX r, rm16
            case 0xAF: { Modrm m; decode_modrm(m);   // IMUL reg, rm
                int64_t p = static_cast<int64_t>(static_cast<int32_t>(o32 ? grw(m.reg) : static_cast<int16_t>(grw(m.reg))))
                          * static_cast<int64_t>(static_cast<int32_t>(o32 ? rvm(m) : static_cast<int16_t>(rvm(m))));
                srw(m.reg, static_cast<uint32_t>(p) & vmask);
                bool ov = (p < -(int64_t)(vmsb) || p > (int64_t)(vmsb - 1));
                set_flag(CF, ov); set_flag(OF, ov); break; }
            case 0xA3: case 0xAB: case 0xB3: case 0xBB: { Modrm m; decode_modrm(m);   // BT/BTS/BTR/BTC r/m, reg
                int g = (op2 == 0xA3) ? 4 : (op2 == 0xAB) ? 5 : (op2 == 0xB3) ? 6 : 7;
                int32_t bit = static_cast<int32_t>(grw(m.reg));
                if (m.is_reg) { int bp = bit & (vbits - 1); uint32_t v = grw(m.rm); set_flag(CF, (v >> bp) & 1);
                    if (g == 5) v |= (1u << bp); else if (g == 6) v &= ~(1u << bp); else if (g == 7) v ^= (1u << bp);
                    if (g != 4) srw(m.rm, v); }
                else { uint32_t addr = pa(m) + (bit >> 3); int bp = bit & 7; uint8_t v = mem_.r8(addr); set_flag(CF, (v >> bp) & 1);
                    if (g == 5) v |= (1u << bp); else if (g == 6) v &= ~(1u << bp); else if (g == 7) v ^= (1u << bp);
                    if (g != 4) mem_.w8(addr, v); }
                break; }
            case 0xBA: { Modrm m; decode_modrm(m); int g = m.reg; uint8_t imm = fetch8();   // BT/BTS/BTR/BTC r/m, imm8
                if (g < 4) break;                    // /0../3 not defined here
                if (m.is_reg) { int bp = imm & (vbits - 1); uint32_t v = grw(m.rm); set_flag(CF, (v >> bp) & 1);
                    if (g == 5) v |= (1u << bp); else if (g == 6) v &= ~(1u << bp); else if (g == 7) v ^= (1u << bp);
                    if (g != 4) srw(m.rm, v); }
                else { uint32_t addr = pa(m) + (imm >> 3); int bp = imm & 7; uint8_t v = mem_.r8(addr); set_flag(CF, (v >> bp) & 1);
                    if (g == 5) v |= (1u << bp); else if (g == 6) v &= ~(1u << bp); else if (g == 7) v ^= (1u << bp);
                    if (g != 4) mem_.w8(addr, v); }
                break; }
            case 0xA4: case 0xA5: case 0xAC: case 0xAD: { Modrm m; decode_modrm(m);   // SHLD/SHRD
                bool left = (op2 == 0xA4 || op2 == 0xA5);
                uint8_t cnt = (op2 == 0xA4 || op2 == 0xAC) ? fetch8() : gb(CX);
                cnt &= (o32 ? 31 : 15);
                uint32_t dst = rvm(m), src = grw(m.reg);
                if (cnt) {
                    uint32_t res;
                    if (left)  res = ((dst << cnt) | (src >> (vbits - cnt))) & vmask;
                    else       res = ((dst >> cnt) | (src << (vbits - cnt))) & vmask;
                    bool carry = left ? ((dst >> (vbits - cnt)) & 1) : ((dst >> (cnt - 1)) & 1);
                    set_flag(CF, carry);
                    set_flag(ZF, res == 0); set_flag(SF, (res & vmsb) != 0); set_flag(PF, parity(res & 0xFF));
                    wvm(m, res);
                }
                break; }
            case 0xBC: { Modrm m; decode_modrm(m); uint32_t s = rvm(m); set_flag(ZF, s == 0);   // BSF
                if (s) { uint32_t i = 0; while (!((s >> i) & 1)) ++i; srw(m.reg, i); } break; }
            case 0xBD: { Modrm m; decode_modrm(m); uint32_t s = rvm(m); set_flag(ZF, s == 0);   // BSR
                if (s) { uint32_t i = vbits - 1; while (!((s >> i) & 1)) --i; srw(m.reg, i); } break; }
            case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
                Modrm m; decode_modrm(m); (void)m; break; }   // NOP/prefetch hints
            default:
                fail(std::string("unimplemented 0F instruction 0x0F ") , op2);
        }
    };

    // ---- opcode dispatch ----
    switch (op) {
        // ALU group: 00-3D, pattern (op<<3)|dir/size
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint8_t v = alu8(a, r8m(m), gb(m.reg)); if (a != 7) w8m(m, v); break; }
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint32_t v = aluv(a, rvm(m), grw(m.reg)); if (a != 7) wvm(m, v); break; }
        case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint8_t v = alu8(a, gb(m.reg), r8m(m)); if (a != 7) sb(m.reg, v); break; }
        case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint32_t v = aluv(a, grw(m.reg), rvm(m)); if (a != 7) srw(m.reg, v); break; }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int a = op >> 3; uint8_t v = alu8(a, gb(AX), fetch8()); if (a != 7) sb(AX, v); break; }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int a = op >> 3; uint32_t v = aluv(a, grw(AX), fetchImmV()); if (a != 7) srw(AX, v); break; }

        // PUSH/POP segment and general regs
        case 0x06: pushV(sreg[ES]); break;
        case 0x07: set_seg(ES, static_cast<uint16_t>(popV())); break;
        case 0x0E: pushV(sreg[CS]); break;
        case 0x16: pushV(sreg[SS]); break;
        case 0x17: set_seg(SS, static_cast<uint16_t>(popV())); break;
        case 0x1E: pushV(sreg[DS]); break;
        case 0x1F: set_seg(DS, static_cast<uint16_t>(popV())); break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            pushV(grw(op & 7)); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            srw(op & 7, popV()); break;

        // PUSHA / POPA (PUSHAD / POPAD under a 0x66 prefix)
        case 0x60: { uint32_t sp = grw(SP);
            pushV(grw(AX)); pushV(grw(CX)); pushV(grw(DX)); pushV(grw(BX));
            pushV(sp);      pushV(grw(BP)); pushV(grw(SI)); pushV(grw(DI)); break; }
        case 0x61: { srw(DI, popV()); srw(SI, popV()); srw(BP, popV()); popV();
            srw(BX, popV()); srw(DX, popV()); srw(CX, popV()); srw(AX, popV()); break; }
        // BOUND r, m — an 80186 array-bounds check: the memory operand holds the lower
        // and upper limits back to back, and an index outside them raises INT 5. Rarely
        // emitted by C compilers, but Watcom's loader uses it.
        case 0x62: { Modrm m; decode_modrm(m);
            if (!m.is_reg) {
                const int32_t idx = static_cast<int32_t>(o32 ? gd(m.reg)
                                        : static_cast<int16_t>(r[m.reg]));
                const uint32_t a = pa(m);
                const int32_t lo = o32 ? static_cast<int32_t>(mem_.r32(a))
                                       : static_cast<int16_t>(mem_.r16(a));
                const int32_t hi = o32 ? static_cast<int32_t>(mem_.r32(a + 4))
                                       : static_cast<int16_t>(mem_.r16(a + 2));
                if (idx < lo || idx > hi) interrupt(5);
            }
            break; }
        // PUSH imm
        case 0x68: pushV(fetchImmV()); break;
        case 0x6A: pushV(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fetch8())))); break;
        // IMUL reg, rm, imm
        case 0x69: case 0x6B: { Modrm m; decode_modrm(m);
            int32_t imm = (op == 0x6B) ? static_cast<int8_t>(fetch8())
                                       : (o32 ? static_cast<int32_t>(fetch32()) : static_cast<int16_t>(fetch16()));
            int64_t p = static_cast<int64_t>(static_cast<int32_t>(o32 ? rvm(m) : static_cast<int16_t>(rvm(m)))) * imm;
            srw(m.reg, static_cast<uint32_t>(p) & vmask);
            bool ov = (p < -(int64_t)(vmsb) || p > (int64_t)(vmsb - 1));
            set_flag(CF, ov); set_flag(OF, ov); break; }

        // INC/DEC reg (40-4F) — preserve CF
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            bool c = get_flag(CF); srw(op & 7, aluv(0, grw(op & 7), 1)); set_flag(CF, c); break; }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            bool c = get_flag(CF); srw(op & 7, aluv(5, grw(op & 7), 1)); set_flag(CF, c); break; }

        // Jcc rel8 (70-7F)
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t rel = static_cast<int8_t>(fetch8()); if (cc(op & 0xF)) jump(ip + rel); break; }

        // Group 1: ALU rm, imm  (80/81/83)
        // 0x82 is an undocumented alias of 0x80 that every x86 implements, and real
        // assemblers do emit it. Same encoding, same behaviour.
        case 0x80: case 0x82: { Modrm m; decode_modrm(m); uint8_t imm = fetch8(); uint8_t v = alu8(m.reg, r8m(m), imm); if (m.reg != 7) w8m(m, v); break; }
        case 0x81: { Modrm m; decode_modrm(m); uint32_t imm = fetchImmV(); uint32_t v = aluv(m.reg, rvm(m), imm); if (m.reg != 7) wvm(m, v); break; }
        case 0x83: { Modrm m; decode_modrm(m); uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fetch8()))) & vmask; uint32_t v = aluv(m.reg, rvm(m), imm); if (m.reg != 7) wvm(m, v); break; }

        // TEST rm, reg
        case 0x84: { Modrm m; decode_modrm(m); alu8(4, r8m(m), gb(m.reg)); break; }
        case 0x85: { Modrm m; decode_modrm(m); aluv(4, rvm(m), grw(m.reg)); break; }
        // XCHG rm, reg
        case 0x86: { Modrm m; decode_modrm(m); uint8_t t = r8m(m); w8m(m, gb(m.reg)); sb(m.reg, t); break; }
        case 0x87: { Modrm m; decode_modrm(m); uint32_t t = rvm(m); wvm(m, grw(m.reg)); srw(m.reg, t); break; }
        // MOV
        case 0x88: { Modrm m; decode_modrm(m); w8m(m, gb(m.reg)); break; }
        case 0x89: { Modrm m; decode_modrm(m); wvm(m, grw(m.reg)); break; }
        case 0x8A: { Modrm m; decode_modrm(m); sb(m.reg, r8m(m)); break; }
        case 0x8B: { Modrm m; decode_modrm(m); srw(m.reg, rvm(m)); break; }
        case 0x8C: { Modrm m; decode_modrm(m);   // MOV rm, sreg
            if (m.is_reg) { if (o32) sd(m.rm, sreg[m.reg]); else r[m.rm] = sreg[m.reg]; } else mem_.w16(pa(m), sreg[m.reg]); break; }
        case 0x8D: { Modrm m; decode_modrm(m); srw(m.reg, m.off); break; }           // LEA
        case 0x8E: { Modrm m; decode_modrm(m); set_seg(m.reg, m.is_reg ? r[m.rm] : mem_.r16(pa(m))); break; }  // MOV sreg, rm
        // POP rm — the pop happens *first*. When ESP is the base register the effective
        // address is computed from the incremented ESP, which Intel spells out and which
        // matters exactly once in a blue moon: X-32's interrupt handler rewrites the
        // frame it was entered on with `pop dword [esp+8]` / `pop dword [esp]`, and
        // computing those addresses before the pop lands CS and EIP one slot apart. Its
        // closing 32-bit `retf` then returned to CS=<flags>:EIP=<CS> and ran off into
        // zeroed memory — 20 million instructions of `add [bx+si],al`.
        case 0x8F: { uint32_t v = popV(); Modrm m; decode_modrm(m); wvm(m, v); break; }

        case 0x90: break;   // NOP (XCHG AX,AX)
        case 0x9B: break;   // FWAIT: nothing to wait for, the FPU here is synchronous
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint32_t t = grw(AX); srw(AX, grw(op & 7)); srw(op & 7, t); break; }     // XCHG AX, r
        case 0x98: if (o32) sd(AX, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(r[AX])))); // CWDE
                   else r[AX] = static_cast<int16_t>(static_cast<int8_t>(r[AX] & 0xFF)); break;               // CBW
        case 0x99: if (o32) sd(DX, (gd(AX) & 0x80000000u) ? 0xFFFFFFFFu : 0);        // CDQ
                   else r[DX] = (r[AX] & 0x8000) ? 0xFFFF : 0x0000; break;           // CWD
        case 0x9C: pushV(flags); break;                                             // PUSHF(D)
        case 0x9D: flags = (popV() & 0xFFFF) | 0x0002; break;                       // POPF(D)
        case 0x9E: flags = (flags & 0xFF00) | (r[AX] >> 8); break;                  // SAHF
        case 0x9F: r[AX] = (r[AX] & 0x00FF) | ((flags & 0xFF) << 8); break;         // LAHF

        // ---- the BCD adjust group -------------------------------------------------
        // Packed (DAA/DAS) and unpacked (AAA/AAS/AAM/AAD) decimal fixups. A C compiler
        // never emits them, which is why nothing here needed them for four toolchains —
        // but they are as much part of the 8086 integer set as ADD is, and they are the
        // instructions a guest lands on first when it starts executing data: `0x27` (DAA)
        // is the byte the runaway linker died on, and reading "unimplemented instruction
        // 0x27" as a missing feature rather than as a wild jump cost real time. Straight
        // from the SDM pseudocode, including the two places where the second `IF` has no
        // `ELSE` and so leaves CF as the first one set it.
        case 0x27: case 0x2F: {                                                     // DAA / DAS
            const bool sub = (op == 0x2F);
            const uint8_t old_al = gb(AX);
            const bool old_cf = get_flag(CF);
            set_flag(CF, false);
            uint8_t al = old_al;
            if ((al & 0x0F) > 9 || get_flag(AF)) {
                const uint16_t t = sub ? static_cast<uint16_t>(al - 6) : static_cast<uint16_t>(al + 6);
                al = static_cast<uint8_t>(t);
                set_flag(CF, old_cf || (t & 0x100) != 0);     // carry out, or borrow in
                set_flag(AF, true);
            } else set_flag(AF, false);
            if (old_al > 0x99 || old_cf) {
                al = static_cast<uint8_t>(sub ? al - 0x60 : al + 0x60);
                set_flag(CF, true);
            }
            sb(AX, al);
            set_flag(ZF, al == 0); set_flag(SF, (al & 0x80) != 0); set_flag(PF, parity(al));
            break; }
        case 0x37: case 0x3F: {                                                     // AAA / AAS
            const bool sub = (op == 0x3F);
            uint8_t al = gb(AX), ah = gb(4 /*AH*/);
            if ((al & 0x0F) > 9 || get_flag(AF)) {
                al = static_cast<uint8_t>(sub ? al - 6 : al + 6);
                ah = static_cast<uint8_t>(sub ? ah - 1 : ah + 1);
                set_flag(AF, true); set_flag(CF, true);
            } else { set_flag(AF, false); set_flag(CF, false); }
            al &= 0x0F;
            sb(AX, al); sb(4, ah);
            set_flag(ZF, al == 0); set_flag(SF, (al & 0x80) != 0); set_flag(PF, parity(al));
            break; }
        case 0xD4: {                                                                // AAM imm8
            const uint8_t div = fetch8();
            if (!div) { interrupt(0); break; }                 // divide error, as on hardware
            const uint8_t al = gb(AX);
            sb(4, static_cast<uint8_t>(al / div));             // AH = quotient
            sb(AX, static_cast<uint8_t>(al % div));            // AL = remainder
            const uint8_t r8v = gb(AX);
            set_flag(ZF, r8v == 0); set_flag(SF, (r8v & 0x80) != 0); set_flag(PF, parity(r8v));
            break; }
        case 0xD5: {                                                                // AAD imm8
            const uint8_t mul = fetch8();
            const uint8_t al = static_cast<uint8_t>(gb(AX) + gb(4) * mul);
            sb(AX, al); sb(4, 0);
            set_flag(ZF, al == 0); set_flag(SF, (al & 0x80) != 0); set_flag(PF, parity(al));
            break; }
        case 0xD6: sb(AX, get_flag(CF) ? 0xFF : 0x00); break;   // SALC (undocumented, but real)
        case 0xF1: interrupt(1); break;                         // ICEBP / INT1

        // MOV AL/eAX <-> moffs
        case 0xA0: { uint32_t o = a32 ? fetch32() : fetch16(); int si = d.have_ovr ? d.seg_ovr_idx : DS; sb(AX, mem_.r8(lin(si, o))); break; }
        case 0xA1: { uint32_t o = a32 ? fetch32() : fetch16(); int si = d.have_ovr ? d.seg_ovr_idx : DS; uint32_t base = lin(si, o); srw(AX, o32 ? mem_.r32(base) : mem_.r16(base)); break; }
        case 0xA2: { uint32_t o = a32 ? fetch32() : fetch16(); int si = d.have_ovr ? d.seg_ovr_idx : DS; mem_.w8(lin(si, o), gb(AX)); break; }
        case 0xA3: { uint32_t o = a32 ? fetch32() : fetch16(); int si = d.have_ovr ? d.seg_ovr_idx : DS; uint32_t base = lin(si, o); if (o32) mem_.w32(base, gd(AX)); else mem_.w16(base, r[AX]); break; }
        // TEST AL/eAX, imm
        case 0xA8: alu8(4, gb(AX), fetch8()); break;
        case 0xA9: aluv(4, grw(AX), fetchImmV()); break;

        // String ops (address size selects SI/DI/CX vs ESI/EDI/ECX). INS/OUTS (6C-6F) are
        // here too: they are string operations whose other end is a port rather than
        // memory, and they get REP for free by living in this block. Nothing here has a
        // device behind it — io_in returns zero — so INS fills memory with zeros and OUTS
        // discards, which is what "no hardware there" means. DOS/4GW uses INSB, and an
        // unimplemented opcode was the next thing after its banner finally appeared.
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xA6: case 0xA7: case 0xAE: case 0xAF: {
            bool word = op & 1; int w = word ? (o32 ? 4 : 2) : 1; int delta = get_flag(DF) ? -w : w;
            int dsi = d.have_ovr ? d.seg_ovr_idx : DS;
            auto getSI = [&]() -> uint32_t { return a32 ? gd(SI) : r[SI]; };
            auto getDI = [&]() -> uint32_t { return a32 ? gd(DI) : r[DI]; };
            auto addSI = [&](int dl) { if (a32) sd(SI, gd(SI) + dl); else r[SI] += dl; };
            auto addDI = [&](int dl) { if (a32) sd(DI, gd(DI) + dl); else r[DI] += dl; };
            auto rdsrc = [&]() -> uint32_t { uint32_t a = lin(dsi, getSI()); return word ? (o32 ? mem_.r32(a) : mem_.r16(a)) : mem_.r8(a); };
            auto rddst = [&]() -> uint32_t { uint32_t a = lin(ES, getDI()); return word ? (o32 ? mem_.r32(a) : mem_.r16(a)) : mem_.r8(a); };
            auto wrdst = [&](uint32_t v) { uint32_t a = lin(ES, getDI()); if (!word) mem_.w8(a, static_cast<uint8_t>(v)); else if (o32) mem_.w32(a, v); else mem_.w16(a, static_cast<uint16_t>(v)); };
            auto once = [&]() {
                switch (op & 0xFE) {
                    case 0x6C: {                                                                                     // INS
                        uint32_t v = io_in(r[DX]);
                        if (word) v |= static_cast<uint32_t>(io_in(r[DX] + 1)) << 8;
                        if (word && o32) v |= (static_cast<uint32_t>(io_in(r[DX] + 2)) << 16)
                                            | (static_cast<uint32_t>(io_in(r[DX] + 3)) << 24);
                        wrdst(v); addDI(delta); break;
                    }
                    case 0x6E: {                                                                                     // OUTS
                        const uint32_t v = rdsrc();
                        for (int b = 0; b < w; ++b) io_out(static_cast<uint16_t>(r[DX] + b),
                                                           static_cast<uint8_t>(v >> (b * 8)));
                        addSI(delta); break;
                    }
                    case 0xA4: wrdst(rdsrc()); addSI(delta); addDI(delta); break;                                    // MOVS
                    case 0xAA: wrdst(grw(AX)); addDI(delta); break;                                                  // STOS
                    // LODS. The byte form loads AL and must leave AH alone: srw() writes the
                    // whole of AX, which zeroed it. Nothing had noticed, because the idiom
                    // that catches it is counting in AH across a `lodsb` copy loop — which
                    // is exactly how OpenWatcom v2's DOS/4G stub measures a PATH element
                    // before deciding whether to append a `\`. With the count destroyed it
                    // appended nothing and searched for `A:\BINWDOS4GW.EXE`.
                    case 0xAC: { uint32_t v = rdsrc();
                        if (word) srw(AX, v); else sb(AX, static_cast<uint8_t>(v));
                        addSI(delta); break; }
                    case 0xA6: { uint32_t s = rdsrc(), t = rddst(); if (word) aluv(7, s, t); else alu8(7, s, t); addSI(delta); addDI(delta); break; }  // CMPS
                    case 0xAE: { uint32_t t = rddst(); if (word) aluv(7, grw(AX), t); else alu8(7, gb(AX), t); addDI(delta); break; }                  // SCAS
                }
            };
            bool cmp = (op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE;
            if (rep) {
                auto getCX = [&]() -> uint32_t { return a32 ? gd(CX) : r[CX]; };
                auto decCX = [&]() { if (a32) sd(CX, gd(CX) - 1); else --r[CX]; };
                // A whole REP runs inside one step(), so it counts as one instruction and a
                // runaway count is invisible: the emulator looks wedged while the sample
                // trace insists it is executing normally. Worth saying out loud — but only
                // for the copying forms, where the count *is* a length. `repne scasb` with
                // ECX = -1 is how everyone writes strlen: it stops on the flag, and the
                // count is there to say "no limit".
                if (!cmp && getCX() > (1u << 24)) {
                    static int n = 0;
                    if (++n <= 4)
                        printf("[rep] %02X moving %08X bytes at %04X:%08X — that is not a"
                               " real length\n", op, getCX(), sreg[CS], ip - 1);
                }
                while (getCX()) { decCX(); once(); if (cmp) { bool z = get_flag(ZF); if (rep == 0xF3 && !z) break; if (rep == 0xF2 && z) break; } }
            } else once();
            break;
        }

        // MOV reg, imm
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: sb(op & 7, fetch8()); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (o32) sd(op & 7, fetch32()); else r[op & 7] = fetch16(); break;

        // Group 2: shifts/rotates
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            Modrm m; decode_modrm(m);
            bool word = op & 1;
            uint8_t cnt;
            if (op == 0xC0 || op == 0xC1) cnt = fetch8();
            else if (op == 0xD0 || op == 0xD1) cnt = 1;
            else cnt = gb(CX);
            cnt &= 0x1F;
            uint32_t msb = word ? vmsb : 0x80; uint32_t mask = word ? vmask : 0xFF;
            uint32_t val = word ? rvm(m) : r8m(m);
            for (uint8_t k = 0; k < cnt; ++k) {
                switch (m.reg) {
                    case 0: { bool c = val & msb; val = ((val << 1) | (c?1:0)) & mask; set_flag(CF, c); break; }               // ROL
                    case 1: { bool c = val & 1; val = ((val >> 1) | (c?msb:0)) & mask; set_flag(CF, c); break; }               // ROR
                    case 2: { bool c = val & msb; val = ((val << 1) | (get_flag(CF)?1:0)) & mask; set_flag(CF, c); break; }    // RCL
                    case 3: { bool c = val & 1; val = ((val >> 1) | (get_flag(CF)?msb:0)) & mask; set_flag(CF, c); break; }    // RCR
                    case 4: case 6: { set_flag(CF, val & msb); val = (val << 1) & mask; break; }                              // SHL/SAL
                    case 5: { set_flag(CF, val & 1); val = (val >> 1) & mask; break; }                                        // SHR
                    case 7: { set_flag(CF, val & 1); uint32_t s = val & msb; val = ((val >> 1) | s) & mask; break; }          // SAR
                }
            }
            if (cnt) { set_flag(ZF, (val & mask) == 0); set_flag(SF, val & msb); set_flag(PF, parity(val & 0xFF)); }
            if (word) wvm(m, val & mask); else w8m(m, val & mask);
            break;
        }

        // RET / RET imm16 (near)
        case 0xC2: { uint16_t n = fetch16(); jump(popV()); sp_set(sp_get() + n); break; }
        case 0xC3: jump(popV()); break;
        // LES / LDS
        case 0xC4: { Modrm m; decode_modrm(m); srw(m.reg, o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m))); set_seg(ES, mem_.r16(pa(m) + (o32 ? 4 : 2))); break; }
        case 0xC5: { Modrm m; decode_modrm(m); srw(m.reg, o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m))); set_seg(DS, mem_.r16(pa(m) + (o32 ? 4 : 2))); break; }
        // MOV rm, imm
        case 0xC6: { Modrm m; decode_modrm(m); w8m(m, fetch8()); break; }
        case 0xC7: { Modrm m; decode_modrm(m); wvm(m, fetchImmV()); break; }
        // ENTER / LEAVE
        case 0xC8: { uint16_t frame = fetch16(); uint8_t level = fetch8() & 0x1F;
            pushV(grw(BP)); uint32_t fp = grw(SP);
            for (uint8_t i = 1; i < level; ++i) { srw(BP, grw(BP) - (o32 ? 4 : 2)); pushV(grw(BP)); }
            if (level) pushV(fp);
            srw(BP, fp); srw(SP, grw(SP) - frame); break; }
        case 0xC9: { srw(SP, grw(BP)); srw(BP, popV()); break; }                    // LEAVE
        // RET far. Returning to a less privileged segment pops the caller's stack with
        // it — the other half of the privilege-changing gate in interrupt().
        case 0xCA: { uint16_t n = fetch16(); uint32_t no = popV(); uint16_t ns = static_cast<uint16_t>(popV());
                     far_ret(ns, no, o32, n); break; }
        case 0xCB: { uint32_t no = popV(); far_ret(static_cast<uint16_t>(popV()), no, o32); break; }
        // INT
        case 0xCC: interrupt(3); break;
        case 0xCD: { uint8_t n = fetch8(); interrupt(n); break; }
        case 0xCE: if (get_flag(OF)) interrupt(4); break;
        case 0xCF: { iret_flags = flags;                                                  // IRET
                     uint32_t no = popV(); uint16_t ns = static_cast<uint16_t>(popV());
                     flags = (popV() & 0xFFFF) | 0x0002; far_ret(ns, no, o32); break; }

        // CALL / JMP
        case 0xE8: { int32_t rel = o32 ? static_cast<int32_t>(fetch32()) : static_cast<int16_t>(fetch16()); pushV(ip); jump(ip + rel); break; }   // CALL near rel
        case 0xE9: { int32_t rel = o32 ? static_cast<int32_t>(fetch32()) : static_cast<int16_t>(fetch16()); jump(ip + rel); break; }              // JMP near rel
        case 0xEA: { uint32_t no = fetchImmV(); uint16_t ns = fetch16(); set_seg(CS, ns); jump(no); break; }                                 // JMP far
        case 0xEB: { int8_t rel = static_cast<int8_t>(fetch8()); jump(ip + rel); break; }                                                        // JMP short
        case 0x9A: { uint32_t no = fetchImmV(); uint16_t ns = fetch16(); pushV(sreg[CS]); pushV(ip); set_seg(CS, ns); jump(no); break; }     // CALL far

        // LOOP / JCXZ
        case 0xE0: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0 && !get_flag(ZF)) jump(ip + rel); break; }  // LOOPNZ
        case 0xE1: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0 &&  get_flag(ZF)) jump(ip + rel); break; }  // LOOPZ
        case 0xE2: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0) jump(ip + rel); break; }                   // LOOP
        case 0xE3: { int8_t rel = static_cast<int8_t>(fetch8()); if (r[CX] == 0) jump(ip + rel); break; }                     // JCXZ

        // IN/OUT (no hardware; read 0, ignore writes)
        case 0xE4: { uint8_t p = fetch8(); sb(AX, io_in(p)); break; }              // IN AL, imm8
        case 0xE5: { uint8_t p = fetch8(); srw(AX, io_in(p) | (io_in(p + 1) << 8)); break; }
        case 0xE6: { uint8_t p = fetch8(); io_out(p, gb(AX)); break; }              // OUT imm8, AL
        case 0xE7: { uint8_t p = fetch8(); io_out(p, gb(AX)); io_out(p + 1, r[AX] >> 8); break; }
        case 0xEC: sb(AX, io_in(r[DX])); break;                                     // IN AL, DX
        case 0xED: srw(AX, io_in(r[DX]) | (io_in(r[DX] + 1) << 8)); break;
        case 0xEE: io_out(r[DX], gb(AX)); break;                                    // OUT DX, AL
        case 0xEF: io_out(r[DX], gb(AX)); io_out(r[DX] + 1, r[AX] >> 8); break;

        // Flag ops
        case 0xF5: set_flag(CF, !get_flag(CF)); break;   // CMC
        case 0xF8: set_flag(CF, false); break;
        case 0xF9: set_flag(CF, true); break;
        case 0xFA: set_flag(IF, false); break;
        case 0xFB: set_flag(IF, true); break;
        case 0xFC: set_flag(DF, false); break;
        case 0xFD: set_flag(DF, true); break;
        case 0xF4: halted = true; break;                 // HLT

        // Group 3: F6/F7 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV)
        case 0xF6: { Modrm m; decode_modrm(m);
            switch (m.reg) {
                case 0: case 1: alu8(4, r8m(m), fetch8()); break;                               // TEST
                case 2: w8m(m, ~r8m(m)); break;                                                 // NOT
                case 3: { uint8_t v = r8m(m); bool c = v != 0; w8m(m, alu8(5, 0, v)); set_flag(CF, c); break; }  // NEG
                case 4: { uint16_t p = (uint16_t)gb(AX) * r8m(m); r[AX] = p; set_flag(CF, (p>>8)!=0); set_flag(OF, (p>>8)!=0); break; }   // MUL
                case 5: { int16_t p = (int16_t)(int8_t)gb(AX) * (int8_t)r8m(m); r[AX] = p; bool s = (int8_t)(p&0xFF) != p; set_flag(CF,s); set_flag(OF,s); break; } // IMUL
                case 6: { uint16_t dv = r[AX]; uint8_t b = r8m(m); if (!b) { interrupt(0); break; } uint8_t q = dv / b, rem = dv % b; r[AX] = q | (rem << 8); break; }  // DIV
                case 7: { int16_t dv = (int16_t)r[AX]; int8_t b = (int8_t)r8m(m); if (!b) { interrupt(0); break; } int8_t q = dv / b; int8_t rem = dv % b; r[AX] = (uint8_t)q | ((uint8_t)rem << 8); break; }  // IDIV
            } break; }
        case 0xF7: { Modrm m; decode_modrm(m);
            switch (m.reg) {
                case 0: case 1: aluv(4, rvm(m), fetchImmV()); break;                            // TEST
                case 2: wvm(m, ~rvm(m) & vmask); break;                                         // NOT
                case 3: { uint32_t v = rvm(m); bool c = v != 0; wvm(m, aluv(5, 0, v)); set_flag(CF, c); break; }  // NEG
                case 4: { if (o32) { uint64_t p = (uint64_t)gd(AX) * rvm(m); sd(AX, (uint32_t)p); sd(DX, (uint32_t)(p >> 32)); set_flag(CF, gd(DX) != 0); set_flag(OF, gd(DX) != 0); }
                          else { uint32_t p = (uint32_t)r[AX] * (uint16_t)rvm(m); r[AX] = p & 0xFFFF; r[DX] = p >> 16; set_flag(CF, r[DX] != 0); set_flag(OF, r[DX] != 0); } break; }   // MUL
                case 5: { if (o32) { int64_t p = (int64_t)(int32_t)gd(AX) * (int32_t)rvm(m); sd(AX, (uint32_t)p); sd(DX, (uint32_t)((uint64_t)p >> 32)); bool s = (int32_t)(uint32_t)p != p; set_flag(CF,s); set_flag(OF,s); }
                          else { int32_t p = (int32_t)(int16_t)r[AX] * (int16_t)rvm(m); r[AX] = p & 0xFFFF; r[DX] = (p >> 16) & 0xFFFF; bool s = (int16_t)(p&0xFFFF) != p; set_flag(CF,s); set_flag(OF,s); } break; }  // IMUL
                case 6: { if (o32) { uint64_t dv = ((uint64_t)gd(DX) << 32) | gd(AX); uint32_t b = rvm(m); if (!b) { interrupt(0); break; } sd(AX, (uint32_t)(dv / b)); sd(DX, (uint32_t)(dv % b)); }
                          else { uint32_t dv = ((uint32_t)r[DX] << 16) | r[AX]; uint16_t b = (uint16_t)rvm(m); if (!b) { interrupt(0); break; } r[AX] = dv / b; r[DX] = dv % b; } break; }   // DIV
                case 7: { if (o32) { int64_t dv = (int64_t)(((uint64_t)gd(DX) << 32) | gd(AX)); int32_t b = (int32_t)rvm(m); if (!b) { interrupt(0); break; } sd(AX, (uint32_t)(int32_t)(dv / b)); sd(DX, (uint32_t)(int32_t)(dv % b)); }
                          else { int32_t dv = ((uint32_t)r[DX] << 16) | r[AX]; int16_t b = (int16_t)rvm(m); if (!b) { interrupt(0); break; } r[AX] = (int16_t)(dv / b); r[DX] = (int16_t)(dv % b); } break; }  // IDIV
            } break; }

        // Group 4/5: FE/FF (INC/DEC/CALL/JMP/PUSH)
        case 0xFE: { Modrm m; decode_modrm(m); bool c = get_flag(CF);
            if (m.reg == 0) w8m(m, alu8(0, r8m(m), 1)); else w8m(m, alu8(5, r8m(m), 1)); set_flag(CF, c); break; }
        case 0xFF: { Modrm m; decode_modrm(m); bool c = get_flag(CF);
            switch (m.reg) {
                case 0: wvm(m, aluv(0, rvm(m), 1)); set_flag(CF, c); break;   // INC
                case 1: wvm(m, aluv(5, rvm(m), 1)); set_flag(CF, c); break;   // DEC
                // Read the target before pushing: with 32-bit addressing the operand can be
                // ESP-relative, and a real 386 evaluates it against the pre-push ESP.
                case 2: { uint32_t no = rvm(m); pushV(ip); jump(no); break; }                       // CALL near rm
                case 3: { uint32_t no = o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m)); uint16_t ns = mem_.r16(pa(m) + (o32 ? 4 : 2)); pushV(sreg[CS]); pushV(ip); set_seg(CS, ns); jump(no); break; }  // CALL far rm
                case 4: jump(rvm(m)); break;                                 // JMP near rm
                case 5: { uint32_t no = o32 ? mem_.r32(pa(m)) : mem_.r16(pa(m)); uint16_t ns = mem_.r16(pa(m) + (o32 ? 4 : 2)); set_seg(CS, ns); jump(no); break; }  // JMP far rm
                case 6: pushV(rvm(m)); break;                               // PUSH rm
            } break; }

        // 0x0F two-byte opcodes (80386+)
        case 0x0F: do_0f(); break;

        // ARPL: bring the destination selector's RPL up to the source's, and say in ZF
        // whether it had to. An operating system uses it to stop a caller from handing it a
        // selector more privileged than the caller itself; a DOS extender validating what a
        // client passed does the same thing. Now that clients here run at ring 3 (their
        // selectors say so) this comes up.
        case 0x63: { Modrm m; decode_modrm(m);
            const uint16_t src = static_cast<uint16_t>(grw(m.reg));
            const uint16_t dst = static_cast<uint16_t>(m.is_reg ? r[m.rm] : mem_.r16(pa(m)));
            if ((dst & 3) < (src & 3)) {
                const uint16_t v = static_cast<uint16_t>((dst & ~3u) | (src & 3));
                if (m.is_reg) r[m.rm] = v; else mem_.w16(pa(m), v);
                set_flag(ZF, true);
            } else set_flag(ZF, false);
            break;
        }

        // XLAT: AL indexes a table at DS:(E)BX. An 8086 instruction that no guest here
        // had used until DOS/4GW, which needs it before it will print its own error
        // message — so the first symptom was an unimplemented opcode standing where the
        // explanation should have been.
        case 0xD7: {
            const uint32_t b = d.a32 ? gd(BX) : r[BX];
            const int s = d.have_ovr ? d.seg_ovr_idx : DS;
            sb(AX, mem_.r8(lin(s, b + (r[AX] & 0xFF))));
            break;
        }

        // x87 FPU escape opcodes (D8-DF). The FPU is not modelled; LSI C's small
        // model does floating point in software and only touches the FPU to probe
        // and initialise it at startup. Consume the ModRM/operand and no-op, but
        // answer FNSTSW AX (DF /4, mod=3) so a CRT believes the FPU came up clean.
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            Modrm m; decode_modrm(m);
            ++fpu_ops;
            fpu_exec(op, m.reg, m.mod, m.rm, m.is_reg ? 0 : pa(m));
            if (fpu_trace)
                printf("[x87] %02X /%d mod=%d rm=%d -> st0=%g st1=%g top=%d fsw=%04X\n",
                       op, m.reg, m.mod, m.rm, fst[ftop & 7], fst[(ftop + 1) & 7], ftop, fsw);
            break;
        }

        default:
            fail("unimplemented instruction", op);
    }
}

}  // namespace dosemu
