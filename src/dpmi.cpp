#include "dpmi.h"
#include <cstdio>
#include <cstdlib>

namespace dosemu {

Dpmi::Dpmi(Cpu& cpu, Memory& mem) : cpu_(cpu), mem_(mem) {
    trace = getenv("DOSEMU_DPMI_TRACE") != nullptr;

    // The entry the client far-calls. The CPU intercepts the address rather than
    // executing anything, but put a far RET there so a stray call still returns
    // instead of running whatever happens to be in memory.
    mem_.wb(kEntrySeg, 0, 0xCB);
    // 0305h's save/restore-state procedures are far RETs and nothing more, because there
    // is no host state to preserve across a mode switch here: the switch keeps the whole
    // CPU. We say so in AX (a size of zero means "you need not call these"), but a client
    // is entitled to call them anyway, so they have to be real code.
    mem_.wb(kEntrySeg, kOffSaveRm, 0xCB);
    mem_.wb(kEntrySeg, kOffSavePm, 0xCB);
    cpu_.hook_lo = Memory::phys(kEntrySeg, 0);
    for (int i = 0; i < kCallbacks; ++i) mem_.wb(kEntrySeg, kOffCbBase + i, 0xCB);
    for (int i = 0; i < 256; ++i) mem_.wb(kEntrySeg, kOffPmInt + i, 0xCF);   // IRET, if reached raw
    cpu_.hook_hi = Memory::phys(kEntrySeg, kEntryBytes);
    cpu_.on_hook = [this](uint32_t off) {
        if (off == 0)             { switch_to_pm();    return true; }
        if (off == kOffRawToPm)   { raw_switch(true);  return true; }
        if (off == kOffRawToRm)   { raw_switch(false); return true; }
        if (off == kOffCbRet)     { cb_return();       return true; }
        if (off == kOffRmRet)     { rm_return();       return true; }
        if (off >= kOffCbBase && off < kOffCbBase + kCallbacks) {
            call_back(static_cast<int>(off - kOffCbBase)); return true;
        }
        if (off >= kOffPmInt && off < kOffPmInt + 256) {
            pm_int_default(static_cast<uint8_t>(off - kOffPmInt)); return true;
        }
        return false;
    };

    cpu_.ldt_base = kLdtBase;
    cpu_.ldt_limit = kLdtCount * 8 - 1;   // so LAR/LSL can say what is out of range
    for (int i = 0; i < 8; ++i) mem_.w8(kLdtBase + i, 0);   // null descriptor
}

uint16_t Dpmi::alloc_sel(int count) {
    if (next_sel_ + count > kLdtCount) return 0;
    // bit 2 = LDT, and RPL 3, because that is what a DPMI client is: a ring-3 program.
    // The descriptors already say DPL 3; the selectors said RPL 0, and a client can read
    // its own CPL off CS. DOS/4GW does, and at ring 0 it concludes it owns the machine and
    // starts reflecting DOS calls by switching modes itself rather than asking the host.
    uint16_t sel = static_cast<uint16_t>(next_sel_ * 8 + 7);
    next_sel_ += count;
    return sel;
}

// Write one 8-byte descriptor. Limits above 1 MiB switch on the granularity bit, so
// the byte limit the caller asked for is rounded up to the next 4 KiB page.
void Dpmi::set_desc(uint16_t sel, uint32_t base, uint32_t limit, bool code, bool big) {
    uint32_t a = desc_addr(sel);
    uint8_t g = 0;
    if (limit > 0xFFFFF) { limit >>= 12; g = 0x80; }
    mem_.w16(a, static_cast<uint16_t>(limit));
    mem_.w16(a + 2, static_cast<uint16_t>(base));
    mem_.w8(a + 4, static_cast<uint8_t>(base >> 16));
    // present, DPL 3, S=1, then executable/read or data/write, accessed
    mem_.w8(a + 5, static_cast<uint8_t>(0xF0 | (code ? 0x0B : 0x03)));
    mem_.w8(a + 6, static_cast<uint8_t>(g | (big ? 0x40 : 0) | ((limit >> 16) & 0x0F)));
    mem_.w8(a + 7, static_cast<uint8_t>(base >> 24));
}

// INT 2Fh AX=1687h: yes, there is a DPMI host here.
bool Dpmi::int2f() {
    if (cpu_.r[AX] != 0x1687) return false;
    cpu_.r[AX] = 0;                       // 0 = DPMI present
    cpu_.r[BX] = 1;                       // bit 0: 32-bit programs supported
    cpu_.sb(1 /*CL*/, 3);                 // 80386
    cpu_.r[DX] = 0x005A;                  // DPMI version 0.90 (DH=0, DL=90)
    cpu_.r[SI] = 0;                       // paragraphs of private data we need: none
    cpu_.set_seg(ES, kEntrySeg);
    cpu_.r[DI] = 0;                       // ES:DI = the mode-switch entry
    cpu_.set_flag(CF, false);
    if (trace) std::fprintf(stderr, "[dpmi] 2F/1687 -> host at %04X:0000\n", kEntrySeg);
    return true;
}

// The client far-called our entry. Build descriptors for the segments it is using,
// turn on protected mode, and far-return into it.
//
// The return address is still on the real-mode stack: the far CALL pushed CS then IP.
// We consume it here and resume at that address with CS reloaded as a selector, which
// is what the client's `retf` would have done had it stayed in real mode.
void Dpmi::switch_to_pm() {
    const uint16_t ret_ip = mem_.rw(cpu_.sreg[SS], cpu_.r[SP]);
    const uint16_t ret_cs = mem_.rw(cpu_.sreg[SS], cpu_.r[SP] + 2);
    if (trace)
        std::fprintf(stderr, "[dpmi] mode-switch entry: ax=%04X ss:sp=%04X:%04X stack=%04X %04X %04X %04X\n",
               cpu_.r[AX], cpu_.sreg[SS], cpu_.r[SP],
               mem_.rw(cpu_.sreg[SS], cpu_.r[SP]), mem_.rw(cpu_.sreg[SS], cpu_.r[SP] + 2),
               mem_.rw(cpu_.sreg[SS], cpu_.r[SP] + 4), mem_.rw(cpu_.sreg[SS], cpu_.r[SP] + 6));
    cpu_.r[SP] += 4;

    const bool is32 = (cpu_.r[AX] & 1) != 0;
    client32_ = is32;
    const uint16_t rm_ds = cpu_.sreg[DS], rm_ss = cpu_.sreg[SS];
    // ES must come out as a selector for the *PSP*, not an alias of whatever ES the
    // client happened to be holding when it called. That is the spec, and it is not a
    // formality: it is how an extender locates the command tail at PSP:0x80 and the
    // environment segment at PSP:0x2C. Get it wrong and the client starts with argc 0
    // and an empty environment, with nothing else visibly broken.
    const uint16_t psp = get_psp ? get_psp() : cpu_.sreg[ES];

    // Four 16-bit descriptors over the segments the client resumes with.
    const uint16_t cs_sel = alloc_sel();
    const uint16_t ds_sel = alloc_sel();
    const uint16_t es_sel = alloc_sel();
    const uint16_t ss_sel = alloc_sel();
    set_desc(cs_sel, static_cast<uint32_t>(ret_cs) << 4, 0xFFFF, true,  false);
    set_desc(ds_sel, static_cast<uint32_t>(rm_ds)  << 4, 0xFFFF, false, false);
    set_desc(es_sel, static_cast<uint32_t>(psp)    << 4, 0x00FF, false, false);   // the PSP is 256 bytes
    set_desc(ss_sel, static_cast<uint32_t>(rm_ss)  << 4, 0xFFFF, false, false);

    // The environment field of the PSP has to become a *selector*, not the real-mode
    // segment DOS put there. This is not a guess: crt1.c does
    //
    //     movedata(_stubinfo->psp_selector, 0x2c, ds, &env_selector, 2);
    //     movedata(env_selector, 0, ds, dos_environ, _stubinfo->env_size);
    //
    // — it reads that word through a selector and immediately uses it as one. In
    // protected mode the host owns how the PSP reads, so converting the field is the
    // host's job. Leave it as a segment and the client copies its environment out of
    // whatever descriptor that number happens to name: no fault, no DOS error, just an
    // empty environment and an empty argv[0].
    const uint16_t env_seg = mem_.rw(psp, 0x2C);
    if (env_seg) {
        const uint16_t env_sel = alloc_sel();
        set_desc(env_sel, static_cast<uint32_t>(env_seg) << 4, 0xFFFF, false, false);
        mem_.ww(psp, 0x2C, env_sel);
    }

    cpu_.cr[0] |= 1;                      // PE — from here set_seg() reads descriptors
    cpu_.set_seg(DS, ds_sel);
    cpu_.set_seg(ES, es_sel);
    cpu_.set_seg(SS, ss_sel);
    cpu_.set_seg(CS, cs_sel);
    cpu_.ip = ret_ip;
    cpu_.set_flag(CF, false);

    if (trace) {
        std::fprintf(stderr, "[dpmi] switch to %s protected mode: cs=%04X(%04X) ds=%04X es=%04X(psp %04X) ss=%04X ip=%04X\n",
               is32 ? "32-bit" : "16-bit", cs_sel, ret_cs, ds_sel, es_sel, psp, ss_sel, ret_ip);
        // The two things a client reads out of the PSP, at the moment it gets it: the
        // command tail and the environment segment. Everything a program knows about
        // how it was invoked comes through these, so when a client starts up blind this
        // says immediately whether it was handed nothing or misread something.
        const uint16_t env = mem_.rw(psp, 0x2C);
        std::fprintf(stderr, "[dpmi]   psp:80 tail(%u) \"", mem_.rb(psp, 0x80));
        for (uint8_t i = 0; i < mem_.rb(psp, 0x80) && i < 40; ++i) std::fprintf(stderr, "%c", mem_.rb(psp, 0x81 + i));
        std::fprintf(stderr, "\"  psp:2C env=%04X \"", env);
        for (uint16_t i = 0; i < 120; ++i) {
            const uint8_t c = mem_.rb(env, i);
            if (!c && !mem_.rb(env, i + 1)) break;
            std::fprintf(stderr, "%c", c ? c : '|');
        }
        std::fprintf(stderr, "\"\n");
    }
}

// INT 31h 0300h — "simulate real-mode interrupt". This is the hinge of the whole
// design: it is how a protected-mode client does file and console I/O. The client
// fills a 50-byte register frame at ES:EDI, names an interrupt in BL, and expects the
// host to run it as though in real mode and hand the registers back.
//
// We satisfy it by *actually* dropping the CPU to real mode for the duration. The DOS
// layer reads its arguments as segment:offset (read_asciiz(seg, off) and friends), so
// with cr0.PE clear and the frame's segment values loaded, every INT 21h handler works
// unmodified — no second, protected-mode-aware copy of the DOS layer. The frame's
// segments are real-mode paragraphs by definition, which is exactly what it wants.
//
// A selector covering the entry paragraph, so the protected-mode halves of 0305h/0306h
// have an address a client can far-jump to. 32-bit, because the client that wants raw
// mode switching is a 32-bit one and its far RET out of the save-state stub has to pop
// the same width its far CALL pushed.
uint16_t Dpmi::entry_sel() {
    if (!entry_sel_) {
        entry_sel_ = alloc_sel();
        set_desc(entry_sel_, Memory::phys(kEntrySeg, 0), 0xFFFF, true, true);
    }
    return entry_sel_;
}

// Deliver an interrupt to the handler the client installed with 0205h, as an interrupt
// handler is delivered: a frame on the stack it is already using, and it returns with
// IRET. Returns false when nothing is hooked, and then the DOS layer services the call
// exactly as before.
//
// This is the same shape as the guest-owns-the-IDT case in Cpu::interrupt(): a client that
// hooks a vector means it. DOS/4GW hooks protected-mode INT 21h so that *its* handler can
// translate the call — turn a flat 32-bit pointer into something DOS can reach — before
// passing it down through 0300h. Servicing the call ourselves instead skipped the
// translation, so the DOS layer got a buffer address of "selector with base 0, offset 0"
// and dutifully read 21 KiB of a file over the interrupt vector table.
//
// The frame is 16- or 32-bit according to what the *client* declared itself to be at the
// mode-switch entry, not according to the handler's code segment. DOS/4GW registers its
// handlers in the 16-bit alias selector it was handed and unwinds them with IRETD.
bool Dpmi::pm_int(uint8_t n) {
    const Handler& h = pmint_[n];
    if (!h.sel || in_default_) return false;
    const bool wide = client32_;
    if (wide) { cpu_.push32(cpu_.flags); cpu_.push32(cpu_.sreg[CS]); cpu_.push32(cpu_.ip); }
    else      { cpu_.push16(cpu_.flags); cpu_.push16(cpu_.sreg[CS]);
                cpu_.push16(static_cast<uint16_t>(cpu_.ip)); }
    if (trace) std::fprintf(stderr, "[dpmi]%llu pm int %02X ax=%04X -> client %04X:%08X (%s frame)\n",
                      static_cast<unsigned long long>(cpu_.insns), n, cpu_.r[AX],
                      h.sel, h.off, wide ? "32-bit" : "16-bit");
    cpu_.set_flag(IF, false);
    cpu_.set_seg(CS, h.sel);
    cpu_.jump(h.off);
    return true;
}

// The IRET a protected-mode interrupt handler owes: pop the frame this host pushed in
// pm_int() and resume the interrupted code.
void Dpmi::unwind_int_frame(bool wide) {
    const uint32_t ip = wide ? cpu_.pop32() : cpu_.pop16();
    const uint16_t cs = static_cast<uint16_t>(wide ? cpu_.pop32() : cpu_.pop16());
    cpu_.flags = static_cast<uint16_t>((wide ? cpu_.pop32() : cpu_.pop16()) | 0x0002);
    cpu_.set_seg(CS, cs);
    cpu_.jump(ip);
}

// The address 0204h reports as the *host's* handler for a vector, so a client can chain
// to it. Reaching it services the interrupt the ordinary way and then unwinds the frame
// itself, because the client may have arrived here by jumping with a frame we built.
//
// Unless servicing it transferred control. 0301h/0302h do not return a result: they hand
// the CPU to a real-mode procedure and expect the guest to run it next. Unwinding here
// would throw that away and jump to whatever three dwords the *real-mode* stack happened
// to hold — which is how DOS/4GW ended up executing the interrupt vector table. The IRET
// is not cancelled, only deferred: rm_return() performs it when the procedure comes back
// and the client's context is whole again.
void Dpmi::pm_int_default(uint8_t n) {
    const bool wide = client32_;
    rm_transferred_ = false;
    in_default_ = true;
    if (real_int) real_int(n);
    in_default_ = false;
    if (rm_transferred_) {
        rm_transferred_ = false;
        rm_[rm_depth_ - 1].unwind = true;
        rm_[rm_depth_ - 1].wide = wide;
        return;
    }
    unwind_int_frame(wide);
}

// The 50-byte real-mode call structure, in and out. Shared by 0300h and 0301h/0302h.
void Dpmi::load_frame(uint32_t f) {
    cpu_.sd(DI, mem_.r32(f + 0x00)); cpu_.sd(SI, mem_.r32(f + 0x04));
    cpu_.sd(BP, mem_.r32(f + 0x08)); cpu_.sd(BX, mem_.r32(f + 0x10));
    cpu_.sd(DX, mem_.r32(f + 0x14)); cpu_.sd(CX, mem_.r32(f + 0x18));
    cpu_.sd(AX, mem_.r32(f + 0x1C));
    cpu_.flags = mem_.r16(f + 0x20);
    // The segment fields hold real-mode paragraphs, and this takes them at their word.
    //
    // It used to try to be helpful: if the value looked like one of our LDT selectors and
    // its descriptor covered conventional memory, it substituted base>>4, on the theory
    // that a client reflecting a call it received in protected mode might copy its own
    // segment registers straight in. **That cannot be made to work, because a paragraph and
    // a selector are the same sixteen bits.** DOS/4GW puts the paragraph 0x018F in the
    // frame, exactly as the spec says it should; 0x018F & 4 is set, and slot 49 happened to
    // be a live descriptor with base 0xE1E0, so the substitution sent an `open` to
    // 0xE1E0 + 0x2380 and DOS/4GW reported `can't find file A:\WCC.EXE to load` about a file
    // that was right there. Adding a present-bit check only moved the failure: while the
    // slot was empty it read as base 0 and the garbled `DOS/16M error: ...` message came out
    // of linear 0x0E18 instead of 0x18F0 + 0x0E18.
    //
    // The conversion was introduced when the linker printed nothing at all, and it did make
    // that better — but what was actually wrong was elsewhere and is fixed: the CF result
    // discarded by rm_return()'s IRET, the arena holes that misplaced DOS/4GW's transfer
    // buffer, and LODSB clobbering AH. With those gone, taking the frame literally is both
    // simpler and what the spec says, and it is what lets OpenWatcom v2's own DOS binaries
    // run. If a client ever really does put a selector here, the answer is to fail the call,
    // not to guess which of the two meanings a number has.
    cpu_.set_seg(ES, mem_.r16(f + 0x22));
    cpu_.set_seg(DS, mem_.r16(f + 0x24));
    cpu_.set_seg(FS, mem_.r16(f + 0x26));
    cpu_.set_seg(GS, mem_.r16(f + 0x28));
}

void Dpmi::store_frame(uint32_t f) { store_frame(f, cpu_.flags); }

void Dpmi::store_frame(uint32_t f, uint16_t flags) {
    mem_.w32(f + 0x00, cpu_.gd(DI)); mem_.w32(f + 0x04, cpu_.gd(SI));
    mem_.w32(f + 0x08, cpu_.gd(BP)); mem_.w32(f + 0x10, cpu_.gd(BX));
    mem_.w32(f + 0x14, cpu_.gd(DX)); mem_.w32(f + 0x18, cpu_.gd(CX));
    mem_.w32(f + 0x1C, cpu_.gd(AX));
    mem_.w16(f + 0x20, flags);
    mem_.w16(f + 0x22, cpu_.sreg[ES]); mem_.w16(f + 0x24, cpu_.sreg[DS]);
    mem_.w16(f + 0x26, cpu_.sreg[FS]); mem_.w16(f + 0x28, cpu_.sreg[GS]);
}

// INT 31h 0301h/0302h: call a real-mode procedure whose address is in the frame, rather
// than an interrupt vector. This is how DOS/4GW passes DOS calls down — it reads the
// real-mode INT 21h vector out of the IVT itself and calls it as a procedure — and it is
// the reason the linker printed nothing at all: we answered 0302h with "unsupported", so
// its reflector reported failure for every call, including the seven writes carrying its
// own error message.
//
// Unlike 0300h, which the DOS layer services directly, this has to *run guest code* and
// come back. So: drop to real mode with the frame's registers, push a return address that
// the CPU hook recognises, and jump to the procedure. When it returns there, rm_return()
// writes the registers back and restores the client. 0302h's procedure returns with IRET,
// so it wants flags on the stack under the return address; 0301h's uses RETF.
void Dpmi::rm_call(bool iret_frame) {
    const uint32_t f = cpu_.lin(ES, cpu_.gd(DI));
    const uint16_t client_cx = cpu_.r[CX];               // before the frame overwrites it
    if (rm_depth_ >= 4) {                                // four deep is already absurd
        cpu_.set_flag(CF, true); cpu_.r[AX] = 0x8001; return;
    }
    RmCall& rc = rm_[rm_depth_++];
    rc.saved = cpu_.save();
    rc.frame = f;
    rc.unwind = false; rc.wide = false; rc.iret = iret_frame;

    const uint16_t target_cs = mem_.r16(f + 0x2C), target_ip = mem_.r16(f + 0x2A);
    uint16_t ss = mem_.r16(f + 0x30), sp = mem_.r16(f + 0x2E);

    cpu_.cr[0] &= ~1u;                                   // real mode
    cpu_.ip_mask = 0xFFFFu; cpu_.cs_d = cpu_.ss_d = false;
    load_frame(f);
    // SP has to be *inside* the scratch block, which is 512 bytes. 0x0F00 was seven times
    // past its end, so a client that left SS:SP zero would have had every push land on
    // whatever followed it in conventional memory. simulate_real_int() has always used
    // 0x0100; this now matches it.
    if (!ss && !sp) { ss = scratch_stack_seg(); sp = 0x0100; }
    cpu_.set_seg(SS, ss); cpu_.r[SP] = sp;

    // The client may have asked us to copy words from its stack onto the real-mode one,
    // and CX said how many — CX *at the INT 31h*, which is not the CX the frame carries.
    // Reading it after load_frame() meant a 7-byte DOS write asked for seven words of
    // stack copy, and those words landed on top of the very string it was writing:
    // `strlen` measured "DOS/16M" as 7 characters one instruction before the call, and
    // seven records of interrupt-vector table came out of it.
    const uint16_t words = client_cx & 0xFF;
    const uint32_t pm_sp = rc.saved.ss_d
        ? (rc.saved.r[SP] | (static_cast<uint32_t>(rc.saved.rhi[SP]) << 16))
        : rc.saved.r[SP];
    for (int i = static_cast<int>(words) - 1; i >= 0; --i) {
        const uint16_t v = mem_.r16(rc.saved.sbase[SS] + pm_sp + i * 2);
        cpu_.r[SP] -= 2;
        mem_.ww(cpu_.sreg[SS], cpu_.r[SP], v);
    }
    if (iret_frame) { cpu_.r[SP] -= 2; mem_.ww(cpu_.sreg[SS], cpu_.r[SP], cpu_.flags); }
    cpu_.r[SP] -= 2; mem_.ww(cpu_.sreg[SS], cpu_.r[SP], kEntrySeg);
    cpu_.r[SP] -= 2; mem_.ww(cpu_.sreg[SS], cpu_.r[SP], kOffRmRet);

    if (trace) std::fprintf(stderr, "[dpmi]%llu real-mode call %04X:%04X (%s frame) ax=%04X ds=%04X(%08X)"
                      " edx=%08X  frame ds=%04X es=%04X ebx=%08X\n",
                      static_cast<unsigned long long>(cpu_.insns), target_cs, target_ip,
                      iret_frame ? "iret" : "retf",
                      cpu_.r[AX], cpu_.sreg[DS], cpu_.sbase[DS], cpu_.gd(DX),
                      mem_.r16(f + 0x24), mem_.r16(f + 0x22), mem_.r32(f + 0x10));
    cpu_.set_seg(CS, target_cs);
    cpu_.jump(target_ip);
    rm_transferred_ = true;   // the CPU is the procedure's now; nobody may unwind over it
}

// The procedure returned. Hand its registers back through the frame and put the client
// exactly as it was, which is what makes this transparent to a client that only knows it
// asked for a DOS call.
void Dpmi::rm_return() {
    if (!rm_depth_) { cpu_.jump(cpu_.ip); return; }
    RmCall& rc = rm_[--rm_depth_];
    // The flags the procedure computed — for 0302h that is *not* the flags register now.
    // The procedure ends with IRET, and an IRET restores the flags image from the frame,
    // which for 0302h this host fabricated: it is our own value from before the call and
    // says nothing. What the client is waiting for is what the procedure produced, and
    // after the IRET it survives only in Cpu::iret_flags. This is the whole result of a
    // reflected DOS call: `open` reports "no such file" in CF and nothing else. Returning
    // the stale image told wlink every failed open had succeeded, so it used the error
    // code as a file handle and reported "invalid library file attribute" about a library
    // it had never read — the "success but blank" failure mode again, one layer down.
    const uint16_t result_flags = rc.iret ? cpu_.iret_flags : cpu_.flags;
    store_frame(rc.frame, result_flags);
    if (trace) std::fprintf(stderr, "[dpmi] real-mode call returned ax=%04X flags=%04X\n",
                      cpu_.r[AX], result_flags);
    cpu_.restore(rc.saved);
    cpu_.set_flag(CF, false);                            // the DPMI call itself succeeded
    // The context just restored has CS:IP back at the host's default interrupt handler,
    // which is where the 0301h/0302h came from if pm_int_default() deferred its IRET to us.
    // Resuming there would re-enter the handler forever, so perform the unwind it skipped.
    if (rc.unwind) unwind_int_frame(rc.wide);
}

// A selector over the whole first megabyte. A callback hands the client a pointer to the
// real-mode stack, which needs a descriptor that can address a paragraph.
uint16_t Dpmi::low_sel() {
    if (!low_sel_) { low_sel_ = alloc_sel(); set_desc(low_sel_, 0, 0xFFFFF, false, true); }
    return low_sel_;
}

// The stack a callback's procedure runs on. The client does not supply one — the host is
// required to — and it cannot be the real-mode stack that was current when the callback
// fired, which may be 64 bytes of BIOS scratch.
void Dpmi::cb_stack(uint16_t& sel, uint32_t& sp) {
    if (!cb_ss_) {
        const uint32_t base = alloc_mem(0x2000);
        cb_ss_ = alloc_sel();
        set_desc(cb_ss_, base, 0x1FFF, false, true);
        cb_sp_ = 0x2000;
    }
    sel = cb_ss_; sp = cb_sp_;
}

// A real-mode callback fired: real-mode code far-called the address 0303h handed out.
// Lay the real-mode machine state into the structure the client nominated, switch to
// protected mode, and enter its procedure — with DS:ESI pointing at the real-mode stack
// and ES:EDI at that structure, which is the whole interface. The procedure ends with
// IRETD, so the frame we push under it returns to kOffCbRet and cb_return() takes the
// machine back to real mode using whatever CS:IP the client left in the structure.
//
// Everything a client uses this for is a real-mode event it wants to handle in protected
// mode: DJGPP registers one to hook the FPU emulator (which is why every DJGPP program
// prints `Coprocessor not present and DPMI setup failed!` -- this was the missing half),
// and DOS/4GW will not start without them.
void Dpmi::call_back(int slot) {
    const Callback& cb = cb_[slot];
    if (!cb.used) { cpu_.jump(cpu_.ip); return; }        // freed under us; do nothing
    const uint32_t f = cpu_.read_desc(cb.str_sel).base + cb.str_off;

    mem_.w32(f + 0x00, cpu_.gd(DI)); mem_.w32(f + 0x04, cpu_.gd(SI));
    mem_.w32(f + 0x08, cpu_.gd(BP)); mem_.w32(f + 0x10, cpu_.gd(BX));
    mem_.w32(f + 0x14, cpu_.gd(DX)); mem_.w32(f + 0x18, cpu_.gd(CX));
    mem_.w32(f + 0x1C, cpu_.gd(AX));
    mem_.w16(f + 0x20, cpu_.flags);
    mem_.w16(f + 0x22, cpu_.sreg[ES]); mem_.w16(f + 0x24, cpu_.sreg[DS]);
    mem_.w16(f + 0x26, cpu_.sreg[FS]); mem_.w16(f + 0x28, cpu_.sreg[GS]);
    mem_.w16(f + 0x2C, 0); mem_.w16(f + 0x2A, 0);        // CS:IP -- the client fills these
    mem_.w16(f + 0x30, cpu_.sreg[SS]); mem_.w16(f + 0x2E, cpu_.r[SP]);

    const uint32_t rm_stack = static_cast<uint32_t>(cpu_.sreg[SS]) * 16 + cpu_.r[SP];
    if (trace)
        std::fprintf(stderr, "[dpmi] callback %d -> %04X:%08X, real stack %04X:%04X\n",
               slot, cb.proc_sel, cb.proc_off, cpu_.sreg[SS], cpu_.r[SP]);

    uint16_t ss; uint32_t sp;
    cb_stack(ss, sp);
    cpu_.cr[0] |= 1u;                                    // protected mode
    cpu_.set_seg(DS, low_sel());   cpu_.sd(SI, rm_stack);
    cpu_.set_seg(ES, cb.str_sel);  cpu_.sd(DI, cb.str_off);
    cpu_.set_seg(SS, ss);          cpu_.sp_set(sp);
    cpu_.push32(cpu_.flags);                             // the frame its IRETD unwinds
    cpu_.push32(entry_sel());
    cpu_.push32(kOffCbRet);
    cpu_.set_seg(CS, cb.proc_sel);
    cpu_.jump(cb.proc_off);
}

// The client's callback procedure has IRETD'd. Back to real mode, with the register
// state it left in the structure — including the CS:IP it wants execution to resume at,
// which for a callback invoked by a far CALL is the return address it popped itself.
void Dpmi::cb_return() {
    const uint32_t f = cpu_.read_desc(cpu_.sreg[ES]).base + cpu_.gd(DI);
    cpu_.cr[0] &= ~1u;
    cpu_.ip_mask = 0xFFFFu; cpu_.cs_d = cpu_.ss_d = false;

    cpu_.sd(DI, mem_.r32(f + 0x00)); cpu_.sd(SI, mem_.r32(f + 0x04));
    cpu_.sd(BP, mem_.r32(f + 0x08)); cpu_.sd(BX, mem_.r32(f + 0x10));
    cpu_.sd(DX, mem_.r32(f + 0x14)); cpu_.sd(CX, mem_.r32(f + 0x18));
    cpu_.sd(AX, mem_.r32(f + 0x1C));
    cpu_.flags = mem_.r16(f + 0x20);
    cpu_.set_seg(ES, mem_.r16(f + 0x22)); cpu_.set_seg(DS, mem_.r16(f + 0x24));
    cpu_.set_seg(FS, mem_.r16(f + 0x26)); cpu_.set_seg(GS, mem_.r16(f + 0x28));
    cpu_.set_seg(SS, mem_.r16(f + 0x30)); cpu_.r[SP] = mem_.r16(f + 0x2E);
    cpu_.set_seg(CS, mem_.r16(f + 0x2C));
    cpu_.jump(mem_.r16(f + 0x2A));
    if (trace) std::fprintf(stderr, "[dpmi] callback returns to %04X:%04X\n", cpu_.sreg[CS], cpu_.r[SP]);
}

// The raw mode switches of INT 31h 0306h. Entered by a far JMP -- not a call, there is no
// return address -- with the whole new machine state in registers:
//
//     AX = new DS   CX = new ES   DX = new SS   (E)BX = new (E)SP
//     SI = new CS   (E)DI = new (E)IP          BP preserved, FS/GS zero afterwards
//
// A client uses these when it wants to own the switch rather than be handed a mode:
// DOS/4GW does, and will not start without them. Everything else about the CPU carries
// across untouched, which is what makes 0305h's "no state to save" answer true.
void Dpmi::raw_switch(bool to_pm) {
    const uint16_t nds = cpu_.r[AX], nes = cpu_.r[CX], nss = cpu_.r[DX], ncs = cpu_.r[SI];
    const uint32_t nsp = cpu_.gd(BX), nip = cpu_.gd(DI);
    if (trace)
        std::fprintf(stderr, "[dpmi] raw switch to %s: cs=%04X ip=%08X ds=%04X es=%04X ss=%04X sp=%08X\n",
               to_pm ? "pm" : "real", ncs, nip, nds, nes, nss, nsp);
    if (to_pm) cpu_.cr[0] |= 1u; else cpu_.cr[0] &= ~1u;
    // Order matters twice over: SS before the stack pointer, because whether ESP is 16-
    // or 32-bit comes from the SS descriptor's B bit; and CS before the jump, because
    // whether EIP wraps at 64 KiB comes from the CS descriptor's D bit.
    cpu_.set_seg(DS, nds);
    cpu_.set_seg(ES, nes);
    cpu_.sreg[FS] = 0; cpu_.sbase[FS] = 0;
    cpu_.sreg[GS] = 0; cpu_.sbase[GS] = 0;
    cpu_.set_seg(SS, nss);
    cpu_.sp_set(nsp);
    cpu_.set_seg(CS, ncs);
    cpu_.jump(nip);
}

// The client's own protected-mode state is saved and restored around the call; only
// the frame is written back.
void Dpmi::simulate_real_int() {
    const uint8_t vec = static_cast<uint8_t>(cpu_.r[BX] & 0xFF);   // BL
    const uint32_t f = cpu_.lin(ES, cpu_.gd(DI));

    // Snapshot everything the call is allowed to disturb.
    const Cpu::State saved = cpu_.save();

    cpu_.cr[0] &= ~1u;                                   // back to real mode
    cpu_.ip_mask = 0xFFFFu; cpu_.cs_d = cpu_.ss_d = false;

    cpu_.sd(DI, mem_.r32(f + 0x00)); cpu_.sd(SI, mem_.r32(f + 0x04));
    cpu_.sd(BP, mem_.r32(f + 0x08)); cpu_.sd(BX, mem_.r32(f + 0x10));
    cpu_.sd(DX, mem_.r32(f + 0x14)); cpu_.sd(CX, mem_.r32(f + 0x18));
    cpu_.sd(AX, mem_.r32(f + 0x1C));
    cpu_.flags = mem_.r16(f + 0x20);
    cpu_.set_seg(ES, mem_.r16(f + 0x22)); cpu_.set_seg(DS, mem_.r16(f + 0x24));
    cpu_.set_seg(FS, mem_.r16(f + 0x26)); cpu_.set_seg(GS, mem_.r16(f + 0x28));

    // SS:SP zero means "host, provide a stack". Nothing we service actually runs guest
    // code on it, but a handler that pushes would otherwise scribble on segment 0.
    uint16_t ss = mem_.r16(f + 0x30), sp = mem_.r16(f + 0x2E);
    if (!ss && !sp) { ss = scratch_stack_seg(); sp = 0x0100; }
    cpu_.set_seg(SS, ss); cpu_.r[SP] = sp;

    if (trace) {
        std::fprintf(stderr, "[dpmi]   reflect INT %02X ah=%02X al=%02X ds=%04X dx=%04X cx=%04X",
               vec, cpu_.r[AX] >> 8, cpu_.r[AX] & 0xFF, cpu_.sreg[DS], cpu_.r[DX], cpu_.r[CX]);
        // Show what a write actually writes. Whether a client's output is empty is the
        // difference between "the DOS call failed" and "the client had nothing to say",
        // and those two have completely different causes.
        // For the calls that take a path at DS:DX, show the path. A client that cannot
        // find a file and one that never looked for it produce the same silence.
        const uint8_t ah = static_cast<uint8_t>(cpu_.r[AX] >> 8);
        if (vec == 0x21 && (ah == 0x3D || ah == 0x3C || ah == 0x41 || ah == 0x43 ||
                            ah == 0x4E || ah == 0x60 || ah == 0x39 || ah == 0x3B)) {
            std::fprintf(stderr, "  \"");
            for (int i = 0; i < 80; ++i) {
                const uint8_t c = mem_.rb(cpu_.sreg[DS], static_cast<uint16_t>(cpu_.r[DX] + i));
                if (!c) break;
                std::fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            std::fprintf(stderr, "\"");
        }
        if (vec == 0x21 && (cpu_.r[AX] >> 8) == 0x40) {
            std::fprintf(stderr, "  \"");
            for (uint16_t i = 0; i < cpu_.r[CX] && i < 60; ++i) {
                const uint8_t c = mem_.rb(cpu_.sreg[DS], static_cast<uint16_t>(cpu_.r[DX] + i));
                std::fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            std::fprintf(stderr, "\"");
        }
        std::fprintf(stderr, "\n");
    }
    if (real_int) real_int(vec);

    // Hand the results back through the frame.
    mem_.w32(f + 0x00, cpu_.gd(DI)); mem_.w32(f + 0x04, cpu_.gd(SI));
    mem_.w32(f + 0x08, cpu_.gd(BP)); mem_.w32(f + 0x10, cpu_.gd(BX));
    mem_.w32(f + 0x14, cpu_.gd(DX)); mem_.w32(f + 0x18, cpu_.gd(CX));
    mem_.w32(f + 0x1C, cpu_.gd(AX));
    mem_.w16(f + 0x20, cpu_.flags);
    mem_.w16(f + 0x22, cpu_.sreg[ES]); mem_.w16(f + 0x24, cpu_.sreg[DS]);
    mem_.w16(f + 0x26, cpu_.sreg[FS]); mem_.w16(f + 0x28, cpu_.sreg[GS]);

    cpu_.restore(saved);                                 // back to the client's world
    cpu_.set_flag(CF, false);                            // 0300h itself succeeded
}

// First-fit over a list of blocks, with adjacent free blocks merged. Deliberately
// simple, but it must actually reclaim: DJGPP's sbrk grows the heap by allocating a
// larger block, copying into it and freeing the old one, so a free that does nothing
// makes each growth step leak the entire previous heap.
uint32_t Dpmi::alloc_mem(uint32_t len) {
    len = (len + 0xFFF) & ~0xFFFu;                       // page granularity
    if (!len) len = 0x1000;
    // Index, not a reference: the push_back below can reallocate the vector, and
    // writing through a reference taken before it is a use-after-free.
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].used || blocks_[i].len < len) continue;
        const uint32_t base = blocks_[i].base, have = blocks_[i].len;
        blocks_[i].len = len; blocks_[i].used = true;
        if (have > len) blocks_.push_back({base + len, have - len, false});
        return base;
    }
    if (static_cast<uint64_t>(heap_next_) + len > Memory::kSize) return 0;
    const uint32_t base = heap_next_;
    heap_next_ += len;
    blocks_.push_back({base, len, true});
    return base;
}

bool Dpmi::free_mem(uint32_t base) {
    size_t f = blocks_.size();
    for (size_t i = 0; i < blocks_.size(); ++i)
        if (blocks_[i].used && blocks_[i].base == base) { f = i; break; }
    if (f == blocks_.size()) return false;
    blocks_[f].used = false;
    // Absorb any free block that starts where this one ends, so a grow/copy/free cycle
    // reuses the space instead of leaving a staircase of unusable gaps behind.
    for (bool merged = true; merged; ) {
        merged = false;
        for (size_t i = 0; i < blocks_.size(); ++i)
            if (i != f && !blocks_[i].used && blocks_[i].len &&
                blocks_[i].base == blocks_[f].base + blocks_[f].len) {
                blocks_[f].len += blocks_[i].len;
                blocks_[i].len = 0;                 // len 0 never satisfies a request
                merged = true;
            }
    }
    return true;
}

// A scratch real-mode stack for reflected interrupts, allocated on first use so a
// program that never reflects anything does not pay for it.
uint16_t Dpmi::scratch_stack_seg() {
    if (!scratch_ss_ && alloc_dos) scratch_ss_ = alloc_dos(0x20);   // 512 bytes
    return scratch_ss_;
}

bool Dpmi::int31() {
    const uint16_t fn = cpu_.r[AX];
    auto ok   = [&] { cpu_.set_flag(CF, false); };
    auto fail = [&](uint16_t code) { cpu_.r[AX] = code; cpu_.set_flag(CF, true); };

    switch (fn) {
        case 0x0000: {                                   // allocate LDT descriptors
            uint16_t sel = alloc_sel(cpu_.r[CX] ? cpu_.r[CX] : 1);
            if (!sel) { fail(0x8011); break; }
            for (int i = 0; i < (cpu_.r[CX] ? cpu_.r[CX] : 1); ++i)
                set_desc(static_cast<uint16_t>(sel + i * 8), 0, 0, false, false);
            cpu_.r[AX] = sel; ok(); break;
        }
        case 0x0001: ok(); break;                        // free descriptor: we never reuse
        case 0x0003: cpu_.r[AX] = 8; ok(); break;        // selector increment
        case 0x0006: {                                   // get segment base -> CX:DX
            uint32_t b = mem_.r16(desc_addr(cpu_.r[BX]) + 2)
                       | (static_cast<uint32_t>(mem_.r8(desc_addr(cpu_.r[BX]) + 4)) << 16)
                       | (static_cast<uint32_t>(mem_.r8(desc_addr(cpu_.r[BX]) + 7)) << 24);
            cpu_.r[CX] = static_cast<uint16_t>(b >> 16); cpu_.r[DX] = static_cast<uint16_t>(b); ok(); break;
        }
        case 0x0007: {                                   // set segment base = CX:DX
            uint32_t a = desc_addr(cpu_.r[BX]);
            uint32_t b = (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            mem_.w16(a + 2, static_cast<uint16_t>(b));
            mem_.w8(a + 4, static_cast<uint8_t>(b >> 16));
            mem_.w8(a + 7, static_cast<uint8_t>(b >> 24));
            ok(); break;
        }
        case 0x0008: {                                   // set segment limit = CX:DX
            uint32_t a = desc_addr(cpu_.r[BX]);
            uint32_t lim = (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            uint8_t g = mem_.r8(a + 6) & 0xC0;
            if (lim > 0xFFFFF) { lim >>= 12; g |= 0x80; }
            mem_.w16(a, static_cast<uint16_t>(lim));
            mem_.w8(a + 6, static_cast<uint8_t>(g | ((lim >> 16) & 0x0F)));
            ok(); break;
        }
        case 0x0009: {                                   // set access rights
            uint32_t a = desc_addr(cpu_.r[BX]);
            mem_.w8(a + 5, static_cast<uint8_t>(cpu_.r[CX] & 0xFF));
            mem_.w8(a + 6, static_cast<uint8_t>((mem_.r8(a + 6) & 0x0F) | (cpu_.r[CX] >> 8 & 0xF0)));
            ok(); break;
        }
        case 0x000A: {                                   // create a data alias for a selector
            uint16_t sel = alloc_sel();
            if (!sel) { fail(0x8011); break; }
            uint32_t a = desc_addr(cpu_.r[BX]), b = desc_addr(sel);
            for (int i = 0; i < 8; ++i) mem_.w8(b + i, mem_.r8(a + i));
            mem_.w8(b + 5, static_cast<uint8_t>(mem_.r8(b + 5) & ~0x08u));   // clear "code"
            cpu_.r[AX] = sel; ok(); break;
        }
        case 0x000B: {                                   // get descriptor -> ES:EDI
            uint32_t a = desc_addr(cpu_.r[BX]), d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 8; ++i) mem_.w8(d + i, mem_.r8(a + i));
            ok(); break;
        }
        case 0x000C: {                                   // set descriptor from ES:EDI
            uint32_t a = desc_addr(cpu_.r[BX]), d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 8; ++i) mem_.w8(a + i, mem_.r8(d + i));
            ok(); break;
        }
        case 0x0100: {                                   // allocate DOS memory block
            const uint16_t paras = cpu_.r[BX];
            const uint16_t seg = alloc_dos ? alloc_dos(paras) : 0;
            if (!seg) { cpu_.r[BX] = 0; fail(0x0008); break; }   // DOS error 8: out of memory
            const uint16_t sel = alloc_sel();
            set_desc(sel, static_cast<uint32_t>(seg) << 4, paras * 16u - 1, false, false);
            cpu_.r[AX] = seg; cpu_.r[DX] = sel; ok(); break;
        }
        case 0x0101: case 0x0102: ok(); break;           // free/resize DOS block: bump allocator
        // Interrupt and exception handler management. Nothing here raises an exception
        // or delivers a hardware interrupt yet, so these only have to remember what the
        // client installed and give it back. Refusing them is not neutral: DJGPP's crt0
        // reports "DPMI setup failed" and abandons its FPU-emulator setup.
        case 0x0200: {                                   // get real-mode interrupt vector
            const uint32_t v = mem_.r32((cpu_.r[BX] & 0xFF) * 4);
            cpu_.r[CX] = static_cast<uint16_t>(v >> 16); cpu_.r[DX] = static_cast<uint16_t>(v);
            ok(); break;
        }
        case 0x0201:                                     // set real-mode interrupt vector
            mem_.w32((cpu_.r[BX] & 0xFF) * 4,
                     (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX]);
            ok(); break;
        case 0x0202: case 0x0204: {                      // get exception / PM interrupt handler
            const int i = (fn == 0x0202) ? (cpu_.r[BX] & 0x1F) : (cpu_.r[BX] & 0xFF);
            // A client reads this to find the handler it should chain to. Reporting 0:0
            // for a vector nobody has hooked is not a neutral answer: DOS/4GW reads all
            // 256 of them at startup and saves them, and a chain through a null selector
            // is a jump to nowhere. Report our own default handler instead — it services
            // the interrupt and unwinds the frame, which is what chaining is for.
            if (fn == 0x0204 && !pmint_[i].sel) {
                cpu_.r[CX] = entry_sel();
                cpu_.sd(DX, static_cast<uint32_t>(kOffPmInt + i));
                ok(); break;
            }
            const Handler& h = (fn == 0x0202) ? exc_[i] : pmint_[i];
            cpu_.r[CX] = h.sel; cpu_.sd(DX, h.off); ok(); break;
        }
        case 0x0203: case 0x0205: {                      // set exception / PM interrupt handler
            const int i = (fn == 0x0203) ? (cpu_.r[BX] & 0x1F) : (cpu_.r[BX] & 0xFF);
            Handler& h = (fn == 0x0203) ? exc_[i] : pmint_[i];
            h.sel = cpu_.r[CX]; h.off = cpu_.gd(DX); ok(); break;
        }
        case 0x0900: case 0x0901:                        // disable/enable virtual interrupts
            cpu_.sb(AX, cpu_.get_flag(IF) ? 1 : 0);      // AL = previous state
            cpu_.set_flag(IF, fn == 0x0901); ok(); break;
        case 0x0902: cpu_.sb(AX, cpu_.get_flag(IF) ? 1 : 0); ok(); break;   // get state

        case 0x0300: simulate_real_int(); break;         // simulate real-mode interrupt
        case 0x0301: rm_call(false); break;             // call real-mode proc, RETF frame
        case 0x0302: rm_call(true);  break;             // ...and with an IRET frame
        case 0x0303: {                                   // allocate real-mode callback
            int i = 0;
            while (i < kCallbacks && cb_[i].used) ++i;
            if (i == kCallbacks) { fail(0x8015); break; }
            cb_[i] = {cpu_.sreg[DS], cpu_.gd(SI), cpu_.sreg[ES], cpu_.gd(DI), true};
            cpu_.r[CX] = kEntrySeg; cpu_.r[DX] = static_cast<uint16_t>(kOffCbBase + i);
            ok(); break;
        }
        case 0x0304: {                                   // free real-mode callback (CX:DX)
            const int i = static_cast<int>(cpu_.r[DX]) - kOffCbBase;
            if (cpu_.r[CX] != kEntrySeg || i < 0 || i >= kCallbacks) { fail(0x8024); break; }
            cb_[i].used = false; ok(); break;
        }
        case 0x0305:                                     // get save-state addresses
            // Size zero: there is no host state a client has to preserve across a raw
            // mode switch, because the switch preserves the whole CPU. The procedures
            // still exist and still return properly, for a client that calls them anyway.
            cpu_.r[AX] = 0;
            cpu_.r[BX] = kEntrySeg; cpu_.r[CX] = kOffSaveRm;
            cpu_.r[SI] = entry_sel(); cpu_.sd(DI, kOffSavePm);
            ok(); break;
        case 0x0306:                                     // get raw mode-switch addresses
            cpu_.r[BX] = kEntrySeg; cpu_.r[CX] = kOffRawToPm;
            cpu_.r[SI] = entry_sel(); cpu_.sd(DI, kOffRawToRm);
            ok(); break;
        case 0x0400:                                     // get DPMI version
            cpu_.r[AX] = 0x005A;                         // 0.90
            cpu_.r[BX] = 0x0005;                         // no V86, no virtual memory
            cpu_.sb(2 /*CL*/, 3);                        // 80386
            cpu_.r[DX] = 0xFFFF;                         // master/slave PIC base
            ok(); break;
        case 0x0500: {                                   // free memory information
            uint32_t d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 0x30; ++i) mem_.w8(d + i, 0xFF);
            const uint32_t free = Memory::kSize - heap_next_;
            mem_.w32(d + 0x00, free);                    // largest free block
            mem_.w32(d + 0x04, free >> 12);              // in pages
            ok(); break;
        }
        case 0x0501: {                                   // allocate memory block
            const uint32_t len = (static_cast<uint32_t>(cpu_.r[BX]) << 16) | cpu_.r[CX];
            const uint32_t base = alloc_mem(len);
            if (!base) { fail(0x8013); break; }
            cpu_.r[BX] = static_cast<uint16_t>(base >> 16); cpu_.r[CX] = static_cast<uint16_t>(base);
            cpu_.r[SI] = static_cast<uint16_t>(base >> 16); cpu_.r[DI] = static_cast<uint16_t>(base);
            ok(); break;                                 // handle == address
        }
        case 0x0502:                                     // free memory block (SI:DI = handle)
            free_mem((static_cast<uint32_t>(cpu_.r[SI]) << 16) | cpu_.r[DI]);
            ok(); break;
        case 0x0600: case 0x0601: case 0x0602: case 0x0603:
            ok(); break;                                 // lock/unlock: no paging here
        case 0x0604: cpu_.r[BX] = 0; cpu_.r[CX] = 0x1000; ok(); break;   // page size
        default:
            fail(0x8001);                                // unsupported function
            break;
    }

    if (trace)
        std::fprintf(stderr, "[dpmi]%llu int31 AX=%04X BX=%04X CX=%04X DX=%04X -> %s AX=%04X\n",
               (unsigned long long)cpu_.insns, fn, cpu_.r[BX], cpu_.r[CX], cpu_.r[DX],
               cpu_.get_flag(CF) ? "FAIL" : "ok", cpu_.r[AX]);
    return true;
}

}  // namespace dosemu
