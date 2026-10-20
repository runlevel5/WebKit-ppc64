/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(ASSEMBLER) && CPU(PPC64)

#include <JavaScriptCore/AssemblerBuffer.h>
#include <JavaScriptCore/AssemblerCommon.h>
#include <JavaScriptCore/PPC64Registers.h>
#include <algorithm>
#if OS(LINUX)
#include <sys/auxv.h>
#endif

// Power ISA 2.07 (POWER8) is the baseline. Instructions from later ISA
// versions are marked as such and emitted only behind runtime checks.

namespace JSC {

namespace PPC64Registers {

typedef enum : int8_t {
#define REGISTER_ID(id, name, r, cs) id,
    FOR_EACH_GP_REGISTER(REGISTER_ID)
#undef REGISTER_ID

#define REGISTER_ALIAS(id, name, alias) id = alias,
    FOR_EACH_REGISTER_ALIAS(REGISTER_ALIAS)
#undef REGISTER_ALIAS

    InvalidGPRReg = -1,
} RegisterID;

typedef enum : int8_t {
#define REGISTER_ID(id, name) id,
    FOR_EACH_SP_REGISTER(REGISTER_ID)
#undef REGISTER_ID

    InvalidSPReg = -1,
} SPRegisterID;

typedef enum : int8_t {
#define REGISTER_ID(id, name, r, cs) id,
    FOR_EACH_FP_REGISTER(REGISTER_ID)
#undef REGISTER_ID

    InvalidFPRReg = -1,
} FPRegisterID;

// VMX vector registers v0-v31. A separate enum from FPRegisterID: both live
// in the VSR file (v0..v31 = VSR32..VSR63, f0..f31 = doubleword 0 of
// VSR0..VSR31), but they are encoded differently and have different
// callee-save rules.
typedef enum : int8_t {
#define REGISTER_ID(id, name, r, cs) id,
    FOR_EACH_VR_REGISTER(REGISTER_ID)
#undef REGISTER_ID

    InvalidVRReg = -1,
} VRegisterID;

} // namespace PPC64Registers

class PPC64Assembler {
public:
    using RegisterID = PPC64Registers::RegisterID;
    using SPRegisterID = PPC64Registers::SPRegisterID;
    using FPRegisterID = PPC64Registers::FPRegisterID;
    using VRegisterID = PPC64Registers::VRegisterID;

    static constexpr RegisterID firstRegister() { return PPC64Registers::r0; }
    static constexpr RegisterID lastRegister() { return PPC64Registers::r31; }
    static constexpr unsigned numberOfRegisters() { return lastRegister() - firstRegister() + 1; }

    static constexpr SPRegisterID firstSPRegister() { return PPC64Registers::pc; }
    static constexpr SPRegisterID lastSPRegister() { return PPC64Registers::ctr; }
    static constexpr unsigned numberOfSPRegisters() { return lastSPRegister() - firstSPRegister() + 1; }

    static constexpr FPRegisterID firstFPRegister() { return PPC64Registers::f0; }
    static constexpr FPRegisterID lastFPRegister() { return PPC64Registers::f31; }
    static constexpr unsigned numberOfFPRegisters() { return lastFPRegister() - firstFPRegister() + 1; }

    static constexpr VRegisterID firstVRegister() { return PPC64Registers::v0; }
    static constexpr VRegisterID lastVRegister() { return PPC64Registers::v31; }
    static constexpr unsigned numberOfVRegisters() { return lastVRegister() - firstVRegister() + 1; }

    static ASCIILiteral gprName(RegisterID id)
    {
        ASSERT(id >= firstRegister() && id <= lastRegister());
        static constexpr ASCIILiteral nameForRegister[numberOfRegisters()] = {
#define REGISTER_NAME(id, name, r, cs) name,
            FOR_EACH_GP_REGISTER(REGISTER_NAME)
#undef REGISTER_NAME
        };
        return nameForRegister[id];
    }

    static ASCIILiteral sprName(SPRegisterID id)
    {
        ASSERT(id >= firstSPRegister() && id <= lastSPRegister());
        static constexpr ASCIILiteral nameForRegister[numberOfSPRegisters()] = {
#define REGISTER_NAME(id, name) name,
            FOR_EACH_SP_REGISTER(REGISTER_NAME)
#undef REGISTER_NAME
        };
        return nameForRegister[id];
    }

    static ASCIILiteral fprName(FPRegisterID id)
    {
        ASSERT(id >= firstFPRegister() && id <= lastFPRegister());
        static constexpr ASCIILiteral nameForRegister[numberOfFPRegisters()] = {
#define REGISTER_NAME(id, name, r, cs) name,
            FOR_EACH_FP_REGISTER(REGISTER_NAME)
#undef REGISTER_NAME
        };
        return nameForRegister[id];
    }

    static ASCIILiteral vrName(VRegisterID id)
    {
        ASSERT(id >= firstVRegister() && id <= lastVRegister());
        static constexpr ASCIILiteral nameForRegister[numberOfVRegisters()] = {
#define REGISTER_NAME(id, name, r, cs) name,
            FOR_EACH_VR_REGISTER(REGISTER_NAME)
#undef REGISTER_NAME
        };
        return nameForRegister[id];
    }

    PPC64Assembler() = default;

    AssemblerBuffer& buffer() LIFETIME_BOUND { return m_buffer; }

    // A watchpoint label may later be overwritten in place by replaceWithJump,
    // which needs maxJumpReplacementSize() bytes. As on ARM64 and x86_64, any
    // label taken inside that shadow is pushed past it with nops, so nothing
    // another path can still branch to is overwritten. Jump and branch slots
    // started inside the shadow are emitted at fixed size, so branch
    // compaction cannot pull a later label back into it either.
    AssemblerLabel label()
    {
        AssemblerLabel result = m_buffer.label();
        while (static_cast<int>(result.offset()) < m_indexOfTailOfLastWatchpoint) [[unlikely]] {
            insn(PPC_NOP);
            result = m_buffer.label();
        }
        return result;
    }

    AssemblerLabel labelIgnoringWatchpoints()
    {
        return m_buffer.label();
    }

    AssemblerLabel labelForWatchpoint()
    {
        AssemblerLabel result = m_buffer.label();
        if (static_cast<int>(result.offset()) != m_indexOfLastWatchpoint)
            result = label();
        m_indexOfLastWatchpoint = result.offset();
        m_indexOfTailOfLastWatchpoint = result.offset() + maxJumpReplacementSize();
        return result;
    }

    unsigned debugOffset() { return m_buffer.codeSize(); }

    size_t codeSize() const { return m_buffer.codeSize(); }

    // Integer arithmetic and logical.

    void ori(RegisterID ra, RegisterID rs, uint16_t ui)
    {
        insn(dForm(24, rs, ra, ui));
    }

    // andi. always sets CR0.
    void andi_(RegisterID ra, RegisterID rs, uint16_t ui)
    {
        insn(dForm(28, rs, ra, ui));
    }

    void xori(RegisterID ra, RegisterID rs, uint16_t ui)
    {
        insn(dForm(26, rs, ra, ui));
    }

    // not RA, RS is nor RA, RS, RS.
    void nor(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 124, /*Rc*/ 0));
    }

    // RA = RS & ~RB.
    void andc(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 60, /*Rc*/ 0));
    }

    // Carrying adds and subtracts (XER.CA). adde: RT = RA + RB + CA;
    // subfc: RT = RB - RA with CA = no borrow; subfe: RT = ~RA + RB + CA.
    void addc(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 10, /*Rc*/ 0));
    }
    void adde(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 138, /*Rc*/ 0));
    }
    void subfc(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 8, /*Rc*/ 0));
    }
    void subfe(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 136, /*Rc*/ 0));
    }
    void addze(RegisterID rt, RegisterID ra)
    {
        insn(xoForm(31, rt, ra, PPC64Registers::r0, /*OE*/ 0, /*XO*/ 202, /*Rc*/ 0));
    }
    void addic(RegisterID rt, RegisterID ra, int16_t si)
    {
        insn(dForm(12, rt, ra, uint16_t(si)));
    }

    // The full-CR form (bit 11 clear), not mfocrf.
    void mfcr(RegisterID rt)
    {
        insn((31u << 26) | (uint32_t(registerValue(rt)) << 21) | (19u << 1));
    }

    // RT = CR[bc] ? (RA == 0 ? 0 : GPR[RA]) : GPR[RB].
    void isel(RegisterID rt, RegisterID ra, RegisterID rb, unsigned bc)
    {
        insn((31u << 26) | (uint32_t(registerValue(rt)) << 21) | (uint32_t(registerValue(ra)) << 16)
            | (uint32_t(registerValue(rb)) << 11) | ((bc & 31u) << 6) | (15u << 1));
    }

    // RA = RS | (UI << 16). With ori, fills the low word without touching the
    // high word, as in the 5-instruction 64-bit immediate load.
    void oris(RegisterID ra, RegisterID rs, uint16_t ui)
    {
        insn(dForm(25, rs, ra, ui));
    }

    // Multiply. mullw leaves the high word of RT undefined; mulh* give the
    // high half of the double-width product.
    void mullw(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 235, /*Rc*/ 0));
    }

    void mulld(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 233, /*Rc*/ 0));
    }

    void mulhw(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 75, /*Rc*/ 0));
    }

    void mulhwu(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 11, /*Rc*/ 0));
    }

    void mulhd(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 73, /*Rc*/ 0));
    }

    void mulhdu(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 9, /*Rc*/ 0));
    }

    void mulli(RegisterID rt, RegisterID ra, int16_t si)
    {
        insn(dForm(7, rt, ra, static_cast<uint16_t>(si)));
    }

    // Divide. The result is undefined for x / 0 and INT_MIN / -1 (no trap);
    // callers check for those.
    void divw(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 491, /*Rc*/ 0));
    }

    void divwu(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 459, /*Rc*/ 0));
    }

    void divd(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 489, /*Rc*/ 0));
    }

    void divdu(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 457, /*Rc*/ 0));
    }

    // RT = RB - RA: note the reversed operands. `sub RT, RA, RB` is
    // `subf RT, RB, RA`.
    void subf(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 40, /*Rc*/ 0));
    }

    void neg(RegisterID rt, RegisterID ra)
    {
        insn(xoForm(31, rt, ra, PPC64Registers::r0, /*OE*/ 0, /*XO*/ 104, /*Rc*/ 0));
    }

    // RT = (RA == 0 ? 0 : RA) + (SI << 16). With RA = 0 this is lis.
    void addis(RegisterID rt, RegisterID ra, int16_t si)
    {
        insn(dForm(15, rt, ra, static_cast<uint16_t>(si)));
    }

    void lis(RegisterID rt, int16_t si)
    {
        addis(rt, PPC64Registers::r0, si);
    }

    void add(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xoForm(31, rt, ra, rb, /*OE*/ 0, /*XO*/ 266, /*Rc*/ 0));
    }

    // RT = (RA == 0 ? 0 : RA) + SI. RA = 0 means a literal zero, not r0, which
    // is why r0 is never used as a base register; with RA = 0 this is li.
    void addi(RegisterID rt, RegisterID ra, int16_t si)
    {
        insn(dForm(14, rt, ra, static_cast<uint16_t>(si)));
    }

    // D-form loads and stores: any 16-bit signed displacement (unlike the
    // DS-form ld/std/lwa, which need a multiple of 4).
    void lwz(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(32, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stw(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(36, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lbz(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(34, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stb(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(38, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lhz(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(40, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lha(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(42, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void sth(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(44, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    // Sign-extend a byte, halfword or word to 64 bits.
    void extsb(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 954, /*Rc*/ 0));
    }

    void extsh(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 922, /*Rc*/ 0));
    }

    void extsw(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 986, /*Rc*/ 0));
    }

    // Floating-point loads and stores. lfs widens to double format on load;
    // stfs rounds to single on store.
    void lfd(FPRegisterID frt, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(50, frt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stfd(FPRegisterID frs, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(54, frs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lfs(FPRegisterID frt, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(48, frt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stfs(FPRegisterID frs, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(52, frs, ra, static_cast<uint16_t>(byteOffset)));
    }

    // Floating-point arithmetic (A-form; opcode 63 double, 59 single). The
    // multiplier of fmul/fmuls is in the FRC slot, not FRB.

    void fadd(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(63, frt, fra, frb, PPC64Registers::f0, /*XO*/ 21, /*Rc*/ 0));
    }

    void fsub(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(63, frt, fra, frb, PPC64Registers::f0, /*XO*/ 20, /*Rc*/ 0));
    }

    void fmul(FPRegisterID frt, FPRegisterID fra, FPRegisterID frc)
    {
        insn(aForm(63, frt, fra, PPC64Registers::f0, frc, /*XO*/ 25, /*Rc*/ 0));
    }

    void fdiv(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(63, frt, fra, frb, PPC64Registers::f0, /*XO*/ 18, /*Rc*/ 0));
    }

    void fadds(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(59, frt, fra, frb, PPC64Registers::f0, /*XO*/ 21, /*Rc*/ 0));
    }

    void fsubs(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(59, frt, fra, frb, PPC64Registers::f0, /*XO*/ 20, /*Rc*/ 0));
    }

    void fmuls(FPRegisterID frt, FPRegisterID fra, FPRegisterID frc)
    {
        insn(aForm(59, frt, fra, PPC64Registers::f0, frc, /*XO*/ 25, /*Rc*/ 0));
    }

    void fdivs(FPRegisterID frt, FPRegisterID fra, FPRegisterID frb)
    {
        insn(aForm(59, frt, fra, frb, PPC64Registers::f0, /*XO*/ 18, /*Rc*/ 0));
    }

    // FRT = FRA * FRC + FRB, fused.
    void fmadd(FPRegisterID frt, FPRegisterID fra, FPRegisterID frc, FPRegisterID frb)
    {
        insn(aForm(63, frt, fra, frb, frc, /*XO*/ 29, /*Rc*/ 0));
    }

    void fneg(FPRegisterID frt, FPRegisterID frb)
    {
        insn(xFormFp(63, frt, PPC64Registers::f0, frb, /*XO*/ 40, /*Rc*/ 0));
    }

    void fabs(FPRegisterID frt, FPRegisterID frb)
    {
        insn(xFormFp(63, frt, PPC64Registers::f0, frb, /*XO*/ 264, /*Rc*/ 0));
    }

    void fmr(FPRegisterID frt, FPRegisterID frb)
    {
        insn(xFormFp(63, frt, PPC64Registers::f0, frb, /*XO*/ 72, /*Rc*/ 0));
    }

    // Floating-point compares set CR field BF to LT/GT/EQ/UN. fcmpo also
    // raises invalid-operation on quiet NaNs; fcmpu only on signalling ones.
    void fcmpu(uint32_t bf, FPRegisterID fra, FPRegisterID frb)
    {
        insn(fpCmpXForm(63, bf, fra, frb, /*XO*/ 0));
    }

    void fcmpo(uint32_t bf, FPRegisterID fra, FPRegisterID frb)
    {
        insn(fpCmpXForm(63, bf, fra, frb, /*XO*/ 32));
    }

    // Floating-point <-> integer conversions. fct*z truncate; the others round
    // per FPSCR[RN]. The single-precision forms are opcode 59.
    void fcfids(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(59, frt, PPC64Registers::f0, frb, 846, 0)); }
    void fcfidus(FPRegisterID frt, FPRegisterID frb) { insn(xFormFp(59, frt, PPC64Registers::f0, frb, 974, 0)); }
    void frsp(FPRegisterID frt, FPRegisterID frb)    { insn(xFormFp(63, frt, PPC64Registers::f0, frb,  12, 0)); }

    void fsqrt(FPRegisterID frt, FPRegisterID frb)   { insn(aForm(63, frt, PPC64Registers::f0, frb, PPC64Registers::f0, 22, 0)); }
    void fsqrts(FPRegisterID frt, FPRegisterID frb)  { insn(aForm(59, frt, PPC64Registers::f0, frb, PPC64Registers::f0, 22, 0)); }

    // Round to integral: frin nearest (ties away from zero), friz toward zero,
    // frip ceiling, frim floor.
    void frin(FPRegisterID frt, FPRegisterID frb)    { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 392, 0)); }
    // xsrdpic rounds per the current mode, ties-to-even by default (as ARM64
    // frintn). f0-f31 are vs0-vs31, so TX = BX = 0.
    void xsrdpic(FPRegisterID frt, FPRegisterID frb) { insn(0xF00001ACu | (uint32_t(frt) & 31) << 21 | (uint32_t(frb) & 31) << 11); }
    void friz(FPRegisterID frt, FPRegisterID frb)    { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 424, 0)); }
    void frip(FPRegisterID frt, FPRegisterID frb)    { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 456, 0)); }
    void frim(FPRegisterID frt, FPRegisterID frb)    { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 488, 0)); }

    // GPR <-> FPR moves (FPR fN is VSR N, so TX/SX = 0).
    void mtvsrd(FPRegisterID frt, RegisterID ra)  { insn((31u << 26) | (uint32_t(fprValue(frt)) << 21) | (uint32_t(registerValue(ra)) << 16) | (179u << 1)); }
    void mtvsrwa(FPRegisterID frt, RegisterID ra) { insn((31u << 26) | (uint32_t(fprValue(frt)) << 21) | (uint32_t(registerValue(ra)) << 16) | (211u << 1)); }
    void mtvsrwz(FPRegisterID frt, RegisterID ra) { insn((31u << 26) | (uint32_t(fprValue(frt)) << 21) | (uint32_t(registerValue(ra)) << 16) | (243u << 1)); }
    void mfvsrd(RegisterID ra, FPRegisterID frs)  { insn((31u << 26) | (uint32_t(fprValue(frs)) << 21) | (uint32_t(registerValue(ra)) << 16) | (51u << 1)); }
    void mfvsrwz(RegisterID ra, FPRegisterID frs) { insn((31u << 26) | (uint32_t(fprValue(frs)) << 21) | (uint32_t(registerValue(ra)) << 16) | (115u << 1)); }

    void fcfid(FPRegisterID frt, FPRegisterID frb)   { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 846, 0)); }
    void fctid(FPRegisterID frt, FPRegisterID frb)   { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 814, 0)); }
    void fctidz(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 815, 0)); }
    void fctiw(FPRegisterID frt, FPRegisterID frb)   { insn(xFormFp(63, frt, PPC64Registers::f0, frb,  14, 0)); }
    void fctiwz(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(63, frt, PPC64Registers::f0, frb,  15, 0)); }
    void fcfidu(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 974, 0)); }
    void fctidu(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 942, 0)); }
    void fctiduz(FPRegisterID frt, FPRegisterID frb) { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 943, 0)); }
    void fctiwu(FPRegisterID frt, FPRegisterID frb)  { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 142, 0)); }
    void fctiwuz(FPRegisterID frt, FPRegisterID frb) { insn(xFormFp(63, frt, PPC64Registers::f0, frb, 143, 0)); }

    // Indexed loads and stores: EA = (RA == 0 ? 0 : RA) + RB.
    void ldx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 21, /*Rc*/ 0));
    }

    // Byte-reversed loads and stores (X-form, EA = (RA|0) + RB).
    void lhbrx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 790, /*Rc*/ 0));
    }
    void lwbrx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 534, /*Rc*/ 0));
    }
    void ldbrx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 532, /*Rc*/ 0));
    }
    void sthbrx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 918, /*Rc*/ 0));
    }
    void stwbrx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 662, /*Rc*/ 0));
    }
    void stdbrx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 660, /*Rc*/ 0));
    }

    void stdx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 149, /*Rc*/ 0));
    }

    void lwzx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 23, /*Rc*/ 0));
    }

    void stwx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 151, /*Rc*/ 0));
    }

    void lbzx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 87, /*Rc*/ 0));
    }

    void stbx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 215, /*Rc*/ 0));
    }

    void lhzx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 279, /*Rc*/ 0));
    }

    void lhax(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 343, /*Rc*/ 0));
    }

    void sthx(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 407, /*Rc*/ 0));
    }

    // Loads and stores with update: after the access, RA = EA. RA = 0, and
    // RA = RT for loads, are invalid forms.
    void ldu(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dsForm(58, rt, ra, byteOffset, /*XO*/ 1));
    }

    void stdu(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dsForm(62, rs, ra, byteOffset, /*XO*/ 1));
    }

    void lwzu(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(33, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stwu(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(37, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lbzu(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(35, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stbu(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(39, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lhzu(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(41, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lhau(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(43, rt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void sthu(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dForm(45, rs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void ldux(RegisterID rt, RegisterID ra, RegisterID rb)   { insn(xForm(31, rt, ra, rb,  53, 0)); }
    void stdux(RegisterID rs, RegisterID ra, RegisterID rb)  { insn(xForm(31, rs, ra, rb, 181, 0)); }
    void lwzux(RegisterID rt, RegisterID ra, RegisterID rb)  { insn(xForm(31, rt, ra, rb,  55, 0)); }
    void stwux(RegisterID rs, RegisterID ra, RegisterID rb)  { insn(xForm(31, rs, ra, rb, 183, 0)); }
    void lbzux(RegisterID rt, RegisterID ra, RegisterID rb)  { insn(xForm(31, rt, ra, rb, 119, 0)); }
    void stbux(RegisterID rs, RegisterID ra, RegisterID rb)  { insn(xForm(31, rs, ra, rb, 247, 0)); }
    void lhzux(RegisterID rt, RegisterID ra, RegisterID rb)  { insn(xForm(31, rt, ra, rb, 311, 0)); }
    void lhaux(RegisterID rt, RegisterID ra, RegisterID rb)  { insn(xForm(31, rt, ra, rb, 375, 0)); }
    void sthux(RegisterID rs, RegisterID ra, RegisterID rb)  { insn(xForm(31, rs, ra, rb, 439, 0)); }

    // Floating-point indexed and update forms.
    void lfdx(FPRegisterID frt, RegisterID ra, RegisterID rb)   { insn(xFormFpMem(31, frt, ra, rb, 599)); }
    void stfdx(FPRegisterID frs, RegisterID ra, RegisterID rb)  { insn(xFormFpMem(31, frs, ra, rb, 727)); }
    void lfsx(FPRegisterID frt, RegisterID ra, RegisterID rb)   { insn(xFormFpMem(31, frt, ra, rb, 535)); }
    void stfsx(FPRegisterID frs, RegisterID ra, RegisterID rb)  { insn(xFormFpMem(31, frs, ra, rb, 663)); }
    void lfdux(FPRegisterID frt, RegisterID ra, RegisterID rb)  { insn(xFormFpMem(31, frt, ra, rb, 631)); }
    void stfdux(FPRegisterID frs, RegisterID ra, RegisterID rb) { insn(xFormFpMem(31, frs, ra, rb, 759)); }
    void lfsux(FPRegisterID frt, RegisterID ra, RegisterID rb)  { insn(xFormFpMem(31, frt, ra, rb, 567)); }
    void stfsux(FPRegisterID frs, RegisterID ra, RegisterID rb) { insn(xFormFpMem(31, frs, ra, rb, 695)); }

    void lfdu(FPRegisterID frt, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(51, frt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stfdu(FPRegisterID frs, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(55, frs, ra, static_cast<uint16_t>(byteOffset)));
    }

    void lfsu(FPRegisterID frt, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(49, frt, ra, static_cast<uint16_t>(byteOffset)));
    }

    void stfsu(FPRegisterID frs, int16_t byteOffset, RegisterID ra)
    {
        insn(dFormFp(53, frs, ra, static_cast<uint16_t>(byteOffset)));
    }

    // DS-form: the byte offset must be a multiple of 4 (its low 2 bits hold XO).
    void ld(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dsForm(58, rt, ra, byteOffset, /*XO*/ 0));
    }

    // Load word, sign-extended.
    void lwa(RegisterID rt, int16_t byteOffset, RegisterID ra)
    {
        insn(dsForm(58, rt, ra, byteOffset, /*XO*/ 2));
    }

    void std(RegisterID rs, int16_t byteOffset, RegisterID ra)
    {
        insn(dsForm(62, rs, ra, byteOffset, /*XO*/ 0));
    }

    // Special-purpose register numbers for mfspr/mtspr.
    static constexpr uint32_t SPR_XER = 1;
    static constexpr uint32_t SPR_LR  = 8;
    static constexpr uint32_t SPR_CTR = 9;

    // The 10-bit SPR field holds the SPR number with its two 5-bit halves
    // swapped (xfxForm does the swap).
    void mfspr(RegisterID rt, uint32_t sprNum)
    {
        insn(xfxForm(31, rt, sprNum, /*XO*/ 339));
    }

    void mtspr(uint32_t sprNum, RegisterID rs)
    {
        insn(xfxForm(31, rs, sprNum, /*XO*/ 467));
    }

    void mflr(RegisterID rt)  { mfspr(rt, SPR_LR); }
    void mtlr(RegisterID rs)  { mtspr(SPR_LR, rs); }
    void mfctr(RegisterID rt) { mfspr(rt, SPR_CTR); }
    void mtctr(RegisterID rs) { mtspr(SPR_CTR, rs); }

    // Logical instructions. `and`, `or` and `xor` are C++ alternative tokens,
    // hence the trailing underscores. In these X-forms the destination RA is
    // the second register field, not the first as in arithmetic XO-forms.
    void and_(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 28, /*Rc*/ 0));
    }

    // and.: also sets CR0 from a signed compare of the result with 0.
    void and_rc(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 28, /*Rc*/ 1));
    }

    void or_(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 444, /*Rc*/ 0));
    }

    void xor_(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 316, /*Rc*/ 0));
    }

    void mr(RegisterID ra, RegisterID rs)
    {
        or_(ra, rs, rs);
    }

    // Shifts by register. The count is the low 6 (32-bit) or 7 (64-bit) bits
    // of RB, and counts >= the width give 0 or all sign bits rather than
    // wrapping, so callers wanting modulo semantics mask RB first.
    void slw(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 24, /*Rc*/ 0));
    }

    void srw(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 536, /*Rc*/ 0));
    }

    void sraw(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 792, /*Rc*/ 0));
    }

    void sld(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 27, /*Rc*/ 0));
    }

    void srd(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 539, /*Rc*/ 0));
    }

    void srad(RegisterID ra, RegisterID rs, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 794, /*Rc*/ 0));
    }

    // 64-bit rotate-and-mask (MD-form): rotate left by SH, then clear left of
    // MB (rldicl), right of ME (rldicr), both (rldic), or insert (rldimi).

    void rldicl(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t mb)
    {
        insn(mdForm(30, rs, ra, sh, mb, /*XO*/ 0, /*Rc*/ 0));
    }

    void rldicr(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t me)
    {
        insn(mdForm(30, rs, ra, sh, me, /*XO*/ 1, /*Rc*/ 0));
    }

    // Rotate left by the low 6 bits of RB, then clear the high MB bits
    // (rotld is MB = 0). MDS-form: no SH field and a 4-bit XO; MB is split as
    // in MD-form.
    void rldcl(RegisterID ra, RegisterID rs, RegisterID rb, uint32_t mb)
    {
        ASSERT(mb < 64);
        uint32_t mbField = ((mb & 0x1F) << 1) | ((mb >> 5) & 1);
        insn((30u << 26)
            | (registerValue(rs) << 21)
            | (registerValue(ra) << 16)
            | (registerValue(rb) << 11)
            | (mbField << 5)
            | (8u << 1));
    }

    void rldic(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t mb)
    {
        insn(mdForm(30, rs, ra, sh, mb, /*XO*/ 2, /*Rc*/ 0));
    }

    void rldimi(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t mb)
    {
        insn(mdForm(30, rs, ra, sh, mb, /*XO*/ 3, /*Rc*/ 0));
    }

    // Simplified mnemonics.
    void sldi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 64);
        rldicr(ra, rs, n, 63 - n);
    }

    void srdi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 64);
        rldicl(ra, rs, (64 - n) % 64, n);
    }

    void clrldi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 64);
        rldicl(ra, rs, 0, n);
    }

    void clrrdi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 64);
        rldicr(ra, rs, 0, 63 - n);
    }

    // 32-bit rotate-and-mask (M-form). MB and ME are plain 5-bit fields, unlike
    // MD-form's split 6-bit ones. When MB <= ME the mask lies within the low
    // word, so the high word of the result is zero.
    void rlwinm(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t mb, uint32_t me)
    {
        insn(mFormImm(21, rs, ra, sh, mb, me, /*Rc*/ 0));
    }

    void rlwimi(RegisterID ra, RegisterID rs, uint32_t sh, uint32_t mb, uint32_t me)
    {
        insn(mFormImm(20, rs, ra, sh, mb, me, /*Rc*/ 0));
    }

    void rlwnm(RegisterID ra, RegisterID rs, RegisterID rb, uint32_t mb, uint32_t me)
    {
        insn(mFormReg(23, rs, ra, rb, mb, me, /*Rc*/ 0));
    }

    // 32-bit simplified mnemonics.
    void slwi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 32);
        rlwinm(ra, rs, n, 0, 31 - n);
    }

    void srwi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 32);
        rlwinm(ra, rs, (32 - n) % 32, n, 31);
    }

    void clrlwi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 32);
        rlwinm(ra, rs, 0, n, 31);
    }

    void rotlwi(RegisterID ra, RegisterID rs, uint32_t n)
    {
        ASSERT(n < 32);
        rlwinm(ra, rs, n, 0, 31);
    }

    // Arithmetic right shifts by immediate (both set XER[CA]). sradi's 6-bit
    // SH is split: low 5 bits in the RB field, the high bit at bit 30.
    void srawi(RegisterID ra, RegisterID rs, uint32_t sh)
    {
        ASSERT(sh < 32);
        insn((31u << 26)
           | (registerValue(rs) << 21)
           | (registerValue(ra) << 16)
           | (sh << 11)
           | (824u << 1));
    }

    void sradi(RegisterID ra, RegisterID rs, uint32_t sh)
    {
        ASSERT(sh < 64);
        insn(xsForm(31, rs, ra, sh, /*XO*/ 413, /*Rc*/ 0));
    }

    // Count leading zeros and population count. popcntb and popcntw count each
    // byte or word separately.
    void cntlzw(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 26, /*Rc*/ 0));
    }

    void cntlzd(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 58, /*Rc*/ 0));
    }

    void popcntw(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 378, /*Rc*/ 0));
    }

    void popcntd(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 506, /*Rc*/ 0));
    }

    void popcntb(RegisterID ra, RegisterID rs)
    {
        insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 122, /*Rc*/ 0));
    }

    // RT = SI - RA; sets XER[CA].
    void subfic(RegisterID rt, RegisterID ra, int16_t si)
    {
        insn(dForm(8, rt, ra, static_cast<uint16_t>(si)));
    }

    // Compares set CR field BF. The first register field holds BF(3), a
    // reserved bit and L, which selects a 32-bit (0) or 64-bit (1) compare.
    // The logical (cmpl*) forms are unsigned.
    void cmpd(uint32_t bf, RegisterID ra, RegisterID rb)
    {
        insn(cmpXForm(31, bf, /*L*/ 1, ra, rb, /*XO*/ 0));
    }

    void cmpw(uint32_t bf, RegisterID ra, RegisterID rb)
    {
        insn(cmpXForm(31, bf, /*L*/ 0, ra, rb, /*XO*/ 0));
    }

    void cmpld(uint32_t bf, RegisterID ra, RegisterID rb)
    {
        insn(cmpXForm(31, bf, /*L*/ 1, ra, rb, /*XO*/ 32));
    }

    void cmplw(uint32_t bf, RegisterID ra, RegisterID rb)
    {
        insn(cmpXForm(31, bf, /*L*/ 0, ra, rb, /*XO*/ 32));
    }

    void cmpdi(uint32_t bf, RegisterID ra, int16_t si)
    {
        insn(cmpDForm(11, bf, /*L*/ 1, ra, static_cast<uint16_t>(si)));
    }

    void cmpwi(uint32_t bf, RegisterID ra, int16_t si)
    {
        insn(cmpDForm(11, bf, /*L*/ 0, ra, static_cast<uint16_t>(si)));
    }

    void cmpldi(uint32_t bf, RegisterID ra, uint16_t ui)
    {
        insn(cmpDForm(10, bf, /*L*/ 1, ra, ui));
    }

    void cmplwi(uint32_t bf, RegisterID ra, uint16_t ui)
    {
        insn(cmpDForm(10, bf, /*L*/ 0, ra, ui));
    }

    // Branch BO values (no hints).
    static constexpr uint32_t BO_ALWAYS        = 20;
    static constexpr uint32_t BO_IF_TRUE       = 12; // CR[BI] = 1
    static constexpr uint32_t BO_IF_FALSE      =  4; // CR[BI] = 0

    // Bits within a CR field; BI = 4 * field + bit.
    static constexpr uint32_t CR_LT = 0;
    static constexpr uint32_t CR_GT = 1;
    static constexpr uint32_t CR_EQ = 2;
    static constexpr uint32_t CR_SO = 3;

    // Branches. b/bl reach +/-32 MB, bc +/-32 KB.
    void b(int32_t byteOffset)
    {
        insn(iForm(18, byteOffset, /*AA*/ 0, /*LK*/ 0));
    }

    void bl(int32_t byteOffset)
    {
        insn(iForm(18, byteOffset, /*AA*/ 0, /*LK*/ 1));
    }

    void bc(uint32_t bo, uint32_t bi, int32_t byteOffset)
    {
        insn(bForm(16, bo, bi, byteOffset, /*AA*/ 0, /*LK*/ 0));
    }

    // CR bit logic: CR[BT] = CR[BA] op CR[BB].
    void cror(uint32_t bt, uint32_t ba, uint32_t bb)
    {
        insn((19u << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (449u << 1));
    }
    void crandc(uint32_t bt, uint32_t ba, uint32_t bb)
    {
        insn((19u << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (129u << 1));
    }

    // Branch to LR / CTR. BH is a prediction hint; LK = 1 also sets LR.
    void bclr(uint32_t bo, uint32_t bi, uint32_t bh = 0, uint32_t lk = 0)
    {
        insn(xlForm(19, bo, bi, bh, /*XO*/ 16, lk));
    }

    void bcctr(uint32_t bo, uint32_t bi, uint32_t bh = 0, uint32_t lk = 0)
    {
        insn(xlForm(19, bo, bi, bh, /*XO*/ 528, lk));
    }

    void blr()   { bclr(BO_ALWAYS, 0, 0, 0); }
    void blrl()  { bclr(BO_ALWAYS, 0, 0, 1); }
    void bctr()  { bcctr(BO_ALWAYS, 0, 0, 0); }
    void bctrl() { bcctr(BO_ALWAYS, 0, 0, 1); }

    // The preferred nop, ori 0,0,0.
    void nop()
    {
        ori(PPC64Registers::r0, PPC64Registers::r0, 0);
    }

    // Trap if any condition in the TO mask holds (LT 16, GT 8, EQ 4, unsigned
    // GT 2, unsigned LT 1; 31 always traps).
    void tw(uint32_t to, RegisterID ra, RegisterID rb)
    {
        ASSERT(to < 32);
        insn((31 << 26)
            | (to << 21)
            | (registerValue(ra) << 16)
            | (registerValue(rb) << 11)
            | (4 << 1));
    }

    // tw 31,0,0: the kernel delivers SIGTRAP.
    void trap()
    {
        tw(31, PPC64Registers::r0, PPC64Registers::r0);
    }

    // Barriers. sync (L = 0) is a full barrier; lwsync (L = 1) orders
    // everything except store-then-load. isync discards prefetched
    // instructions; after a dependent branch it acts as an acquire barrier.
    void sync(uint32_t l = 0)
    {
        ASSERT(l < 4);
        insn((31u << 26) | (l << 21) | (598u << 1));
    }

    void lwsync() { sync(1); }
    void hwsync() { sync(0); }
    void ptesync() { sync(2); }

    void isync()
    {
        insn((19u << 26) | (150u << 1));
    }

    void eieio()
    {
        insn((31u << 26) | (854u << 1));
    }

    // Cache management. After writing code: dcbst, sync, icbi, isync.
    void dcbst(RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, PPC64Registers::r0, ra, rb, /*XO*/ 54, /*Rc*/ 0));
    }

    void dcbf(RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, PPC64Registers::r0, ra, rb, /*XO*/ 86, /*Rc*/ 0));
    }

    void icbi(RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, PPC64Registers::r0, ra, rb, /*XO*/ 982, /*Rc*/ 0));
    }

    void dcbz(RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, PPC64Registers::r0, ra, rb, /*XO*/ 1014, /*Rc*/ 0));
    }

    // VMX (Altivec) vector instructions, VX-form.
    void vand(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)
    {
        insn(vxForm(4, vrt, vra, vrb, /*XO*/ 1028));
    }

    void vor(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)
    {
        insn(vxForm(4, vrt, vra, vrb, /*XO*/ 1156));
    }

    void vxor(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)
    {
        insn(vxForm(4, vrt, vra, vrb, /*XO*/ 1220));
    }

    void vnor(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)
    {
        insn(vxForm(4, vrt, vra, vrb, /*XO*/ 1284));
    }

    void vandc(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)
    {
        insn(vxForm(4, vrt, vra, vrb, /*XO*/ 1092));
    }

    // Modulo (wrapping) add and subtract.
    void vaddubm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 0));    }
    void vadduhm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 64));   }
    void vadduwm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 128));  }
    void vaddudm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 192));  }
    void vsububm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1024)); }
    void vsubuhm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1088)); }
    void vsubuwm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1152)); }
    void vsubudm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1216)); }

    // Saturating add and subtract (b/h/w only; there are no doubleword forms).
    void vaddubs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  512)); }
    void vadduhs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  576)); }
    void vadduws(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  640)); }
    void vaddsbs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  768)); }
    void vaddshs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  832)); }
    void vaddsws(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  896)); }
    void vsububs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1536)); }
    void vsubuhs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1600)); }
    void vsubuws(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1664)); }
    void vsubsbs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1792)); }
    void vsubshs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1856)); }
    void vsubsws(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1920)); }

    // Integer multiply. vmuluwm is i32x4.mul; the even/odd forms produce
    // double-width products of alternate lanes.
    void vmuluwm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 137)); }
    void vmulesh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 840)); }
    void vmulosh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 328)); }
    void vmulesw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 904)); }
    void vmulosw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 392)); }

    // VRT[i] = VRA[2i] * VRB[2i] + VRA[2i+1] * VRB[2i+1] + VRC[i], signed
    // halfwords (i32x4.dot_i16x8_s with VRC = 0).
    void vmsumshm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc)
    {
        insn(vaForm(4, vrt, vra, vrb, vrc, /*XO*/ 40));
    }

    // VMX loads and stores. lvx/stvx ignore the low 4 bits of the address;
    // unaligned accesses need lxvd2x/stxvd2x.
    void lvx(VRegisterID vrt, RegisterID ra, RegisterID rb)    { insn(xFormVrMem(31, vrt, ra, rb, 103)); }
    void stvx(VRegisterID vrs, RegisterID ra, RegisterID rb)   { insn(xFormVrMem(31, vrs, ra, rb, 231)); }
    void lvebx(VRegisterID vrt, RegisterID ra, RegisterID rb)  { insn(xFormVrMem(31, vrt, ra, rb,   7)); }
    void lvehx(VRegisterID vrt, RegisterID ra, RegisterID rb)  { insn(xFormVrMem(31, vrt, ra, rb,  39)); }
    void lvewx(VRegisterID vrt, RegisterID ra, RegisterID rb)  { insn(xFormVrMem(31, vrt, ra, rb,  71)); }
    void stvebx(VRegisterID vrs, RegisterID ra, RegisterID rb) { insn(xFormVrMem(31, vrs, ra, rb, 135)); }
    void stvehx(VRegisterID vrs, RegisterID ra, RegisterID rb) { insn(xFormVrMem(31, vrs, ra, rb, 167)); }
    void stvewx(VRegisterID vrs, RegisterID ra, RegisterID rb) { insn(xFormVrMem(31, vrs, ra, rb, 199)); }

    // VSX 128-bit indexed load/store and move, for VSR 0-31 (TX/AX/BX = 0).
    // On little-endian, lxvd2x/stxvd2x leave the two doublewords swapped
    // relative to lxv/stxv: a stxvd2x/lxvd2x round trip is exact, but lane
    // operations must account for the doubleword order.
    void lxvd2x(FPRegisterID xt, RegisterID ra, RegisterID rb)  { insn(xFormFpMem(31, xt, ra, rb, 844)); }
    void stxvd2x(FPRegisterID xs, RegisterID ra, RegisterID rb) { insn(xFormFpMem(31, xs, ra, rb, 972)); }
    void xxlor(FPRegisterID xt, FPRegisterID xa, FPRegisterID xb)
    {
        insn((60u << 26) | (fprValue(xt) << 21) | (fprValue(xa) << 16) | (fprValue(xb) << 11) | (146u << 3));
    }

    // Vector compares: each lane becomes all ones or all zeros. The record
    // forms (not used here) also set CR6.
    void vcmpequb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0,  6)); }
    void vcmpequh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 70)); }
    void vcmpequw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 134)); }
    void vcmpequd(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 199)); }
    void vcmpgtub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 518)); }
    void vcmpgtuh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 582)); }
    void vcmpgtuw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 646)); }
    void vcmpgtud(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 711)); }
    void vcmpgtsb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 774)); }
    void vcmpgtsh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 838)); }
    void vcmpgtsw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 902)); }
    void vcmpgtsd(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 967)); }

    // Splats. vsplt[bhw] broadcast lane UIMM of VRB, numbered in big-endian
    // element order; vspltis[bhw] broadcast a 5-bit signed immediate.
    void vspltb(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 16);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 524u);
    }

    void vsplth(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 8);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 588u);
    }

    void vspltw(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 4);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 652u);
    }

    void vspltisb(VRegisterID vrt, int32_t simm)
    {
        ASSERT(simm >= -16 && simm < 16);
        insn((4u << 26) | (vrValue(vrt) << 21) | ((static_cast<uint32_t>(simm) & 0x1F) << 16) | 780u);
    }

    void vspltish(VRegisterID vrt, int32_t simm)
    {
        ASSERT(simm >= -16 && simm < 16);
        insn((4u << 26) | (vrValue(vrt) << 21) | ((static_cast<uint32_t>(simm) & 0x1F) << 16) | 844u);
    }

    void vspltisw(VRegisterID vrt, int32_t simm)
    {
        ASSERT(simm >= -16 && simm < 16);
        insn((4u << 26) | (vrValue(vrt) << 21) | ((static_cast<uint32_t>(simm) & 0x1F) << 16) | 908u);
    }

    // Merges. "High" and "low" are big-endian element order, which on
    // little-endian is the opposite of wasm lane order.
    void vmrghb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,   12)); }
    void vmrghh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,   76)); }
    void vmrghw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  140)); }
    void vmrglb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  268)); }
    void vmrglh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  332)); }
    void vmrglw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  396)); }
    void vmrgew(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1932)); }
    void vmrgow(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1676)); }

    // Per-lane shifts and rotates; each count is taken modulo the lane width.
    void vslb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  260)); }
    void vslh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  324)); }
    void vslw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  388)); }
    void vsld(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb, 1476)); }
    void vsrb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  516)); }
    void vsrh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  580)); }
    void vsrw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  644)); }
    void vsrd(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb, 1732)); }
    void vsrab(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  772)); }
    void vsrah(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  836)); }
    void vsraw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  900)); }
    void vsrad(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  964)); }
    void vrlb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,    4)); }
    void vrlh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,   68)); }
    void vrlw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  132)); }
    void vrld(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  196)); }

    // Per-lane integer min and max.
    void vminub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 514)); }
    void vmaxub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,   2)); }
    void vminuh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 578)); }
    void vmaxuh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb,  66)); }
    void vminuw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 642)); }
    void vmaxuw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 130)); }
    void vminud(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 706)); }
    void vmaxud(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 194)); }
    void vminsb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 770)); }
    void vmaxsb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 258)); }
    void vminsh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 834)); }
    void vmaxsh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 322)); }
    void vminsw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 898)); }
    void vmaxsw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 386)); }
    void vminsd(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 962)); }
    void vmaxsd(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 450)); }

    // Four-operand (VA-form) permute, select and multiply-add.
    void vperm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc)
    {
        insn(vaForm(4, vrt, vra, vrb, vrc, /*XO*/ 43));
    }

    void vsel(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc)
    {
        insn(vaForm(4, vrt, vra, vrb, vrc, /*XO*/ 42));
    }

    // VRT = VRA * VRC + VRB. Operands are in assembler order.
    void vmaddfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrc, VRegisterID vrb)
    {
        insn(vaForm(4, vrt, vra, vrb, vrc, /*XO*/ 46));
    }

    // VMX single-precision arithmetic, rounding and compares (VMX has no
    // double-precision vector operations; those are VSX).
    void vaddfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  10)); }
    void vsubfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb,  74)); }
    void vminfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb, 1098)); }
    void vmaxfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb)  { insn(vxForm(4, vrt, vra, vrb, 1034)); }

    void vrfin(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 522)); }
    void vrfiz(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 586)); }
    void vrfip(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 650)); }
    void vrfim(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 714)); }

    void vcmpeqfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 198)); }
    void vcmpgefp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 454)); }
    void vcmpgtfp(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vcForm(4, vrt, vra, vrb, 0, 710)); }

    // VMX float <-> int32 conversions. UIMM is a fixed-point scale (0 for plain
    // conversion); the float-to-int forms saturate.
    void vctsxs(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 32);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 970u);
    }

    void vctuxs(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 32);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 906u);
    }

    void vcfsx(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 32);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 842u);
    }

    void vcfux(VRegisterID vrt, VRegisterID vrb, uint32_t uimm)
    {
        ASSERT(uimm < 32);
        insn((4u << 26) | (vrValue(vrt) << 21) | (uimm << 16) | (vrValue(vrb) << 11) | 778u);
    }

    // Per-lane population count.
    void vpopcntb(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1795)); }
    void vpopcnth(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1859)); }
    void vpopcntw(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1923)); }
    void vpopcntd(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1987)); }

    // Unpack with sign extension. "High" and "low" are big-endian element
    // order: on little-endian, vupkhs* extends the wasm low half and vupkls*
    // the high half. There is no zero-extending unpack; merge with zero instead.
    void vupkhsb(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb,  526)); }
    void vupklsb(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb,  654)); }
    void vupkhsh(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb,  590)); }
    void vupklsh(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb,  718)); }
    void vupkhsw(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1614)); }
    void vupklsw(VRegisterID vrt, VRegisterID vrb) { insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 1742)); }

    // Saturating packs.
    void vpkshss(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 398)); }
    void vpkshus(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 270)); }
    void vpkswss(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 462)); }
    void vpkswus(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 334)); }

    // Unsigned rounding average.
    void vavgub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1026)); }
    void vavguh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1090)); }

    // VRT[i] = sat(((VRA[i] * VRB[i] + 0x4000) >> 15) + VRC[i])
    // (i16x8.q15mulr_sat_s with VRC = 0).
    void vmhraddshs(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc)
    {
        insn(vaForm(4, vrt, vra, vrb, vrc, /*XO*/ 33));
    }

    // VSX. Registers are VSR numbers 0-63: VSR0-31 overlay the FPRs and
    // VSR32-63 the VMX registers. The high bit of each 6-bit register number
    // is encoded separately from the low 5 bits. Unlike lvx/stvx,
    // lxvd2x/stxvd2x do not align the address.
    void lxvd2x(uint32_t vsrT, RegisterID ra, RegisterID rb)
    {
        insn(xx1Form(31, vsrT, ra, rb, /*XO*/ 844));
    }

    void stxvd2x(uint32_t vsrS, RegisterID ra, RegisterID rb)
    {
        insn(xx1Form(31, vsrS, ra, rb, /*XO*/ 972));
    }

    void lxvw4x(uint32_t vsrT, RegisterID ra, RegisterID rb)
    {
        insn(xx1Form(31, vsrT, ra, rb, /*XO*/ 780));
    }

    void stxvw4x(uint32_t vsrS, RegisterID ra, RegisterID rb)
    {
        insn(xx1Form(31, vsrS, ra, rb, /*XO*/ 908));
    }

    void xxland(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)  { insn(xx3Form(60, vsrT, vsrA, vsrB, 130)); }
    void xxlandc(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 138)); }
    void xxlor(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)   { insn(xx3Form(60, vsrT, vsrA, vsrB, 146)); }
    void xxlxor(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)  { insn(xx3Form(60, vsrT, vsrA, vsrB, 154)); }
    void xxlnor(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)  { insn(xx3Form(60, vsrT, vsrA, vsrB, 162)); }

    // VSX vector arithmetic.
    void xvmulsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB,  80)); }
    void xvdivsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB,  88)); }
    void xvadddp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB,  96)); }
    void xvsubdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 104)); }
    void xvmuldp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 112)); }
    void xvdivdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 120)); }

    void xvsqrtsp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 139)); }
    void xvsqrtdp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 203)); }

    // VSX double-precision compares, min/max and rounding. xvrdpic rounds per
    // FPSCR[RN], which JSC leaves at round-to-nearest-even.
    void xvcmpeqdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB,  99)); }
    void xvcmpgtdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 107)); }
    void xvcmpgedp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 115)); }
    void xvmindp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)   { insn(xx3Form(60, vsrT, vsrA, vsrB, 232)); }
    void xvmaxdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB)   { insn(xx3Form(60, vsrT, vsrA, vsrB, 224)); }

    void xvnegdp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 505)); }
    void xvabsdp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 473)); }
    void xvrdpip(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 233)); }
    void xvrdpim(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 249)); }
    void xvrdpiz(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 217)); }
    void xvrdpic(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 235)); }

    // VSX single-precision negate/abs and conversions.
    void xvnegsp(uint32_t vsrT, uint32_t vsrB)    { insn(xx2Form(60, vsrT, vsrB, 441)); }
    void xvabssp(uint32_t vsrT, uint32_t vsrB)    { insn(xx2Form(60, vsrT, vsrB, 409)); }
    void xvcvdpsxws(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 216)); }
    void xvcvdpuxws(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 200)); }
    void xvcvsxwdp(uint32_t vsrT, uint32_t vsrB)  { insn(xx2Form(60, vsrT, vsrB, 248)); }
    void xvcvuxwdp(uint32_t vsrT, uint32_t vsrB)  { insn(xx2Form(60, vsrT, vsrB, 232)); }
    void xvcvdpsp(uint32_t vsrT, uint32_t vsrB)   { insn(xx2Form(60, vsrT, vsrB, 393)); }
    void xvcvspdp(uint32_t vsrT, uint32_t vsrB)   { insn(xx2Form(60, vsrT, vsrB, 457)); }

    // More VSX, all within the POWER8 baseline.

    // OR-complement, NAND, equivalence.
    void xxlorc(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 170)); }
    void xxlnand(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 178)); }
    void xxleqv(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 186)); }

    // Permutes, with a 2-bit field inside the XO.
    //   xxpermdi T,A,B,DM: T.dw0 = DM&2 ? A.dw1 : A.dw0; T.dw1 = DM&1 ? B.dw1 : B.dw0
    //   xxsldwi  T,A,B,SHW: words SHW..SHW+3 of A||B
    void xxpermdi(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB, uint32_t dm)
    {
        ASSERT(dm < 4);
        insn(xx3Form(60, vsrT, vsrA, vsrB, (dm << 5) | 10));
    }
    void xxswapd(uint32_t vsrT, uint32_t vsrA) { xxpermdi(vsrT, vsrA, vsrA, 2); }
    void xxsldwi(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB, uint32_t shw)
    {
        ASSERT(shw < 4);
        insn(xx3Form(60, vsrT, vsrA, vsrB, (shw << 5) | 2));
    }
    void xxmrghw(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 18)); }
    void xxmrglw(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 50)); }
    // UIM sits in bits 14-15.
    void xxspltw(uint32_t vsrT, uint32_t vsrB, uint32_t uim)
    {
        ASSERT(uim < 4);
        insn(xx2Form(60, vsrT, vsrB, 164) | (uim << 16));
    }
    // T = (A & ~C) | (B & C).
    void xxsel(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB, uint32_t vsrC)
    {
        ASSERT(vsrT < 64 && vsrA < 64 && vsrB < 64 && vsrC < 64);
        insn((60u << 26)
            | ((vsrT & 0x1F) << 21) | ((vsrA & 0x1F) << 16) | ((vsrB & 0x1F) << 11) | ((vsrC & 0x1F) << 6)
            | (3u << 4)
            | (((vsrC >> 5) & 1) << 3) | (((vsrA >> 5) & 1) << 2) | (((vsrB >> 5) & 1) << 1) | ((vsrT >> 5) & 1));
    }

    // Single-precision vector arithmetic and compares.
    void xvaddsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 64)); }
    void xvsubsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 72)); }
    void xvmaxsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 192)); }
    void xvminsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 200)); }
    void xvcmpeqsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 67)); }
    void xvcmpgtsp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 75)); }
    void xvcmpgesp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 83)); }
    // Fused multiply-add, "A" forms: T = A*B + T; nmsub: T = -(A*B - T).
    void xvmaddasp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 65)); }
    void xvmaddadp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 97)); }
    void xvnmsubasp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 209)); }
    void xvnmsubadp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 241)); }
    // Round to integral: p = toward +inf, m = toward -inf, z = toward zero,
    // c = current mode (round-to-nearest-even, JSC never changes FPSCR[RN]).
    void xvrspip(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 169)); }
    void xvrspim(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 185)); }
    void xvrspiz(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 153)); }
    void xvrspic(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 171)); }
    void xvcvspsxws(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 152)); }
    void xvcvspuxws(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 136)); }
    void xvcvsxwsp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 184)); }
    void xvcvuxwsp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 168)); }
    // Scalar single <-> double, non-signalling: xscvdpspn leaves the
    // single in word 0 of T; xscvspdpn reads the single from word 0 of B.
    void xscvdpspn(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 267)); }
    // Power ISA 3.0 (POWER9) only: binary16 <-> binary64, the half in bits
    // 48:63 of doubleword 0 (XX2-form, XO=347, bits 11:15 select the op).
    void xscvhpdp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 347) | (16u << 16)); }
    void xscvdphp(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 347) | (17u << 16)); }
    void xscvspdpn(uint32_t vsrT, uint32_t vsrB) { insn(xx2Form(60, vsrT, vsrB, 331)); }

    // Scalar and splat loads and stores.
    //   lxsdx    T.dw0 = doubleword at EA
    //   stxsdx   doubleword at EA = S.dw0
    //   lxsiwzx  T.dw0 = zero-extended word at EA
    //   stxsiwx  word at EA = S.word1
    //   lxvdsx   T.dw0 = T.dw1 = doubleword at EA
    void lxsdx(uint32_t vsrT, RegisterID ra, RegisterID rb) { insn(xx1Form(31, vsrT, ra, rb, 588)); }
    void stxsdx(uint32_t vsrS, RegisterID ra, RegisterID rb) { insn(xx1Form(31, vsrS, ra, rb, 716)); }
    void lxsiwzx(uint32_t vsrT, RegisterID ra, RegisterID rb) { insn(xx1Form(31, vsrT, ra, rb, 12)); }
    void stxsiwx(uint32_t vsrS, RegisterID ra, RegisterID rb) { insn(xx1Form(31, vsrS, ra, rb, 140)); }
    void lxvdsx(uint32_t vsrT, RegisterID ra, RegisterID rb) { insn(xx1Form(31, vsrT, ra, rb, 332)); }

    // GPR <-> VSR moves addressing all 64 VSRs.
    void mtvsrd(uint32_t vsrT, RegisterID ra) { insn(xx1Form(31, vsrT, ra, PPC64Registers::r0, 179)); }
    void mtvsrwz(uint32_t vsrT, RegisterID ra) { insn(xx1Form(31, vsrT, ra, PPC64Registers::r0, 243)); }
    void mfvsrd(RegisterID ra, uint32_t vsrS) { insn(xx1Form(31, vsrS, ra, PPC64Registers::r0, 51)); }
    void mfvsrwz(RegisterID ra, uint32_t vsrS) { insn(xx1Form(31, vsrS, ra, PPC64Registers::r0, 115)); }

    // vsldoi T,A,B,SH: bytes SH..SH+15 of A||B.
    void vsldoi(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, uint32_t shb)
    {
        ASSERT(shb < 16);
        insn(vaForm(4, vrt, vra, vrb, static_cast<VRegisterID>(shb), /*XO*/ 44));
    }
    void vmladduhm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc) { insn(vaForm(4, vrt, vra, vrb, vrc, 34)); }
    void vmsumuhm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc) { insn(vaForm(4, vrt, vra, vrb, vrc, 38)); }
    void vmsummbm(VRegisterID vrt, VRegisterID vra, VRegisterID vrb, VRegisterID vrc) { insn(vaForm(4, vrt, vra, vrb, vrc, 37)); }
    void vmulesb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 776)); }
    void vmulosb(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 264)); }
    void vmuleub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 520)); }
    void vmuloub(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 8)); }
    void vmuleuh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 584)); }
    void vmulouh(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 72)); }
    void vmuleuw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 648)); } // 2.07
    void vmulouw(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 136)); } // 2.07
    void vpkuhum(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 14)); }
    void vpkuwum(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 78)); }
    void vpkudum(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1102)); } // 2.07
    // vbpermq: T.dw0 bits 48:63 = the 16 bits of A selected by the
    // byte indices in B (index >= 128 selects 0); T.dw1 = 0.
    void vbpermq(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 1356)); }

    // Power ISA 3.0 (POWER9) and 3.1 (POWER10), emitted only behind
    // MacroAssemblerPPC64::supportsISA3_00() / supportsISA3_1().
    void cnttzw(RegisterID ra, RegisterID rs) { insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 538, /*Rc*/ 0)); }
    void cnttzd(RegisterID ra, RegisterID rs) { insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 570, /*Rc*/ 0)); }
    // Remainders. Like divw/divd the result is undefined for x / 0 and
    // INT_MIN / -1; callers guard those cases exactly as they do for div.
    void modsw(RegisterID rt, RegisterID ra, RegisterID rb) { insn(xForm(31, rt, ra, rb, /*XO*/ 779, /*Rc*/ 0)); }
    void moduw(RegisterID rt, RegisterID ra, RegisterID rb) { insn(xForm(31, rt, ra, rb, /*XO*/ 267, /*Rc*/ 0)); }
    void modsd(RegisterID rt, RegisterID ra, RegisterID rb) { insn(xForm(31, rt, ra, rb, /*XO*/ 777, /*Rc*/ 0)); }
    void modud(RegisterID rt, RegisterID ra, RegisterID rb) { insn(xForm(31, rt, ra, rb, /*XO*/ 265, /*Rc*/ 0)); }
    // "Java" min/max: a NaN operand yields that NaN and -0 < +0, as JS
    // Math.min/max. A signalling NaN comes back unquieted, which wasm forbids.
    void xsmaxjdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 144)); }
    void xsminjdp(uint32_t vsrT, uint32_t vsrA, uint32_t vsrB) { insn(xx3Form(60, vsrT, vsrA, vsrB, 152)); }
    // Insert word element 1 of XB / byte 7 of VRB / halfword 3 of VRB at byte
    // UIM (big-endian numbering) of the target, leaving its other bytes.
    void xxinsertw(uint32_t vsrT, uint32_t vsrB, unsigned uim)
    {
        ASSERT(uim <= 12);
        insn(xx2Form(60, vsrT, vsrB, 181) | (uim << 16));
    }
    void vinsertb(VRegisterID vrt, VRegisterID vrb, unsigned uim)
    {
        ASSERT(uim <= 15);
        insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 781) | (uim << 16));
    }
    void vinserth(VRegisterID vrt, VRegisterID vrb, unsigned uim)
    {
        ASSERT(uim <= 14);
        insn(vxForm(4, vrt, PPC64Registers::v0, vrb, 845) | (uim << 16));
    }
    // CR field BF's GT bit = whether the low byte of RA equals any byte of RB.
    void cmpeqb(unsigned bf, RegisterID ra, RegisterID rb)
    {
        ASSERT(bf < 8);
        insn((31u << 26) | (bf << 23) | (registerValue(ra) << 16) | (registerValue(rb) << 11) | (224u << 1));
    }
    // Byte-reverse each halfword / word / doubleword of RS.
    void brh(RegisterID ra, RegisterID rs) { insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 219, /*Rc*/ 0)); }
    void brw(RegisterID ra, RegisterID rs) { insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 155, /*Rc*/ 0)); }
    void brd(RegisterID ra, RegisterID rs) { insn(xForm(31, rs, ra, PPC64Registers::r0, /*XO*/ 187, /*Rc*/ 0)); }
    // RT = CR bit BI ? 1 : 0 (setbc) or ? 0 : 1 (setbcr).
    void setbc(RegisterID rt, unsigned bi)
    {
        ASSERT(bi < 32);
        insn((31u << 26) | (registerValue(rt) << 21) | (bi << 16) | (384u << 1));
    }
    void setbcr(RegisterID rt, unsigned bi)
    {
        ASSERT(bi < 32);
        insn((31u << 26) | (registerValue(rt) << 21) | (bi << 16) | (416u << 1));
    }
    void vmulld(VRegisterID vrt, VRegisterID vra, VRegisterID vrb) { insn(vxForm(4, vrt, vra, vrb, 457)); }
    // RT = the most significant bit of each element of VRB, element 0 in the
    // most significant result bit (the RA field selects the element width).
    void vextractbm(RegisterID rt, VRegisterID vrb) { insn(vectorExtractMask(rt, 8, vrb)); }
    void vextracthm(RegisterID rt, VRegisterID vrb) { insn(vectorExtractMask(rt, 9, vrb)); }
    void vextractwm(RegisterID rt, VRegisterID vrb) { insn(vectorExtractMask(rt, 10, vrb)); }
    void vextractdm(RegisterID rt, VRegisterID vrb) { insn(vectorExtractMask(rt, 11, vrb)); }
    static constexpr uint32_t vectorExtractMask(RegisterID rt, uint32_t width, VRegisterID vrb)
    {
        return (4u << 26) | (registerValue(rt) << 21) | (width << 16) | (vrValue(vrb) << 11) | 1602u;
    }

    // Load-reserve / store-conditional, indexed only. stcx. sets CR0[EQ] on
    // success. A failed compare leaves the reservation set; that only matters
    // to an stcx. not paired with its own larx, and every stcx. here directly
    // follows its larx, so no clearing store is needed.
    void lwarx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 20, /*Rc*/ 0));
    }

    void ldarx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 84, /*Rc*/ 0));
    }

    void lbarx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 52, /*Rc*/ 0));
    }

    void lharx(RegisterID rt, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rt, ra, rb, /*XO*/ 116, /*Rc*/ 0));
    }

    void stwcx_(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 150, /*Rc*/ 1));
    }

    void stdcx_(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 214, /*Rc*/ 1));
    }

    void stbcx_(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 694, /*Rc*/ 1));
    }

    void sthcx_(RegisterID rs, RegisterID ra, RegisterID rb)
    {
        insn(xForm(31, rs, ra, rb, /*XO*/ 726, /*Rc*/ 1));
    }

protected:
    void insn(uint32_t instruction)
    {
        m_buffer.putInt(instruction);
    }

    // Instruction formats. Bit numbers are the ISA's: bit 0 is the MSB.
    static constexpr uint32_t dForm(uint32_t opcode, RegisterID rtOrRs, RegisterID ra, uint16_t imm)
    {
        ASSERT(opcode < 64);
        return (opcode << 26)
             | (registerValue(rtOrRs) << 21)
             | (registerValue(ra) << 16)
             | imm;
    }

    // DS-form: the low 16 bits hold (byteOffset & 0xFFFC) | XO.
    static constexpr uint32_t dsForm(uint32_t opcode, RegisterID rtOrRs, RegisterID ra, int16_t byteOffset, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 4);
        ASSERT((byteOffset & 0x3) == 0);
        return (opcode << 26)
             | (registerValue(rtOrRs) << 21)
             | (registerValue(ra) << 16)
             | (static_cast<uint32_t>(static_cast<uint16_t>(byteOffset)) & 0xFFFC)
             | xo;
    }

    // M-form: [op | RS | RA | SH or RB | MB | ME | Rc], all plain 5-bit fields.
    static constexpr uint32_t mFormImm(uint32_t opcode, RegisterID rs, RegisterID ra,
                                       uint32_t sh, uint32_t mb, uint32_t me, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(sh < 32);
        ASSERT(mb < 32);
        ASSERT(me < 32);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (registerValue(rs) << 21)
             | (registerValue(ra) << 16)
             | (sh << 11)
             | (mb << 6)
             | (me << 1)
             | rc;
    }

    static constexpr uint32_t mFormReg(uint32_t opcode, RegisterID rs, RegisterID ra,
                                       RegisterID rb, uint32_t mb, uint32_t me, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(mb < 32);
        ASSERT(me < 32);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (registerValue(rs) << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (mb << 6)
             | (me << 1)
             | rc;
    }

    // XS-form (sradi): [op | RS | RA | sh[0:4] | XO(9) | sh[5] | Rc].
    static constexpr uint32_t xsForm(uint32_t opcode, RegisterID rs, RegisterID ra,
                                     uint32_t sh, uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(sh < 64);
        ASSERT(xo < 512);
        ASSERT(rc < 2);
        uint32_t shLow5 = sh & 0x1F;
        uint32_t sh2 = (sh >> 5) & 1;
        return (opcode << 26)
             | (registerValue(rs) << 21)
             | (registerValue(ra) << 16)
             | (shLow5 << 11)
             | (xo << 2)
             | (sh2 << 1)
             | rc;
    }

    // MD-form: [op | RS | RA | sh[0:4] | mb/me(6) | XO(3) | sh[5] | Rc]. Both
    // 6-bit fields are split: the low 5 bits of SH go in bits 16-20 and its
    // high bit in bit 30, and MB/ME is stored rotated left by one (low 5 bits
    // first, then the high bit).
    static constexpr uint32_t mdForm(uint32_t opcode, RegisterID rs, RegisterID ra,
                                     uint32_t sh, uint32_t mbOrMe, uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(sh < 64);
        ASSERT(mbOrMe < 64);
        ASSERT(xo < 8);
        ASSERT(rc < 2);
        uint32_t shLow5 = sh & 0x1F;
        uint32_t sh2 = (sh >> 5) & 1;
        uint32_t mbField = ((mbOrMe & 0x1F) << 1) | ((mbOrMe >> 5) & 1);
        return (opcode << 26)
             | (registerValue(rs) << 21)
             | (registerValue(ra) << 16)
             | (shLow5 << 11)
             | (mbField << 5)
             | (xo << 2)
             | (sh2 << 1)
             | rc;
    }

    // X-form: [op | RT/RS | RA | RB | XO(10) | Rc]. The first register field is
    // the destination for loads but the source for logical operations.
    static constexpr uint32_t xForm(uint32_t opcode, RegisterID rtOrRs, RegisterID ra, RegisterID rb, uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 1024);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (registerValue(rtOrRs) << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (xo << 1)
             | rc;
    }

    // Compare X-form: [op | BF(3) | / | L | RA | RB | XO(10) | /].
    static constexpr uint32_t cmpXForm(uint32_t opcode, uint32_t bf, uint32_t l,
                                       RegisterID ra, RegisterID rb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(bf < 8);
        ASSERT(l < 2);
        ASSERT(xo < 1024);
        return (opcode << 26)
             | (bf << 23)
             | (l << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (xo << 1);
    }

    // Compare D-form: [op | BF(3) | / | L | RA | SI/UI(16)].
    static constexpr uint32_t cmpDForm(uint32_t opcode, uint32_t bf, uint32_t l,
                                       RegisterID ra, uint16_t imm)
    {
        ASSERT(opcode < 64);
        ASSERT(bf < 8);
        ASSERT(l < 2);
        return (opcode << 26)
             | (bf << 23)
             | (l << 21)
             | (registerValue(ra) << 16)
             | imm;
    }

    // I-form: [op | LI(24) | AA | LK]. LI is a word offset, so the byte offset
    // goes in as is under mask 0x03FFFFFC.
    static constexpr uint32_t iForm(uint32_t opcode, int32_t byteOffset, uint32_t aa, uint32_t lk)
    {
        ASSERT(opcode < 64);
        ASSERT(aa < 2);
        ASSERT(lk < 2);
        ASSERT((byteOffset & 0x3) == 0);
        // 26-bit signed byte range: [-2^25, 2^25 - 4].
        ASSERT(byteOffset >= -(1 << 25) && byteOffset < (1 << 25));
        return (opcode << 26)
             | (static_cast<uint32_t>(byteOffset) & 0x03FFFFFC)
             | (aa << 1)
             | lk;
    }

    // B-form: [op | BO | BI | BD(14) | AA | LK].
    static constexpr uint32_t bForm(uint32_t opcode, uint32_t bo, uint32_t bi,
                                    int32_t byteOffset, uint32_t aa, uint32_t lk)
    {
        ASSERT(opcode < 64);
        ASSERT(bo < 32);
        ASSERT(bi < 32);
        ASSERT(aa < 2);
        ASSERT(lk < 2);
        ASSERT((byteOffset & 0x3) == 0);
        // 16-bit signed byte range: [-2^15, 2^15 - 4].
        ASSERT(byteOffset >= -(1 << 15) && byteOffset < (1 << 15));
        return (opcode << 26)
             | (bo << 21)
             | (bi << 16)
             | (static_cast<uint32_t>(byteOffset) & 0xFFFC)
             | (aa << 1)
             | lk;
    }

    // XL-form: [op | BO | BI | BH | XO(10) | LK].
    static constexpr uint32_t xlForm(uint32_t opcode, uint32_t bo, uint32_t bi,
                                     uint32_t bh, uint32_t xo, uint32_t lk)
    {
        ASSERT(opcode < 64);
        ASSERT(bo < 32);
        ASSERT(bi < 32);
        ASSERT(bh < 4);
        ASSERT(xo < 1024);
        ASSERT(lk < 2);
        return (opcode << 26)
             | (bo << 21)
             | (bi << 16)
             | (bh << 11)
             | (xo << 1)
             | lk;
    }

    // XFX-form: [op | RT/RS | spr(10) | XO(10) | /], with the SPR number's
    // two 5-bit halves swapped.
    static constexpr uint32_t xfxForm(uint32_t opcode, RegisterID rtOrRs, uint32_t sprNum, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(sprNum < 1024);
        ASSERT(xo < 1024);
        uint32_t sprField = ((sprNum & 0x1F) << 5) | ((sprNum >> 5) & 0x1F);
        return (opcode << 26)
             | (registerValue(rtOrRs) << 21)
             | (sprField << 11)
             | (xo << 1);
    }

    static constexpr uint32_t xoForm(uint32_t opcode, RegisterID rt, RegisterID ra, RegisterID rb,
                                     uint32_t oe, uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(oe < 2);
        ASSERT(xo < 512);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (registerValue(rt) << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (oe << 10)
             | (xo << 1)
             | rc;
    }

    static constexpr uint32_t registerValue(RegisterID r)
    {
        ASSERT(r >= firstRegister() && r <= lastRegister());
        return static_cast<uint32_t>(r);
    }

    static constexpr uint32_t fprValue(FPRegisterID r)
    {
        ASSERT(r >= firstFPRegister() && r <= lastFPRegister());
        return static_cast<uint32_t>(r);
    }

    static constexpr uint32_t vrValue(VRegisterID r)
    {
        ASSERT(r >= firstVRegister() && r <= lastVRegister());
        return static_cast<uint32_t>(r);
    }

    static constexpr uint32_t dFormFp(uint32_t opcode, FPRegisterID frtOrFrs, RegisterID ra, uint16_t imm)
    {
        ASSERT(opcode < 64);
        return (opcode << 26)
             | (fprValue(frtOrFrs) << 21)
             | (registerValue(ra) << 16)
             | imm;
    }

    static constexpr uint32_t xFormFpMem(uint32_t opcode, FPRegisterID frtOrFrs,
                                         RegisterID ra, RegisterID rb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 1024);
        return (opcode << 26)
             | (fprValue(frtOrFrs) << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (xo << 1);
    }

    // A-form: [op | FRT | FRA | FRB | FRC | XO(5) | Rc].
    static constexpr uint32_t aForm(uint32_t opcode, FPRegisterID frt, FPRegisterID fra,
                                    FPRegisterID frb, FPRegisterID frc,
                                    uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 32);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (fprValue(frt) << 21)
             | (fprValue(fra) << 16)
             | (fprValue(frb) << 11)
             | (fprValue(frc) << 6)
             | (xo << 1)
             | rc;
    }

    static constexpr uint32_t xFormFp(uint32_t opcode, FPRegisterID frtOrFrs, FPRegisterID fra,
                                      FPRegisterID frb, uint32_t xo, uint32_t rc)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 1024);
        ASSERT(rc < 2);
        return (opcode << 26)
             | (fprValue(frtOrFrs) << 21)
             | (fprValue(fra) << 16)
             | (fprValue(frb) << 11)
             | (xo << 1)
             | rc;
    }

    static constexpr uint32_t xFormVrMem(uint32_t opcode, VRegisterID vrtOrVrs,
                                         RegisterID ra, RegisterID rb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 1024);
        return (opcode << 26)
             | (vrValue(vrtOrVrs) << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (xo << 1);
    }

    // XX forms take VSR numbers 0-63 (an FPR's number, or a VR's plus 32). The
    // low 5 bits go in the usual register field and the high bit (TX/AX/BX)
    // near the bottom of the instruction.
    static constexpr uint32_t xx1Form(uint32_t opcode, uint32_t vsrT,
                                      RegisterID ra, RegisterID rb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(vsrT < 64);
        ASSERT(xo < 1024);
        uint32_t tLow5 = vsrT & 0x1F;
        uint32_t tx = (vsrT >> 5) & 1;
        return (opcode << 26)
             | (tLow5 << 21)
             | (registerValue(ra) << 16)
             | (registerValue(rb) << 11)
             | (xo << 1)
             | tx;
    }

    // XX2-form: [op | T | // | B | XO(9) | BX | TX]. Some instructions put an
    // immediate or a sub-opcode in the // field; callers OR it in.
    static constexpr uint32_t xx2Form(uint32_t opcode, uint32_t vsrT, uint32_t vsrB, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(vsrT < 64);
        ASSERT(vsrB < 64);
        ASSERT(xo < 512);
        return (opcode << 26)
             | ((vsrT & 0x1F) << 21)
             | ((vsrB & 0x1F) << 11)
             | (xo << 2)
             | (((vsrB >> 5) & 1) << 1)
             | ((vsrT >> 5) & 1);
    }

    // XX3-form: [op | T | A | B | XO(8) | AX | BX | TX].
    static constexpr uint32_t xx3Form(uint32_t opcode, uint32_t vsrT,
                                      uint32_t vsrA, uint32_t vsrB, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(vsrT < 64);
        ASSERT(vsrA < 64);
        ASSERT(vsrB < 64);
        ASSERT(xo < 256);
        return (opcode << 26)
             | ((vsrT & 0x1F) << 21)
             | ((vsrA & 0x1F) << 16)
             | ((vsrB & 0x1F) << 11)
             | (xo << 3)
             | (((vsrA >> 5) & 1) << 2)
             | (((vsrB >> 5) & 1) << 1)
             | ((vsrT >> 5) & 1);
    }

    // VA-form: [op | VRT | VRA | VRB | VRC | XO(6)], in encoding order, which
    // for some mnemonics (vmaddfp) differs from assembler order.
    static constexpr uint32_t vaForm(uint32_t opcode, VRegisterID vrt, VRegisterID vra,
                                     VRegisterID vrb, VRegisterID vrc, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 64);
        return (opcode << 26)
             | (vrValue(vrt) << 21)
             | (vrValue(vra) << 16)
             | (vrValue(vrb) << 11)
             | (vrValue(vrc) << 6)
             | xo;
    }

    // VC-form: [op | VRT | VRA | VRB | Rc | XO(10)]. Rc = 1 also sets CR6.
    static constexpr uint32_t vcForm(uint32_t opcode, VRegisterID vrt, VRegisterID vra,
                                     VRegisterID vrb, uint32_t rc, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(rc < 2);
        ASSERT(xo < 1024);
        return (opcode << 26)
             | (vrValue(vrt) << 21)
             | (vrValue(vra) << 16)
             | (vrValue(vrb) << 11)
             | (rc << 10)
             | xo;
    }

    // VX-form: [op | VRT | VRA | VRB | XO(11)].
    static constexpr uint32_t vxForm(uint32_t opcode, VRegisterID vrt, VRegisterID vra,
                                     VRegisterID vrb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(xo < 2048);
        return (opcode << 26)
             | (vrValue(vrt) << 21)
             | (vrValue(vra) << 16)
             | (vrValue(vrb) << 11)
             | xo;
    }

    // FP compare X-form: [op | BF(3) | // | FRA | FRB | XO(10) | /].
    static constexpr uint32_t fpCmpXForm(uint32_t opcode, uint32_t bf,
                                         FPRegisterID fra, FPRegisterID frb, uint32_t xo)
    {
        ASSERT(opcode < 64);
        ASSERT(bf < 8);
        ASSERT(xo < 1024);
        return (opcode << 26)
             | (bf << 23)
             | (fprValue(fra) << 16)
             | (fprValue(frb) << 11)
             | (xo << 1);
    }

public:
    static void* getRelocatedAddress(void* code, AssemblerLabel label)
    {
        ASSERT(label.isSet());
        return reinterpret_cast<void*>(reinterpret_cast<ptrdiff_t>(code) + label.offset());
    }

    static int getDifferenceBetweenLabels(AssemblerLabel a, AssemblerLabel b)
    {
        return b.offset() - a.offset();
    }

    static unsigned getCallReturnOffset(AssemblerLabel call)
    {
        ASSERT(call.isSet());
        return call.offset();
    }

    // ==================================================================
    // Linking and patching.
    //
    // Every MacroAssembler jump, conditional branch, and call reserves a
    // FIXED 8-instruction (32-byte) slot.  At link/relink time the slot is
    // rewritten in place to the shortest form that reaches the target and
    // padded with nops.  Jumps and branches linked inside their own buffer or
    // to a thunk are instead shrunk by branch compaction (see JumpType below);
    // calls and fixed-size (patchable) slots keep this layout.
    //
    //   Unconditional jump slot        Conditional branch slot
    //   [0] marker or b/li64...        [0] bc BO,BI (offset 0 when unlinked)
    //   [1..7] nops / stanza           [1] marker or b/li64...
    //                                  [2..7] nops / stanza
    //
    //   Call slot (branch is LAST so the return address is fixed at +32):
    //   [0..4] nops or li64(r12,target)
    //   [5]    nop  or mtctr r12
    //   [6]    nop
    //   [7]    bl target  or  bctrl
    //
    // The far forms use r12, the MacroAssembler scratch, which is also the
    // ELFv2 function-entry register.
    //
    // Rewrite forms for jumps/branches (anchor = the slot's branch insn):
    //   near:   b target                      (±32MB)
    //   far:    nop; li64 r12, target (5 insns); mtctr r12; bctr
    // Conditional slots put an inverted bc over the stanza when the target
    // does not fit the bc's own ±32KB reach. See computeJumpSlot and
    // applyCallSlot for the exact layouts and which rewrites are atomic.
    //
    // Concurrent repatching. The Power ISA only allows b/bl/nop-class words to
    // change under a thread that may be executing them; anything
    // else can execute as a mix of old and new. Wasm tier-up retargets wasm
    // calls and tail calls while other threads run them, so those sites must
    // change by a single b/bl store. A target out of b reach is then reached
    // through a jump island (ENABLE(JUMP_ISLANDS), as on ARM64):
    // ExecutableAllocator splits the pool into regions of nearJumpRange,
    // each ending in an area of islands within b reach of the region, and
    // an island is a whole jump slot (fillNearTailCall). Every other site is
    // repatched only by the thread that owns it, or with that thread
    // stopped, and keeps the inline far form, which costs no extra branch.
    // ==================================================================

    static constexpr unsigned JUMP_SLOT_INSNS = 8;
    static constexpr uint32_t PPC_NOP = 0x60000000u;             // ori 0,0,0
    // Distinctive never-executed markers (ori 0,0,imm forms).
    static constexpr uint32_t MARKER_JUMP = 0x6000FB01u;
    static constexpr uint32_t MARKER_CALL = 0x6000FB02u;
    static constexpr uint32_t MARKER_BRANCH_TAIL = 0x6000FB03u;
    static constexpr uint32_t MARKER_JUMP_FIXED = 0x6000FB04u;
    static constexpr uint32_t MARKER_BRANCH_TAIL_FIXED = 0x6000FB05u;
    // Written over the marker once a slot has been recorded for linking, so
    // that linking the same slot twice is caught instead of corrupting the
    // compaction bookkeeping.
    static constexpr uint32_t MARKER_RECORDED = 0x6000FB06u;

    static constexpr RegisterID scratchRegister() { return PPC64Registers::r12; }

    // Static instruction constructors for patching.
    static constexpr uint32_t insnLis(RegisterID rt, uint16_t imm)  { return dForm(15, rt, PPC64Registers::r0, imm); }
    static constexpr uint32_t insnOri(RegisterID ra, RegisterID rs, uint16_t imm)  { return dForm(24, rs, ra, imm); }
    static constexpr uint32_t insnOris(RegisterID ra, RegisterID rs, uint16_t imm) { return dForm(25, rs, ra, imm); }
    static constexpr uint32_t insnRldicr32_31(RegisterID ra, RegisterID rs) { return mdForm(30, rs, ra, 32, 31, /*XO*/ 1, /*Rc*/ 0); }
    static constexpr uint32_t insnB(int32_t byteOffset, uint32_t lk) { return iForm(18, byteOffset, 0, lk); }
    static constexpr uint32_t insnBc(uint32_t bo, uint32_t bi, int32_t byteOffset) { return bForm(16, bo, bi, byteOffset, 0, 0); }
    static constexpr uint32_t insnMtctr(RegisterID rs) { return xfxForm(31, rs, SPR_CTR, /*XO*/ 467); }
    static constexpr uint32_t insnBctr()  { return xlForm(19, BO_ALWAYS, 0, 0, 528, 0); }
    static constexpr uint32_t insnBctrl() { return xlForm(19, BO_ALWAYS, 0, 0, 528, 1); }

    static bool isBInsn(uint32_t insn)  { return (insn >> 26) == 18; }
    static bool isBcInsn(uint32_t insn) { return (insn >> 26) == 16; }
    static bool isLisTo(uint32_t insn, RegisterID rt) { return (insn >> 16) == ((15u << 10) | (uint32_t(registerValue(rt)) << 5)); }

    static bool fitsBranch26(intptr_t offset) { return offset >= -(1 << 25) && offset < (1 << 25) && !(offset & 3); }
    static bool fitsBranch16(intptr_t offset) { return offset >= -(1 << 15) && offset < (1 << 15) && !(offset & 3); }

    // Write the 5-instruction li64(rt, value) into location[0..4].
    static void writeLi64(uint32_t* location, RegisterID rt, uint64_t value)
    {
        uint32_t words[5] = {
            insnLis(rt, uint16_t(value >> 48)),
            insnOri(rt, rt, uint16_t(value >> 32)),
            insnRldicr32_31(rt, rt),
            insnOris(rt, rt, uint16_t(value >> 16)),
            insnOri(rt, rt, uint16_t(value)),
        };
        machineCodeCopy<memcpyRepatch>(location, words, sizeof(words));
    }

    // Read the value from a 5-instruction li64 sequence.
    static uint64_t readLi64(const uint32_t* location)
    {
        return (uint64_t(location[0] & 0xFFFF) << 48)
             | (uint64_t(location[1] & 0xFFFF) << 32)
             | (uint64_t(location[3] & 0xFFFF) << 16)
             |  uint64_t(location[4] & 0xFFFF);
    }

    // --- Emission of unlinked slots -----------------------------------

    // A slot must keep its full size through linking when it may be
    // repatched later (patchableJump and friends), when some caller
    // overwrites the code around it in place assuming its pre-link size
    // (FixedSizeJumpScope), or when it starts inside a watchpoint's
    // replacement shadow (see label()). Every other slot that is linked
    // inside its own buffer is shrunk to its short form by branch compaction.
    bool nextJumpMustBeFixedSize() const
    {
        return m_fixedSizeJumpDepth || static_cast<int>(m_buffer.codeSize()) < m_indexOfTailOfLastWatchpoint;
    }

    void beginFixedSizeJumps() { ++m_fixedSizeJumpDepth; }
    void endFixedSizeJumps() { ASSERT(m_fixedSizeJumpDepth); --m_fixedSizeJumpDepth; }

    // Returns the label of the slot start; Jump carries it.
    AssemblerLabel emitUnlinkedJump()
    {
        AssemblerLabel result = m_buffer.label();
        insn(nextJumpMustBeFixedSize() ? MARKER_JUMP_FIXED : MARKER_JUMP);
        for (unsigned i = 1; i < JUMP_SLOT_INSNS; ++i)
            insn(PPC_NOP);
        return result;
    }

    // bo/bi describe the CR0 condition to branch on when TAKEN.
    AssemblerLabel emitUnlinkedBranch(uint32_t bo, uint32_t bi)
    {
        AssemblerLabel result = m_buffer.label();
        bool fixedSize = nextJumpMustBeFixedSize();
        insn(insnBc(bo, bi, 0));            // offset 0 = unlinked
        insn(fixedSize ? MARKER_BRANCH_TAIL_FIXED : MARKER_BRANCH_TAIL);
        for (unsigned i = 2; i < JUMP_SLOT_INSNS; ++i)
            insn(PPC_NOP);
        return result;
    }

    // Returns the label AFTER the slot (= the return address), which is
    // JSC's convention for Call labels; getCallReturnOffset uses it.
    AssemblerLabel emitUnlinkedCall(bool threadSafe = false)
    {
        insn(MARKER_CALL);
        for (unsigned i = 1; i < JUMP_SLOT_INSNS - 2; ++i)
            insn(PPC_NOP);
        insn(threadSafe ? THREAD_SAFE_CALL_TAG : PPC_NOP); // see applyNearCallSlot
        insn(insnBctrl());                  // placeholder; rewritten at link
        return m_buffer.label();
    }

    // --- In-place slot rewriters ---------------------------------------

    // Rewrite a slot in place. Words other than the branch are written
    // first, and only where they change; the branch word goes last, as one
    // aligned 4-byte store. Retargeting a b/bl therefore writes exactly one
    // word, which the Power ISA allows under a concurrently executing thread
    // (b/bl/nop are patch class). When other words change too, they are
    // flushed to instruction storage (dcbst/sync/icbi/isync) before the branch
    // word is written, so no processor can fetch the new branch word with the
    // old stanza behind it: a near -> far change is atomic. A far -> far
    // change still rewrites a live stanza and is only safe where no other
    // thread runs the slot.
    static void writeSlot(uint32_t* location, const uint32_t (&words)[JUMP_SLOT_INSNS], unsigned branchIndex)
    {
        RELEASE_ASSERT(roundUpToMultipleOf<sizeof(uint32_t)>(location) == location);
        bool wroteStanza = false;
        for (unsigned i = 0; i < JUMP_SLOT_INSNS; ++i) {
            if (i != branchIndex && location[i] != words[i]) {
                writeWord(&location[i], words[i]);
                wroteStanza = true;
            }
        }
        if (wroteStanza)
            cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
        if (location[branchIndex] != words[branchIndex])
            writeWord(&location[branchIndex], words[branchIndex]);
    }

    static void writeWord(uint32_t* location, uint32_t word)
    {
        static constexpr RepatchingInfo memcpyRepatchAtomic = RepatchingInfo { RepatchingFlag::Memcpy, RepatchingFlag::Atomic };
        machineCodeCopy<memcpyRepatchAtomic>(location, &word, sizeof(word));
    }

    // Unconditional jump slots (jumps, patchable jumps, near tail calls):
    //   near: [0] b target           [1..7] left as they are (never reached)
    //   far:  [0] nop  [1..5] li64 r12,target  [6] mtctr r12  [7] bctr
    // Word [0] is always a nop or a b, so moving to a target a b reaches --
    // or to a jump island, see prepareForAtomicRelinkJumpConcurrently -- is
    // the single store of [0], from either form. Moving to the far form
    // writes and flushes the stanza while [0] still holds the old b, then
    // [0] = nop, so a near -> far change is atomic as well (see writeSlot);
    // only far -> far rewrites a live stanza.
    //
    // The slot writers take the address the slot will execute at separately
    // from the address they write to: branch compaction links into a staging
    // buffer that is copied to executable memory afterwards.
    static void computeJumpSlot(uint32_t (&words)[JUMP_SLOT_INSNS], const uint32_t* executableAt, const void* to)
    {
        intptr_t offset = intptr_t(to) - intptr_t(executableAt);
        for (auto& w : words) w = PPC_NOP;
        if (fitsBranch26(offset))
            words[0] = insnB(int32_t(offset), 0);
        else {
            words[1] = insnLis(scratchRegister(), uint16_t(uint64_t(to) >> 48));
            words[2] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 32));
            words[3] = insnRldicr32_31(scratchRegister(), scratchRegister());
            words[4] = insnOris(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 16));
            words[5] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to)));
            words[6] = insnMtctr(scratchRegister());
            words[7] = insnBctr();
        }
    }

    static void applyJumpSlot(uint32_t* location, void* to)
    {
        uint32_t words[JUMP_SLOT_INSNS];
        computeJumpSlot(words, location, to);
        if (isBInsn(words[0]))
            writeWord(location, words[0]);
        else
            writeSlot(location, words, 0);
    }

    // Extract (bo, bi) from a conditional slot in ANY of its states.
    // Unlinked/short state: [0] is the bc carrying the real condition.
    // Inverted states: [0] is bc with BO bit 3 (branch-true/false sense)
    // flipped, skipping the stanza.
    static bool extractBranchCondition(const uint32_t* location, uint32_t& bo, uint32_t& bi)
    {
        if (!isBcInsn(location[0]))
            return false;
        bo = (location[0] >> 21) & 0x1F;
        bi = (location[0] >> 16) & 0x1F;
        int32_t bd = int32_t(int16_t(location[0] & 0xFFFC));
        // Inverted iff the bc jumps just past this slot's stanza (offset
        // is small positive and the following insn is not a nop-marker
        // tail): normalize by re-inverting.
        if (bd == int32_t(JUMP_SLOT_INSNS * sizeof(uint32_t)) && (isBInsn(location[1]) || isLisTo(location[1], scratchRegister())))
            bo ^= 0x8;
        return true;
    }

    static void computeBranchSlot(uint32_t (&words)[JUMP_SLOT_INSNS], const uint32_t* executableAt, uint32_t bo, uint32_t bi, const void* to)
    {
        intptr_t offset = intptr_t(to) - intptr_t(executableAt);
        for (auto& w : words) w = PPC_NOP;
        if (fitsBranch16(offset))
            words[0] = insnBc(bo, bi, int32_t(offset));
        else {
            // Inverted bc skips the whole slot; stanza does the far jump.
            words[0] = insnBc(bo ^ 0x8, bi, JUMP_SLOT_INSNS * sizeof(uint32_t));
            intptr_t offset1 = intptr_t(to) - intptr_t(executableAt + 1);
            if (fitsBranch26(offset1))
                words[1] = insnB(int32_t(offset1), 0);
            else {
                words[1] = insnLis(scratchRegister(), uint16_t(uint64_t(to) >> 48));
                words[2] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 32));
                words[3] = insnRldicr32_31(scratchRegister(), scratchRegister());
                words[4] = insnOris(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 16));
                words[5] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to)));
                words[6] = insnMtctr(scratchRegister());
                words[7] = insnBctr();
            }
        }
    }

    static void applyBranchSlot(uint32_t* location, void* to)
    {
        uint32_t bo, bi;
        bool ok = extractBranchCondition(location, bo, bi);
        RELEASE_ASSERT(ok);
        uint32_t words[JUMP_SLOT_INSNS];
        computeBranchSlot(words, location, bo, bi, to);
        // Retargeting within the bc + b form writes only the b in [1]. Other
        // changes of form write [0] and the stanza and are not atomic;
        // conditional slots are never repatched while another thread may run
        // them (only wasm tier-up does that, on calls and tail calls).
        writeSlot(location, words, isBInsn(words[1]) ? 1 : 0);
    }

    // Call slots. location = slot START (callLabel - 8 insns); the branch
    // stays in [7] so the return address is always location + 32.
    //
    // call() form, for C functions and for any target out of b reach:
    //   [0..4] li64 r12,target  [5] nop      [6] nop  [7] bl target
    //   [0..4] li64 r12,target  [5] mtctr r12 [6] nop  [7] bctrl
    // nearCall() form, for JIT code in b reach:
    //   [0..6] nops                                    [7] bl target
    // JIT code never reads r12 on entry -- only an ELFv2 C function computes
    // its TOC from it -- so a near call need not materialise it, and
    // retargeting it between JIT targets is the single store of [7].
    //
    // threadSafePatchableNearCall() (wasm calls, which tier-up retargets while
    // other threads run them) tags [6] with an executed no-op, `oris 0,0,0`,
    // and never takes the far form: a target out of b reach is called through
    // a jump island, so the slot keeps its single bl.
    static constexpr uint32_t THREAD_SAFE_CALL_TAG = 0x64000000u; // oris 0,0,0

    static void applyCallSlot(uint32_t* location, void* to)
    {
        intptr_t offset = intptr_t(to) - intptr_t(location + 7);
        uint32_t words[JUMP_SLOT_INSNS];
        for (auto& w : words) w = PPC_NOP;
        // Materialize r12 = target even for a relative call: an ELFv2 function
        // entered at its global entry point computes its TOC pointer from r12.
        words[0] = insnLis(scratchRegister(), uint16_t(uint64_t(to) >> 48));
        words[1] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 32));
        words[2] = insnRldicr32_31(scratchRegister(), scratchRegister());
        words[3] = insnOris(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 16));
        words[4] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to)));
        if (fitsBranch26(offset))
            words[7] = insnB(int32_t(offset), 1);   // bl target (relative)
        else {
            words[5] = insnMtctr(scratchRegister());
            words[7] = insnBctrl();
        }
        writeSlot(location, words, 7);
    }

    static void applyNearCallSlot(uint32_t* location, void* to)
    {
        bool threadSafe = location[6] == THREAD_SAFE_CALL_TAG;
        void* target = nullptr;
        if (isJITPC(to)) {
            if (fitsBranch26(intptr_t(to) - intptr_t(location + 7)))
                target = to;
#if ENABLE(JUMP_ISLANDS)
            else if (threadSafe && isJITPC(location)) {
                // Not concurrent (the code is not running yet, or the caller
                // owns it): this replaces islands made earlier for the site.
                target = ExecutableAllocator::singleton().getJumpIslandToUsingMemcpy(location + 7, to);
                RELEASE_ASSERT(fitsBranch26(intptr_t(target) - intptr_t(location + 7)));
            }
#endif
        }
        if (!target) {
            RELEASE_ASSERT(!threadSafe || !isJITPC(to));
            applyCallSlot(location, to);
            return;
        }
        uint32_t words[JUMP_SLOT_INSNS];
        for (auto& w : words) w = PPC_NOP;
        if (threadSafe)
            words[6] = THREAD_SAFE_CALL_TAG;
        words[7] = insnB(int32_t(intptr_t(target) - intptr_t(location + 7)), 1);
        writeSlot(location, words, 7);
    }

    // Dispatch: a jump-flavored location can be an unconditional slot (its
    // [0] is a marker, b, lis-r12, or nop) or a conditional slot ([0] is bc).
    static void applyJumpOrBranchSlot(uint32_t* location, void* to)
    {
        if (isBcInsn(location[0]))
            applyBranchSlot(location, to);
        else
            applyJumpSlot(location, to);
    }

    // Patchable-jump sizes: the full 8-instruction slot.
    static constexpr ptrdiff_t maxJumpReplacementSize() { return JUMP_SLOT_INSNS * sizeof(uint32_t); }
    static constexpr ptrdiff_t patchableJumpSize()      { return JUMP_SLOT_INSNS * sizeof(uint32_t); }

    template<RepatchingInfo copy = memcpyRepatch>
    static void fillNops(void* base, size_t size)
    {
        uint32_t* ptr = static_cast<uint32_t*>(base);
        RELEASE_ASSERT(roundUpToMultipleOf<sizeof(uint32_t)>(ptr) == ptr);
        RELEASE_ASSERT(!(size % sizeof(uint32_t)));
        uint32_t nop = 0x60000000u;
        for (size_t i = 0, n = size / sizeof(uint32_t); i < n; ++i)
            machineCodeCopy<copy>(&ptr[i], &nop, sizeof(uint32_t));
    }

    // ==================================================================
    // Branch compaction (ENABLE(BRANCH_COMPACTION), as on ARM64).
    //
    // A jump or branch linked inside its own buffer (Jump::link/linkTo) or to
    // a thunk (Jump::linkThunk) is not written into its slot here. It is
    // recorded, and LinkBuffer::copyCompactAndLinkCode shrinks the slot to the
    // shortest form that reaches the target while copying the code out:
    //
    //   LinkJumpShort    b target                       4 bytes
    //   LinkBranchShort  bc target                      4 bytes  (±32 KB)
    //   LinkBranchMedium bc !cond,+8 ; b target         8 bytes  (±32 MB)
    //   LinkFull         the whole 8-instruction slot  32 bytes
    //
    // Slots marked fixed-size (patchable ones, see nextJumpMustBeFixedSize)
    // always get LinkFull. A LinkRecord's from() is the END of its slot, which
    // is what the generic compaction loop expects: the short form is written
    // so that it ends where the slot did.
    // ==================================================================

    enum JumpType : uint8_t {
        JumpNoCondition,
        JumpCondition,
        JumpNoConditionFixedSize,
        JumpConditionFixedSize,
    };

    enum JumpLinkType : uint8_t {
        LinkInvalid,
        LinkJumpShort,
        LinkBranchShort,
        LinkBranchMedium,
        LinkFull,
    };

    class LinkRecord {
    public:
        LinkRecord(intptr_t from, intptr_t to, JumpType type, uint32_t bo, uint32_t bi, bool isThunk)
            : m_from(from)
            , m_to(to)
            , m_type(type)
            , m_bo(static_cast<uint8_t>(bo))
            , m_bi(static_cast<uint8_t>(bi))
            , m_isThunk(isThunk)
        {
        }

        intptr_t from() const { return m_from; }
        void setFrom(const PPC64Assembler*, intptr_t from) { m_from = from; }
        intptr_t to(const PPC64Assembler*) const { return m_to; }
        JumpType type() const { return m_type; }
        JumpLinkType linkType() const { return m_linkType; }
        void setLinkType(JumpLinkType linkType) { ASSERT(m_linkType == LinkInvalid); m_linkType = linkType; }
        uint32_t bo() const { return m_bo; }
        uint32_t bi() const { return m_bi; }
        bool isThunk() const { return m_isThunk; }

    private:
        intptr_t m_from;
        intptr_t m_to;
        JumpType m_type;
        JumpLinkType m_linkType { LinkInvalid };
        uint8_t m_bo;
        uint8_t m_bi;
        bool m_isThunk;
    };

    Vector<LinkRecord, 0, UnsafeVectorOverflow>& jumpsToLink() LIFETIME_BOUND
    {
        std::ranges::sort(m_jumpsToLink, { }, &LinkRecord::from);
        return m_jumpsToLink;
    }

    static constexpr bool canCompact(JumpType type)
    {
        return type == JumpNoCondition || type == JumpCondition;
    }

    static constexpr int linkSize(JumpLinkType linkType)
    {
        switch (linkType) {
        case LinkJumpShort:
        case LinkBranchShort:
            return sizeof(uint32_t);
        case LinkBranchMedium:
            return 2 * sizeof(uint32_t);
        case LinkInvalid:
        case LinkFull:
            break;
        }
        return JUMP_SLOT_INSNS * sizeof(uint32_t);
    }

    static int jumpSizeDelta(JumpType, JumpLinkType linkType)
    {
        return JUMP_SLOT_INSNS * sizeof(uint32_t) - linkSize(linkType);
    }

    // `from` is the end of the full-size slot in executable memory, before
    // this slot is compacted; the branch itself will end up somewhere in
    // [from - 32, from - 4]. A forward `to` is an upper bound (later slots
    // can only shrink), a backward one is exact. Require the short form to
    // reach from both ends of that window.
    static JumpLinkType computeJumpType(LinkRecord& record, const uint8_t* from, const uint8_t* to)
    {
        JumpLinkType linkType = LinkFull;
        if (canCompact(record.type())) {
            intptr_t nearest = intptr_t(to) - intptr_t(from - sizeof(uint32_t));
            intptr_t farthest = intptr_t(to) - intptr_t(from - JUMP_SLOT_INSNS * sizeof(uint32_t));
            if (record.type() == JumpNoCondition) {
                if (fitsBranch26(nearest) && fitsBranch26(farthest))
                    linkType = LinkJumpShort;
            } else if (fitsBranch16(nearest) && fitsBranch16(farthest))
                linkType = LinkBranchShort;
            else if (fitsBranch26(nearest) && fitsBranch26(farthest))
                linkType = LinkBranchMedium;
        }
        record.setLinkType(linkType);
        return linkType;
    }

    // `from` is where the (possibly compacted) slot ends in the buffer being
    // written, `fromInstruction` where it ends in executable memory.
    template<RepatchingInfo copy>
    static void link(LinkRecord& record, uint8_t* from, const uint8_t* fromInstruction, uint8_t* to)
    {
        JumpLinkType linkType = record.linkType();
        if (linkType == LinkInvalid)
            linkType = LinkFull; // Compaction was off for this LinkBuffer.
        int size = linkSize(linkType);
        uint32_t* writeAt = reinterpret_cast<uint32_t*>(from - size);
        const uint32_t* executableAt = reinterpret_cast<const uint32_t*>(fromInstruction - size);
        bool isBranch = record.type() == JumpCondition || record.type() == JumpConditionFixedSize;
        uint32_t words[JUMP_SLOT_INSNS];
        switch (linkType) {
        case LinkJumpShort: {
            intptr_t offset = intptr_t(to) - intptr_t(executableAt);
            RELEASE_ASSERT(!isBranch && fitsBranch26(offset));
            words[0] = insnB(int32_t(offset), 0);
            break;
        }
        case LinkBranchShort: {
            intptr_t offset = intptr_t(to) - intptr_t(executableAt);
            RELEASE_ASSERT(isBranch && fitsBranch16(offset));
            words[0] = insnBc(record.bo(), record.bi(), int32_t(offset));
            break;
        }
        case LinkBranchMedium: {
            intptr_t offset = intptr_t(to) - intptr_t(executableAt + 1);
            RELEASE_ASSERT(isBranch && fitsBranch26(offset));
            words[0] = insnBc(record.bo() ^ 0x8, record.bi(), 2 * sizeof(uint32_t));
            words[1] = insnB(int32_t(offset), 0);
            break;
        }
        case LinkInvalid:
        case LinkFull:
            if (isBranch)
                computeBranchSlot(words, executableAt, record.bo(), record.bi(), to);
            else
                computeJumpSlot(words, executableAt, to);
            break;
        }
        machineCodeCopy<copy>(writeAt, words, size);
    }

    // In-buffer link (both labels inside m_buffer; used by Jump::link).
    void linkJump(AssemblerLabel from, AssemblerLabel to)
    {
        RELEASE_ASSERT(from.isSet() && to.isSet());
        recordJump(from, to.offset(), /* isThunk */ false);
    }

    // Jump::linkThunk: the target is outside the buffer but already known.
    void linkJumpThunk(AssemblerLabel from, void* to)
    {
        RELEASE_ASSERT(from.isSet());
        recordJump(from, reinterpret_cast<intptr_t>(to), /* isThunk */ true);
    }

    void recordJump(AssemblerLabel from, intptr_t to, bool isThunk)
    {
        uint32_t* slot = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(m_buffer.data()) + from.offset());
        intptr_t end = from.offset() + JUMP_SLOT_INSNS * sizeof(uint32_t);
        if (isBcInsn(slot[0])) {
            RELEASE_ASSERT(slot[1] == MARKER_BRANCH_TAIL || slot[1] == MARKER_BRANCH_TAIL_FIXED);
            JumpType type = slot[1] == MARKER_BRANCH_TAIL ? JumpCondition : JumpConditionFixedSize;
            uint32_t bo = (slot[0] >> 21) & 0x1F;
            uint32_t bi = (slot[0] >> 16) & 0x1F;
            slot[1] = MARKER_RECORDED;
            m_jumpsToLink.append(LinkRecord(end, to, type, bo, bi, isThunk));
            return;
        }
        RELEASE_ASSERT(slot[0] == MARKER_JUMP || slot[0] == MARKER_JUMP_FIXED);
        JumpType type = slot[0] == MARKER_JUMP ? JumpNoCondition : JumpNoConditionFixedSize;
        slot[0] = MARKER_RECORDED;
        m_jumpsToLink.append(LinkRecord(end, to, type, 0, 0, isThunk));
    }

    static void linkJump(void* code, AssemblerLabel from, void* to)
    {
        RELEASE_ASSERT(from.isSet());
        uint32_t* location = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(code) + from.offset());
        applyJumpOrBranchSlot(location, to);
    }

    // `from` is the label AFTER the call slot (the return address).
    static void linkCall(void* code, AssemblerLabel from, void* to)
    {
        RELEASE_ASSERT(from.isSet());
        uint32_t* location = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(code) + from.offset()) - JUMP_SLOT_INSNS;
        applyCallSlot(location, to);
    }

    static void linkNearCall(void* code, AssemblerLabel from, void* to)
    {
        RELEASE_ASSERT(from.isSet());
        uint32_t* location = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(code) + from.offset()) - JUMP_SLOT_INSNS;
        applyNearCallSlot(location, to);
    }

    // A DataLabelPtr marks the start of the 5-instruction li64 emitted by
    // MacroAssemblerPPC64::moveWithPatch, as repatchPointer() assumes.
    static void linkPointer(void* code, AssemblerLabel where, void* valuePtr)
    {
        uint32_t* location = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(code) + where.offset());
        RegisterID rt = RegisterID((location[0] >> 21) & 0x1F);
        writeLi64(location, rt, uint64_t(valuePtr));
    }

    static void relinkJump(void* from, void* to)
    {
        uint32_t* location = static_cast<uint32_t*>(from);
        applyJumpOrBranchSlot(location, to);
        cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
    }

    // `from` is the return address of a near call slot
    // (AbstractMacroAssembler::repatchNearCall).
    static void relinkCall(void* from, void* to)
    {
        uint32_t* location = static_cast<uint32_t*>(from) - JUMP_SLOT_INSNS;
        applyNearCallSlot(location, to);
        cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
    }

    // `from` is the return address of a call() slot
    // (MacroAssemblerPPC64::repatchCall).
    static void relinkCCall(void* from, void* to)
    {
        uint32_t* location = static_cast<uint32_t*>(from) - JUMP_SLOT_INSNS;
        applyCallSlot(location, to);
        cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
    }

    static void relinkTailCall(void* from, void* to)
    {
        relinkJump(from, to);
    }

#if ENABLE(JUMP_ISLANDS)
    // Wasm tier-up retargets near calls and tail calls that other threads may
    // be running. The island must exist before the branch is rewritten, and
    // islands already reachable from the site must survive (a thread may be
    // in one), so this allocates fresh ones without freeing; the repatch that
    // follows then writes a single b/bl to the returned address.
    // `from` is the branch itself: a tail call's jump slot start.
    static void* prepareForAtomicRelinkJumpConcurrently(void* from, void* to)
    {
        if (fitsBranch26(intptr_t(to) - intptr_t(from)) || !isJITPC(from) || !isJITPC(to))
            return to;
        void* island = ExecutableAllocator::singleton().getJumpIslandToConcurrently(from, to);
        RELEASE_ASSERT(fitsBranch26(intptr_t(island) - intptr_t(from)));
        return island;
    }

    // `from` is the return address; the bl is the slot's last word.
    static void* prepareForAtomicRelinkCallConcurrently(void* from, void* to)
    {
        return prepareForAtomicRelinkJumpConcurrently(static_cast<uint32_t*>(from) - 1, to);
    }
#endif

    // Jump island hooks for ExecutableAllocator. A PPC64 island is a whole
    // jump slot (JUMP_SLOT_INSNS words): `b target` when that reaches, else
    // li64 r12,target; mtctr r12; bctr. One island therefore reaches any
    // target, so ExecutableAllocator never chains them (ARM64 chains single
    // b's region by region, which in a 1 GB pool of 28 MB regions would be
    // up to 36 taken branches per call). The far form clobbers r12 and CTR,
    // exactly as the far slot form at the site always has: r12 is the
    // MacroAssembler scratch and CTR is never live across a jump, call or
    // tail call. An island is written before any branch to it is published
    // and is never rewritten while reachable (concurrent repatching
    // allocates fresh ones), so it need not be patch-class.
    static constexpr size_t jumpIslandSize = JUMP_SLOT_INSNS * sizeof(uint32_t);

    static bool canEmitJump(void*, void*)
    {
        return true;
    }

    template<RepatchingInfo repatch>
    static void fillNearTailCall(void* from, void* to)
    {
        static_assert((*repatch).contains(RepatchingFlag::Flush));
        uint32_t* location = static_cast<uint32_t*>(from);
        RELEASE_ASSERT(roundUpToMultipleOf<sizeof(uint32_t)>(location) == location);
        uint32_t words[JUMP_SLOT_INSNS];
        for (auto& w : words) w = PPC_NOP;
        intptr_t offset = intptr_t(to) - intptr_t(location);
        if (fitsBranch26(offset))
            words[0] = insnB(int32_t(offset), 0);
        else {
            words[0] = insnLis(scratchRegister(), uint16_t(uint64_t(to) >> 48));
            words[1] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 32));
            words[2] = insnRldicr32_31(scratchRegister(), scratchRegister());
            words[3] = insnOris(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to) >> 16));
            words[4] = insnOri(scratchRegister(), scratchRegister(), uint16_t(uint64_t(to)));
            words[5] = insnMtctr(scratchRegister());
            words[6] = insnBctr();
        }
        machineCodeCopy<noFlush(repatch)>(location, words, sizeof(words));
        cacheFlush(location, sizeof(words));
    }

    // `where` points AT a 5-instruction li64 sequence.
    static void repatchPointer(void* where, void* valuePtr)
    {
        uint32_t* location = static_cast<uint32_t*>(where);
        RegisterID rt = RegisterID((location[0] >> 21) & 0x1F);
        writeLi64(location, rt, uint64_t(valuePtr));
        cacheFlush(location, 5 * sizeof(uint32_t));
    }

    static void* readPointer(void* where)
    {
        return reinterpret_cast<void*>(readLi64(static_cast<uint32_t*>(where)));
    }

    // `from` is the return address of a call slot; target only readable
    // from the far (li64) form — near bl calls are decoded from the offset.
    static void* readCallTarget(void* from)
    {
        uint32_t* location = static_cast<uint32_t*>(from) - JUMP_SLOT_INSNS;
        if (isLisTo(location[0], scratchRegister()))
            return reinterpret_cast<void*>(readLi64(location));
        RELEASE_ASSERT(isBInsn(location[7]));
        int32_t liField = int32_t(location[7] << 6) >> 6;           // sign-extend 26-bit
        intptr_t offset = intptr_t(liField & ~3);
        return reinterpret_cast<uint8_t*>(location + 7) + offset;
    }

    // Signal-based VM traps: VMTraps installs this at every DFG/FTL
    // invalidation point of the code block on top of the stack and catches
    // the fault with its Signal::AccessFault (SIGSEGV/SIGBUS) handler, which
    // matches the faulting PC against the installed sites. It must therefore
    // fault with SIGSEGV, not trap: `trap` (tw 31,0,0) raises SIGTRAP, which
    // nothing handles. `ld r0,0(0)` loads from absolute address 0 (RA=0 means
    // a zero base, not r0), which is never mapped (vm.mmap_min_addr), and it
    // writes no memory — like ARM64's `dc zva` of address 0 and RISCV64's
    // `sd zero,0(zero)`.
    //
    // The word is written while the only thread that runs this code (the
    // VM's mutator) is suspended (VMTraps::SignalSender via sendMessage), and
    // the kernel's return to that thread is context synchronizing, so the
    // replaced instruction need not be of the concurrent-modification class.
    static void replaceWithVMHalt(void* where)
    {
        uint32_t* location = static_cast<uint32_t*>(where);
        RELEASE_ASSERT(roundUpToMultipleOf<sizeof(uint32_t)>(location) == location);
        uint32_t halt = 0xE8000000u;        // ld r0,0(0)
        machineCodeCopy<memcpyRepatch>(location, &halt, sizeof(uint32_t));
        cacheFlush(location, sizeof(uint32_t));
    }

    static void replaceWithJump(void* from, void* to)
    {
        // Every replacement site has maxJumpReplacementSize bytes that stay
        // that size through branch compaction: watchpoint labels pad the
        // following label past it and keep the slots inside it at full size,
        // and the other users (inline caches, JIT math ICs, direct tail calls)
        // reserve it with nops or fixed-size jumps. Write only as much as the
        // target needs, though: a plain b leaves the rest of the shadow alone.
        uint32_t* location = static_cast<uint32_t*>(from);
        intptr_t offset = intptr_t(to) - intptr_t(location);
        if (fitsBranch26(offset)) {
            writeWord(location, insnB(int32_t(offset), 0));
            cacheFlush(location, sizeof(uint32_t));
            return;
        }
        applyJumpSlot(location, to);
        cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
    }

    static void revertJumpReplacementToPatch(void* from, void* valuePtr)
    {
        // Restore the li64 that replaceWithJump overwrote (branchPtrWithPatch sites).
        uint32_t* location = static_cast<uint32_t*>(from);
        RegisterID rt = isLisTo(location[0], scratchRegister()) ? scratchRegister() : RegisterID((location[0] >> 21) & 0x1F);
        uint32_t nops[JUMP_SLOT_INSNS];
        for (auto& w : nops) w = PPC_NOP;
        machineCodeCopy<memcpyRepatch>(location, nops, sizeof(nops));
        writeLi64(location, rt, uint64_t(valuePtr));
        cacheFlush(location, JUMP_SLOT_INSNS * sizeof(uint32_t));
    }

    // Smallest of the D- and I-cache block sizes, from the kernel's auxv.
    // A step smaller than the real block size is always safe (it only issues
    // redundant dcbst/icbi); a larger one would skip blocks, so fall back low.
    static size_t cacheBlockSize()
    {
        static const size_t blockSize = [] () -> size_t {
            unsigned long dcache = 0;
            unsigned long icache = 0;
#if OS(LINUX)
            dcache = getauxval(AT_DCACHEBSIZE);
            icache = getauxval(AT_ICACHEBSIZE);
#endif
            return std::min(dcache ? static_cast<size_t>(dcache) : 32,
                icache ? static_cast<size_t>(icache) : 32);
        }();
        return blockSize;
    }

    static void cacheFlush(void* code, size_t size)
    {
        // Not __builtin___clear_cache: GCC compiles it to nothing on
        // powerpc64le, and executable memory is recycled, so stale instruction
        // cache blocks are a real possibility.
        //
        // The instruction cache is not coherent with the data cache: write the
        // data cache blocks back, sync, invalidate the instruction cache
        // blocks, sync, then isync to discard anything already prefetched.
        // Other processors that may run this code need their own isync; that
        // is WTF::crossModifyingCodeFence().
        if (!size)
            return;

        const size_t blockSize = cacheBlockSize();
        uintptr_t begin = reinterpret_cast<uintptr_t>(code) & ~(uintptr_t(blockSize) - 1);
        uintptr_t end = reinterpret_cast<uintptr_t>(code) + size;

        for (uintptr_t address = begin; address < end; address += blockSize)
            asm volatile("dcbst 0,%0" :: "r"(address) : "memory");
        asm volatile("sync" ::: "memory");
        for (uintptr_t address = begin; address < end; address += blockSize)
            asm volatile("icbi 0,%0" :: "r"(address) : "memory");
        asm volatile("sync" ::: "memory");
        asm volatile("isync" ::: "memory");
    }

private:
    AssemblerBuffer m_buffer;
    Vector<LinkRecord, 0, UnsafeVectorOverflow> m_jumpsToLink;
    int m_indexOfLastWatchpoint { INT_MIN };
    int m_indexOfTailOfLastWatchpoint { INT_MIN };
    unsigned m_fixedSizeJumpDepth { 0 };
};

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && CPU(PPC64)
