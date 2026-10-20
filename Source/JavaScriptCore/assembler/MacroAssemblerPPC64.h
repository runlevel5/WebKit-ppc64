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

#if !defined(_CALL_ELF) || _CALL_ELF != 2
#error "The PPC64 JIT supports the ELFv2 ABI only"
#endif

#include <JavaScriptCore/AbstractMacroAssembler.h>
#include <JavaScriptCore/PPC64Assembler.h>
#include <limits>
#include <JavaScriptCore/SIMDInfo.h>

// The semantic contract of each operation follows MacroAssemblerARM64.h.

namespace JSC {

using Assembler = TARGET_ASSEMBLER;

// PPC64_UNSUPPORTED: deliberately not provided; the method exists only so
// shared code compiles. Reaching one means a caller that should never route
// here on PPC64 did.
#define PPC64_UNSUPPORTED() do { \
        WTFLogAlways("PPC64 MacroAssembler unsupported on this platform (feature should have been gated off): %s", __PRETTY_FUNCTION__); \
        RELEASE_ASSERT_NOT_REACHED(); \
    } while (0)

class MacroAssemblerPPC64 : public AbstractMacroAssembler<Assembler> {
public:
    static constexpr unsigned numGPRs = 32;
    static constexpr unsigned numFPRs = 32;

    // The reach of a direct branch (b/bl: -32 MB .. +32 MB - 4). Every
    // patchable jump, branch and call is a full 8-instruction slot, linked to
    // a plain b/bl when that reaches and to an inline far form (li64 r12;
    // mtctr; bctr/bctrl) otherwise. Wasm calls and tail calls, which are
    // retargeted while other threads run them, reach out-of-range targets
    // through jump islands instead (ENABLE(JUMP_ISLANDS)), so that they
    // change by a single b/bl store; ExecutableAllocator splits the pool into
    // regions of this size for that. See PPC64Assembler's linking notes.
    static constexpr size_t nearJumpRange = 32 * MB;

    // r11 and r12 are volatile in ELFv2 and not allocated. f0 is the only FP
    // scratch: the other volatile FPRs, f1-f13, are all argument registers.
    static constexpr RegisterID dataTempRegister = PPC64Registers::r11;
    static constexpr RegisterID memoryTempRegister = PPC64Registers::r12;
    static constexpr FPRegisterID fpTempRegister = PPC64Registers::f0;

    static constexpr RegisterID InvalidGPRReg = PPC64Registers::InvalidGPRReg;

    // Branch compaction: see the comment above PPC64Assembler::JumpType.
    using LinkRecord = Assembler::LinkRecord;
    using JumpType = Assembler::JumpType;
    using JumpLinkType = Assembler::JumpLinkType;

    Vector<LinkRecord, 0, UnsafeVectorOverflow>& jumpsToLink() LIFETIME_BOUND { return m_assembler.jumpsToLink(); }
    static bool canCompact(JumpType jumpType) { return Assembler::canCompact(jumpType); }
    static JumpLinkType computeJumpType(LinkRecord& record, const uint8_t* from, const uint8_t* to) { return Assembler::computeJumpType(record, from, to); }
    static int jumpSizeDelta(JumpType jumpType, JumpLinkType jumpLinkType) { return Assembler::jumpSizeDelta(jumpType, jumpLinkType); }

    template<RepatchingInfo repatch>
    ALWAYS_INLINE static void link(LinkRecord& record, uint8_t* from, const uint8_t* fromInstruction, uint8_t* to) { return Assembler::link<repatch>(record, from, fromInstruction, to); }

    // While one of these is alive, every jump and branch is emitted at its
    // full 32-byte size and stays that size through linking. Code that will
    // later be overwritten in place by a jump slot, on the assumption that it
    // is at least as long as it was when it was emitted, needs this; so does
    // any jump that will be repatched (patchableJump and friends use it).
    class FixedSizeJumpScope {
        WTF_MAKE_NONCOPYABLE(FixedSizeJumpScope);
    public:
        explicit FixedSizeJumpScope(MacroAssemblerPPC64& masm)
            : m_masm(masm)
        {
            m_masm.m_assembler.beginFixedSizeJumps();
        }
        ~FixedSizeJumpScope() { m_masm.m_assembler.endFixedSizeJumps(); }
    private:
        MacroAssemblerPPC64& m_masm;
    };

    RegisterID scratchRegister()
    {
        RELEASE_ASSERT(m_allowScratchRegister);
        return dataTempRegister;
    }

    // ABI-mandated register roles.
    static constexpr RegisterID stackPointerRegister = PPC64Registers::sp;   // r1
    static constexpr RegisterID framePointerRegister = PPC64Registers::fp;   // r31
    // LR is an SPR, not a GPR. linkRegister is a never-allocated placeholder;
    // code that needs LR uses mflr/mtlr.
    static constexpr RegisterID linkRegister = PPC64Registers::r0;

    static bool supportsFloatingPointRounding() { return true; } // frip/frim/friz + xsrdpic
    // Power ISA level beyond the POWER8 (2.07) baseline, from AT_HWCAP2 and
    // capped by Options::maximumPPC64ISALevel() so POWER9/POWER10 machines can
    // also exercise the older sequences. Every instruction newer than 2.07 is
    // emitted only behind one of these checks.
    JS_EXPORT_PRIVATE static unsigned isaLevel();
    static bool supportsISA3_00() { return isaLevel() >= 300; }
    static bool supportsISA3_1() { return isaLevel() >= 310; }

    // binary16 conversions (xscvhpdp/xscvdphp) are Power ISA 3.0, so POWER9 and
    // later. On POWER8 this is false and Float16 stays on the C++ paths (the
    // DFG, the FTL and the typed-array inline caches all check it).
    static bool supportsFloat16() { return supportsISA3_00(); }
    // xsmaxjdp/xsminjdp (ISA 3.0) implement JS min/max directly (not wasm's:
    // see doubleMax()).
    static bool supportsFloatingPointMinMax() { return supportsISA3_00(); }

    // Opaque to callers; the branch emitters map them to (BO, BI) pairs.
    enum RelationalCondition : uint8_t {
        Equal,
        NotEqual,
        Above,
        AboveOrEqual,
        Below,
        BelowOrEqual,
        GreaterThan,
        GreaterThanOrEqual,
        LessThan,
        LessThanOrEqual,
    };

    static constexpr RelationalCondition invert(RelationalCondition cond)
    {
        switch (cond) {
        case Equal:              return NotEqual;
        case NotEqual:           return Equal;
        case Above:              return BelowOrEqual;
        case AboveOrEqual:       return Below;
        case Below:              return AboveOrEqual;
        case BelowOrEqual:       return Above;
        case GreaterThan:        return LessThanOrEqual;
        case GreaterThanOrEqual: return LessThan;
        case LessThan:           return GreaterThanOrEqual;
        case LessThanOrEqual:    return GreaterThan;
        }
        return Equal; // unreachable; silences -Wreturn-type
    }

    enum ResultCondition : uint8_t {
        Carry,
        Overflow,
        Signed,
        PositiveOrZero,
        Zero,
        NonZero,
    };

    enum ZeroCondition : uint8_t {
        IsZero,
        IsNonZero,
    };

    enum DoubleCondition : uint8_t {
        DoubleEqualAndOrdered,
        DoubleNotEqualAndOrdered,
        DoubleGreaterThanAndOrdered,
        DoubleGreaterThanOrEqualAndOrdered,
        DoubleLessThanAndOrdered,
        DoubleLessThanOrEqualAndOrdered,
        DoubleEqualOrUnordered,
        DoubleNotEqualOrUnordered,
        DoubleGreaterThanOrUnordered,
        DoubleGreaterThanOrEqualOrUnordered,
        DoubleLessThanOrUnordered,
        DoubleLessThanOrEqualOrUnordered,
    };

    void nop()
    {
        m_assembler.nop();
    }

    // tw 31,0,0: an unconditional trap (SIGTRAP).
    void breakpoint()
    {
        m_assembler.trap();
    }

    void move(RegisterID src, RegisterID dest)
    {
        if (src != dest)
            m_assembler.mr(dest, src);
    }

    // li, lis, or lis + ori (ori, unlike addi, does not sign-extend its
    // immediate).
    void move(TrustedImm32 imm, RegisterID dest)
    {
        // As with ARM64 `mov wN` / x86_64 `movl`, the upper 32 bits of dest are
        // zeroed, not sign-extended: JSValue boxing depends on it (move(-1) then
        // or64(numberTag) must give 0xFFFE0000FFFFFFFF). li/lis sign-extend, so
        // clear the upper word for negative values.
        int32_t v = imm.m_value;
        if (v >= INT16_MIN && v <= INT16_MAX) {
                m_assembler.addi(dest, PPC64Registers::r0, static_cast<int16_t>(v));
            if (v < 0)
                m_assembler.rldicl(dest, dest, 0, 32); // clrldi: zero upper 32
            return;
        }

        int16_t hi16 = static_cast<int16_t>(static_cast<uint32_t>(v) >> 16);
        uint16_t lo16 = static_cast<uint16_t>(v);

        if (lo16 == 0) {
            m_assembler.lis(dest, hi16);
            if (v < 0)
                m_assembler.rldicl(dest, dest, 0, 32);
            return;
        }

        m_assembler.lis(dest, hi16);
        m_assembler.ori(dest, dest, lo16);
        if (v < 0)
            m_assembler.rldicl(dest, dest, 0, 32);
    }

    // Shortest sequence (a user-space pointer below 2^47 takes four: li, sldi,
    // oris, ori). Nothing patches it in place; patchable pointers go through
    // moveWithPatch, which keeps the fixed 5-instruction li64.
    void move(TrustedImmPtr imm, RegisterID dest)
    {
        moveImmToScratch(static_cast<int64_t>(reinterpret_cast<uintptr_t>(imm.m_value)), dest);
    }

    void add64(RegisterID src, RegisterID dest)
    {
        m_assembler.add(dest, dest, src);
    }

    void add64(RegisterID src1, RegisterID src2, RegisterID dest)
    {
        m_assembler.add(dest, src1, src2);
    }

    // High half of the full-width product. mulhw/mulhwu leave the top 32 bits
    // of the target undefined, so the 32-bit forms re-extend (UMulHigh32's Air
    // form declares a zero-extended destination).
    void mulHigh32(RegisterID left, RegisterID right, RegisterID dest)
    {
        m_assembler.mulhw(dest, left, right);
        signExtend32ToPtr(dest, dest);
    }
    void uMulHigh32(RegisterID left, RegisterID right, RegisterID dest)
    {
        m_assembler.mulhwu(dest, left, right);
        zeroExtend32ToWordInternal(dest);
    }
    void mulHigh64(RegisterID left, RegisterID right, RegisterID dest)
    {
        m_assembler.mulhd(dest, left, right);
    }

    // 128-bit add/sub halves, with ARM64's carry convention (carry set = no
    // borrow on subtract), which XER.CA follows too. The low half must be
    // emitted immediately before the high half: nothing in between may touch
    // XER.CA. Each reads both sources before writing dest.
    void add64AndSetFlags(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.addc(dest, a, b); }
    void addCarry64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.adde(dest, a, b); }
    void sub64AndSetFlags(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.subfc(dest, b, a); }
    void subBorrow64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.subfe(dest, b, a); }
    // uMulHigh64 is defined with the other 64-bit multiplies below.

    // divw/divwu leave the upper half of the target undefined, so re-establish
    // the zero-extended-32 convention on the result.
    void div32(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        m_assembler.divw(dest, dividend, divisor);
        zeroExtend32ToWordInternal(dest);
    }
    void uDiv32(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        m_assembler.divwu(dest, dividend, divisor);
        zeroExtend32ToWordInternal(dest);
    }
    void div64(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        m_assembler.divd(dest, dividend, divisor);
    }
    void uDiv64(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        m_assembler.divdu(dest, dividend, divisor);
    }

    // Remainders: modsw/moduw/modsd/modud, ISA 3.0 only (supportsISA3_00()).
    // As with div, x % 0 and INT_MIN % -1 are undefined; callers guard them.
    void mod32(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        ASSERT(supportsISA3_00());
        m_assembler.modsw(dest, dividend, divisor);
        zeroExtend32ToWordInternal(dest);
    }
    void uMod32(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        ASSERT(supportsISA3_00());
        m_assembler.moduw(dest, dividend, divisor);
        zeroExtend32ToWordInternal(dest);
    }
    void mod64(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        ASSERT(supportsISA3_00());
        m_assembler.modsd(dest, dividend, divisor);
    }
    void uMod64(RegisterID dividend, RegisterID divisor, RegisterID dest)
    {
        ASSERT(supportsISA3_00());
        m_assembler.modud(dest, dividend, divisor);
    }

    void add64(TrustedImm32 imm, RegisterID dest)
    {
        add64(imm, dest, dest);
    }

    void add64(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        if (imm.m_value >= INT16_MIN && imm.m_value <= INT16_MAX) {
            m_assembler.addi(dest, src, static_cast<int16_t>(imm.m_value));
            return;
        }
        // Sign-extend the immediate: move(TrustedImm32) zero-extends, which
        // would turn a negative addend into +2^32.
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.add(dest, src, dataTempRegister);
    }

    // subf RT, RA, RB computes RB - RA: the operand order is reversed.
    void sub64(RegisterID src, RegisterID dest)
    {
        m_assembler.subf(dest, src, dest);
    }

    void sub64(RegisterID src1, RegisterID src2, RegisterID dest)
    {
        m_assembler.subf(dest, src2, src1);
    }

    void sub64(TrustedImm32 imm, RegisterID dest)
    {
        sub64(imm, dest, dest);
    }
    void add64(TrustedImm64 imm, RegisterID dest) { add64(imm, dest, dest); }
    void add64(TrustedImm64 imm, RegisterID src, RegisterID dest)
    {
        if (isInt16(imm.m_value))
            m_assembler.addi(dest, src, int16_t(imm.m_value));
        else {
            moveImmToScratch(imm.m_value, dataTempRegister);
            m_assembler.add(dest, src, dataTempRegister);
        }
    }
    void sub64(TrustedImm64 imm, RegisterID dest)
    {
        if (isInt16(-imm.m_value) && imm.m_value != INT64_MIN)
            m_assembler.addi(dest, dest, int16_t(-imm.m_value));
        else {
            moveImmToScratch(imm.m_value, dataTempRegister);
            m_assembler.subf(dest, dataTempRegister, dest);
        }
    }
    void xor64(TrustedImm64 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.xor_(dest, dest, dataTempRegister);
    }
    void and64(TrustedImm64 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.and_(dest, src, dataTempRegister);
    }
    void xor64(TrustedImm64 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.xor_(dest, src, dataTempRegister);
    }
    void or64(TrustedImm64 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.or_(dest, src, dataTempRegister);
    }
    void or64(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);   // sign-extend
        m_assembler.or_(dest, src, dataTempRegister);
    }
    void and64(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.and_(dest, src, dataTempRegister);
    }
    void xor64(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.xor_(dest, src, dataTempRegister);
    }

    void sub64(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        sub64(imm, src, dest);
    }

    void sub64(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        // addi with -imm; the lower bound excludes INT16_MIN, whose negation
        // does not fit.
        if (imm.m_value >= -INT16_MAX && imm.m_value <= INT16_MAX) {
            m_assembler.addi(dest, src, static_cast<int16_t>(-imm.m_value));
            return;
        }
        // Sign-extended, as in add64.
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.subf(dest, dataTempRegister, src);
    }

    // ld/std are DS-form: the displacement must be a multiple of 4. Other
    // offsets go through ldx/stdx with the offset in a scratch register.

    void load64(Address address, RegisterID dest)
    {
        int32_t offset = address.offset;
        if (offset >= INT16_MIN && offset <= INT16_MAX && (offset & 0x3) == 0) {
            m_assembler.ld(dest, static_cast<int16_t>(offset), address.base);
            return;
        }
        moveImmToScratch(int64_t(offset), memoryTempRegister);
        m_assembler.ldx(dest, address.base, memoryTempRegister);
    }

    void store64(RegisterID src, Address address)
    {
        int32_t offset = address.offset;
        if (offset >= INT16_MIN && offset <= INT16_MAX && (offset & 0x3) == 0) {
            m_assembler.std(src, static_cast<int16_t>(offset), address.base);
            return;
        }
        moveImmToScratch(int64_t(offset), memoryTempRegister);
        m_assembler.stdx(src, address.base, memoryTempRegister);
    }

    static constexpr int stackSlotSize = 8;

    void ret()
    {
        m_assembler.blr();
    }

    void push(RegisterID src)
    {
        m_assembler.stdu(src, -stackSlotSize, stackPointerRegister);
    }

    // There is no post-update load (ldu is pre-update), so pop is ld + addi.
    void pop(RegisterID dest)
    {
        m_assembler.ld(dest, 0, stackPointerRegister);
        m_assembler.addi(stackPointerRegister, stackPointerRegister, stackSlotSize);
    }

    // Conventions:
    // - r11 (dataTempRegister) and r12 (memoryTempRegister) are the scratches.
    // - Every 32-bit operation zero-extends its result to 64 bits, as x86
    //   32-bit operations and ARM64 Wn writes do (and as offlineasm's i-ops do).
    // - Compares set CR0 (LT = bit 0, GT = bit 1, EQ = bit 2); branches test
    //   one CR0 bit with BO = 12 (bit set) or BO = 4 (bit clear).

    static bool isInt16(int64_t v) { return v >= INT16_MIN && v <= INT16_MAX; }
    static bool isUInt16(int64_t v) { return v >= 0 && v <= 0xFFFF; }

    void zeroExtend32ToWordInternal(RegisterID reg)
    {
        m_assembler.rldicl(reg, reg, 0, 32);   // clrldi reg, reg, 32
    }

    // Materialise a 64-bit immediate in 1 to 5 instructions:
    //   int16:  li                                   (addi rt,0,si sign-extends)
    //   int32:  lis [; ori]                          (lis sign-extends)
    //   other:  <upper 32 bits as an int32, as above> ; sldi 32 [; oris] [; ori]
    // The upper word only has to be right in bits 0-31 before the sldi, which
    // shifts out whatever sign extension li/lis put above it; oris/ori then
    // fill the zeroed low word and are skipped when their halfword is 0.
    // E.g. 0x00007fff12345678 = li 0x7fff; sldi 32; oris 0x1234; ori 0x5678
    // and 0xfffe000000000000 = lis 0xfffe; sldi 32.
    void moveImmToScratch(int64_t value, RegisterID scratch)
    {
        auto moveInt32 = [&](int32_t v) {
            if (isInt16(v)) {
                m_assembler.addi(scratch, PPC64Registers::r0, int16_t(v));
                return;
            }
            m_assembler.lis(scratch, int16_t(uint16_t(uint32_t(v) >> 16)));
            if (uint16_t(v))
                m_assembler.ori(scratch, scratch, uint16_t(v));
        };
        if (value >= INT32_MIN && value <= INT32_MAX) {
            moveInt32(static_cast<int32_t>(value));
            return;
        }
        moveInt32(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32)));
        m_assembler.sldi(scratch, scratch, 32);
        if (uint16_t(uint64_t(value) >> 16))
            m_assembler.oris(scratch, scratch, uint16_t(uint64_t(value) >> 16));
        if (uint16_t(value))
            m_assembler.ori(scratch, scratch, uint16_t(value));
    }

    // Fixed-width 5-instruction li64 (never optimized), so the sequence can be
    // rewritten in place by PPC64Assembler::repatchPointer / linkPointer. This
    // is the emit-side twin of writeLi64 — used by the *WithPatch primitives.
    void moveFixedLi64(RegisterID dest, uint64_t value)
    {
        m_assembler.lis(dest, int16_t(uint16_t(value >> 48)));
        m_assembler.ori(dest, dest, uint16_t(value >> 32));
        m_assembler.rldicr(dest, dest, 32, 31);
        m_assembler.oris(dest, dest, uint16_t(value >> 16));
        m_assembler.ori(dest, dest, uint16_t(value));
    }

    // Returns a base register and a 16-bit displacement for a D-form access,
    // computing into `scratch` when the address does not fit one directly.
    struct ResolvedAddress {
        RegisterID base;
        int16_t offset;
    };

    ResolvedAddress resolveAddress(Address address, RegisterID scratch)
    {
        if (isInt16(address.offset) && address.base != PPC64Registers::r0)
            return { address.base, int16_t(address.offset) };
        moveImmToScratch(address.offset, scratch);
        m_assembler.add(scratch, scratch, address.base);
        return { scratch, 0 };
    }

    ResolvedAddress resolveAddress(BaseIndex address, RegisterID scratch)
    {
        if (address.scale) {
            m_assembler.sldi(scratch, address.index, address.scale);
            m_assembler.add(scratch, scratch, address.base);
        } else
            m_assembler.add(scratch, address.index, address.base);
        if (isInt16(address.offset))
            return { scratch, int16_t(address.offset) };
        // Large offset: materialize and add. (An addis high-adjust would
        // overflow int16 for offsets >= 0x7FFF8000.)
        moveImmToScratch(int64_t(address.offset), dataTempRegister);
        m_assembler.add(scratch, scratch, dataTempRegister);
        return { scratch, 0 };
    }

    // --- Condition plumbing --------------------------------------------

    struct BranchBits { uint32_t bo; uint32_t bi; };

    static BranchBits branchBitsFor(RelationalCondition cond)
    {
        // CR0 bit indices: 0=LT, 1=GT, 2=EQ.  bo 12 = branch if bit set,
        // bo 4 = branch if bit clear.  Signed vs unsigned is chosen by the
        // COMPARE (cmpw/cmpd vs cmplw/cmpld); the bits read the same.
        switch (cond) {
        case Equal:                 return { 12, 2 };
        case NotEqual:              return { 4,  2 };
        case Above:                 return { 12, 1 };
        case AboveOrEqual:          return { 4,  0 };
        case Below:                 return { 12, 0 };
        case BelowOrEqual:          return { 4,  1 };
        case GreaterThan:           return { 12, 1 };
        case GreaterThanOrEqual:    return { 4,  0 };
        case LessThan:              return { 12, 0 };
        case LessThanOrEqual:       return { 4,  1 };
        }
        RELEASE_ASSERT_NOT_REACHED();
        return { 20, 0 };
    }

    static bool isUnsignedCondition(RelationalCondition cond)
    {
        return cond == Above || cond == AboveOrEqual || cond == Below || cond == BelowOrEqual;
    }

    Jump makeBranch(RelationalCondition cond)
    {
        return Jump(m_assembler.emitUnlinkedBranch(branchBitsFor(cond).bo, branchBitsFor(cond).bi));
    }

    // Compare reg/reg or reg/imm at the given width, setting CR0.
    void emitCompare32(RelationalCondition cond, RegisterID left, RegisterID right)
    {
        if (isUnsignedCondition(cond))
            m_assembler.cmplw(0, left, right);
        else
            m_assembler.cmpw(0, left, right);
    }

    void emitCompare32(RelationalCondition cond, RegisterID left, TrustedImm32 right)
    {
        // The immediate scratch must not alias `left`: callers such as
        // branch32(Address, TrustedImm32) load the memory operand into
        // dataTempRegister and pass it as `left`, so materializing the
        // immediate there would clobber it before the compare.
        RegisterID scratch = (left == dataTempRegister) ? memoryTempRegister : dataTempRegister;
        if (isUnsignedCondition(cond)) {
            if (isUInt16(uint32_t(right.m_value)))
                m_assembler.cmplwi(0, left, uint16_t(right.m_value));
            else {
                moveImmToScratch(uint32_t(right.m_value), scratch);
                m_assembler.cmplw(0, left, scratch);
            }
        } else {
            if (isInt16(right.m_value))
                m_assembler.cmpwi(0, left, int16_t(right.m_value));
            else {
                moveImmToScratch(right.m_value, scratch);
                m_assembler.cmpw(0, left, scratch);
            }
        }
    }

    void emitCompare64(RelationalCondition cond, RegisterID left, RegisterID right)
    {
        if (isUnsignedCondition(cond))
            m_assembler.cmpld(0, left, right);
        else
            m_assembler.cmpd(0, left, right);
    }

    void emitCompare64(RelationalCondition cond, RegisterID left, TrustedImm64 right)
    {
        // See emitCompare32: the immediate scratch must not alias `left`.
        RegisterID scratch = (left == dataTempRegister) ? memoryTempRegister : dataTempRegister;
        if (isUnsignedCondition(cond)) {
            if (isUInt16(right.m_value))
                m_assembler.cmpldi(0, left, uint16_t(right.m_value));
            else {
                moveImmToScratch(right.m_value, scratch);
                m_assembler.cmpld(0, left, scratch);
            }
        } else {
            if (isInt16(right.m_value))
                m_assembler.cmpdi(0, left, int16_t(right.m_value));
            else {
                moveImmToScratch(right.m_value, scratch);
                m_assembler.cmpd(0, left, scratch);
            }
        }
    }

    // dest = 1 if CR0 bit `bi` is set (whenSet) or clear (!whenSet), else 0.
    // setbc on POWER10; otherwise isel, which reads the CR bit directly and is
    // much cheaper than mfcr (microcoded) + rlwinm. isel's RA = r0 means a
    // literal 0. Clobbers memoryTempRegister (or
    // dataTempRegister when dest is memoryTempRegister); every caller has
    // finished with them once CR0 is set.
    void setFromCR0Bit(unsigned bi, bool whenSet, RegisterID dest)
    {
        if (supportsISA3_1()) {
            if (whenSet)
                m_assembler.setbc(dest, bi);
            else
                m_assembler.setbcr(dest, bi);
            return;
        }
        if (whenSet) {
            RegisterID one = dest == memoryTempRegister ? dataTempRegister : memoryTempRegister;
            m_assembler.addi(one, PPC64Registers::r0, 1);   // li one, 1
            m_assembler.addi(dest, PPC64Registers::r0, 0);  // li dest, 0
            m_assembler.isel(dest, one, dest, bi);
        } else {
            m_assembler.addi(memoryTempRegister, PPC64Registers::r0, 1);
            m_assembler.isel(dest, PPC64Registers::r0, memoryTempRegister, bi);
        }
    }

    // Set dest = 0/1 from CR0 per cond (bo == 4 is the bit-clear sense).
    void setFromCondition(RelationalCondition cond, RegisterID dest)
    {
        BranchBits bits = branchBitsFor(cond);
        setFromCR0Bit(bits.bi, bits.bo != 4, dest);
    }

    // Set dest = 0/1 from CR0 for a test-style ResultCondition. CR0 must
    // already be set (by cmp against 0). Mirrors branchTest*Impl's bo/bi:
    // EQ bit (bi=2) for Zero/NonZero, LT bit (bi=0) for Signed/PositiveOrZero;
    // bo==4 is the bit-clear sense, inverted via xori.
    void setFromResultCondition(ResultCondition cond, RegisterID dest)
    {
        unsigned bi, bo;
        switch (cond) {
        case Zero:           bi = 2; bo = 12; break;
        case NonZero:        bi = 2; bo = 4;  break;
        case Signed:         bi = 0; bo = 12; break;
        case PositiveOrZero: bi = 0; bo = 4;  break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            bi = 2; bo = 12; break;
        }
        setFromCR0Bit(bi, bo != 4, dest);
    }

    // --- 32-bit arithmetic ---------------------------------------------

    void add32(RegisterID src, RegisterID dest) { add32(src, dest, dest); }
    void add32(RegisterID a, RegisterID b, RegisterID dest)
    {
        m_assembler.add(dest, a, b);
        zeroExtend32ToWordInternal(dest);
    }
    void add32(TrustedImm32 imm, RegisterID dest) { add32(imm, dest, dest); }
    void add32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        if (isInt16(imm.m_value))
            m_assembler.addi(dest, src, int16_t(imm.m_value));
        else {
            moveImmToScratch(imm.m_value, dataTempRegister);
            m_assembler.add(dest, src, dataTempRegister);
        }
        zeroExtend32ToWordInternal(dest);
    }

    void sub32(RegisterID src, RegisterID dest)
    {
        m_assembler.subf(dest, src, dest);
        zeroExtend32ToWordInternal(dest);
    }
    void sub32(RegisterID left, RegisterID right, RegisterID dest)
    {
        m_assembler.subf(dest, right, left);
        zeroExtend32ToWordInternal(dest);
    }
    void sub32(TrustedImm32 imm, RegisterID dest)
    {
        add32(TrustedImm32(-imm.m_value), dest);
    }

    void mul32(RegisterID src, RegisterID dest) { mul32(src, dest, dest); }
    void mul32(RegisterID a, RegisterID b, RegisterID dest)
    {
        m_assembler.mullw(dest, a, b);
        zeroExtend32ToWordInternal(dest);
    }
    void mul32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        mul32(dataTempRegister, src, dest);
    }

    // 64-bit multiply: low 64 bits (mulld) and unsigned high 64 bits (mulhdu).
    void mul64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.mulld(dest, a, b); }
    void mul64(RegisterID src, RegisterID dest) { mul64(src, dest, dest); }
    void uMulHigh64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.mulhdu(dest, a, b); }

    void neg32(RegisterID dest)
    {
        m_assembler.neg(dest, dest);
        zeroExtend32ToWordInternal(dest);
    }

    // --- 32-bit logical -------------------------------------------------

    void and32(RegisterID src, RegisterID dest) { and32(src, dest, dest); }
    void and32(RegisterID a, RegisterID b, RegisterID dest)
    {
        m_assembler.and_(dest, a, b);
        zeroExtend32ToWordInternal(dest);
    }
    void and32(TrustedImm32 imm, RegisterID dest) { and32(imm, dest, dest); }
    void and32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        if (isUInt16(uint32_t(imm.m_value))) {
            m_assembler.andi_(dest, src, uint16_t(imm.m_value)); // clears the upper bits
            return;
        }
        moveImmToScratch(uint32_t(imm.m_value), dataTempRegister);
        and32(dataTempRegister, src, dest);
    }

    void or32(RegisterID src, RegisterID dest) { or32(src, dest, dest); }
    void or32(RegisterID a, RegisterID b, RegisterID dest)
    {
        m_assembler.or_(dest, a, b);
        zeroExtend32ToWordInternal(dest);
    }
    void or32(TrustedImm32 imm, RegisterID dest) { or32(imm, dest, dest); }
    void or32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        if (isUInt16(uint32_t(imm.m_value)) && src == dest) {
            m_assembler.ori(dest, src, uint16_t(imm.m_value));
            zeroExtend32ToWordInternal(dest);
            return;
        }
        moveImmToScratch(uint32_t(imm.m_value), dataTempRegister);
        or32(dataTempRegister, src, dest);
    }

    void xor32(RegisterID src, RegisterID dest) { xor32(src, dest, dest); }
    void xor32(RegisterID a, RegisterID b, RegisterID dest)
    {
        m_assembler.xor_(dest, a, b);
        zeroExtend32ToWordInternal(dest);
    }
    void xor32(TrustedImm32 imm, RegisterID dest) { xor32(imm, dest, dest); }
    void xor32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        if (imm.m_value == -1) {
            m_assembler.nor(dest, src, src);
            zeroExtend32ToWordInternal(dest);
            return;
        }
        moveImmToScratch(uint32_t(imm.m_value), dataTempRegister);
        xor32(dataTempRegister, src, dest);
    }

    void not32(RegisterID dest)
    {
        m_assembler.nor(dest, dest, dest);
        zeroExtend32ToWordInternal(dest);
    }

    // --- 64-bit logical -------------------------------------------------

    void and64(RegisterID src, RegisterID dest) { m_assembler.and_(dest, src, dest); }
    void and64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.and_(dest, a, b); }
    void and64(TrustedImm32 imm, RegisterID dest)
    {
        if (isUInt16(uint32_t(imm.m_value)) && imm.m_value >= 0) {
            m_assembler.andi_(dest, dest, uint16_t(imm.m_value));
            return;
        }
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);   // sign-extended
        m_assembler.and_(dest, dataTempRegister, dest);
    }
    void and64(TrustedImm64 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.and_(dest, dataTempRegister, dest);
    }
    void and64(TrustedImmPtr imm, RegisterID dest)
    {
        moveImmToScratch(int64_t(imm.asIntptr()), dataTempRegister);
        m_assembler.and_(dest, dataTempRegister, dest);
    }

    void or64(RegisterID src, RegisterID dest) { m_assembler.or_(dest, src, dest); }
    void or64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.or_(dest, a, b); }
    void or64(TrustedImm32 imm, RegisterID dest)
    {
        if (imm.m_value >= 0 && isUInt16(imm.m_value)) {
            m_assembler.ori(dest, dest, uint16_t(imm.m_value));
            return;
        }
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.or_(dest, dataTempRegister, dest);
    }
    void or64(TrustedImm64 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.or_(dest, dataTempRegister, dest);
    }

    void xor64(RegisterID src, RegisterID dest) { m_assembler.xor_(dest, src, dest); }
    void xor64(RegisterID a, RegisterID b, RegisterID dest) { m_assembler.xor_(dest, a, b); }
    void xor64(TrustedImm32 imm, RegisterID dest)
    {
        if (imm.m_value == -1) {
            m_assembler.nor(dest, dest, dest);
            return;
        }
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.xor_(dest, dataTempRegister, dest);
    }

    // --- Shifts (count masked like x86/arm64 hardware) -------------------

    void lshift32(RegisterID shiftAmount, RegisterID dest) { lshift32(dest, shiftAmount, dest); }
    void lshift32(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rlwinm(dataTempRegister, shiftAmount, 0, 27, 31);   // count & 31
        m_assembler.slw(dest, src, dataTempRegister);                    // slw zero-extends
    }
    void lshift32(TrustedImm32 imm, RegisterID dest) { lshift32(dest, imm, dest); }
    void lshift32(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        m_assembler.slwi(dest, src, imm.m_value & 31);
    }

    void rshift32(RegisterID shiftAmount, RegisterID dest) { rshift32(dest, shiftAmount, dest); }
    void rshift32(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rlwinm(dataTempRegister, shiftAmount, 0, 27, 31);
        m_assembler.sraw(dest, src, dataTempRegister);
        zeroExtend32ToWordInternal(dest);                                // sraw sign-extends
    }
    void rshift32(TrustedImm32 imm, RegisterID dest) { rshift32(dest, imm, dest); }
    void rshift32(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        m_assembler.srawi(dest, src, imm.m_value & 31);
        zeroExtend32ToWordInternal(dest);
    }

    void urshift32(RegisterID shiftAmount, RegisterID dest) { urshift32(dest, shiftAmount, dest); }
    void urshift32(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rlwinm(dataTempRegister, shiftAmount, 0, 27, 31);
        m_assembler.srw(dest, src, dataTempRegister);                    // srw zero-extends
    }
    void urshift32(TrustedImm32 imm, RegisterID dest) { urshift32(dest, imm, dest); }
    void urshift32(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        if (!(imm.m_value & 31)) {
            move(src, dest);
            zeroExtend32ToWordInternal(dest);
            return;
        }
        m_assembler.srwi(dest, src, imm.m_value & 31);
    }

    // --- Compare-to-register ---------------------------------------------

    void compare32(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID dest)
    {
        emitCompare32(cond, left, right);
        setFromCondition(cond, dest);
    }
    void compare32(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID dest)
    {
        emitCompare32(cond, left, right);
        setFromCondition(cond, dest);
    }
    void compare64(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID dest)
    {
        emitCompare64(cond, left, right);
        setFromCondition(cond, dest);
    }
    void compare64(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID dest)
    {
        emitCompare64(cond, left, TrustedImm64(right.m_value));
        setFromCondition(cond, dest);
    }

    // --- Branches ---------------------------------------------------------

    Jump branch32(RelationalCondition cond, RegisterID left, RegisterID right)
    {
        emitCompare32(cond, left, right);
        return makeBranch(cond);
    }
    Jump branch32(RelationalCondition cond, RegisterID left, TrustedImm32 right)
    {
        emitCompare32(cond, left, right);
        return makeBranch(cond);
    }

    Jump branch64(RelationalCondition cond, RegisterID left, RegisterID right)
    {
        emitCompare64(cond, left, right);
        return makeBranch(cond);
    }
    Jump branch64(RelationalCondition cond, RegisterID left, TrustedImm32 right)
    {
        emitCompare64(cond, left, TrustedImm64(right.m_value));
        return makeBranch(cond);
    }
    Jump branch64(RelationalCondition cond, RegisterID left, TrustedImm64 right)
    {
        emitCompare64(cond, left, right);
        return makeBranch(cond);
    }

    Jump branchPtr(RelationalCondition cond, RegisterID left, RegisterID right)
    {
        return branch64(cond, left, right);
    }

    // --- Memory-operand branches: load the memory side, then reg/reg or
    // reg/imm compare. Narrow widths (8/16) load zero-extended and the
    // immediate is masked to the width. ---
    Jump branch32(RelationalCondition cond, Address left, RegisterID right)
    {
        load32(left, dataTempRegister);
        return branch32(cond, dataTempRegister, right);
    }
    Jump branch32(RelationalCondition cond, BaseIndex left, RegisterID right)
    {
        load32(left, dataTempRegister);
        return branch32(cond, dataTempRegister, right);
    }
    Jump branch32(RelationalCondition cond, RegisterID left, Address right)
    {
        load32(right, dataTempRegister);
        return branch32(cond, left, dataTempRegister);
    }
    Jump branch32(RelationalCondition cond, AbsoluteAddress left, RegisterID right)
    {
        load32(left.m_ptr, dataTempRegister);
        return branch32(cond, dataTempRegister, right);
    }
    Jump branch32(RelationalCondition cond, Address left, TrustedImm32 right)
    {
        load32(left, dataTempRegister);
        return branch32(cond, dataTempRegister, right);
    }

    Jump branch64(RelationalCondition cond, Address left, RegisterID right)
    {
        load64(left, dataTempRegister);
        return branch64(cond, dataTempRegister, right);
    }
    Jump branch64(RelationalCondition cond, BaseIndex left, RegisterID right)
    {
        load64(left, dataTempRegister);
        return branch64(cond, dataTempRegister, right);
    }
    Jump branch64(RelationalCondition cond, RegisterID left, Address right)
    {
        load64(right, dataTempRegister);
        return branch64(cond, left, dataTempRegister);
    }
    Jump branch64(RelationalCondition cond, AbsoluteAddress left, RegisterID right)
    {
        load64(left.m_ptr, dataTempRegister);
        return branch64(cond, dataTempRegister, right);
    }
    Jump branch64(RelationalCondition cond, Address left, TrustedImm64 right)
    {
        load64(left, dataTempRegister);
        return branch64(cond, dataTempRegister, right);
    }

    Jump branchPtr(RelationalCondition cond, Address left, RegisterID right) { return branch64(cond, left, right); }
    Jump branchPtr(RelationalCondition cond, BaseIndex left, RegisterID right) { return branch64(cond, left, right); }
    Jump branchPtr(RelationalCondition cond, RegisterID left, Address right) { return branch64(cond, left, right); }
    Jump branchPtr(RelationalCondition cond, Address left, Address right)
    {
        load64(left, dataTempRegister);
        load64(right, memoryTempRegister);
        return branch64(cond, dataTempRegister, memoryTempRegister);
    }

    // Byte / halfword comparisons: load zero-extended, mask the immediate.
    Jump branch8(RelationalCondition cond, Address left, TrustedImm32 right)
    {
        load8(left, dataTempRegister);
        return branch32(cond, dataTempRegister, TrustedImm32(right.m_value & 0xFF));
    }
    Jump branch8(RelationalCondition cond, BaseIndex left, TrustedImm32 right)
    {
        load8(left, dataTempRegister);
        return branch32(cond, dataTempRegister, TrustedImm32(right.m_value & 0xFF));
    }
    Jump branch8(RelationalCondition cond, AbsoluteAddress left, TrustedImm32 right)
    {
        load8(left.m_ptr, dataTempRegister);
        return branch32(cond, dataTempRegister, TrustedImm32(right.m_value & 0xFF));
    }
    Jump branch16(RelationalCondition cond, Address left, TrustedImm32 right)
    {
        load16(left, dataTempRegister);
        return branch32(cond, dataTempRegister, TrustedImm32(right.m_value & 0xFFFF));
    }

    // ResultCondition-based test branch: and the operands, test CR0.
    // andi. has already set CR0 from its result, a value in [0, 0xffff]: EQ
    // answers Zero/NonZero, and LT is clear, which is also the right answer
    // for Signed/PositiveOrZero at 32 or 64 bits. No compare needed.
    Jump branchOnAndiResult(ResultCondition cond)
    {
        switch (cond) {
        case Zero:           return Jump(m_assembler.emitUnlinkedBranch(12, 2));
        case NonZero:        return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        case Signed:         return Jump(m_assembler.emitUnlinkedBranch(12, 0));
        case PositiveOrZero: return Jump(m_assembler.emitUnlinkedBranch(4, 0));
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return Jump();
        }
    }

    Jump branchTestImpl32(ResultCondition cond, RegisterID valueLow32)
    {
        // valueLow32 has the 32-bit test value zero-extended.
        switch (cond) {
        case Zero:
            m_assembler.cmpdi(0, valueLow32, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 2));
        case NonZero:
            m_assembler.cmpdi(0, valueLow32, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        case Signed:
            m_assembler.cmpwi(0, valueLow32, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 0));
        case PositiveOrZero:
            m_assembler.cmpwi(0, valueLow32, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 0));
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return Jump();
        }
    }

    Jump branchTest32(ResultCondition cond, RegisterID reg, RegisterID mask)
    {
        m_assembler.and_(dataTempRegister, reg, mask);
        zeroExtend32ToWordInternal(dataTempRegister);
        return branchTestImpl32(cond, dataTempRegister);
    }
    Jump branchTest32(ResultCondition cond, RegisterID reg, TrustedImm32 mask = TrustedImm32(-1))
    {
        if (mask.m_value == -1) {
            m_assembler.rldicl(dataTempRegister, reg, 0, 32);
            return branchTestImpl32(cond, dataTempRegister);
        }
        if (isUInt16(uint32_t(mask.m_value))) {
            m_assembler.andi_(dataTempRegister, reg, uint16_t(mask.m_value));
            return branchOnAndiResult(cond);
        }
        moveImmToScratch(uint32_t(mask.m_value), dataTempRegister);
        return branchTest32(cond, reg, dataTempRegister);
    }

    Jump branchTest64(ResultCondition cond, RegisterID reg, RegisterID mask)
    {
        m_assembler.and_(dataTempRegister, reg, mask);
        return branchTest64Impl(cond, dataTempRegister);
    }
    Jump branchTest64(ResultCondition cond, RegisterID reg, TrustedImm32 mask = TrustedImm32(-1))
    {
        if (mask.m_value == -1)
            return branchTest64Impl(cond, reg);
        if (mask.m_value >= 0 && isUInt16(mask.m_value)) {
            m_assembler.andi_(dataTempRegister, reg, uint16_t(mask.m_value));
            return branchOnAndiResult(cond);
        }
        moveImmToScratch(int64_t(mask.m_value), dataTempRegister);
        return branchTest64(cond, reg, dataTempRegister);
    }
    Jump branchTest64(ResultCondition cond, RegisterID reg, TrustedImm64 mask)
    {
        if (mask.m_value == -1)
            return branchTest64Impl(cond, reg);
        moveImmToScratch(mask.m_value, dataTempRegister);
        m_assembler.and_(dataTempRegister, reg, dataTempRegister);
        return branchTest64Impl(cond, dataTempRegister);
    }
    Jump branchTest64(ResultCondition cond, Address address, TrustedImm32 mask = TrustedImm32(-1))
    {
        load64(address, memoryTempRegister);
        return branchTest64(cond, memoryTempRegister, mask);
    }
    Jump branchTest64(ResultCondition cond, BaseIndex address, TrustedImm32 mask = TrustedImm32(-1))
    {
        load64(address, memoryTempRegister);
        return branchTest64(cond, memoryTempRegister, mask);
    }
    Jump branchTest64(ResultCondition cond, Address address, RegisterID mask)
    {
        load64(address, memoryTempRegister);
        return branchTest64(cond, memoryTempRegister, mask);
    }
    Jump branchTest32(ResultCondition cond, Address address, TrustedImm32 mask = TrustedImm32(-1))
    {
        load32(address, memoryTempRegister);
        return branchTest32(cond, memoryTempRegister, mask);
    }

        Jump branchTest64Impl(ResultCondition cond, RegisterID value)
    {
        switch (cond) {
        case Zero:
            m_assembler.cmpdi(0, value, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 2));
        case NonZero:
            m_assembler.cmpdi(0, value, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        case Signed:
            m_assembler.cmpdi(0, value, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 0));
        case PositiveOrZero:
            m_assembler.cmpdi(0, value, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 0));
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return Jump();
        }
    }

    // branchAdd32 / branchSub32 / branchMul32 with Overflow do the arithmetic
    // on sign-extended copies: int32 overflow iff the 64-bit result differs
    // from its own low-word sign extension.
    Jump branchAdd32(ResultCondition cond, RegisterID src, RegisterID dest) { return branchAdd32(cond, src, dest, dest); }
    Jump branchAdd32(ResultCondition cond, RegisterID a, RegisterID b, RegisterID dest)
    {
        if (cond == Carry) {
            // Unsigned carry-out of the 32-bit add is bit 32 of the sum of the
            // zero-extended halves. Read both inputs into the scratches first
            // (dest, a or b may alias, and the immediate overloads pass a
            // scratch in).
            m_assembler.rldicl(dataTempRegister, a, 0, 32);
            m_assembler.rldicl(memoryTempRegister, b, 0, 32);
            m_assembler.add(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.rldicl(dest, dataTempRegister, 0, 32);              // wrapped result, zero-extended
            m_assembler.rldicl(dataTempRegister, dataTempRegister, 32, 63); // (sum >> 32) & 1
            m_assembler.cmpdi(0, dataTempRegister, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));              // != 0 → carry
        }
        if (cond == Overflow) {
            // Read both inputs into the scratches before anything else: the
            // Address and TrustedImm32 overloads materialise their operand
            // into a scratch and pass it in as a or b, so any sequence that
            // clobbers a scratch and then re-reads a or b computes garbage.
            m_assembler.extsw(dataTempRegister, a);      // sign-extended a
            m_assembler.extsw(memoryTempRegister, b);    // sign-extended b
            // Exact 64-bit sum. dest is safe to write now that a and b have
            // been consumed, even when it aliases one of them.
            m_assembler.add(dest, dataTempRegister, memoryTempRegister);
            // Seed XER.CA with the unsigned 32-bit carry-out, derived from the
            // zero-extended halves rather than assuming the inputs arrive
            // zero-extended. Only the B3 CheckAdd recovery of x + x reads it (via
            // setCarry, for the bit the doubling shifted out), and that recovery
            // runs only when every operand is the same register, so a != b skips
            // it: on every other checked add it was dead work on the hot path.
            if (a == b) {
                m_assembler.rldicl(dataTempRegister, dataTempRegister, 0, 32);
                m_assembler.rldicl(memoryTempRegister, memoryTempRegister, 0, 32);
                m_assembler.add(dataTempRegister, dataTempRegister, memoryTempRegister);
                m_assembler.rldicl(dataTempRegister, dataTempRegister, 32, 63); // (sum >> 32) & 1
                m_assembler.addic(dataTempRegister, dataTempRegister, -1);      // CA := carry
            }
            // Overflow iff the wrapped 32-bit result differs from the exact sum.
            m_assembler.extsw(memoryTempRegister, dest);
            m_assembler.cmpd(0, memoryTempRegister, dest);
            m_assembler.rldicl(dest, dest, 0, 32);       // wrapped result, zero-extended
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));           // != → overflow
        }
        add32(a, b, dest);
        return branchTestImpl32(resultConditionForArith(cond), dest);
    }
    Jump branchAdd32(ResultCondition cond, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchAdd32(cond, memoryTempRegister, dest, dest);
    }
    Jump branchAdd32(ResultCondition cond, TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchAdd32(cond, memoryTempRegister, src, dest);
    }

    Jump branchSub32(ResultCondition cond, RegisterID src, RegisterID dest) { return branchSub32(cond, dest, src, dest); }
    Jump branchSub32(ResultCondition cond, RegisterID left, RegisterID right, RegisterID dest)
    {
        if (cond == Overflow) {
            m_assembler.extsw(dataTempRegister, left);
            m_assembler.extsw(memoryTempRegister, right);
            m_assembler.subf(dataTempRegister, memoryTempRegister, dataTempRegister);
            m_assembler.rldicl(dest, dataTempRegister, 0, 32);
            m_assembler.extsw(memoryTempRegister, dataTempRegister);
            m_assembler.cmpd(0, memoryTempRegister, dataTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        }
        sub32(left, right, dest);
        return branchTestImpl32(resultConditionForArith(cond), dest);
    }
    Jump branchSub32(ResultCondition cond, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchSub32(cond, dest, memoryTempRegister, dest);
    }

    Jump branchMul32(ResultCondition cond, RegisterID src, RegisterID dest) { return branchMul32(cond, src, dest, dest); }
    Jump branchMul32(ResultCondition cond, RegisterID a, RegisterID b, RegisterID dest)
    {
        if (cond == Overflow) {
            m_assembler.extsw(dataTempRegister, a);
            m_assembler.extsw(memoryTempRegister, b);
            m_assembler.mulld(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.rldicl(dest, dataTempRegister, 0, 32);
            m_assembler.extsw(memoryTempRegister, dataTempRegister);
            m_assembler.cmpd(0, memoryTempRegister, dataTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        }
        mul32(a, b, dest);
        return branchTestImpl32(resultConditionForArith(cond), dest);
    }

    static ResultCondition resultConditionForArith(ResultCondition cond)
    {
        RELEASE_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == PositiveOrZero);
        return cond;
    }

    Jump jump()
    {
        return Jump(m_assembler.emitUnlinkedJump());
    }

    // branchTest{8,32,64} against an absolute memory location: load, mask, test.
    Jump branchTest64(ResultCondition cond, AbsoluteAddress address, TrustedImm32 mask = TrustedImm32(-1))
    {
        // Load the value into memoryTempRegister (reusing the address slot)
        // so a large-mask materialization into dataTempRegister can't clobber
        // it.
        moveToAbsolute(address.m_ptr);
        m_assembler.ld(memoryTempRegister, 0, memoryTempRegister);
        return branchTest64(cond, memoryTempRegister, mask);
    }
    Jump branchTest32(ResultCondition cond, AbsoluteAddress address, TrustedImm32 mask = TrustedImm32(-1))
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lwz(memoryTempRegister, 0, memoryTempRegister);
        return branchTest32(cond, memoryTempRegister, mask);
    }
    Jump branchTest8(ResultCondition cond, AbsoluteAddress address, TrustedImm32 mask = TrustedImm32(-1))
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lbz(dataTempRegister, 0, memoryTempRegister);
        return finishNarrowTest(cond, mask, 0xFF, /*halfword*/ false);
    }

    // branchTest8/16: load the value, apply the mask, sign-extend, then
    // test as 64-bit.  Sign extension makes Signed/PositiveOrZero read the
    // narrow value's sign bit and is neutral for Zero/NonZero.
    Jump branchTest8(ResultCondition cond, Address address, TrustedImm32 mask = TrustedImm32(-1))
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dataTempRegister, r.offset, r.base);
        return finishNarrowTest(cond, mask, 0xFF, /*halfword*/ false);
    }
    Jump branchTest8(ResultCondition cond, BaseIndex address, TrustedImm32 mask = TrustedImm32(-1))
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dataTempRegister, r.offset, r.base);
        return finishNarrowTest(cond, mask, 0xFF, false);
    }
    Jump branchTest16(ResultCondition cond, Address address, TrustedImm32 mask = TrustedImm32(-1))
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dataTempRegister, r.offset, r.base);
        return finishNarrowTest(cond, mask, 0xFFFF, true);
    }
    Jump branchTest16(ResultCondition cond, BaseIndex address, TrustedImm32 mask = TrustedImm32(-1))
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dataTempRegister, r.offset, r.base);
        return finishNarrowTest(cond, mask, 0xFFFF, true);
    }
    Jump finishNarrowTest(ResultCondition cond, TrustedImm32 mask, uint32_t widthMask, bool halfword)
    {
        if (mask.m_value != -1)
            m_assembler.andi_(dataTempRegister, dataTempRegister, uint16_t(uint32_t(mask.m_value) & widthMask));
        if (halfword)
            m_assembler.extsh(dataTempRegister, dataTempRegister);
        else
            m_assembler.extsb(dataTempRegister, dataTempRegister);
        return branchTest64Impl(cond, dataTempRegister);
    }

    // --- Loads and stores -------------------------------------------------
    // load32/16/8 zero-extend (lwz/lhz/lbz); ld/std are DS-form and need
    // 4-aligned displacements — resolveAddressDS falls back to adding the
    // displacement into the scratch when it is misaligned.

    template<typename AddressType>
    ResolvedAddress resolveAddressDS(AddressType address, RegisterID scratch)
    {
        ResolvedAddress r = resolveAddress(address, scratch);
        if (!(r.offset & 3))
            return r;
        m_assembler.addi(scratch, r.base, r.offset);
        return { scratch, 0 };
    }

    void moveToAbsolute(const void* address)
    {
        moveImmToScratch(int64_t(intptr_t(address)), memoryTempRegister);
    }

    void load64(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.ld(dest, r.offset, r.base);
    }
    void load64(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.ld(dest, 0, memoryTempRegister);
    }

    void load32(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dest, r.offset, r.base);
    }
    void load32(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dest, r.offset, r.base);
    }
    void load32(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lwz(dest, 0, memoryTempRegister);
    }

    void load16(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dest, r.offset, r.base);
    }
    void load16(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dest, r.offset, r.base);
    }
    void load16(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lhz(dest, 0, memoryTempRegister);
    }

    void load8(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dest, r.offset, r.base);
    }
    void load8(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dest, r.offset, r.base);
    }
    void load8(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lbz(dest, 0, memoryTempRegister);
    }

    void store64(RegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.std(src, r.offset, r.base);
    }
    void store64(RegisterID src, void* address)
    {
        moveToAbsolute(address);
        m_assembler.std(src, 0, memoryTempRegister);
    }
    void store64(TrustedImm64 imm, Address address)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        store64(dataTempRegister, address);
    }
    void store64(TrustedImm32 imm, Address address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store64(dataTempRegister, address);
    }
    void store64(TrustedImmPtr imm, Address address)
    {
        moveImmToScratch(int64_t(imm.asIntptr()), dataTempRegister);
        store64(dataTempRegister, address);
    }
    void store64(TrustedImm64 imm, BaseIndex address)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        store64(dataTempRegister, address);
    }
    void store64(TrustedImm32 imm, BaseIndex address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store64(dataTempRegister, address);
    }

    void store32(RegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stw(src, r.offset, r.base);
    }
    void store32(RegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stw(src, r.offset, r.base);
    }
    void store32(RegisterID src, void* address)
    {
        moveToAbsolute(address);
        m_assembler.stw(src, 0, memoryTempRegister);
    }
    void store32(TrustedImm32 imm, Address address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store32(dataTempRegister, address);
    }
    void store32(TrustedImm32 imm, void* address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store32(dataTempRegister, address);
    }

    void store16(RegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.sth(src, r.offset, r.base);
    }
    void store16(RegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.sth(src, r.offset, r.base);
    }
    void store16(TrustedImm32 imm, Address address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store16(dataTempRegister, address);
    }
    void store16(TrustedImm32 imm, BaseIndex address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store16(dataTempRegister, address);
    }

    void store8(RegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stb(src, r.offset, r.base);
    }
    void store8(RegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stb(src, r.offset, r.base);
    }
    void store8(TrustedImm32 imm, Address address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store8(dataTempRegister, address);
    }

    void getEffectiveAddress(BaseIndex address, RegisterID dest)
    {
        if (address.scale) {
            // Shift into the scratch: sldi straight into dest would clobber
            // the base when dest == address.base.
            m_assembler.sldi(dataTempRegister, address.index, address.scale);
            m_assembler.add(dest, dataTempRegister, address.base);
        } else
            m_assembler.add(dest, address.index, address.base);
        if (address.offset) {
            if (isInt16(address.offset))
                m_assembler.addi(dest, dest, int16_t(address.offset));
            else {
                moveImmToScratch(int64_t(address.offset), dataTempRegister);
                m_assembler.add(dest, dest, dataTempRegister);
            }
        }
    }

    void move(TrustedImm64 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, dest);
    }

    // Memory-to-memory transfers, templated over the address forms as on
    // ARM64 so that mixed pairs work: BBQ's wasm GC struct/array field copies
    // pass an Address source with a BaseIndex destination. The value travels
    // through dataTempRegister while address resolution uses
    // memoryTempRegister, so the two never collide.
    template<typename SrcType, typename DestType>
    void transfer8(SrcType src, DestType dest)
    {
        load8(src, dataTempRegister);
        store8(dataTempRegister, dest);
    }
    template<typename SrcType, typename DestType>
    void transfer16(SrcType src, DestType dest)
    {
        load16(src, dataTempRegister);
        store16(dataTempRegister, dest);
    }
    template<typename SrcType, typename DestType>
    void transfer32(SrcType src, DestType dest)
    {
        load32(src, dataTempRegister);
        store32(dataTempRegister, dest);
    }
    template<typename SrcType, typename DestType>
    void transfer64(SrcType src, DestType dest)
    {
        load64(src, dataTempRegister);
        store64(dataTempRegister, dest);
    }
    void transferPtr(Address src, Address dest) { transfer64(src, dest); }
    void transferPtr(BaseIndex src, BaseIndex dest) { transfer64(src, dest); }

    enum BranchTruncateType { BranchIfTruncateFailed, BranchIfTruncateSuccessful };

    // ------------------------------------------------------------------
    // Floating point. float32 values live in FPRs in double format (lfs/stfs
    // and the -s arithmetic forms convert). f0 is the FP scratch.
    // ------------------------------------------------------------------

    void loadDouble(Address address, FPRegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lfd(dest, r.offset, r.base);
    }
    void loadDouble(BaseIndex address, FPRegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lfd(dest, r.offset, r.base);
    }
    void loadDouble(TrustedImmPtr address, FPRegisterID dest)
    {
        move(address, memoryTempRegister);
        m_assembler.lfd(dest, 0, memoryTempRegister);
    }
    void loadFloat(Address address, FPRegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lfs(dest, r.offset, r.base);
    }
    void loadFloat(BaseIndex address, FPRegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lfs(dest, r.offset, r.base);
    }
    void loadFloat(TrustedImmPtr address, FPRegisterID dest)
    {
        move(address, memoryTempRegister);
        m_assembler.lfs(dest, 0, memoryTempRegister);
    }
    void storeDouble(FPRegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stfd(src, r.offset, r.base);
    }
    void storeDouble(FPRegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stfd(src, r.offset, r.base);
    }
    void storeDouble(FPRegisterID src, TrustedImmPtr address)
    {
        move(address, memoryTempRegister);
        m_assembler.stfd(src, 0, memoryTempRegister);
    }
    void storeFloat(FPRegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stfs(src, r.offset, r.base);
    }
    void storeFloat(FPRegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.stfs(src, r.offset, r.base);
    }

    void moveDouble(FPRegisterID src, FPRegisterID dest)
    {
        if (src != dest)
            m_assembler.fmr(dest, src);
    }
    void moveZeroToDouble(FPRegisterID dest)
    {
        m_assembler.addi(dataTempRegister, PPC64Registers::r0, 0);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    void move64ToDouble(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrd(dest, src);
    }
    void move64ToDouble(TrustedImm64 imm, FPRegisterID dest)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    void moveDoubleTo64(FPRegisterID src, RegisterID dest)
    {
        m_assembler.mfvsrd(dest, src);
    }
    // Bitcast int32 <-> float32 must round-trip through memory: FPRs hold
    // float values in double format, so raw float32 bits only become a
    // usable float via lfs's format conversion.  Uses the ELFv2 red zone.
    void move32ToFloat(RegisterID src, FPRegisterID dest)
    {
        m_assembler.stw(src, -8, stackPointerRegister);
        m_assembler.lfs(dest, -8, stackPointerRegister);
    }
    void move32ToFloat(TrustedImm32 imm, FPRegisterID dest)
    {
        moveImmToScratch(int64_t(uint32_t(imm.m_value)), dataTempRegister);
        move32ToFloat(dataTempRegister, dest);
    }
    void moveFloatTo32(FPRegisterID src, RegisterID dest)
    {
        m_assembler.stfs(src, -8, stackPointerRegister);
        m_assembler.lwz(dest, -8, stackPointerRegister);
    }

    void convertInt32ToDouble(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrwa(dest, src);
        m_assembler.fcfid(dest, dest);
    }
    void convertInt32ToDouble(TrustedImm32 imm, FPRegisterID dest)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        convertInt32ToDouble(dataTempRegister, dest);
    }
    void convertInt32ToDouble(BaseIndex address, FPRegisterID dest)
    {
        load32(address, dataTempRegister);
        convertInt32ToDouble(dataTempRegister, dest);
    }
    void convertInt64ToDouble(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrd(dest, src);
        m_assembler.fcfid(dest, dest);
    }
    void convertUInt32ToDouble(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrwz(dest, src);
        m_assembler.fcfid(dest, dest);
    }
    void convertUInt32ToDouble(RegisterID src, FPRegisterID dest, FPRegisterID)
    {
        convertUInt32ToDouble(src, dest);
    }
    void convertUInt64ToDouble(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrd(dest, src);
        m_assembler.fcfidu(dest, dest);
    }
    void convertFloatToDouble(FPRegisterID src, FPRegisterID dest)
    {
        // An FPR already holds a float in double format, so a plain move would be
        // value-preserving, but it would leave a signalling NaN signalling: lfs
        // widens an sNaN bit for bit. wasm's f64.promote_f32 (and the x86/ARM64
        // conversions JSC otherwise relies on) quiet it. frsp does, and is exact
        // for every other value, since the source is single-representable.
        m_assembler.frsp(dest, src);
    }
    void convertDoubleToFloat(FPRegisterID src, FPRegisterID dest)
    {
        m_assembler.frsp(dest, src);
    }

    void addDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest) { m_assembler.fadd(dest, a, b); }
    void addDouble(FPRegisterID src, FPRegisterID dest)               { m_assembler.fadd(dest, dest, src); }
    void subDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest) { m_assembler.fsub(dest, a, b); }
    void subDouble(FPRegisterID src, FPRegisterID dest)               { m_assembler.fsub(dest, dest, src); }
    void mulDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest) { m_assembler.fmul(dest, a, b); }
    void mulDouble(FPRegisterID src, FPRegisterID dest)               { m_assembler.fmul(dest, dest, src); }
    void divDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest) { m_assembler.fdiv(dest, a, b); }
    void divDouble(FPRegisterID src, FPRegisterID dest)               { m_assembler.fdiv(dest, dest, src); }
    void sqrtDouble(FPRegisterID src, FPRegisterID dest)              { m_assembler.fsqrt(dest, src); }

    // FP arith with a memory source: load into the FP scratch, then combine.
    void addDouble(Address src, FPRegisterID dest) { loadDouble(src, fpTempRegister); m_assembler.fadd(dest, dest, fpTempRegister); }
    void subDouble(Address src, FPRegisterID dest) { loadDouble(src, fpTempRegister); m_assembler.fsub(dest, dest, fpTempRegister); }
    void mulDouble(Address src, FPRegisterID dest) { loadDouble(src, fpTempRegister); m_assembler.fmul(dest, dest, fpTempRegister); }
    void mulDouble(BaseIndex src, FPRegisterID dest) { loadDouble(src, fpTempRegister); m_assembler.fmul(dest, dest, fpTempRegister); }
    void divDouble(Address src, FPRegisterID dest) { loadDouble(src, fpTempRegister); m_assembler.fdiv(dest, dest, fpTempRegister); }

    // 16-bit memory compare against a register (signed halfword load).
    Jump branch32WithMemory16(RelationalCondition cond, Address left, RegisterID right)
    {
        load16SignedExtendTo32(left, dataTempRegister);
        return branch32(cond, dataTempRegister, right);
    }

    // dest = base + (shiftee >> shift)  (logical right shift)
    void addUnsignedRightShift32(RegisterID base, RegisterID shiftee, TrustedImm32 shift, RegisterID dest)
    {
        m_assembler.srwi(dataTempRegister, shiftee, shift.m_value & 31);
        m_assembler.add(dest, base, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void absDouble(FPRegisterID src, FPRegisterID dest)               { m_assembler.fabs(dest, src); }
    // JS Math.min/max semantics (a NaN operand wins, -0 < +0), POWER9 only:
    // see supportsFloatingPointMinMax(). A signalling NaN operand comes back
    // unquieted, so wasm, which needs an arithmetic NaN, cannot use these.
    void doubleMax(FPRegisterID op1, FPRegisterID op2, FPRegisterID dest)
    {
        ASSERT(supportsFloatingPointMinMax());
        m_assembler.xsmaxjdp(vsr(dest), vsr(op1), vsr(op2));
    }
    void doubleMin(FPRegisterID op1, FPRegisterID op2, FPRegisterID dest)
    {
        ASSERT(supportsFloatingPointMinMax());
        m_assembler.xsminjdp(vsr(dest), vsr(op1), vsr(op2));
    }
    // Singles are held in double format, where min/max are exact.
    void floatMax(FPRegisterID op1, FPRegisterID op2, FPRegisterID dest) { doubleMax(op1, op2, dest); }
    void floatMin(FPRegisterID op1, FPRegisterID op2, FPRegisterID dest) { doubleMin(op1, op2, dest); }
    void absFloat(FPRegisterID src, FPRegisterID dest)                { m_assembler.fabs(dest, src); }

    // FP bitwise ops treat the doubles as bit patterns (used for sign-bit
    // masking, NaN canonicalization, SIMD-lane logic). FPR f_n aliases
    // VSR n, so the VSX logical ops apply directly.
    void andDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        m_assembler.xxland(dest, a, b);
    }
    void andDouble(FPRegisterID src, FPRegisterID dest)
    {
        m_assembler.xxland(dest, dest, src);
    }
    void orDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        m_assembler.xxlor(dest, a, b);
    }
    void orDouble(FPRegisterID src, FPRegisterID dest)
    {
        m_assembler.xxlor(dest, dest, src);
    }
    void xorDouble(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        m_assembler.xxlxor(dest, a, b);
    }
    void xorDouble(FPRegisterID src, FPRegisterID dest)
    {
        m_assembler.xxlxor(dest, dest, src);
    }
    void negateDouble(FPRegisterID src, FPRegisterID dest)            { m_assembler.fneg(dest, src); }
    void negateFloat(FPRegisterID src, FPRegisterID dest)             { m_assembler.fneg(dest, src); }
    void floorDouble(FPRegisterID src, FPRegisterID dest)             { m_assembler.frim(dest, src); }
    void ceilDouble(FPRegisterID src, FPRegisterID dest)              { m_assembler.frip(dest, src); }
    void truncDouble(FPRegisterID src, FPRegisterID dest)             { m_assembler.friz(dest, src); }
    void roundTowardZeroDouble(FPRegisterID src, FPRegisterID dest)   { m_assembler.friz(dest, src); }
    // Ties-to-even, as ARM64 frintn: frin rounds ties away from zero, so use
    // xsrdpic, which rounds in the current mode (round-to-nearest-even).
    void roundTowardNearestIntDouble(FPRegisterID src, FPRegisterID dest) { m_assembler.xsrdpic(dest, src); }

    void truncateDoubleToInt32(FPRegisterID src, RegisterID dest)
    {
        m_assembler.fctiwz(fpTempRegister, src);
        m_assembler.mfvsrwz(dest, fpTempRegister);
    }
    void truncateDoubleToInt64(FPRegisterID src, RegisterID dest)
    {
        m_assembler.fctidz(fpTempRegister, src);
        m_assembler.mfvsrd(dest, fpTempRegister);
    }
    void truncateDoubleToUint32(FPRegisterID src, RegisterID dest)
    {
        // fctiduz truncates toward zero to unsigned 64-bit (saturating);
        // in-range uint32 values convert exactly, matching the contract's
        // defined domain.
        m_assembler.fctiduz(fpTempRegister, src);
        m_assembler.mfvsrd(dest, fpTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void truncateDoubleToUint64(FPRegisterID src, RegisterID dest)
    {
        m_assembler.fctiduz(fpTempRegister, src);
        m_assembler.mfvsrd(dest, fpTempRegister);
    }
    // The four-operand form exists for backends that need scratch FPRs to
    // synthesise the unsigned conversion; fctiduz does it directly.
    void truncateDoubleToUint64(FPRegisterID src, RegisterID dest, FPRegisterID, FPRegisterID) { truncateDoubleToUint64(src, dest); }

    // A single-precision value in a PPC64 FPR is held in double format (see
    // convertFloatToDouble), and every float is exactly
    // representable as a double, so truncating a float uses the same
    // instructions as truncating the equivalent double.
    void truncateFloatToInt32(FPRegisterID src, RegisterID dest)  { truncateDoubleToInt32(src, dest); }
    void truncateFloatToUint32(FPRegisterID src, RegisterID dest) { truncateDoubleToUint32(src, dest); }
    void truncateFloatToInt64(FPRegisterID src, RegisterID dest)  { truncateDoubleToInt64(src, dest); }
    void truncateFloatToUint64(FPRegisterID src, RegisterID dest) { truncateDoubleToUint64(src, dest); }
    void truncateFloatToUint64(FPRegisterID src, RegisterID dest, FPRegisterID, FPRegisterID) { truncateDoubleToUint64(src, dest); }

    // --- Double compares/branches ---------------------------------------
    // fcmpu sets CR0 bits LT=0, GT=1, EQ=2, UN=3.  Compound conditions
    // fold UN into the tested bit with one CR-logical op (cror/crandc), so
    // every DoubleCondition is a single conditional branch.
    struct DoubleBranchBits { int cropKind; uint32_t bo; uint32_t bi; };  // cropKind: 0 none, 1 cror bi,bi,UN, 2 crandc bi,bi,UN

    static DoubleBranchBits doubleBranchBitsFor(DoubleCondition cond)
    {
        switch (cond) {
        case DoubleEqualAndOrdered:               return { 0, 12, 2 };
        case DoubleNotEqualAndOrdered:            return { 1, 4,  2 };  // !(EQ|UN)
        case DoubleGreaterThanAndOrdered:         return { 0, 12, 1 };
        case DoubleGreaterThanOrEqualAndOrdered:  return { 1, 4,  0 };  // !(LT|UN)
        case DoubleLessThanAndOrdered:            return { 0, 12, 0 };
        case DoubleLessThanOrEqualAndOrdered:     return { 1, 4,  1 };  // !(GT|UN)
        case DoubleEqualOrUnordered:              return { 1, 12, 2 };  // EQ|UN
        case DoubleNotEqualOrUnordered:           return { 2, 4,  2 };  // !(EQ&!UN)
        case DoubleGreaterThanOrUnordered:        return { 1, 12, 1 };
        case DoubleGreaterThanOrEqualOrUnordered: return { 2, 4,  0 };
        case DoubleLessThanOrUnordered:           return { 1, 12, 0 };
        case DoubleLessThanOrEqualOrUnordered:    return { 2, 4,  1 };
        }
        RELEASE_ASSERT_NOT_REACHED();
        return { 0, 20, 0 };
    }

    void emitDoubleConditionCRop(DoubleBranchBits bits)
    {
        if (bits.cropKind == 1)
            m_assembler.cror(bits.bi, bits.bi, 3);
        else if (bits.cropKind == 2)
            m_assembler.crandc(bits.bi, bits.bi, 3);
    }

    Jump branchDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right)
    {
        DoubleBranchBits bits = doubleBranchBitsFor(cond);
        m_assembler.fcmpu(0, left, right);
        emitDoubleConditionCRop(bits);
        return Jump(m_assembler.emitUnlinkedBranch(bits.bo, bits.bi));
    }
    Jump branchFloat(DoubleCondition cond, FPRegisterID left, FPRegisterID right)
    {
        return branchDouble(cond, left, right);   // both are doubles in FPRs
    }
    Jump branchDoubleNonZero(FPRegisterID reg, FPRegisterID scratch)
    {
        moveZeroToDouble(scratch);
        return branchDouble(DoubleNotEqualAndOrdered, reg, scratch);
    }
    Jump branchDoubleZeroOrNaN(FPRegisterID reg, FPRegisterID scratch)
    {
        moveZeroToDouble(scratch);
        return branchDouble(DoubleEqualOrUnordered, reg, scratch);
    }

    // --- Single-precision (float) family, for Air/FTL -------------------
    // Floats live in FPRs as doubles rounded to single (the PPC convention);
    // the -s arithmetic forms keep results in single range and precision.
    void addFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)  { m_assembler.fadds(dest, a, b); }
    void addFloat(FPRegisterID src, FPRegisterID dest)                { m_assembler.fadds(dest, dest, src); }
    void subFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)  { m_assembler.fsubs(dest, a, b); }
    void subFloat(FPRegisterID src, FPRegisterID dest)                { m_assembler.fsubs(dest, dest, src); }
    void mulFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)  { m_assembler.fmuls(dest, a, b); }
    void mulFloat(FPRegisterID src, FPRegisterID dest)                { m_assembler.fmuls(dest, dest, src); }
    void divFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)  { m_assembler.fdivs(dest, a, b); }
    void divFloat(FPRegisterID src, FPRegisterID dest)                { m_assembler.fdivs(dest, dest, src); }
    void sqrtFloat(FPRegisterID src, FPRegisterID dest)               { m_assembler.fsqrts(dest, src); }
    // Rounding an already-single value to an integral yields a value still
    // exactly representable as single (below 2^24 integrals are exact; larger
    // singles are already integral and unchanged), so the double
    // round-to-integral instructions suffice.
    void floorFloat(FPRegisterID src, FPRegisterID dest)              { m_assembler.frim(dest, src); }
    void ceilFloat(FPRegisterID src, FPRegisterID dest)               { m_assembler.frip(dest, src); }
    void truncFloat(FPRegisterID src, FPRegisterID dest)              { m_assembler.friz(dest, src); }
    void roundTowardZeroFloat(FPRegisterID src, FPRegisterID dest)    { m_assembler.friz(dest, src); }
    void roundTowardNearestIntFloat(FPRegisterID src, FPRegisterID dest) { m_assembler.xsrdpic(dest, src); }
    // Bitwise float ops act on the 32-bit single-format pattern: shuttle
    // through the GPR scratches via the single<->double converting bit moves.
    void andFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        moveFloatTo32(a, dataTempRegister);
        moveFloatTo32(b, memoryTempRegister);
        and32(memoryTempRegister, dataTempRegister);
        move32ToFloat(dataTempRegister, dest);
    }
    void andFloat(FPRegisterID src, FPRegisterID dest) { andFloat(src, dest, dest); }
    void orFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        moveFloatTo32(a, dataTempRegister);
        moveFloatTo32(b, memoryTempRegister);
        or32(memoryTempRegister, dataTempRegister);
        move32ToFloat(dataTempRegister, dest);
    }
    void orFloat(FPRegisterID src, FPRegisterID dest) { orFloat(src, dest, dest); }
    void xorFloat(FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        moveFloatTo32(a, dataTempRegister);
        moveFloatTo32(b, memoryTempRegister);
        xor32(memoryTempRegister, dataTempRegister);
        move32ToFloat(dataTempRegister, dest);
    }
    void xorFloat(FPRegisterID src, FPRegisterID dest) { xorFloat(src, dest, dest); }
    void compareFloat(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID dest)
    {
        // Same as a double compare: floats are held as doubles in FPRs.
        DoubleBranchBits bits = doubleBranchBitsFor(cond);
        m_assembler.fcmpu(0, left, right);
        emitDoubleConditionCRop(bits);
        setFromCR0Bit(bits.bi, bits.bo != 4, dest);
    }
    // The generic MacroAssembler::compareDouble branches around a move, which
    // mispredicts on data-dependent comparisons; take the CR0 bit directly.
    void compareDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID dest)
    {
        compareFloat(cond, left, right, dest);
    }
    void compareDoubleWithZero(DoubleCondition, FPRegisterID, RegisterID)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    // --- Width extensions (Air SignExtend/ZeroExtend opcodes) -----------
    void signExtend8To64(RegisterID src, RegisterID dest)  { m_assembler.extsb(dest, src); }
    void signExtend16To64(RegisterID src, RegisterID dest) { m_assembler.extsh(dest, src); }
    void signExtend32To64(RegisterID src, RegisterID dest) { m_assembler.extsw(dest, src); }
    void zeroExtend8To32(RegisterID src, RegisterID dest)  { m_assembler.rldicl(dest, src, 0, 56); }
    void zeroExtend16To32(RegisterID src, RegisterID dest) { m_assembler.rldicl(dest, src, 0, 48); }
    void zeroExtend8To64(RegisterID src, RegisterID dest)  { m_assembler.rldicl(dest, src, 0, 56); }
    void zeroExtend16To64(RegisterID src, RegisterID dest) { m_assembler.rldicl(dest, src, 0, 48); }

    // --- Test32-based conditional moves (mirror the Test64 family) ------
    void emitTest32ToCR0(RegisterID left, RegisterID mask)
    {
        m_assembler.and_(dataTempRegister, left, mask);
        m_assembler.extsw(dataTempRegister, dataTempRegister); // Signed = bit31
        m_assembler.cmpdi(0, dataTempRegister, 0);
    }
    void emitTest32ToCR0(RegisterID left, TrustedImm32 mask)
    {
        moveImmToScratch(int64_t(uint32_t(mask.m_value)), dataTempRegister);
        m_assembler.and_(dataTempRegister, left, dataTempRegister);
        m_assembler.extsw(dataTempRegister, dataTempRegister);
        m_assembler.cmpdi(0, dataTempRegister, 0);
    }
    void moveConditionallyTest32(ResultCondition cond, RegisterID left, RegisterID mask, RegisterID src, RegisterID dest)
    {
        emitTest32ToCR0(left, mask);
        BranchBitsRC b = testBranchBitsFor(cond);
        emitSkipOneInstruction(b.bo ^ 0x8, b.bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionallyTest32(ResultCondition cond, RegisterID left, RegisterID mask, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest32ToCR0(left, mask); });
    }
    void moveConditionallyTest32(ResultCondition cond, RegisterID left, TrustedImm32 mask, RegisterID src, RegisterID dest)
    {
        emitTest32ToCR0(left, mask);
        BranchBitsRC b = testBranchBitsFor(cond);
        emitSkipOneInstruction(b.bo ^ 0x8, b.bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionallyTest32(ResultCondition cond, RegisterID left, TrustedImm32 mask, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest32ToCR0(left, mask); });
    }

    // --- Carry access for the B3 CheckAdd recovery path -----------------
    // Valid after an instruction that maintains XER.CA: the branchAdd64
    // overflow emitters use addc, and branchAdd32's overflow form seeds CA via
    // the addic trick, when both operands are the same register (x + x, the
    // only case whose recovery calls this), precisely so this recovery works.
    void setCarry(RegisterID dest)
    {
        m_assembler.addi(dest, PPC64Registers::r0, 0); // li dest, 0
        m_assembler.addze(dest, dest);                 // dest = 0 + CA
    }

    // --- Vector load/store, BaseIndex forms -----------------------------
    void loadVector(BaseIndex address, FPRegisterID dest)
    {
        RegisterID ea = prepareAtomicAddress(address, memoryTempRegister);
        m_assembler.lxvd2x(dest, PPC64Registers::r0, ea);
    }
    void storeVector(FPRegisterID src, BaseIndex address)
    {
        RegisterID ea = prepareAtomicAddress(address, memoryTempRegister);
        m_assembler.stxvd2x(src, PPC64Registers::r0, ea);
    }

    // --- FP conditional moves (Air MoveDoubleConditionally* / *Float) ----
    template<typename CompareEmitter>
    void moveDoubleConditionallyImpl(BranchBits bits, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest, const CompareEmitter& emitCompareFn)
    {
        emitCompareFn();
        if (dest == thenCase) {
            emitSkipOneInstruction(bits.bo, bits.bi);          // taken: keep then-value
            m_assembler.fmr(dest, elseCase);
        } else if (dest == elseCase) {
            emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi);    // not taken: keep else-value
            m_assembler.fmr(dest, thenCase);
        } else {
            m_assembler.fmr(dest, elseCase);
            emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi);
            m_assembler.fmr(dest, thenCase);
        }
    }
    // Floats live as doubles in FPRs, so the float variants are the double ones.
    void moveConditionallyFloat(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID src, RegisterID dest)
    {
        moveConditionallyDouble(cond, left, right, src, dest);
    }
    void moveConditionallyFloat(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        moveConditionallyDouble(cond, left, right, thenCase, elseCase, dest);
    }
    void moveDoubleConditionallyFloat(DoubleCondition cond, FPRegisterID left, FPRegisterID right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        moveDoubleConditionallyDouble(cond, left, right, thenCase, elseCase, dest);
    }
    void moveDoubleConditionally32(RelationalCondition cond, RegisterID left, RegisterID right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        moveDoubleConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare32(cond, left, right); });
    }
    void moveDoubleConditionally32(RelationalCondition cond, RegisterID left, TrustedImm32 right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        moveDoubleConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare32(cond, left, right); });
    }
    void moveDoubleConditionally64(RelationalCondition cond, RegisterID left, RegisterID right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        moveDoubleConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare64(cond, left, right); });
    }
    void moveDoubleConditionally64(RelationalCondition cond, RegisterID left, TrustedImm32 right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        moveDoubleConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare64(cond, left, TrustedImm64(right.m_value)); });
    }
    void moveDoubleConditionallyTest32(ResultCondition cond, RegisterID left, RegisterID mask, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveDoubleConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest32ToCR0(left, mask); });
    }
    void moveDoubleConditionallyTest32(ResultCondition cond, RegisterID left, TrustedImm32 mask, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveDoubleConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest32ToCR0(left, mask); });
    }
    void moveDoubleConditionallyTest64(ResultCondition cond, RegisterID left, RegisterID mask, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveDoubleConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest64ToCR0(left, mask); });
    }
    void moveDoubleConditionallyTest64(ResultCondition cond, RegisterID left, TrustedImm32 mask, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveDoubleConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest64ToCR0(left, mask); });
    }

    // =====================================================================
    // WebAssembly SIMD (v128) on the POWER8 baseline (VSX + VMX).
    //
    // Register model. A v128 lives in the full 128-bit VSR that aliases
    // FPRegisterID fN (VSR N, N = 0..31), so the register allocators, spill
    // code and the wasm calling convention treat vectors exactly as they treat
    // ARM64's q registers. VSX instructions address all 64 VSRs and work on
    // those registers in place. VMX (AltiVec) instructions can only name VSR
    // 32-63 (v0-v31), so a VMX operation copies its operands into v0..v5 with
    // xxlor, operates there, and copies the result back. v0..v5 are scratch
    // *within a single MacroAssembler call*: nothing is ever live in VSR 32-63
    // between two calls, which is why nothing saves or allocates them. (They
    // are ELFv2-volatile; IPInt's own v0-v7 mapping in offlineasm is never
    // live across JIT code either.) Doubleword 1 of every VSR is volatile under
    // ELFv2 and doubleword 0 of f14-f31 is callee-saved, which is the same
    // Width64 callee-save model ARM64 uses for d8-d15.
    //
    // Lane order. The canonical in-register layout is the one lxvd2x/stxvd2x
    // produce on little-endian: doubleword 0 (ISA bits 0-63, the half that is
    // also the scalar FPR) holds bytes 0-7 of the v128 as a little-endian u64,
    // doubleword 1 holds bytes 8-15. Consequences:
    //   - vector loads and stores are a bare lxvd2x/stxvd2x, no permute;
    //   - f64x2/i64x2 lane 0 is the scalar FPR, as on ARM64 and x86;
    //   - wasm lane i of an S-byte element is ISA (big-endian numbered)
    //     element i ^ (8/S - 1): bytes i ^ 7, halfwords i ^ 3, words i ^ 1,
    //     doublewords i.
    // Element-wise operations do not care. Every lane-indexed or cross-lane
    // operation below (extract/replace/dup lane, splat, shuffle, swizzle,
    // extend, narrow, extmul, promote/demote, conversions of the low half,
    // bitmask) is written against that mapping. The VMX unpack/pack
    // instructions see the two doublewords in the opposite order, so extend
    // and narrow add an xxswapd.
    //
    // f32 in a scalar FPR is double format (the whole port relies on that) but
    // an f32 lane is single format, so scalar <-> lane moves go through
    // xscvdpspn / xscvspdpn (non-signalling, NaN payloads preserved).
    //
    // GPR scratch: dataTempRegister (r11) and memoryTempRegister (r12) only,
    // as everywhere else in this file. No vector operation uses fpTempRegister.
    // =====================================================================
    using VRegisterID = PPC64Registers::VRegisterID;
    static constexpr uint32_t vsr(FPRegisterID r) { return static_cast<uint32_t>(r); }
    static constexpr uint32_t vsr(VRegisterID r) { return 32 + static_cast<uint32_t>(r); }
    static constexpr VRegisterID vecTemp0 = PPC64Registers::v0;
    static constexpr VRegisterID vecTemp1 = PPC64Registers::v1;
    static constexpr VRegisterID vecTemp2 = PPC64Registers::v2;
    static constexpr VRegisterID vecTemp3 = PPC64Registers::v3;
    static constexpr VRegisterID vecTemp4 = PPC64Registers::v4;
    // Used only inside materializeVectorBits().
    static constexpr VRegisterID vecTempMaterialize = PPC64Registers::v5;

    using VMXBinaryOp = void (PPC64Assembler::*)(VRegisterID, VRegisterID, VRegisterID);
    using VMXUnaryOp = void (PPC64Assembler::*)(VRegisterID, VRegisterID);
    using VSXBinaryOp = void (PPC64Assembler::*)(uint32_t, uint32_t, uint32_t);
    using VSXUnaryOp = void (PPC64Assembler::*)(uint32_t, uint32_t);

    void copyToVR(FPRegisterID src, VRegisterID dest) { m_assembler.xxlor(vsr(dest), vsr(src), vsr(src)); }
    void copyFromVR(VRegisterID src, FPRegisterID dest) { m_assembler.xxlor(vsr(dest), vsr(src), vsr(src)); }
    void zeroVR(VRegisterID r) { m_assembler.vxor(r, r, r); }

    void vmxBinary(VMXBinaryOp op, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        (m_assembler.*op)(vecTemp0, vecTemp0, vecTemp1);
        copyFromVR(vecTemp0, dest);
    }
    void vmxUnary(VMXUnaryOp op, FPRegisterID input, FPRegisterID dest)
    {
        copyToVR(input, vecTemp0);
        (m_assembler.*op)(vecTemp0, vecTemp0);
        copyFromVR(vecTemp0, dest);
    }

    static unsigned isaElementForLane(unsigned elementBytes, unsigned lane)
    {
        return lane ^ (8 / elementBytes - 1);
    }

    // Materializes raw ISA doublewords: dw0 is the value mfvsrd would read.
    // Clobbers dataTempRegister and vecTempMaterialize.
    void materializeVectorBits(uint64_t dw0, uint64_t dw1, uint32_t destVSR)
    {
        ASSERT(destVSR != vsr(vecTempMaterialize));
        if (!dw0 && !dw1) {
            m_assembler.xxlxor(destVSR, destVSR, destVSR);
            return;
        }
        if (!~dw0 && !~dw1) {
            m_assembler.xxleqv(destVSR, destVSR, destVSR);
            return;
        }
        moveImmToScratch(static_cast<int64_t>(dw0), dataTempRegister);
        m_assembler.mtvsrd(destVSR, dataTempRegister);
        if (dw1 == dw0) {
            m_assembler.xxpermdi(destVSR, destVSR, destVSR, 0);
            return;
        }
        moveImmToScratch(static_cast<int64_t>(dw1), dataTempRegister);
        m_assembler.mtvsrd(vsr(vecTempMaterialize), dataTempRegister);
        m_assembler.xxpermdi(destVSR, destVSR, vsr(vecTempMaterialize), 0);
    }
    // ISA byte k of a vector built by materializeVectorBits().
    static void setISAByte(uint64_t& dw0, uint64_t& dw1, unsigned k, uint8_t value)
    {
        uint64_t& dw = k < 8 ? dw0 : dw1;
        unsigned shift = 56 - 8 * (k & 7);
        dw = (dw & ~(0xffull << shift)) | (static_cast<uint64_t>(value) << shift);
    }

    void move128ToVector(v128_t value, FPRegisterID dest)
    {
        // Lane bytes 0-7 form doubleword 0 as a little-endian u64.
        materializeVectorBits(value.u64x2[0], value.u64x2[1], vsr(dest));
    }
    void moveZeroToVector(FPRegisterID dest) { m_assembler.xxlxor(vsr(dest), vsr(dest), vsr(dest)); }

    // result byte (wasm lane order) l = concat(first, second) byte map[l], map[l] < 32.
    void vectorPermuteBytes(const uint8_t map[16], FPRegisterID first, FPRegisterID second, FPRegisterID dest)
    {
        // vperm selects ISA byte c of first||second for control byte c. Wasm
        // byte x of either input is ISA byte x ^ 7 of it, so the control for
        // result ISA byte k (= wasm byte k ^ 7) is map[k ^ 7] ^ 7.
        uint64_t dw0 = 0, dw1 = 0;
        for (unsigned k = 0; k < 16; ++k) {
            ASSERT(map[k ^ 7] < 32);
            setISAByte(dw0, dw1, k, map[k ^ 7] ^ 7);
        }
        materializeVectorBits(dw0, dw1, vsr(vecTemp2));
        copyToVR(first, vecTemp0);
        copyToVR(second, vecTemp1);
        m_assembler.vperm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp0, dest);
    }

    // wasm i8x16.shuffle: pattern bytes are 0..31 indices into a||b.
    void vectorShuffle(v128_t pattern, FPRegisterID a, FPRegisterID b, FPRegisterID dest)
    {
        uint8_t map[16];
        for (unsigned i = 0; i < 16; ++i)
            map[i] = pattern.u8x16[i] & 31;
        vectorPermuteBytes(map, a, b, dest);
    }

    // --- Lane access ------------------------------------------------------
    void vectorExtractLaneToGPR(unsigned elementBytes, unsigned lane, bool signExtend, FPRegisterID src, RegisterID dest)
    {
        unsigned perDoubleword = 8 / elementBytes;
        ASSERT(lane < 2 * perDoubleword);
        if (lane >= perDoubleword) {
            m_assembler.xxswapd(vsr(vecTemp0), vsr(src));
            m_assembler.mfvsrd(dest, vsr(vecTemp0));
        } else
            m_assembler.mfvsrd(dest, vsr(src));
        if (elementBytes == 8)
            return;
        unsigned width = elementBytes * 8;
        unsigned shift = (lane % perDoubleword) * width;
        if (signExtend) {
            unsigned left = 64 - shift - width;
            if (left)
                m_assembler.sldi(dest, dest, left);
            m_assembler.sradi(dest, dest, 64 - width);
        } else
            m_assembler.rldicl(dest, dest, (64 - shift) & 63, 64 - width);
    }
    void vectorReplaceLaneFromGPR(unsigned elementBytes, unsigned lane, RegisterID src, FPRegisterID dest)
    {
        ASSERT(src != dataTempRegister);
        if (elementBytes < 8 && supportsISA3_00()) {
            // POWER9 inserts in place. mtvsrwz puts src in word 1, whose low
            // byte and halfword are where vinsertb/vinserth take their element.
            unsigned byteOffset = elementBytes * isaElementForLane(elementBytes, lane);
            if (elementBytes == 4) {
                m_assembler.mtvsrwz(vsr(vecTemp0), src);
                m_assembler.xxinsertw(vsr(dest), vsr(vecTemp0), byteOffset);
                return;
            }
            m_assembler.mtvsrwz(vsr(vecTemp1), src);
            copyToVR(dest, vecTemp0);
            if (elementBytes == 1)
                m_assembler.vinsertb(vecTemp0, vecTemp1, byteOffset);
            else
                m_assembler.vinserth(vecTemp0, vecTemp1, byteOffset);
            copyFromVR(vecTemp0, dest);
            return;
        }
        unsigned perDoubleword = 8 / elementBytes;
        ASSERT(lane < 2 * perDoubleword);
        bool high = lane >= perDoubleword;
        uint32_t temp = vsr(vecTemp0);
        if (elementBytes == 8)
            m_assembler.mtvsrd(temp, src);
        else {
            unsigned width = elementBytes * 8;
            unsigned shift = (lane % perDoubleword) * width;
            if (high) {
                m_assembler.xxswapd(temp, vsr(dest));
                m_assembler.mfvsrd(dataTempRegister, temp);
            } else
                m_assembler.mfvsrd(dataTempRegister, vsr(dest));
            m_assembler.rldimi(dataTempRegister, src, shift, 64 - shift - width);
            m_assembler.mtvsrd(temp, dataTempRegister);
        }
        if (high)
            m_assembler.xxpermdi(vsr(dest), vsr(dest), temp, 0);
        else
            m_assembler.xxpermdi(vsr(dest), temp, vsr(dest), 1);
    }

    void vectorExtractLane(SIMDLane simdLane, SIMDSignMode signMode, TrustedImm32 lane, FPRegisterID src, RegisterID dest)
    {
        ASSERT(scalarTypeIsIntegral(simdLane));
        vectorExtractLaneToGPR(elementByteSize(simdLane), lane.m_value, signMode == SIMDSignMode::Signed, src, dest);
    }
    void vectorExtractLane(SIMDLane simdLane, TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        if (simdLane == SIMDLane::f32x4)
            vectorExtractLaneFloat32(lane, src, dest);
        else {
            ASSERT(simdLane == SIMDLane::f64x2);
            vectorExtractLaneFloat64(lane, src, dest);
        }
    }
    void vectorExtractLaneInt64(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(8, lane.m_value, false, src, dest); }
    void vectorExtractLaneInt32(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(4, lane.m_value, false, src, dest); }
    void vectorExtractLaneSignedInt16(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(2, lane.m_value, true, src, dest); }
    void vectorExtractLaneUnsignedInt16(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(2, lane.m_value, false, src, dest); }
    void vectorExtractLaneSignedInt8(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(1, lane.m_value, true, src, dest); }
    void vectorExtractLaneUnsignedInt8(TrustedImm32 lane, FPRegisterID src, RegisterID dest) { vectorExtractLaneToGPR(1, lane.m_value, false, src, dest); }
    void vectorExtractLaneFloat64(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        if (lane.m_value)
            m_assembler.xxswapd(vsr(dest), vsr(src));
        else if (src != dest)
            m_assembler.xxlor(vsr(dest), vsr(src), vsr(src));
    }
    void vectorExtractLaneFloat32(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        // Rotate the lane's word into word 0, then single -> double format.
        unsigned word = isaElementForLane(4, lane.m_value);
        if (word) {
            m_assembler.xxsldwi(vsr(vecTemp0), vsr(src), vsr(src), word);
            m_assembler.xscvspdpn(vsr(dest), vsr(vecTemp0));
        } else
            m_assembler.xscvspdpn(vsr(dest), vsr(src));
    }

    void vectorReplaceLane(SIMDLane simdLane, TrustedImm32 lane, RegisterID src, FPRegisterID dest)
    {
        ASSERT(scalarTypeIsIntegral(simdLane));
        vectorReplaceLaneFromGPR(elementByteSize(simdLane), lane.m_value, src, dest);
    }
    void vectorReplaceLane(SIMDLane simdLane, TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        if (simdLane == SIMDLane::f32x4)
            vectorReplaceLaneFloat32(lane, src, dest);
        else {
            ASSERT(simdLane == SIMDLane::f64x2);
            vectorReplaceLaneFloat64(lane, src, dest);
        }
    }
    void vectorReplaceLaneInt64(TrustedImm32 lane, RegisterID src, FPRegisterID dest) { vectorReplaceLaneFromGPR(8, lane.m_value, src, dest); }
    void vectorReplaceLaneInt32(TrustedImm32 lane, RegisterID src, FPRegisterID dest) { vectorReplaceLaneFromGPR(4, lane.m_value, src, dest); }
    void vectorReplaceLaneInt16(TrustedImm32 lane, RegisterID src, FPRegisterID dest) { vectorReplaceLaneFromGPR(2, lane.m_value, src, dest); }
    void vectorReplaceLaneInt8(TrustedImm32 lane, RegisterID src, FPRegisterID dest) { vectorReplaceLaneFromGPR(1, lane.m_value, src, dest); }
    void vectorReplaceLaneFloat64(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        if (lane.m_value)
            m_assembler.xxpermdi(vsr(dest), vsr(dest), vsr(src), 0);
        else
            m_assembler.xxpermdi(vsr(dest), vsr(src), vsr(dest), 1);
    }
    void vectorReplaceLaneFloat32(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest)
    {
        if (supportsISA3_00()) {
            // xscvdpspn leaves the single in word 0; xxinsertw takes word 1.
            m_assembler.xscvdpspn(vsr(vecTemp0), vsr(src));
            m_assembler.xxsldwi(vsr(vecTemp0), vsr(vecTemp0), vsr(vecTemp0), 3);
            m_assembler.xxinsertw(vsr(dest), vsr(vecTemp0), 4 * isaElementForLane(4, lane.m_value));
            return;
        }
        // double -> single bits in word 0, then into a GPR and in as an i32 lane.
        m_assembler.xscvdpspn(vsr(vecTemp0), vsr(src));
        m_assembler.mfvsrd(memoryTempRegister, vsr(vecTemp0));
        m_assembler.srdi(memoryTempRegister, memoryTempRegister, 32);
        vectorReplaceLaneFromGPR(4, lane.m_value, memoryTempRegister, dest);
    }

    void vectorSplat(SIMDLane lane, RegisterID src, FPRegisterID dest)
    {
        // mtvsrwz puts the word in ISA word 1, i.e. the low-order end of
        // doubleword 0: byte 7, halfword 3, word 1.
        switch (lane) {
        case SIMDLane::i8x16:
            m_assembler.mtvsrwz(vsr(vecTemp0), src);
            m_assembler.vspltb(vecTemp0, vecTemp0, 7);
            copyFromVR(vecTemp0, dest);
            return;
        case SIMDLane::i16x8:
            m_assembler.mtvsrwz(vsr(vecTemp0), src);
            m_assembler.vsplth(vecTemp0, vecTemp0, 3);
            copyFromVR(vecTemp0, dest);
            return;
        case SIMDLane::i32x4:
            m_assembler.mtvsrwz(vsr(dest), src);
            m_assembler.xxspltw(vsr(dest), vsr(dest), 1);
            return;
        case SIMDLane::i64x2:
            m_assembler.mtvsrd(vsr(dest), src);
            m_assembler.xxpermdi(vsr(dest), vsr(dest), vsr(dest), 0);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    void vectorSplat(SIMDLane lane, FPRegisterID src, FPRegisterID dest)
    {
        if (lane == SIMDLane::f32x4) {
            m_assembler.xscvdpspn(vsr(dest), vsr(src));
            m_assembler.xxspltw(vsr(dest), vsr(dest), 0);
            return;
        }
        ASSERT(lane == SIMDLane::f64x2);
        m_assembler.xxpermdi(vsr(dest), vsr(src), vsr(src), 0);
    }
    void vectorSplatInt8(RegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::i8x16, src, dest); }
    void vectorSplatInt16(RegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::i16x8, src, dest); }
    void vectorSplatInt32(RegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::i32x4, src, dest); }
    void vectorSplatInt64(RegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::i64x2, src, dest); }
    void vectorSplatFloat32(FPRegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::f32x4, src, dest); }
    void vectorSplatFloat64(FPRegisterID src, FPRegisterID dest) { vectorSplat(SIMDLane::f64x2, src, dest); }

    void vectorDupElementInt32(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest) { m_assembler.xxspltw(vsr(dest), vsr(src), isaElementForLane(4, lane.m_value)); }
    void vectorDupElementFloat32(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest) { vectorDupElementInt32(lane, src, dest); }
    void vectorDupElementInt64(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest) { m_assembler.xxpermdi(vsr(dest), vsr(src), vsr(src), lane.m_value ? 3 : 0); }
    void vectorDupElementFloat64(TrustedImm32 lane, FPRegisterID src, FPRegisterID dest) { vectorDupElementInt64(lane, src, dest); }

    // --- Memory lane access ------------------------------------------------
    void vectorLoad8Lane(Address address, TrustedImm32 lane, FPRegisterID dest) { load8(address, memoryTempRegister); vectorReplaceLaneFromGPR(1, lane.m_value, memoryTempRegister, dest); }
    void vectorLoad16Lane(Address address, TrustedImm32 lane, FPRegisterID dest) { load16(address, memoryTempRegister); vectorReplaceLaneFromGPR(2, lane.m_value, memoryTempRegister, dest); }
    void vectorLoad32Lane(Address address, TrustedImm32 lane, FPRegisterID dest) { load32(address, memoryTempRegister); vectorReplaceLaneFromGPR(4, lane.m_value, memoryTempRegister, dest); }
    void vectorLoad64Lane(Address address, TrustedImm32 lane, FPRegisterID dest) { load64(address, memoryTempRegister); vectorReplaceLaneFromGPR(8, lane.m_value, memoryTempRegister, dest); }
    void vectorStore8Lane(FPRegisterID src, Address address, TrustedImm32 lane) { vectorExtractLaneToGPR(1, lane.m_value, false, src, dataTempRegister); store8(dataTempRegister, address); }
    void vectorStore16Lane(FPRegisterID src, Address address, TrustedImm32 lane) { vectorExtractLaneToGPR(2, lane.m_value, false, src, dataTempRegister); store16(dataTempRegister, address); }
    void vectorStore32Lane(FPRegisterID src, Address address, TrustedImm32 lane) { vectorExtractLaneToGPR(4, lane.m_value, false, src, dataTempRegister); store32(dataTempRegister, address); }
    void vectorStore64Lane(FPRegisterID src, Address address, TrustedImm32 lane) { vectorExtractLaneToGPR(8, lane.m_value, false, src, dataTempRegister); store64(dataTempRegister, address); }
    void vectorLoad8Splat(Address address, FPRegisterID dest) { load8(address, memoryTempRegister); vectorSplat(SIMDLane::i8x16, memoryTempRegister, dest); }
    void vectorLoad16Splat(Address address, FPRegisterID dest) { load16(address, memoryTempRegister); vectorSplat(SIMDLane::i16x8, memoryTempRegister, dest); }
    void vectorLoad32Splat(Address address, FPRegisterID dest) { load32(address, memoryTempRegister); vectorSplat(SIMDLane::i32x4, memoryTempRegister, dest); }
    void vectorLoad64Splat(Address address, FPRegisterID dest) { load64(address, memoryTempRegister); vectorSplat(SIMDLane::i64x2, memoryTempRegister, dest); }
    // v128.load32_zero / load64_zero: the loaded bits in the low lane(s), zeros elsewhere.
    template<typename AddressType>
    void vectorLoad32Zero(AddressType address, FPRegisterID dest)
    {
        load32(address, memoryTempRegister);
        m_assembler.mtvsrwz(vsr(vecTemp0), memoryTempRegister);
        m_assembler.xxlxor(vsr(vecTemp1), vsr(vecTemp1), vsr(vecTemp1));
        m_assembler.xxpermdi(vsr(dest), vsr(vecTemp0), vsr(vecTemp1), 0);
    }
    template<typename AddressType>
    void vectorLoad64Zero(AddressType address, FPRegisterID dest)
    {
        load64(address, memoryTempRegister);
        m_assembler.mtvsrd(vsr(vecTemp0), memoryTempRegister);
        m_assembler.xxlxor(vsr(vecTemp1), vsr(vecTemp1), vsr(vecTemp1));
        m_assembler.xxpermdi(vsr(dest), vsr(vecTemp0), vsr(vecTemp1), 0);
    }

    // --- Bitwise -------------------------------------------------------------
    void vectorAnd(SIMDInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { m_assembler.xxland(vsr(dest), vsr(left), vsr(right)); }
    void vectorOr(SIMDInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { m_assembler.xxlor(vsr(dest), vsr(left), vsr(right)); }
    void vectorXor(SIMDInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { m_assembler.xxlxor(vsr(dest), vsr(left), vsr(right)); }
    // left & ~right
    void vectorAndnot(SIMDInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { m_assembler.xxlandc(vsr(dest), vsr(left), vsr(right)); }
    void vectorNot(SIMDInfo, FPRegisterID input, FPRegisterID dest) { m_assembler.xxlnor(vsr(dest), vsr(input), vsr(input)); }
    // inputBitsAndDest = (left & mask) | (right & ~mask), mask = inputBitsAndDest
    void vectorBitwiseSelect(FPRegisterID left, FPRegisterID right, FPRegisterID inputBitsAndDest)
    {
        m_assembler.xxsel(vsr(inputBitsAndDest), vsr(right), vsr(left), vsr(inputBitsAndDest));
    }

    // --- Integer arithmetic --------------------------------------------------
    static VMXBinaryOp vmxAddOp(SIMDLane lane)
    {
        switch (lane) {
        case SIMDLane::i8x16: return &PPC64Assembler::vaddubm;
        case SIMDLane::i16x8: return &PPC64Assembler::vadduhm;
        case SIMDLane::i32x4: return &PPC64Assembler::vadduwm;
        case SIMDLane::i64x2: return &PPC64Assembler::vaddudm;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    static VMXBinaryOp vmxSubOp(SIMDLane lane)
    {
        switch (lane) {
        case SIMDLane::i8x16: return &PPC64Assembler::vsububm;
        case SIMDLane::i16x8: return &PPC64Assembler::vsubuhm;
        case SIMDLane::i32x4: return &PPC64Assembler::vsubuwm;
        case SIMDLane::i64x2: return &PPC64Assembler::vsubudm;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    static VMXBinaryOp vmxMinMaxOp(SIMDLane lane, bool isSigned, bool isMin)
    {
        switch (lane) {
        case SIMDLane::i8x16: return isMin ? (isSigned ? &PPC64Assembler::vminsb : &PPC64Assembler::vminub) : (isSigned ? &PPC64Assembler::vmaxsb : &PPC64Assembler::vmaxub);
        case SIMDLane::i16x8: return isMin ? (isSigned ? &PPC64Assembler::vminsh : &PPC64Assembler::vminuh) : (isSigned ? &PPC64Assembler::vmaxsh : &PPC64Assembler::vmaxuh);
        case SIMDLane::i32x4: return isMin ? (isSigned ? &PPC64Assembler::vminsw : &PPC64Assembler::vminuw) : (isSigned ? &PPC64Assembler::vmaxsw : &PPC64Assembler::vmaxuw);
        case SIMDLane::i64x2: return isMin ? (isSigned ? &PPC64Assembler::vminsd : &PPC64Assembler::vminud) : (isSigned ? &PPC64Assembler::vmaxsd : &PPC64Assembler::vmaxud);
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    static VMXBinaryOp vmxCompareEqualOp(SIMDLane lane)
    {
        switch (lane) {
        case SIMDLane::i8x16: return &PPC64Assembler::vcmpequb;
        case SIMDLane::i16x8: return &PPC64Assembler::vcmpequh;
        case SIMDLane::i32x4: return &PPC64Assembler::vcmpequw;
        case SIMDLane::i64x2: return &PPC64Assembler::vcmpequd;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    static VMXBinaryOp vmxCompareGreaterOp(SIMDLane lane, bool isSigned)
    {
        switch (lane) {
        case SIMDLane::i8x16: return isSigned ? &PPC64Assembler::vcmpgtsb : &PPC64Assembler::vcmpgtub;
        case SIMDLane::i16x8: return isSigned ? &PPC64Assembler::vcmpgtsh : &PPC64Assembler::vcmpgtuh;
        case SIMDLane::i32x4: return isSigned ? &PPC64Assembler::vcmpgtsw : &PPC64Assembler::vcmpgtuw;
        case SIMDLane::i64x2: return isSigned ? &PPC64Assembler::vcmpgtsd : &PPC64Assembler::vcmpgtud;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void vectorAdd(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        if (simdInfo.lane == SIMDLane::f32x4)
            m_assembler.xvaddsp(vsr(dest), vsr(left), vsr(right));
        else if (simdInfo.lane == SIMDLane::f64x2)
            m_assembler.xvadddp(vsr(dest), vsr(left), vsr(right));
        else
            vmxBinary(vmxAddOp(simdInfo.lane), left, right, dest);
    }
    void vectorSub(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        if (simdInfo.lane == SIMDLane::f32x4)
            m_assembler.xvsubsp(vsr(dest), vsr(left), vsr(right));
        else if (simdInfo.lane == SIMDLane::f64x2)
            m_assembler.xvsubdp(vsr(dest), vsr(left), vsr(right));
        else
            vmxBinary(vmxSubOp(simdInfo.lane), left, right, dest);
    }
    void vectorMul(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        switch (simdInfo.lane) {
        case SIMDLane::f32x4:
            m_assembler.xvmulsp(vsr(dest), vsr(left), vsr(right));
            return;
        case SIMDLane::f64x2:
            m_assembler.xvmuldp(vsr(dest), vsr(left), vsr(right));
            return;
        case SIMDLane::i16x8:
            copyToVR(left, vecTemp0);
            copyToVR(right, vecTemp1);
            zeroVR(vecTemp2);
            m_assembler.vmladduhm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
            copyFromVR(vecTemp0, dest);
            return;
        case SIMDLane::i32x4:
            vmxBinary(&PPC64Assembler::vmuluwm, left, right, dest);
            return;
        case SIMDLane::i64x2:
            if (supportsISA3_1()) {
                vmxBinary(&PPC64Assembler::vmulld, left, right, dest);
                return;
            }
            // No doubleword vector multiply before POWER10: one mulld per lane.
            m_assembler.mfvsrd(dataTempRegister, vsr(left));
            m_assembler.mfvsrd(memoryTempRegister, vsr(right));
            m_assembler.mulld(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.mtvsrd(vsr(vecTemp0), dataTempRegister);
            m_assembler.xxswapd(vsr(vecTemp1), vsr(left));
            m_assembler.mfvsrd(dataTempRegister, vsr(vecTemp1));
            m_assembler.xxswapd(vsr(vecTemp1), vsr(right));
            m_assembler.mfvsrd(memoryTempRegister, vsr(vecTemp1));
            m_assembler.mulld(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.mtvsrd(vsr(vecTemp1), dataTempRegister);
            m_assembler.xxpermdi(vsr(dest), vsr(vecTemp0), vsr(vecTemp1), 0);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    void vectorDiv(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        if (simdInfo.lane == SIMDLane::f32x4)
            m_assembler.xvdivsp(vsr(dest), vsr(left), vsr(right));
        else {
            ASSERT(simdInfo.lane == SIMDLane::f64x2);
            m_assembler.xvdivdp(vsr(dest), vsr(left), vsr(right));
        }
    }
    void vectorAddSat(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        if (simdInfo.lane == SIMDLane::i8x16)
            vmxBinary(isSigned ? &PPC64Assembler::vaddsbs : &PPC64Assembler::vaddubs, left, right, dest);
        else {
            ASSERT(simdInfo.lane == SIMDLane::i16x8);
            vmxBinary(isSigned ? &PPC64Assembler::vaddshs : &PPC64Assembler::vadduhs, left, right, dest);
        }
    }
    void vectorSubSat(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        if (simdInfo.lane == SIMDLane::i8x16)
            vmxBinary(isSigned ? &PPC64Assembler::vsubsbs : &PPC64Assembler::vsububs, left, right, dest);
        else {
            ASSERT(simdInfo.lane == SIMDLane::i16x8);
            vmxBinary(isSigned ? &PPC64Assembler::vsubshs : &PPC64Assembler::vsubuhs, left, right, dest);
        }
    }
    void vectorAvgRound(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        vmxBinary(simdInfo.lane == SIMDLane::i8x16 ? &PPC64Assembler::vavgub : &PPC64Assembler::vavguh, left, right, dest);
    }
    void vectorNeg(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        if (simdInfo.lane == SIMDLane::f32x4) {
            m_assembler.xvnegsp(vsr(dest), vsr(input));
            return;
        }
        if (simdInfo.lane == SIMDLane::f64x2) {
            m_assembler.xvnegdp(vsr(dest), vsr(input));
            return;
        }
        copyToVR(input, vecTemp1);
        zeroVR(vecTemp0);
        (m_assembler.*vmxSubOp(simdInfo.lane))(vecTemp0, vecTemp0, vecTemp1);
        copyFromVR(vecTemp0, dest);
    }
    void vectorAbs(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        if (simdInfo.lane == SIMDLane::f32x4) {
            m_assembler.xvabssp(vsr(dest), vsr(input));
            return;
        }
        if (simdInfo.lane == SIMDLane::f64x2) {
            m_assembler.xvabsdp(vsr(dest), vsr(input));
            return;
        }
        // max_s(x, 0 - x); the most negative value maps to itself, as wasm wants.
        copyToVR(input, vecTemp0);
        zeroVR(vecTemp1);
        (m_assembler.*vmxSubOp(simdInfo.lane))(vecTemp1, vecTemp1, vecTemp0);
        (m_assembler.*vmxMinMaxOp(simdInfo.lane, true, false))(vecTemp0, vecTemp0, vecTemp1);
        copyFromVR(vecTemp0, dest);
    }
    void vectorPopcnt(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT_UNUSED(simdInfo, simdInfo.lane == SIMDLane::i8x16);
        vmxUnary(&PPC64Assembler::vpopcntb, input, dest);
    }

    void floatingPointVectorMinMax(bool isMin, SIMDLane lane, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        // xvmin/xvmax order -0 below +0 but return the non-NaN operand when one
        // input is a NaN. Wasm wants a NaN, so lanes where either input is
        // unordered take left + right instead (a quieted input NaN).
        bool isF32 = lane == SIMDLane::f32x4;
        ASSERT(isF32 || lane == SIMDLane::f64x2);
        uint32_t t0 = vsr(vecTemp0), t1 = vsr(vecTemp1), t2 = vsr(vecTemp2), t3 = vsr(vecTemp3);
        if (isMin)
            isF32 ? m_assembler.xvminsp(t0, vsr(left), vsr(right)) : m_assembler.xvmindp(t0, vsr(left), vsr(right));
        else
            isF32 ? m_assembler.xvmaxsp(t0, vsr(left), vsr(right)) : m_assembler.xvmaxdp(t0, vsr(left), vsr(right));
        isF32 ? m_assembler.xvaddsp(t1, vsr(left), vsr(right)) : m_assembler.xvadddp(t1, vsr(left), vsr(right));
        isF32 ? m_assembler.xvcmpeqsp(t2, vsr(left), vsr(left)) : m_assembler.xvcmpeqdp(t2, vsr(left), vsr(left));
        isF32 ? m_assembler.xvcmpeqsp(t3, vsr(right), vsr(right)) : m_assembler.xvcmpeqdp(t3, vsr(right), vsr(right));
        m_assembler.xxland(t2, t2, t3);
        m_assembler.xxsel(vsr(dest), t1, t0, t2);
    }
    void vectorMin(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        if (scalarTypeIsFloatingPoint(simdInfo.lane))
            floatingPointVectorMinMax(true, simdInfo.lane, left, right, dest);
        else
            vmxBinary(vmxMinMaxOp(simdInfo.lane, simdInfo.signMode == SIMDSignMode::Signed, true), left, right, dest);
    }
    void vectorMax(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        if (scalarTypeIsFloatingPoint(simdInfo.lane))
            floatingPointVectorMinMax(false, simdInfo.lane, left, right, dest);
        else
            vmxBinary(vmxMinMaxOp(simdInfo.lane, simdInfo.signMode == SIMDSignMode::Signed, false), left, right, dest);
    }
    void vectorRelaxedMin(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { vectorMin(simdInfo, left, right, dest); }
    void vectorRelaxedMax(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest) { vectorMax(simdInfo, left, right, dest); }
    // pmin(a, b) = b < a ? b : a;  pmax(a, b) = a < b ? b : a
    void vectorPmin(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        uint32_t t0 = vsr(vecTemp0);
        simdInfo.lane == SIMDLane::f32x4 ? m_assembler.xvcmpgtsp(t0, vsr(left), vsr(right)) : m_assembler.xvcmpgtdp(t0, vsr(left), vsr(right));
        m_assembler.xxsel(vsr(dest), vsr(left), vsr(right), t0);
    }
    void vectorPmax(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        uint32_t t0 = vsr(vecTemp0);
        simdInfo.lane == SIMDLane::f32x4 ? m_assembler.xvcmpgtsp(t0, vsr(right), vsr(left)) : m_assembler.xvcmpgtdp(t0, vsr(right), vsr(left));
        m_assembler.xxsel(vsr(dest), vsr(left), vsr(right), t0);
    }
    void vectorPmin(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID) { vectorPmin(simdInfo, left, right, dest); }
    void vectorPmax(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID) { vectorPmax(simdInfo, left, right, dest); }

    // --- Floating-point unary ------------------------------------------------
    void vsxUnary(SIMDLane lane, VSXUnaryOp opF32, VSXUnaryOp opF64, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT(lane == SIMDLane::f32x4 || lane == SIMDLane::f64x2);
        (m_assembler.*(lane == SIMDLane::f32x4 ? opF32 : opF64))(vsr(dest), vsr(input));
    }
    void vectorCeil(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest) { vsxUnary(simdInfo.lane, &PPC64Assembler::xvrspip, &PPC64Assembler::xvrdpip, input, dest); }
    void vectorFloor(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest) { vsxUnary(simdInfo.lane, &PPC64Assembler::xvrspim, &PPC64Assembler::xvrdpim, input, dest); }
    void vectorTrunc(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest) { vsxUnary(simdInfo.lane, &PPC64Assembler::xvrspiz, &PPC64Assembler::xvrdpiz, input, dest); }
    // Rounds in the current mode, which JSC leaves at round-to-nearest-even.
    void vectorNearest(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest) { vsxUnary(simdInfo.lane, &PPC64Assembler::xvrspic, &PPC64Assembler::xvrdpic, input, dest); }
    void vectorSqrt(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest) { vsxUnary(simdInfo.lane, &PPC64Assembler::xvsqrtsp, &PPC64Assembler::xvsqrtdp, input, dest); }

    void vectorFusedMulAdd(SIMDInfo simdInfo, FPRegisterID mul1, FPRegisterID mul2, FPRegisterID addend, FPRegisterID dest, FPRegisterID)
    {
        uint32_t t0 = vsr(vecTemp0);
        m_assembler.xxlor(t0, vsr(addend), vsr(addend));
        simdInfo.lane == SIMDLane::f32x4 ? m_assembler.xvmaddasp(t0, vsr(mul1), vsr(mul2)) : m_assembler.xvmaddadp(t0, vsr(mul1), vsr(mul2));
        m_assembler.xxlor(vsr(dest), t0, t0);
    }
    // addend - mul1 * mul2
    void vectorFusedNegMulAdd(SIMDInfo simdInfo, FPRegisterID mul1, FPRegisterID mul2, FPRegisterID addend, FPRegisterID dest, FPRegisterID)
    {
        uint32_t t0 = vsr(vecTemp0);
        m_assembler.xxlor(t0, vsr(addend), vsr(addend));
        simdInfo.lane == SIMDLane::f32x4 ? m_assembler.xvnmsubasp(t0, vsr(mul1), vsr(mul2)) : m_assembler.xvnmsubadp(t0, vsr(mul1), vsr(mul2));
        m_assembler.xxlor(vsr(dest), t0, t0);
    }

    // --- Comparisons ---------------------------------------------------------
    void compareFloatingPointVector(DoubleCondition cond, SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        bool isF32 = simdInfo.lane == SIMDLane::f32x4;
        ASSERT(isF32 || simdInfo.lane == SIMDLane::f64x2);
        uint32_t l = vsr(left), r = vsr(right), d = vsr(dest), t0 = vsr(vecTemp0), t1 = vsr(vecTemp1);
        auto eq = [&](uint32_t t, uint32_t a, uint32_t b) { isF32 ? m_assembler.xvcmpeqsp(t, a, b) : m_assembler.xvcmpeqdp(t, a, b); };
        auto gt = [&](uint32_t t, uint32_t a, uint32_t b) { isF32 ? m_assembler.xvcmpgtsp(t, a, b) : m_assembler.xvcmpgtdp(t, a, b); };
        auto ge = [&](uint32_t t, uint32_t a, uint32_t b) { isF32 ? m_assembler.xvcmpgesp(t, a, b) : m_assembler.xvcmpgedp(t, a, b); };
        switch (cond) {
        case DoubleEqualAndOrdered: eq(d, l, r); return;
        case DoubleNotEqualOrUnordered: eq(t0, l, r); m_assembler.xxlnor(d, t0, t0); return;
        case DoubleGreaterThanAndOrdered: gt(d, l, r); return;
        case DoubleGreaterThanOrEqualAndOrdered: ge(d, l, r); return;
        case DoubleLessThanAndOrdered: gt(d, r, l); return;
        case DoubleLessThanOrEqualAndOrdered: ge(d, r, l); return;
        case DoubleNotEqualAndOrdered: gt(t0, l, r); gt(t1, r, l); m_assembler.xxlor(d, t0, t1); return;
        case DoubleEqualOrUnordered: gt(t0, l, r); gt(t1, r, l); m_assembler.xxlnor(d, t0, t1); return;
        case DoubleGreaterThanOrUnordered: ge(t0, r, l); m_assembler.xxlnor(d, t0, t0); return;
        case DoubleGreaterThanOrEqualOrUnordered: gt(t0, r, l); m_assembler.xxlnor(d, t0, t0); return;
        case DoubleLessThanOrUnordered: ge(t0, l, r); m_assembler.xxlnor(d, t0, t0); return;
        case DoubleLessThanOrEqualOrUnordered: gt(t0, l, r); m_assembler.xxlnor(d, t0, t0); return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    void compareIntegerVectorInVR(RelationalCondition cond, SIMDLane lane, VRegisterID a, VRegisterID b, VRegisterID result)
    {
        bool invertResult = false;
        auto greater = [&](bool isSigned, VRegisterID x, VRegisterID y) { (m_assembler.*vmxCompareGreaterOp(lane, isSigned))(result, x, y); };
        switch (cond) {
        case Equal: (m_assembler.*vmxCompareEqualOp(lane))(result, a, b); break;
        case NotEqual: (m_assembler.*vmxCompareEqualOp(lane))(result, a, b); invertResult = true; break;
        case GreaterThan: greater(true, a, b); break;
        case LessThan: greater(true, b, a); break;
        case GreaterThanOrEqual: greater(true, b, a); invertResult = true; break;
        case LessThanOrEqual: greater(true, a, b); invertResult = true; break;
        case Above: greater(false, a, b); break;
        case Below: greater(false, b, a); break;
        case AboveOrEqual: greater(false, b, a); invertResult = true; break;
        case BelowOrEqual: greater(false, a, b); invertResult = true; break;
        }
        if (invertResult)
            m_assembler.vnor(result, result, result);
    }
    void compareIntegerVector(RelationalCondition cond, SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        compareIntegerVectorInVR(cond, simdInfo.lane, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp2, dest);
    }
    void compareIntegerVector(RelationalCondition cond, SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID) { compareIntegerVector(cond, simdInfo, left, right, dest); }
    void compareIntegerVectorWithZero(RelationalCondition cond, SIMDInfo simdInfo, FPRegisterID vector, FPRegisterID dest)
    {
        copyToVR(vector, vecTemp0);
        zeroVR(vecTemp1);
        compareIntegerVectorInVR(cond, simdInfo.lane, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp2, dest);
    }
    void compareIntegerVectorWithZero(RelationalCondition cond, SIMDInfo simdInfo, FPRegisterID vector, FPRegisterID dest, RegisterID) { compareIntegerVectorWithZero(cond, simdInfo, vector, dest); }

    // --- Reductions to a GPR -------------------------------------------------
    // dest = (dw0 | dw1 of vr) != 0
    void vectorAnyBitSetInVR(VRegisterID vr, RegisterID dest)
    {
        m_assembler.mfvsrd(dest, vsr(vr));
        m_assembler.xxswapd(vsr(vecTemp4), vsr(vr));
        m_assembler.mfvsrd(dataTempRegister, vsr(vecTemp4));
        m_assembler.or_(dest, dest, dataTempRegister);
        m_assembler.neg(dataTempRegister, dest);
        m_assembler.or_(dest, dest, dataTempRegister);
        m_assembler.srdi(dest, dest, 63);
    }
    // Branch if the low byte of character equals any byte of set, which the
    // caller pads by repeating a member. POWER9 only (cmpeqb).
    Jump branchCharacterInByteSet(RegisterID character, uint64_t set)
    {
        ASSERT(supportsISA3_00());
        ASSERT(character != dataTempRegister);
        moveImmToScratch(static_cast<int64_t>(set), dataTempRegister);
        m_assembler.cmpeqb(0, character, dataTempRegister);
        return Jump(m_assembler.emitUnlinkedBranch(12, 1)); // CR0 GT set
    }

    // Zero / NonZero on all 128 bits of vec.
    Jump branchTest128(ResultCondition cond, FPRegisterID vec)
    {
        ASSERT(cond == Zero || cond == NonZero);
        m_assembler.mfvsrd(dataTempRegister, vsr(vec));
        m_assembler.xxswapd(vsr(vecTemp4), vsr(vec));
        m_assembler.mfvsrd(memoryTempRegister, vsr(vecTemp4));
        m_assembler.or_(dataTempRegister, dataTempRegister, memoryTempRegister);
        return branchTest64(cond, dataTempRegister);
    }
    void vectorAnyTrue(FPRegisterID vec, RegisterID dest)
    {
        copyToVR(vec, vecTemp0);
        vectorAnyBitSetInVR(vecTemp0, dest);
    }
    void vectorAllTrue(SIMDInfo simdInfo, FPRegisterID vec, RegisterID dest)
    {
        copyToVR(vec, vecTemp0);
        zeroVR(vecTemp1);
        (m_assembler.*vmxCompareEqualOp(simdInfo.lane))(vecTemp0, vecTemp0, vecTemp1);
        vectorAnyBitSetInVR(vecTemp0, dest);
        m_assembler.xori(dest, dest, 1);
    }
    void vectorAllTrue(SIMDInfo simdInfo, FPRegisterID vec, RegisterID dest, FPRegisterID) { vectorAllTrue(simdInfo, vec, dest); }
    void vectorBitmask(SIMDInfo simdInfo, FPRegisterID vec, RegisterID dest)
    {
        if (supportsISA3_1()) {
            // vextract*m puts ISA element 0 in the result's top bit. With the
            // doublewords swapped, ISA element e holds wasm lane (lanes - 1 - e),
            // so the result is the wasm bitmask as is.
            m_assembler.xxswapd(vsr(vecTemp0), vsr(vec));
            switch (elementByteSize(simdInfo.lane)) {
            case 1:
                m_assembler.vextractbm(dest, vecTemp0);
                return;
            case 2:
                m_assembler.vextracthm(dest, vecTemp0);
                return;
            case 4:
                m_assembler.vextractwm(dest, vecTemp0);
                return;
            case 8:
                m_assembler.vextractdm(dest, vecTemp0);
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
        // vbpermq gathers 16 selected bits into bits 48-63 of doubleword 0:
        // control byte i selects ISA bit index c (c >= 128 gives 0) into
        // result bit 15 - i. Put lane l's sign bit (the first ISA bit of its
        // element) at control byte 15 - l.
        unsigned bytes = elementByteSize(simdInfo.lane);
        unsigned lanes = 16 / bytes;
        uint64_t dw0 = 0, dw1 = 0;
        for (unsigned i = 0; i < 16; ++i)
            setISAByte(dw0, dw1, i, 128);
        for (unsigned l = 0; l < lanes; ++l)
            setISAByte(dw0, dw1, 15 - l, 8 * bytes * isaElementForLane(bytes, l));
        materializeVectorBits(dw0, dw1, vsr(vecTemp1));
        copyToVR(vec, vecTemp0);
        m_assembler.vbpermq(vecTemp0, vecTemp0, vecTemp1);
        m_assembler.mfvsrd(dest, vsr(vecTemp0));
    }
    void vectorBitmask(SIMDInfo simdInfo, FPRegisterID vec, RegisterID dest, FPRegisterID) { vectorBitmask(simdInfo, vec, dest); }

    // --- Shifts --------------------------------------------------------------
    // VMX shifts take a per-element count (modulo the element width) from the
    // low-order bits of each element of the second operand, so a byte splat of
    // the count serves every element size.
    static VMXBinaryOp vmxShiftOp(SIMDLane lane, int kind) // 0 = shl, 1 = shr_u, 2 = shr_s
    {
        switch (lane) {
        case SIMDLane::i8x16: return kind == 0 ? &PPC64Assembler::vslb : kind == 1 ? &PPC64Assembler::vsrb : &PPC64Assembler::vsrab;
        case SIMDLane::i16x8: return kind == 0 ? &PPC64Assembler::vslh : kind == 1 ? &PPC64Assembler::vsrh : &PPC64Assembler::vsrah;
        case SIMDLane::i32x4: return kind == 0 ? &PPC64Assembler::vslw : kind == 1 ? &PPC64Assembler::vsrw : &PPC64Assembler::vsraw;
        case SIMDLane::i64x2: return kind == 0 ? &PPC64Assembler::vsld : kind == 1 ? &PPC64Assembler::vsrd : &PPC64Assembler::vsrad;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    void vectorShiftByImmediate(int kind, SIMDInfo simdInfo, FPRegisterID input, TrustedImm32 shift, FPRegisterID dest)
    {
        unsigned amount = shift.m_value & (elementByteSize(simdInfo.lane) * 8 - 1);
        if (!amount) {
            moveVector(input, dest);
            return;
        }
        copyToVR(input, vecTemp0);
        if (amount < 16)
            m_assembler.vspltisb(vecTemp1, amount);
        else {
            moveImmToScratch(amount, dataTempRegister);
            m_assembler.mtvsrwz(vsr(vecTemp1), dataTempRegister);
            m_assembler.vspltb(vecTemp1, vecTemp1, 7);
        }
        (m_assembler.*vmxShiftOp(simdInfo.lane, kind))(vecTemp0, vecTemp0, vecTemp1);
        copyFromVR(vecTemp0, dest);
    }
    void vectorShl8(SIMDInfo simdInfo, FPRegisterID input, TrustedImm32 shift, FPRegisterID dest) { vectorShiftByImmediate(0, simdInfo, input, shift, dest); }
    void vectorUshr8(SIMDInfo simdInfo, FPRegisterID input, TrustedImm32 shift, FPRegisterID dest) { vectorShiftByImmediate(1, simdInfo, input, shift, dest); }
    void vectorSshr8(SIMDInfo simdInfo, FPRegisterID input, TrustedImm32 shift, FPRegisterID dest) { vectorShiftByImmediate(2, simdInfo, input, shift, dest); }
    // Per-lane counts in `shift` (only the low log2(lane bits) bits of each lane matter).
    void vectorUshl(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID shift, FPRegisterID dest) { vmxBinary(vmxShiftOp(simdInfo.lane, 0), input, shift, dest); }
    void vectorUshr(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID shift, FPRegisterID dest) { vmxBinary(vmxShiftOp(simdInfo.lane, 1), input, shift, dest); }
    void vectorSshr(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID shift, FPRegisterID dest) { vmxBinary(vmxShiftOp(simdInfo.lane, 2), input, shift, dest); }

    // --- Widening, narrowing and conversions ----------------------------------
    // vupkh*/vmrgh* read ISA elements 0..n/2-1 and vupkl*/vmrgl* the rest. In
    // this layout the wasm low half is doubleword 0 in reversed element order,
    // so the unpacked result has its doublewords swapped: fix with xxswapd.
    void vectorExtendInVR(SIMDLane resultLane, bool isSigned, bool high, VRegisterID input, VRegisterID dest)
    {
        if (isSigned) {
            switch (resultLane) {
            case SIMDLane::i16x8: high ? m_assembler.vupklsb(dest, input) : m_assembler.vupkhsb(dest, input); break;
            case SIMDLane::i32x4: high ? m_assembler.vupklsh(dest, input) : m_assembler.vupkhsh(dest, input); break;
            case SIMDLane::i64x2: high ? m_assembler.vupklsw(dest, input) : m_assembler.vupkhsw(dest, input); break;
            default: RELEASE_ASSERT_NOT_REACHED();
            }
            return;
        }
        ASSERT(input != vecTemp4 && dest != vecTemp4);
        zeroVR(vecTemp4);
        switch (resultLane) {
        case SIMDLane::i16x8: high ? m_assembler.vmrglb(dest, vecTemp4, input) : m_assembler.vmrghb(dest, vecTemp4, input); break;
        case SIMDLane::i32x4: high ? m_assembler.vmrglh(dest, vecTemp4, input) : m_assembler.vmrghh(dest, vecTemp4, input); break;
        case SIMDLane::i64x2: high ? m_assembler.vmrglw(dest, vecTemp4, input) : m_assembler.vmrghw(dest, vecTemp4, input); break;
        default: RELEASE_ASSERT_NOT_REACHED();
        }
    }
    void vectorExtendLow(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        copyToVR(input, vecTemp0);
        vectorExtendInVR(simdInfo.lane, simdInfo.signMode == SIMDSignMode::Signed, false, vecTemp0, vecTemp0);
        m_assembler.xxswapd(vsr(dest), vsr(vecTemp0));
    }
    void vectorExtendHigh(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        copyToVR(input, vecTemp0);
        vectorExtendInVR(simdInfo.lane, simdInfo.signMode == SIMDSignMode::Signed, true, vecTemp0, vecTemp0);
        m_assembler.xxswapd(vsr(dest), vsr(vecTemp0));
    }
    // simdInfo.lane is the input lane: result low half from lower, high half from upper.
    void vectorNarrow(SIMDInfo simdInfo, FPRegisterID lower, FPRegisterID upper, FPRegisterID dest, FPRegisterID)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        m_assembler.xxswapd(vsr(vecTemp0), vsr(lower));
        m_assembler.xxswapd(vsr(vecTemp1), vsr(upper));
        if (simdInfo.lane == SIMDLane::i16x8)
            isSigned ? m_assembler.vpkshss(vecTemp0, vecTemp0, vecTemp1) : m_assembler.vpkshus(vecTemp0, vecTemp0, vecTemp1);
        else {
            ASSERT(simdInfo.lane == SIMDLane::i32x4);
            isSigned ? m_assembler.vpkswss(vecTemp0, vecTemp0, vecTemp1) : m_assembler.vpkswus(vecTemp0, vecTemp0, vecTemp1);
        }
        copyFromVR(vecTemp0, dest);
    }
    // simdInfo.lane is the result lane.
    void vectorExtendedMultiply(SIMDInfo simdInfo, bool high, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        if (simdInfo.lane == SIMDLane::i64x2) {
            // Odd ISA words are lanes 0 and 2, even ones lanes 1 and 3.
            isSigned ? m_assembler.vmulosw(vecTemp2, vecTemp0, vecTemp1) : m_assembler.vmulouw(vecTemp2, vecTemp0, vecTemp1);
            isSigned ? m_assembler.vmulesw(vecTemp3, vecTemp0, vecTemp1) : m_assembler.vmuleuw(vecTemp3, vecTemp0, vecTemp1);
            m_assembler.xxpermdi(vsr(dest), vsr(vecTemp2), vsr(vecTemp3), high ? 3 : 0);
            return;
        }
        // Extend both (results have swapped doublewords), multiply, swap back.
        vectorExtendInVR(simdInfo.lane, isSigned, high, vecTemp0, vecTemp0);
        vectorExtendInVR(simdInfo.lane, isSigned, high, vecTemp1, vecTemp1);
        if (simdInfo.lane == SIMDLane::i16x8) {
            zeroVR(vecTemp2);
            m_assembler.vmladduhm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        } else {
            ASSERT(simdInfo.lane == SIMDLane::i32x4);
            m_assembler.vmuluwm(vecTemp0, vecTemp0, vecTemp1);
        }
        m_assembler.xxswapd(vsr(dest), vsr(vecTemp0));
    }
    void vectorMulLow(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID = PPC64Registers::InvalidFPRReg) { vectorExtendedMultiply(simdInfo, false, left, right, dest); }
    void vectorMulHigh(SIMDInfo simdInfo, FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID = PPC64Registers::InvalidFPRReg) { vectorExtendedMultiply(simdInfo, true, left, right, dest); }

    // simdInfo.lane is the input lane.
    void vectorExtaddPairwise(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        copyToVR(input, vecTemp0);
        if (simdInfo.lane == SIMDLane::i8x16) {
            m_assembler.vspltisb(vecTemp1, 1);
            isSigned ? m_assembler.vmulesb(vecTemp2, vecTemp0, vecTemp1) : m_assembler.vmuleub(vecTemp2, vecTemp0, vecTemp1);
            isSigned ? m_assembler.vmulosb(vecTemp3, vecTemp0, vecTemp1) : m_assembler.vmuloub(vecTemp3, vecTemp0, vecTemp1);
            m_assembler.vadduhm(vecTemp0, vecTemp2, vecTemp3);
        } else {
            ASSERT(simdInfo.lane == SIMDLane::i16x8);
            m_assembler.vspltish(vecTemp1, 1);
            zeroVR(vecTemp2);
            isSigned ? m_assembler.vmsumshm(vecTemp0, vecTemp0, vecTemp1, vecTemp2) : m_assembler.vmsumuhm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        }
        copyFromVR(vecTemp0, dest);
    }
    void vectorExtaddPairwise(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest, RegisterID, FPRegisterID) { vectorExtaddPairwise(simdInfo, input, dest); }

    // i32x4.dot_i16x8_s
    void vectorDotProduct(FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        zeroVR(vecTemp2);
        m_assembler.vmsumshm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp0, dest);
    }
    void vectorDotProduct(FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID) { vectorDotProduct(left, right, dest); }
    // i16x8.q15mulr_sat_s: sat((a * b + 0x4000) >> 15), exactly vmhraddshs with a zero addend.
    void vectorMulSat(FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        zeroVR(vecTemp2);
        m_assembler.vmhraddshs(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp0, dest);
    }
    void vectorMulSat(FPRegisterID left, FPRegisterID right, FPRegisterID dest, RegisterID, FPRegisterID) { vectorMulSat(left, right, dest); }
    void vectorRelaxedQ15Mulr(FPRegisterID left, FPRegisterID right, FPRegisterID dest) { vectorMulSat(left, right, dest); }
    // i16x8.relaxed_dot_i8x16_i7x16_s: wrapping sum of the two signed products per lane.
    void vectorRelaxedDotI8x16I7x16(FPRegisterID left, FPRegisterID right, FPRegisterID dest, FPRegisterID)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        m_assembler.vmulesb(vecTemp2, vecTemp0, vecTemp1);
        m_assembler.vmulosb(vecTemp3, vecTemp0, vecTemp1);
        m_assembler.vadduhm(vecTemp0, vecTemp2, vecTemp3);
        copyFromVR(vecTemp0, dest);
    }
    // i32x4.relaxed_dot_i8x16_i7x16_add_s: the relaxed-dot i16 pairs, then
    // pairwise-added to i32 and added to the addend. Same intermediate as
    // vectorRelaxedDotI8x16I7x16, so the two relaxed ops agree.
    void vectorRelaxedDotI8x16I7x16Add(FPRegisterID left, FPRegisterID right, FPRegisterID addend, FPRegisterID dest, FPRegisterID, FPRegisterID)
    {
        copyToVR(left, vecTemp0);
        copyToVR(right, vecTemp1);
        m_assembler.vmulesb(vecTemp2, vecTemp0, vecTemp1);
        m_assembler.vmulosb(vecTemp3, vecTemp0, vecTemp1);
        m_assembler.vadduhm(vecTemp0, vecTemp2, vecTemp3);
        m_assembler.vspltish(vecTemp1, 1);
        copyToVR(addend, vecTemp2);
        m_assembler.vmsumshm(vecTemp0, vecTemp0, vecTemp1, vecTemp2);
        copyFromVR(vecTemp0, dest);
    }

    // f32x4.convert_i32x4_{s,u}
    void vectorConvert(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT(simdInfo.lane == SIMDLane::i32x4);
        if (simdInfo.signMode == SIMDSignMode::Signed)
            m_assembler.xvcvsxwsp(vsr(dest), vsr(input));
        else
            m_assembler.xvcvuxwsp(vsr(dest), vsr(input));
    }
    // Words of lanes 0 and 1 (ISA words 1 and 0) into ISA words 0 and 2, which
    // is where xvcv*w*dp / xvcvspdp read, then swap so lane 0 lands in dw0.
    void vectorConvertLow(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT(simdInfo.lane == SIMDLane::i32x4);
        uint32_t t0 = vsr(vecTemp0);
        m_assembler.xxmrghw(t0, vsr(input), vsr(input));
        if (simdInfo.signMode == SIMDSignMode::Signed)
            m_assembler.xvcvsxwdp(t0, t0);
        else
            m_assembler.xvcvuxwdp(t0, t0);
        m_assembler.xxswapd(vsr(dest), t0);
    }
    void vectorPromote(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT_UNUSED(simdInfo, simdInfo.lane == SIMDLane::f32x4);
        uint32_t t0 = vsr(vecTemp0);
        m_assembler.xxmrghw(t0, vsr(input), vsr(input));
        m_assembler.xvcvspdp(t0, t0);
        m_assembler.xxswapd(vsr(dest), t0);
    }
    // t holds 32-bit results for f64 lanes 0 and 1 in ISA words 0 and 2;
    // produce [lane1, lane0, 0, 0] in ISA words, i.e. wasm lanes 0,1 then zeros.
    void packDoublewordResultsToLowWords(uint32_t t, FPRegisterID dest)
    {
        uint32_t t1 = vsr(vecTemp1), t2 = vsr(vecTemp2);
        m_assembler.xxswapd(t1, t);
        m_assembler.xxmrghw(t1, t1, t);
        m_assembler.xxlxor(t2, t2, t2);
        m_assembler.xxpermdi(vsr(dest), t1, t2, 0);
    }
    void vectorDemote(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        ASSERT_UNUSED(simdInfo, simdInfo.lane == SIMDLane::f64x2);
        uint32_t t0 = vsr(vecTemp0);
        m_assembler.xvcvdpsp(t0, vsr(input));
        packDoublewordResultsToLowWords(t0, dest);
    }
    // NaN lanes become 0; out-of-range lanes saturate (the hardware already does).
    void vectorTruncSat(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest)
    {
        bool isSigned = simdInfo.signMode == SIMDSignMode::Signed;
        uint32_t t0 = vsr(vecTemp0), t1 = vsr(vecTemp1);
        if (simdInfo.lane == SIMDLane::f32x4) {
            isSigned ? m_assembler.xvcvspsxws(t0, vsr(input)) : m_assembler.xvcvspuxws(t0, vsr(input));
            m_assembler.xvcmpeqsp(t1, vsr(input), vsr(input));
            m_assembler.xxland(vsr(dest), t0, t1);
            return;
        }
        ASSERT(simdInfo.lane == SIMDLane::f64x2);
        isSigned ? m_assembler.xvcvdpsxws(t0, vsr(input)) : m_assembler.xvcvdpuxws(t0, vsr(input));
        m_assembler.xvcmpeqdp(t1, vsr(input), vsr(input));
        m_assembler.xxland(t0, t0, t1);
        packDoublewordResultsToLowWords(t0, dest);
    }
    void vectorTruncSat(SIMDInfo simdInfo, FPRegisterID input, FPRegisterID dest, RegisterID, FPRegisterID, FPRegisterID) { vectorTruncSat(simdInfo, input, dest); }

    // --- Permutes ------------------------------------------------------------
    // i8x16.swizzle: out-of-range indices (>= 16) give 0.
    void vectorSwizzle(FPRegisterID input, FPRegisterID control, FPRegisterID dest)
    {
        // Wasm byte x is ISA byte x ^ 7, so permute with control ^ 7 (vperm
        // only looks at the low five bits), then clear lanes whose index > 15.
        copyToVR(input, vecTemp0);
        copyToVR(control, vecTemp1);
        m_assembler.vspltisb(vecTemp2, 7);
        m_assembler.vxor(vecTemp3, vecTemp1, vecTemp2);
        m_assembler.vperm(vecTemp3, vecTemp0, vecTemp0, vecTemp3);
        m_assembler.vspltisb(vecTemp2, 15);
        m_assembler.vcmpgtub(vecTemp1, vecTemp1, vecTemp2);
        m_assembler.vandc(vecTemp3, vecTemp3, vecTemp1);
        copyFromVR(vecTemp3, dest);
    }
    // Two-table lookup (OMG's i8x16.shuffle): result byte i is byte control[i]
    // of a||b, or 0 when control[i] > 31. Same ^ 7 index fixup as vectorSwizzle;
    // vperm reads only the low five bits, so lanes with control >> 5 != 0 are cleared.
    void vectorSwizzle2(FPRegisterID a, FPRegisterID b, FPRegisterID control, FPRegisterID dest)
    {
        copyToVR(a, vecTemp0);
        copyToVR(b, vecTemp1);
        copyToVR(control, vecTemp2);
        m_assembler.vspltisb(vecTemp3, 7);
        m_assembler.vxor(vecTemp4, vecTemp2, vecTemp3);
        m_assembler.vperm(vecTemp4, vecTemp0, vecTemp1, vecTemp4);
        m_assembler.vspltisb(vecTemp3, 5);
        m_assembler.vsrb(vecTemp3, vecTemp2, vecTemp3);
        zeroVR(vecTemp0);
        m_assembler.vcmpequb(vecTemp3, vecTemp3, vecTemp0);
        m_assembler.vand(vecTemp4, vecTemp4, vecTemp3);
        copyFromVR(vecTemp4, dest);
    }
    // Lane-structured permutes (the B3 canonical shuffle forms), as vperm with
    // a constant control. Semantics follow ARM64's UZP/ZIP/EXT/REV.
    void vectorUnzip(SIMDInfo simdInfo, bool odd, FPRegisterID n, FPRegisterID m, FPRegisterID dest)
    {
        unsigned size = elementByteSize(simdInfo.lane);
        uint8_t map[16];
        for (unsigned i = 0; i < 16 / size; ++i) {
            for (unsigned b = 0; b < size; ++b)
                map[i * size + b] = (2 * i + odd) * size + b;
        }
        vectorPermuteBytes(map, n, m, dest);
    }
    void vectorUnzipEven(SIMDInfo simdInfo, FPRegisterID n, FPRegisterID m, FPRegisterID dest) { vectorUnzip(simdInfo, false, n, m, dest); }
    void vectorUnzipOdd(SIMDInfo simdInfo, FPRegisterID n, FPRegisterID m, FPRegisterID dest) { vectorUnzip(simdInfo, true, n, m, dest); }
    void vectorZip(SIMDInfo simdInfo, bool higher, FPRegisterID n, FPRegisterID m, FPRegisterID dest)
    {
        unsigned size = elementByteSize(simdInfo.lane);
        unsigned half = 8 / size;
        uint8_t map[16];
        for (unsigned i = 0; i < half; ++i) {
            unsigned source = (higher ? half + i : i) * size;
            for (unsigned b = 0; b < size; ++b) {
                map[2 * i * size + b] = source + b;
                map[(2 * i + 1) * size + b] = 16 + source + b;
            }
        }
        vectorPermuteBytes(map, n, m, dest);
    }
    void vectorZipLower(SIMDInfo simdInfo, FPRegisterID n, FPRegisterID m, FPRegisterID dest) { vectorZip(simdInfo, false, n, m, dest); }
    void vectorZipHigher(SIMDInfo simdInfo, FPRegisterID n, FPRegisterID m, FPRegisterID dest) { vectorZip(simdInfo, true, n, m, dest); }
    void vectorExtractPair(SIMDInfo simdInfo, TrustedImm32 firstLane, FPRegisterID n, FPRegisterID m, FPRegisterID dest)
    {
        ASSERT_UNUSED(simdInfo, simdInfo.lane == SIMDLane::i8x16);
        uint8_t map[16];
        for (unsigned i = 0; i < 16; ++i)
            map[i] = (firstLane.m_value + i) & 31;
        vectorPermuteBytes(map, n, m, dest);
    }
    void vectorReverse(SIMDInfo simdInfo, TrustedImm32 groupSize, FPRegisterID input, FPRegisterID dest)
    {
        unsigned size = elementByteSize(simdInfo.lane);
        unsigned group = groupSize.m_value;
        ASSERT(group > size && !(group % size) && !(16 % group));
        uint8_t map[16];
        for (unsigned g = 0; g < 16; g += group) {
            for (unsigned j = 0; j < group / size; ++j) {
                for (unsigned b = 0; b < size; ++b)
                    map[g + j * size + b] = g + (group / size - 1 - j) * size + b;
            }
        }
        vectorPermuteBytes(map, input, input, dest);
    }

    // ARM64- and x86-shaped forms. Nothing routes PPC64 here: BBQ and the B3
    // lowering use the operations above on this port.
    void vectorSshl(SIMDInfo, FPRegisterID, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorUshl8(FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorSshr8(FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorUshr8(FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorHorizontalAdd(SIMDInfo, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorUnsignedMin(SIMDInfo, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }
    void vectorUnsignedMax(SIMDInfo, FPRegisterID, FPRegisterID) { PPC64_UNSUPPORTED(); }

    Jump branchTruncateDoubleToInt32(FPRegisterID src, RegisterID dest, BranchTruncateType branchType = BranchIfTruncateFailed)
    {
        // fctiwz saturates out-of-range/NaN inputs to INT32_MIN/INT32_MAX;
        // treat either saturation value as failure (conservative, matches
        // other RISC ports): failed iff uint32(dest + 0x80000001) <= 1.
        truncateDoubleToInt32(src, dest);
        add32(TrustedImm32(int32_t(0x80000001)), dest, dataTempRegister);
        return branch32(branchType == BranchIfTruncateFailed ? BelowOrEqual : Above, dataTempRegister, TrustedImm32(1));
    }

    // --- Conditional moves: short forward bc (skip 1 insn) over an mr ----

    void emitSkipOneInstruction(uint32_t bo, uint32_t bi)
    {
        m_assembler.bc(bo, bi, 8);
    }

    template<typename CompareEmitter>
    void moveConditionallyImpl(BranchBits bits, RegisterID thenCase, RegisterID elseCase, RegisterID dest, const CompareEmitter& emitCompareFn)
    {
        emitCompareFn();
        if (dest == thenCase) {
            emitSkipOneInstruction(bits.bo, bits.bi);          // taken: keep then-value
            m_assembler.mr(dest, elseCase);
        } else if (dest == elseCase) {
            emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi);    // not taken: keep else-value
            m_assembler.mr(dest, thenCase);
        } else {
            m_assembler.mr(dest, elseCase);
            emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi);
            m_assembler.mr(dest, thenCase);
        }
    }

    void moveConditionally32(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID src, RegisterID dest)
    {
        emitCompare32(cond, left, right);
        emitSkipOneInstruction(branchBitsFor(cond).bo ^ 0x8, branchBitsFor(cond).bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionally32(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        moveConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare32(cond, left, right); });
    }
    void moveConditionally32(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        moveConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare32(cond, left, right); });
    }
    void moveConditionally64(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID src, RegisterID dest)
    {
        emitCompare64(cond, left, right);
        emitSkipOneInstruction(branchBitsFor(cond).bo ^ 0x8, branchBitsFor(cond).bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionally64(RelationalCondition cond, RegisterID left, RegisterID right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        moveConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare64(cond, left, right); });
    }
    void moveConditionallyDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID src, RegisterID dest)
    {
        DoubleBranchBits bits = doubleBranchBitsFor(cond);
        m_assembler.fcmpu(0, left, right);
        emitDoubleConditionCRop(bits);
        emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionallyDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        DoubleBranchBits dbits = doubleBranchBitsFor(cond);
        moveConditionallyImpl({ dbits.bo, dbits.bi }, thenCase, elseCase, dest, [&] {
            m_assembler.fcmpu(0, left, right);
            emitDoubleConditionCRop(dbits);
        });
    }

    // --- 64-bit negate ----------------------------------------------------

    // --- Signed-extending loads (movs[bwl]-style: sign to 32 zero-extends
    // the upper word; sign to 64 keeps the full extension) ----------------

    template<typename AddressType>
    void load16SignedExtendTo32(AddressType address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lha(dest, r.offset, r.base);
        zeroExtend32ToWordInternal(dest);
    }
    template<typename AddressType>
    void load8SignedExtendTo32(AddressType address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dest, r.offset, r.base);
        m_assembler.extsb(dest, dest);
        zeroExtend32ToWordInternal(dest);
    }
    template<typename AddressType>
    void load8SignedExtendTo64(AddressType address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dest, r.offset, r.base);
        m_assembler.extsb(dest, dest);
    }
    template<typename AddressType>
    void load16SignedExtendTo64(AddressType address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lha(dest, r.offset, r.base);
    }
    template<typename AddressType>
    void load32SignedExtendTo64(AddressType address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.lwa(dest, r.offset, r.base);
    }
    void load8SignedExtendTo64(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lbz(dest, 0, memoryTempRegister);
        m_assembler.extsb(dest, dest);
    }
    void load16SignedExtendTo64(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lha(dest, 0, memoryTempRegister);
    }
    void load32SignedExtendTo64(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lwa(dest, 0, memoryTempRegister);
    }

    // --- Pair loads/stores (two scalar accesses; no ldp on PPC) ----------

    void loadPair32(RegisterID base, TrustedImm32 offset, RegisterID dest1, RegisterID dest2)
    {
        // dest1 may alias base; load into it LAST when needed.
        if (dest1 == base) {
            load32(Address(base, offset.m_value + 4), dest2);
            load32(Address(base, offset.m_value), dest1);
        } else {
            load32(Address(base, offset.m_value), dest1);
            load32(Address(base, offset.m_value + 4), dest2);
        }
    }
    void loadPair32(RegisterID base, RegisterID dest1, RegisterID dest2)
    {
        loadPair32(base, TrustedImm32(0), dest1, dest2);
    }
    void loadPair64(RegisterID base, TrustedImm32 offset, RegisterID dest1, RegisterID dest2)
    {
        if (dest1 == base) {
            load64(Address(base, offset.m_value + 8), dest2);
            load64(Address(base, offset.m_value), dest1);
        } else {
            load64(Address(base, offset.m_value), dest1);
            load64(Address(base, offset.m_value + 8), dest2);
        }
    }
    void loadPair64(RegisterID base, RegisterID dest1, RegisterID dest2)
    {
        loadPair64(base, TrustedImm32(0), dest1, dest2);
    }
    void loadPair64(Address address, RegisterID dest1, RegisterID dest2)
    {
        loadPair64(address.base, TrustedImm32(address.offset), dest1, dest2);
    }
    void loadPair32(Address address, RegisterID dest1, RegisterID dest2)
    {
        loadPair32(address.base, TrustedImm32(address.offset), dest1, dest2);
    }
    void storePair32(RegisterID src1, RegisterID src2, RegisterID base, TrustedImm32 offset)
    {
        store32(src1, Address(base, offset.m_value));
        store32(src2, Address(base, offset.m_value + 4));
    }
    void storePair32(RegisterID src1, RegisterID src2, Address address)
    {
        store32(src1, address);
        store32(src2, address.withOffset(4));
    }
    void storePair32(RegisterID src1, RegisterID src2, BaseIndex address)
    {
        store32(src1, address);
        store32(src2, BaseIndex(address.base, address.index, address.scale, address.offset + 4));
    }
    void storePair64(RegisterID src1, RegisterID src2, RegisterID base)
    {
        storePair64(src1, src2, base, TrustedImm32(0));
    }
    void storePair64(RegisterID src1, RegisterID src2, RegisterID base, TrustedImm32 offset)
    {
        store64(src1, Address(base, offset.m_value));
        store64(src2, Address(base, offset.m_value + 8));
    }
    void storePair64(RegisterID src1, RegisterID src2, Address address)
    {
        store64(src1, address);
        store64(src2, address.withOffset(8));
    }

    // --- 32-bit arith, memory-operand forms -------------------------------

    void sub32(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        add32(TrustedImm32(-imm.m_value), src, dest);
    }
    void sub32(TrustedImm32 imm, RegisterID src, RegisterID dest)
    {
        // dest = imm - src
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        m_assembler.subf(dest, src, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void sub32(Address address, RegisterID dest)
    {
        load32(address, dataTempRegister);
        sub32(dataTempRegister, dest);
    }
    void sub32(RegisterID src, Address address)
    {
        load32(address, dataTempRegister);
        m_assembler.subf(dataTempRegister, src, dataTempRegister);
        store32(dataTempRegister, address);
    }
    void sub32(TrustedImm32 imm, Address address)
    {
        add32(TrustedImm32(-imm.m_value), address);
    }
    void add32(Address address, RegisterID dest)
    {
        load32(address, dataTempRegister);
        add32(dataTempRegister, dest);
    }
    void add32(RegisterID src, Address address)
    {
        // Keep the resolved address: resolveAddress may cache into r12 and
        // the value scratch is r11 — no overlap.
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.add(dataTempRegister, dataTempRegister, src);
        m_assembler.stw(dataTempRegister, r.offset, r.base);
    }
    void add32(TrustedImm32 imm, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        if (isInt16(imm.m_value))
            m_assembler.addi(dataTempRegister, dataTempRegister, int16_t(imm.m_value));
        else {
            // r11 and r12 are both busy; rebuild the address afterwards.
            add32(imm, dataTempRegister, dataTempRegister);
            r = resolveAddress(address, memoryTempRegister);
        }
        m_assembler.stw(dataTempRegister, r.offset, r.base);
    }
    void add32(TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lwz(dataTempRegister, 0, memoryTempRegister);
        if (isInt16(imm.m_value))
            m_assembler.addi(dataTempRegister, dataTempRegister, int16_t(imm.m_value));
        else {
            add32(imm, dataTempRegister, dataTempRegister);
            moveToAbsolute(address.m_ptr);
        }
        m_assembler.stw(dataTempRegister, 0, memoryTempRegister);
    }
    void add32(AbsoluteAddress address, RegisterID dest)
    {
        load32(address.m_ptr, dataTempRegister);
        add32(dataTempRegister, dest);
    }

    // --- 64-bit shifts (count masked & 63, matching x86/arm64 hardware) --

    void lshift64(TrustedImm32 imm, RegisterID dest) { m_assembler.sldi(dest, dest, imm.m_value & 63); }
    void lshift64(RegisterID src, TrustedImm32 imm, RegisterID dest) { m_assembler.sldi(dest, src, imm.m_value & 63); }
    // dest = imm << (shiftAmount & 63): the immediate is the value, the register
    // the count, as on ARM64/x86_64/RISCV64 (Yarr's character-class bit test).
    void lshift64(TrustedImm32 imm, RegisterID shiftAmount, RegisterID dest)
    {
        move(imm, dataTempRegister);
        m_assembler.rldicl(memoryTempRegister, shiftAmount, 0, 58);   // count & 63
        m_assembler.sld(dest, dataTempRegister, memoryTempRegister);
    }
    void lshift64(RegisterID shiftAmount, RegisterID dest) { lshift64(dest, shiftAmount, dest); }
    void lshift64(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rldicl(dataTempRegister, shiftAmount, 0, 58);   // count & 63
        m_assembler.sld(dest, src, dataTempRegister);
    }

    void rshift64(TrustedImm32 imm, RegisterID dest) { m_assembler.sradi(dest, dest, imm.m_value & 63); }
    void rshift64(RegisterID src, TrustedImm32 imm, RegisterID dest) { m_assembler.sradi(dest, src, imm.m_value & 63); }
    void rshift64(RegisterID shiftAmount, RegisterID dest) { rshift64(dest, shiftAmount, dest); }
    void rshift64(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rldicl(dataTempRegister, shiftAmount, 0, 58);
        m_assembler.srad(dest, src, dataTempRegister);
    }

    void urshift64(TrustedImm32 imm, RegisterID dest) { urshift64(dest, imm, dest); }
    void urshift64(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        if (!(imm.m_value & 63)) {
            move(src, dest);
            return;
        }
        m_assembler.srdi(dest, src, imm.m_value & 63);
    }
    void urshift64(RegisterID shiftAmount, RegisterID dest) { urshift64(dest, shiftAmount, dest); }
    void urshift64(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        m_assembler.rldicl(dataTempRegister, shiftAmount, 0, 58);
        m_assembler.srd(dest, src, dataTempRegister);
    }

    void rotateRight64(TrustedImm32 imm, RegisterID dest)
    {
        // rotr64(n) = rotl64(64 - n); rldicl with mb=0 is rotldi.
        m_assembler.rldicl(dest, dest, (64 - (imm.m_value & 63)) & 63, 0);
    }
    void rotateRight64(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        m_assembler.rldicl(dest, src, (64 - (imm.m_value & 63)) & 63, 0);
    }
    void rotateRight64(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        // rldcl uses only the low 6 bits of the rotate operand, so 64 - amount
        // wraps correctly for amount == 0 (64 -> 0) and for amount >= 64.
        m_assembler.subfic(dataTempRegister, shiftAmount, 64);
        m_assembler.rldcl(dest, src, dataTempRegister, 0);
    }

    // rotr32(n) = rotl32(32 - n). rlwinm/rlwnm rotate the low word and clear
    // the high 32 bits of the result, which keeps the int32-values-are-
    // zero-extended invariant for free.
    void rotateRight32(RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        m_assembler.rlwinm(dest, src, (32 - (imm.m_value & 31)) & 31, 0, 31);
    }
    void rotateRight32(TrustedImm32 imm, RegisterID srcDst) { rotateRight32(srcDst, imm, srcDst); }
    void rotateRight32(RegisterID src, RegisterID shiftAmount, RegisterID dest)
    {
        // rlwnm uses only the low 5 bits of the rotate operand, so 32 - amount
        // wraps correctly for amount == 0 (32 -> 0) and for amount >= 32.
        m_assembler.subfic(dataTempRegister, shiftAmount, 32);
        m_assembler.rlwnm(dest, src, dataTempRegister, 0, 31);
    }

    void not64(RegisterID srcDest) { m_assembler.nor(srcDest, srcDest, srcDest); }
    void not64(RegisterID src, RegisterID dest) { m_assembler.nor(dest, src, src); }

    void countLeadingZeros32(RegisterID src, RegisterID dest) { m_assembler.cntlzw(dest, src); }
    void countLeadingZeros64(RegisterID src, RegisterID dest) { m_assembler.cntlzd(dest, src); }

    // An assembler temp other than `live`, an operand still read after the temp
    // has been written. Callers may pass dataTempRegister as an operand.
    static RegisterID tempRegisterAvoiding(RegisterID live)
    {
        return live == dataTempRegister ? memoryTempRegister : dataTempRegister;
    }

    // cnttzw/cnttzd are ISA 3.0. Before that, ctz(x) == 32 - clz(~x & (x - 1)),
    // which also gives ctz(0) == 32.
    void countTrailingZeros32(RegisterID src, RegisterID dest)
    {
        if (supportsISA3_00()) {
            m_assembler.cnttzw(dest, src);
            return;
        }
        RegisterID temp = tempRegisterAvoiding(src);
        m_assembler.addi(temp, src, -1);           // x - 1
        m_assembler.andc(temp, temp, src);         // (x - 1) & ~x
        m_assembler.cntlzw(dest, temp);
        m_assembler.subfic(dest, dest, 32);
    }
    void countTrailingZeros64(RegisterID src, RegisterID dest)
    {
        if (supportsISA3_00()) {
            m_assembler.cnttzd(dest, src);
            return;
        }
        RegisterID temp = tempRegisterAvoiding(src);
        m_assembler.addi(temp, src, -1);
        m_assembler.andc(temp, temp, src);
        m_assembler.cntlzd(dest, temp);
        m_assembler.subfic(dest, dest, 64);
    }

    static bool supportsCountPopulation() { return true; }
    // The FP temp is part of the shared signature (ARM64 needs a vector
    // register for its cnt/addv sequence); PPC64 has a direct instruction.
    void countPopulation32(RegisterID src, RegisterID dest, FPRegisterID)
    {
        // popcntw counts each word separately, so a non-zero-extended src
        // would leave the high word's count in bits 32:63. Clear them.
        m_assembler.popcntw(dest, src);
        m_assembler.rldicl(dest, dest, 0, 32);
    }
    void countPopulation64(RegisterID src, RegisterID dest, FPRegisterID) { m_assembler.popcntd(dest, src); }

    // dest = minuend - mulLeft * mulRight: no integer multiply-subtract, so
    // mullw/mulld then subf. The product temp must not be the minuend, which is read after it is written.
    void multiplySub32(RegisterID mulLeft, RegisterID mulRight, RegisterID minuend, RegisterID dest)
    {
        RegisterID product = tempRegisterAvoiding(minuend);
        m_assembler.mullw(product, mulLeft, mulRight);
        m_assembler.subf(dest, product, minuend);
        zeroExtend32ToWordInternal(dest);
    }
    void multiplySub64(RegisterID mulLeft, RegisterID mulRight, RegisterID minuend, RegisterID dest)
    {
        RegisterID product = tempRegisterAvoiding(minuend);
        m_assembler.mulld(product, mulLeft, mulRight);
        m_assembler.subf(dest, product, minuend);
    }

    // --- Memory-destination logical read-modify-write ---------------------
    // load width, combine, store back. Byte/halfword forms operate on the
    // narrow width; the 32-bit result is not required to zero-extend in
    // memory. src register combines the full low bits (callers pass masked
    // values for narrow widths).

    void or32(RegisterID src, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.or_(dataTempRegister, dataTempRegister, src);
        m_assembler.stw(dataTempRegister, r.offset, r.base);
    }
    void or32(TrustedImm32 imm, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        if (imm.m_value >= 0 && isUInt16(imm.m_value))
            m_assembler.ori(dataTempRegister, dataTempRegister, uint16_t(imm.m_value));
        else {
            // r11 (data) and r12 (mem) are both live; stash the address.
            m_assembler.mr(PPC64Registers::r0, r.base);
            moveImmToScratch(int64_t(imm.m_value), memoryTempRegister);
            m_assembler.or_(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.stw(dataTempRegister, r.offset, PPC64Registers::r0);
            return;
        }
        m_assembler.stw(dataTempRegister, r.offset, r.base);
    }
    void or32(RegisterID src, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lwz(dataTempRegister, 0, memoryTempRegister);
        m_assembler.or_(dataTempRegister, dataTempRegister, src);
        m_assembler.stw(dataTempRegister, 0, memoryTempRegister);
    }
    void or32(TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lwz(dataTempRegister, 0, memoryTempRegister);
        if (imm.m_value >= 0 && isUInt16(imm.m_value))
            m_assembler.ori(dataTempRegister, dataTempRegister, uint16_t(imm.m_value));
        else {
            moveImmToScratch(int64_t(imm.m_value), PPC64Registers::r0);
            m_assembler.or_(dataTempRegister, dataTempRegister, PPC64Registers::r0);
        }
        m_assembler.stw(dataTempRegister, 0, memoryTempRegister);
    }

    void or16(TrustedImm32 imm, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dataTempRegister, r.offset, r.base);
        m_assembler.ori(dataTempRegister, dataTempRegister, uint16_t(imm.m_value));
        m_assembler.sth(dataTempRegister, r.offset, r.base);
    }
    void or16(TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lhz(dataTempRegister, 0, memoryTempRegister);
        m_assembler.ori(dataTempRegister, dataTempRegister, uint16_t(imm.m_value));
        m_assembler.sth(dataTempRegister, 0, memoryTempRegister);
    }
    void or16(RegisterID src, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lhz(dataTempRegister, 0, memoryTempRegister);
        m_assembler.or_(dataTempRegister, dataTempRegister, src);
        m_assembler.sth(dataTempRegister, 0, memoryTempRegister);
    }

    void or8(TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lbz(dataTempRegister, 0, memoryTempRegister);
        m_assembler.ori(dataTempRegister, dataTempRegister, uint16_t(imm.m_value & 0xFF));
        m_assembler.stb(dataTempRegister, 0, memoryTempRegister);
    }

    void add8(TrustedImm32 imm, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dataTempRegister, r.offset, r.base);
        m_assembler.addi(dataTempRegister, dataTempRegister, int16_t(imm.m_value));
        m_assembler.stb(dataTempRegister, r.offset, r.base);
    }

    void sub32(TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.lwz(dataTempRegister, 0, memoryTempRegister);
        if (isInt16(-int64_t(imm.m_value)))
            m_assembler.addi(dataTempRegister, dataTempRegister, int16_t(-imm.m_value));
        else {
            moveImmToScratch(int64_t(imm.m_value), PPC64Registers::r0);
            m_assembler.subf(dataTempRegister, PPC64Registers::r0, dataTempRegister);
        }
        m_assembler.stw(dataTempRegister, 0, memoryTempRegister);
    }

    // --- Memory-source logical (dest is a register) -----------------------

    void and32(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.and_(dest, dest, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void and32(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.and_(dest, dest, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void xor32(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.xor_(dest, dest, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }
    void xor32(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.xor_(dest, dest, dataTempRegister);
        zeroExtend32ToWordInternal(dest);
    }

    void neg64(RegisterID srcDest) { m_assembler.neg(srcDest, srcDest); }

    // --- Conditional moves with immediate compare / test operands ---------
    // emitTestAndSetCR0: and reg&mask, setting CR0 (via a masked AND that
    // updates CR0); branch bits come from the ResultCondition (Zero/NonZero).
    struct BranchBitsRC { uint32_t bo; uint32_t bi; };
    static BranchBitsRC testBranchBitsFor(ResultCondition cond)
    {
        switch (cond) {
        case Zero:          return { 12, 2 };
        case NonZero:       return { 4,  2 };
        case Signed:        return { 12, 0 };
        case PositiveOrZero:return { 4,  0 };
        default: RELEASE_ASSERT_NOT_REACHED(); return { 20, 0 };
        }
    }

    void moveConditionally32(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID src, RegisterID dest)
    {
        emitCompare32(cond, left, right);
        emitSkipOneInstruction(branchBitsFor(cond).bo ^ 0x8, branchBitsFor(cond).bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionally32(RelationalCondition cond, RegisterID left, TrustedImm32 right, TrustedImm32 thenImm, RegisterID elseCase, RegisterID dest)
    {
        // dest = (cond) ? thenImm : elseCase. Compare first: a `right` that does not
        // fit the compare's 16-bit immediate is materialised in dataTempRegister,
        // which would overwrite a thenImm materialised there before it.
        // moveImmToScratch's li/lis/ori/sldi/oris leave CR0 alone, so the compare
        // result survives.
        ASSERT(elseCase != dataTempRegister);
        moveConditionallyImpl(branchBitsFor(cond), dataTempRegister, elseCase, dest, [&] {
            emitCompare32(cond, left, right);
            moveImmToScratch(int64_t(thenImm.m_value), dataTempRegister);
        });
    }
    void moveConditionally64(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID src, RegisterID dest)
    {
        emitCompare64(cond, left, TrustedImm64(right.m_value));
        emitSkipOneInstruction(branchBitsFor(cond).bo ^ 0x8, branchBitsFor(cond).bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionally64(RelationalCondition cond, RegisterID left, TrustedImm32 right, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        moveConditionallyImpl(branchBitsFor(cond), thenCase, elseCase, dest, [&] { emitCompare64(cond, left, TrustedImm64(right.m_value)); });
    }

    void emitTest64ToCR0(RegisterID reg, RegisterID mask)
    {
        m_assembler.and_(dataTempRegister, reg, mask);
        m_assembler.cmpdi(0, dataTempRegister, 0);
    }
    void emitTest64ToCR0(RegisterID reg, TrustedImm32 mask)
    {
        if (mask.m_value == -1)
            m_assembler.cmpdi(0, reg, 0);
        else if (mask.m_value >= 0 && isUInt16(mask.m_value))
            m_assembler.andi_(dataTempRegister, reg, uint16_t(mask.m_value));  // sets CR0
        else {
            moveImmToScratch(int64_t(mask.m_value), dataTempRegister);
            m_assembler.and_(dataTempRegister, reg, dataTempRegister);
            m_assembler.cmpdi(0, dataTempRegister, 0);
        }
    }
    void moveConditionallyTest64(ResultCondition cond, RegisterID left, RegisterID mask, RegisterID src, RegisterID dest)
    {
        emitTest64ToCR0(left, mask);
        BranchBitsRC b = testBranchBitsFor(cond);
        emitSkipOneInstruction(b.bo ^ 0x8, b.bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionallyTest64(ResultCondition cond, RegisterID left, RegisterID mask, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest64ToCR0(left, mask); });
    }
    void moveConditionallyTest64(ResultCondition cond, RegisterID left, TrustedImm32 mask, RegisterID src, RegisterID dest)
    {
        emitTest64ToCR0(left, mask);
        BranchBitsRC b = testBranchBitsFor(cond);
        emitSkipOneInstruction(b.bo ^ 0x8, b.bi);
        m_assembler.mr(dest, src);
    }
    void moveConditionallyTest64(ResultCondition cond, RegisterID left, TrustedImm32 mask, RegisterID thenCase, RegisterID elseCase, RegisterID dest)
    {
        BranchBitsRC b = testBranchBitsFor(cond);
        moveConditionallyImpl({ b.bo, b.bi }, thenCase, elseCase, dest, [&] { emitTest64ToCR0(left, mask); });
    }
    void neg64(RegisterID src, RegisterID dest) { m_assembler.neg(dest, src); }

    // LR access for prologue/epilogue code (LR is an SPR, not a GPR).
    void moveFromLR(RegisterID dest) { m_assembler.mflr(dest); }
    void moveToLR(RegisterID src)    { m_assembler.mtlr(src); }

    // Indirect jumps: mtctr + bctr (PtrTag is ignored — no pointer auth).
    void farJump(RegisterID target, PtrTag)
    {
        // ELFv2: a tail jump to a C function reaches its global entry, whose
        // prologue recomputes the TOC as r2 = addis/addi(r12, .TOC.-func). So
        // r12 must equal the target (its own entry address); a bare mtctr/bctr
        // leaves r12 stale, giving the callee a wrong TOC that faults on its
        // first TOC-relative or PLT call. Harmless for JIT-to-JIT targets.
        // memoryTempRegister is r12.
        if (target != memoryTempRegister)
            m_assembler.mr(memoryTempRegister, target);
        m_assembler.mtctr(memoryTempRegister);
        m_assembler.bctr();
    }
    void farJump(Address address, PtrTag tag)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.ld(dataTempRegister, r.offset, r.base);
        farJump(dataTempRegister, tag);
    }
    void farJump(BaseIndex address, PtrTag tag)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.ld(dataTempRegister, r.offset, r.base);
        farJump(dataTempRegister, tag);
    }
    void farJump(AbsoluteAddress address, PtrTag tag)
    {
        moveToAbsolute(address.m_ptr);
        m_assembler.ld(dataTempRegister, 0, memoryTempRegister);
        farJump(dataTempRegister, tag);
    }
    void farJump(TrustedImmPtr imm, PtrTag tag)
    {
        moveImmToScratch(int64_t(imm.asIntptr()), dataTempRegister);
        farJump(dataTempRegister, tag);
    }
    void farJump(RegisterID target, RegisterID)                            { farJump(target, NoPtrTag); }
    void farJump(Address address, RegisterID)                              { farJump(address, NoPtrTag); }

    void moveVector(FPRegisterID src, FPRegisterID dest)
    {
        if (src != dest)
            m_assembler.xxlor(dest, src, src); // 128-bit VSR copy
    }

    void push(TrustedImm32 imm)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        push(dataTempRegister);
    }

    // lxvd2x/stxvd2x are indexed (RA+RB) with no displacement, so compute the
    // effective address into a GPR and use RA=0 (literal zero).
    void loadVector(Address address, FPRegisterID dest)
    {
        add64(TrustedImm32(address.offset), address.base, memoryTempRegister);
        m_assembler.lxvd2x(dest, PPC64Registers::r0, memoryTempRegister);
    }
    void storeVector(FPRegisterID src, Address address)
    {
        add64(TrustedImm32(address.offset), address.base, memoryTempRegister);
        m_assembler.stxvd2x(src, PPC64Registers::r0, memoryTempRegister);
    }

    Jump branchPtrWithPatch(RelationalCondition cond, Address left, DataLabelPtr& dataLabel, TrustedImmPtr initialRightValue = TrustedImmPtr(nullptr))
    {
        load64(left, dataTempRegister);
        dataLabel = moveWithPatch(initialRightValue, memoryTempRegister);
        return branch64(cond, dataTempRegister, memoryTempRegister);
    }
    Jump branch32WithPatch(RelationalCondition cond, Address left, DataLabel32& dataLabel, TrustedImm32 initialRightValue = TrustedImm32(0))
    {
        load32(left, dataTempRegister);
        dataLabel = moveWithPatch(initialRightValue, memoryTempRegister);
        return branch32(cond, dataTempRegister, memoryTempRegister);
    }

    void abortWithReason(AbortReason reason)
    {
        moveImmToScratch(static_cast<int64_t>(reason), dataTempRegister);
        breakpoint();
    }
    void abortWithReason(AbortReason reason, intptr_t misc)
    {
        moveImmToScratch(static_cast<int64_t>(reason), dataTempRegister);
        moveImmToScratch(static_cast<int64_t>(misc), memoryTempRegister);
        breakpoint();
    }

    // Additional add64 overloads required by MacroAssembler.h addPtr wrappers.
    void add64(Address address, RegisterID dest)
    {
        load64(address, dataTempRegister);
        add64(dataTempRegister, dest);
    }
    void add64(TrustedImm32 imm, Address address)
    {
        load64(address, dataTempRegister);
        add64(imm, dataTempRegister, dataTempRegister);
        store64(dataTempRegister, address);
    }
    void add64(AbsoluteAddress address, RegisterID dest)
    {
        load64(address.m_ptr, dataTempRegister);
        add64(dataTempRegister, dest);
    }
    void add64(TrustedImm32 imm, AbsoluteAddress address)
    {
        load64(address.m_ptr, dataTempRegister);
        add64(imm, dataTempRegister, dataTempRegister);
        store64(dataTempRegister, const_cast<void*>(address.m_ptr));
    }

    // Additional xor64 overloads (MacroAssembler::xorPtr)
    void xor64(Address address, RegisterID dest)
    {
        load64(address, dataTempRegister);
        xor64(dataTempRegister, dest);
    }
    void xor64(RegisterID src, Address address)
    {
        load64(address, dataTempRegister);
        m_assembler.xor_(dataTempRegister, dataTempRegister, src);
        store64(dataTempRegister, address);
    }

    // test64 (MacroAssembler::testPtr)
    void test64(ResultCondition cond, RegisterID reg, TrustedImm32 mask, RegisterID dest)
    {
        if (mask.m_value == -1)
            m_assembler.mr(dataTempRegister, reg);
        else if (mask.m_value >= 0 && isUInt16(mask.m_value))
            m_assembler.andi_(dataTempRegister, reg, uint16_t(mask.m_value));
        else {
            moveImmToScratch(int64_t(mask.m_value), dataTempRegister);
            m_assembler.and_(dataTempRegister, reg, dataTempRegister);
        }
        m_assembler.cmpdi(0, dataTempRegister, 0);
        setFromResultCondition(cond, dest);
    }
    void test64(ResultCondition cond, RegisterID reg, RegisterID mask, RegisterID dest)
    {
        m_assembler.and_(dataTempRegister, reg, mask);
        m_assembler.cmpdi(0, dataTempRegister, 0);
        setFromResultCondition(cond, dest);
    }

    // branchAdd64 / branchSub64 (MacroAssembler::branchAddPtr/branchSubPtr)
    // dest = dest + src, branch on cond. Signed overflow iff the operands
    // agree in sign but the result differs: (origLeft ^ result) & (src ^ result) < 0.
    Jump branchAdd64(ResultCondition cond, RegisterID src, RegisterID dest)
    {
        if (cond == Carry)
            return branchAdd64(cond, dest, src, dest);
        if (cond == Overflow && src != dest && canUseBranchAdd64Overflow(src, dest, dest))
            return branchAdd64Overflow(dest, src, dest);
        if (cond == Overflow) {
            m_assembler.mr(dataTempRegister, dest);              // origLeft
            m_assembler.addc(dest, dest, src);                   // addc keeps XER.CA for setCarry recovery
            xor64(dataTempRegister, dest, memoryTempRegister);   // origLeft ^ result
            xor64(src, dest, dataTempRegister);                  // src ^ result
            m_assembler.and_(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.cmpdi(0, dataTempRegister, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 0));  // LT (negative) => overflow
        }
        add64(src, dest);
        return branchTest64Impl(resultConditionForArith(cond), dest);
    }
    Jump branchAdd64(ResultCondition cond, RegisterID a, RegisterID b, RegisterID dest)
    {
        if (cond == Carry) {
            // Unsigned carry-out of the 64-bit add: addc sets XER.CA, which no
            // branch can test directly, so materialise it (li 0; addze) and
            // branch if it is set. addc reads both inputs before writing, so
            // dest may alias either.
            m_assembler.addc(dest, a, b);
            setCarry(dataTempRegister);
            m_assembler.cmpdi(0, dataTempRegister, 0);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));  // != 0 → carry
        }
        if (cond == Overflow && a != b && canUseBranchAdd64Overflow(a, b, dest))
            return branchAdd64Overflow(a, b, dest);
        if (cond == Overflow) {
            // Save both operands first: dest may alias either. addc keeps
            // XER.CA correct for the B3 CheckAdd setCarry recovery.
            m_assembler.mr(dataTempRegister, a);
            m_assembler.mr(memoryTempRegister, b);
            m_assembler.addc(dest, a, b);
            xor64(dataTempRegister, dest, dataTempRegister);     // origA ^ result
            xor64(memoryTempRegister, dest, memoryTempRegister); // origB ^ result
            m_assembler.and_(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.cmpdi(0, dataTempRegister, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 0));  // LT (negative) => overflow
        }
        add64(a, b, dest);
        return branchTest64Impl(resultConditionForArith(cond), dest);
    }
    // Signed 64-bit overflow iff (a ^ result) & (b ^ result) is negative. With
    // a != b no carry is needed (only the B3 recovery of x + x reads XER.CA), so
    // this uses add rather than addc, and the record-form and. instead of a
    // separate cmpdi; dest still holds the wrapped sum when the branch is taken,
    // as the recovery expects. dest may alias a or b (not both, since a != b):
    // the operand it aliases is read last. Not used when an operand or dest is
    // one of the scratch registers the sequence writes.
    static bool canUseBranchAdd64Overflow(RegisterID a, RegisterID b, RegisterID dest)
    {
        auto isScratch = [](RegisterID r) { return r == dataTempRegister || r == memoryTempRegister; };
        return !isScratch(a) && !isScratch(b) && !isScratch(dest);
    }
    Jump branchAdd64Overflow(RegisterID a, RegisterID b, RegisterID dest)
    {
        ASSERT(a != b);
        if (dest == a)
            std::swap(a, b);
        m_assembler.add(dataTempRegister, a, b);                       // result
        m_assembler.xor_(memoryTempRegister, a, dataTempRegister);     // a ^ result
        m_assembler.xor_(dest, b, dataTempRegister);                   // b ^ result (b may be dest)
        m_assembler.and_rc(memoryTempRegister, memoryTempRegister, dest);
        m_assembler.mr(dest, dataTempRegister);
        return Jump(m_assembler.emitUnlinkedBranch(12, 0));            // LT (negative) => overflow
    }
    Jump branchAdd64(ResultCondition cond, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchAdd64(cond, memoryTempRegister, dest);
    }
    Jump branchSub64(ResultCondition cond, RegisterID left, RegisterID right, RegisterID dest)
    {
        if (cond == Overflow) {
            // Signed a-b overflows iff (a^b) & (a^result) is negative. Save the
            // original left first, since dest may alias it.
            m_assembler.mr(dataTempRegister, left);
            sub64(left, right, dest);
            xor64(dataTempRegister, right, memoryTempRegister);   // origLeft ^ right
            xor64(dataTempRegister, dest, dataTempRegister);      // origLeft ^ result
            m_assembler.and_(dataTempRegister, dataTempRegister, memoryTempRegister);
            m_assembler.cmpdi(0, dataTempRegister, 0);
            return Jump(m_assembler.emitUnlinkedBranch(12, 0));   // branch if LT (negative => overflow)
        }
        sub64(left, right, dest);
        return branchTest64Impl(resultConditionForArith(cond), dest);
    }
    Jump branchSub64(ResultCondition cond, RegisterID src, RegisterID dest)              { return branchSub64(cond, dest, src, dest); }
    Jump branchSub64(ResultCondition cond, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchSub64(cond, dest, memoryTempRegister, dest);
    }
    Jump branchSub64(ResultCondition cond, RegisterID left, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchSub64(cond, left, memoryTempRegister, dest);
    }

    // Additional and64 / xor64 / or64 / sub64 / compare64 overloads.
    void sub64(RegisterID src, TrustedImm64 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        sub64(src, memoryTempRegister, dest);   // dest = src - imm
    }
    void compare64(RelationalCondition cond, RegisterID left, TrustedImm64 right, RegisterID dest)
    {
        emitCompare64(cond, left, right);
        setFromCondition(cond, dest);
    }

    // 3-operand 32-bit shifts.
    // Shift an immediate value by a register count (dest = imm OP (count & 31)).
    void lshift32(TrustedImm32 imm, RegisterID shiftAmount, RegisterID dest)
    {
        move(imm, dataTempRegister);
        m_assembler.rlwinm(memoryTempRegister, shiftAmount, 0, 27, 31);   // count & 31
        m_assembler.slw(dest, dataTempRegister, memoryTempRegister);      // slw zero-extends
    }
    void rshift32(TrustedImm32 imm, RegisterID shiftAmount, RegisterID dest)
    {
        move(imm, dataTempRegister);
        m_assembler.rlwinm(memoryTempRegister, shiftAmount, 0, 27, 31);
        m_assembler.sraw(dest, dataTempRegister, memoryTempRegister);
        zeroExtend32ToWordInternal(dest);                                // sraw sign-extends
    }
    void urshift32(TrustedImm32 imm, RegisterID shiftAmount, RegisterID dest)
    {
        move(imm, dataTempRegister);
        m_assembler.rlwinm(memoryTempRegister, shiftAmount, 0, 27, 31);
        m_assembler.srw(dest, dataTempRegister, memoryTempRegister);      // srw zero-extends
    }

    // Additional branchAdd32 / branchMul32 / branchSub32 overloads.
    Jump branchAdd32(ResultCondition cond, RegisterID src, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchAdd32(cond, src, memoryTempRegister, dest);
    }
    Jump branchMul32(ResultCondition cond, RegisterID op1, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchMul32(cond, op1, memoryTempRegister, dest);
    }
    Jump branchSub32(ResultCondition cond, RegisterID op1, TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(imm.m_value, memoryTempRegister);
        return branchSub32(cond, op1, memoryTempRegister, dest);
    }

    // ------------------------------------------------------------------
    // Calls.
    //
    // Unlike offlineasm, the PtrTag is a compile-time argument here, so C
    // vs JS discrimination is direct.  C callees follow the full ELFv2
    // ABI: they run their GEP off r12 (we set it), scribble the caller's
    // linkage area at sp+0..31, and may clobber r2 (TOC).  Every JSC
    // C-call site reserves maxFrameExtentForSlowPathCall (96 bytes on
    // PPC64) below the live frame, so sp+24 — the ELFv2 TOC save slot —
    // is ours to save/restore r2 across the call.  JS callees follow the
    // JSC convention (caller-arranged frame; writing sp+24 would corrupt
    // an argument slot), so they get a bare mtctr/bctrl.
    // ------------------------------------------------------------------

    static bool isCFunctionCallTag(PtrTag tag)
    {
        return tag == OperationPtrTag
            || tag == CFunctionPtrTag
            || tag == HostFunctionPtrTag
            || tag == CustomAccessorPtrTag;
    }

    ALWAYS_INLINE void emitCCallGuardPre()
    {
        m_assembler.std(PPC64Registers::r2, 24, stackPointerRegister);
    }
    ALWAYS_INLINE void emitCCallGuardPost()
    {
        m_assembler.ld(PPC64Registers::r2, 24, stackPointerRegister);
    }

    // nearCall / nearTailCall: fixed patchable slots, linked via linkCall.
    Call nearCall()
    {
        return Call(m_assembler.emitUnlinkedCall(), Call::LinkableNear);
    }
    Call nearTailCall()
    {
        // A tail call is a jump slot; the label is the slot START (what
        // linkJump expects).
        return Call(m_assembler.emitUnlinkedJump(), Call::LinkableNearTail);
    }

    // Thread-safe patchable variants (wasm calls, which tier-up retargets
    // while other threads run them). A tail call is a jump slot, whose [0] is
    // always a single b or nop, so any slot retargets with one store. A call
    // slot is tagged so that it never takes the far form, which could not be
    // retargeted with one store (PPC64Assembler::applyNearCallSlot).
    Call threadSafePatchableNearCall()
    {
        return Call(m_assembler.emitUnlinkedCall(/* threadSafe */ true), Call::LinkableNear);
    }
    Call threadSafePatchableNearTailCall() { return nearTailCall(); }

    Call call(PtrTag tag)
    {
        bool cCall = isCFunctionCallTag(tag);
        if (cCall)
            emitCCallGuardPre();
        AssemblerLabel label = m_assembler.emitUnlinkedCall();
        if (cCall)
            emitCCallGuardPost();
        // General repatchable call (callOperation etc.): must be Linkable and NOT
        // Near — locationOf()/link() require a non-near call, and JITMathIC queries
        // locationOf(slowPathCall) to repatch it; for a Near call locationOf
        // returns the wrong location. Only nearCall() is LinkableNear. Both emit the same slot; the flag picks the
        // linked form (see linkCall below) as well as location/repatch routing.
        return Call(label, Call::Linkable);
    }

    Call call(RegisterID target, PtrTag tag)
    {
        bool cCall = isCFunctionCallTag(tag);
        if (cCall)
            emitCCallGuardPre();
        // r12 = entry so a C callee's GEP computes its TOC (harmless for JS).
        m_assembler.mr(memoryTempRegister, target);
        m_assembler.mtctr(memoryTempRegister);
        m_assembler.bctrl();
        if (cCall)
            emitCCallGuardPost();
        return Call(m_assembler.labelIgnoringWatchpoints(), Call::None);
    }

    Call call(Address address, PtrTag tag)
    {
        ResolvedAddress r = resolveAddressDS(address, memoryTempRegister);
        m_assembler.ld(dataTempRegister, r.offset, r.base);
        return call(dataTempRegister, tag);
    }
    Call call(RegisterID callTag)                           { UNUSED_PARAM(callTag); return call(NoPtrTag); }
    Call call(RegisterID target, RegisterID callTag)        { UNUSED_PARAM(callTag); return call(target, NoPtrTag); }
    Call call(Address address, RegisterID callTag)          { UNUSED_PARAM(callTag); return call(address, NoPtrTag); }

    // patchableJumpSize — used by JITMathIC for size computation.
    static ptrdiff_t patchableJumpSize() { return Assembler::patchableJumpSize(); }

    // Sign/zero extension.
    void signExtend32ToPtr(TrustedImm32 imm, RegisterID dest)
    {
        moveImmToScratch(int64_t(imm.m_value), dest);
    }
    void signExtend32ToPtr(RegisterID src, RegisterID dest)
    {
        m_assembler.extsw(dest, src);
    }
    void zeroExtend32ToWord(RegisterID src, RegisterID dest)
    {
        m_assembler.rldicl(dest, src, 0, 32);
    }

    // Memory barriers. lwsync orders all pairs
    // except StoreLoad; a full StoreLoad / seq-cst fence needs sync.
    void memoryFence()    { m_assembler.sync(); }       // full (StoreLoad) fence
    void storeFence()     { m_assembler.lwsync(); }     // release ordering
    void loadFence()      { m_assembler.lwsync(); }     // acquire ordering
    void storeLoadFence() { m_assembler.sync(); }

    void swap(RegisterID a, RegisterID b)
    {
        if (a == b)
            return;
        m_assembler.mr(dataTempRegister, a);
        m_assembler.mr(a, b);
        m_assembler.mr(b, dataTempRegister);
    }

    // 32-bit negate
    void neg32(RegisterID src, RegisterID dest)
    {
        m_assembler.neg(dest, src);
        zeroExtend32ToWordInternal(dest);
    }

    // load8SignedExtendTo32 / load16 / load16SignedExtendTo32
    void load8SignedExtendTo32(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lbz(dest, 0, memoryTempRegister);
        m_assembler.extsb(dest, dest);
        zeroExtend32ToWordInternal(dest);
    }
    void load16SignedExtendTo32(const void* address, RegisterID dest)
    {
        moveToAbsolute(address);
        m_assembler.lha(dest, 0, memoryTempRegister);
        zeroExtend32ToWordInternal(dest);
    }

    // Float16 (IEEE binary16), used only when supportsFloat16(). A Float16
    // value in an FPR is its 16 bits in bits 48:63 of the VSR's doubleword 0
    // (the low halfword of the FPR image), the format xscvhpdp reads and
    // xscvdphp writes. Loads, stores and moves are POWER8 instructions; only
    // the conversions need ISA 3.0.
    void loadFloat16(Address address, FPRegisterID dest)
    {
        load16(address, dataTempRegister);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    void loadFloat16(BaseIndex address, FPRegisterID dest)
    {
        load16(address, dataTempRegister);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    void convertFloat16ToDouble(FPRegisterID src, FPRegisterID dest)
    {
        ASSERT(supportsFloat16());
        m_assembler.xscvhpdp(vsr(dest), vsr(src));
    }

    Jump branchIfNaN(FPRegisterID reg)
    {
        // NaN iff reg is unordered with itself. fcmpu sets CR0[FU] (bit 3).
        m_assembler.fcmpu(0, reg, reg);
        return Jump(m_assembler.emitUnlinkedBranch(12, 3));   // branch if FU (unordered) set
    }

    // Byte/half store, FP type conversion, FP16, and 32-bit transfers.
    void store8(TrustedImm32 imm, BaseIndex address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store8(dataTempRegister, address);
    }
    void convertDoubleToFloat16(FPRegisterID src, FPRegisterID dest)
    {
        ASSERT(supportsFloat16());
        m_assembler.xscvdphp(vsr(dest), vsr(src)); // rounds per FPSCR[RN], nearest-even
    }
    // The address goes into memoryTempRegister first: forming it can use
    // dataTempRegister, which then carries the bits.
    void storeFloat16(FPRegisterID src, Address address)
    {
        add64(TrustedImm32(address.offset), address.base, memoryTempRegister);
        m_assembler.mfvsrd(dataTempRegister, src);
        m_assembler.sth(dataTempRegister, 0, memoryTempRegister);
    }
    void storeFloat16(FPRegisterID src, BaseIndex address)
    {
        getEffectiveAddress(address, memoryTempRegister);
        m_assembler.mfvsrd(dataTempRegister, src);
        m_assembler.sth(dataTempRegister, 0, memoryTempRegister);
    }

    // callOperation — InlineCacheCompiler invokes this for slow-path C calls.
    template<PtrTag tag>
    void callOperation(const CodePtr<tag> operation)
    {
        move(TrustedImmPtr(operation.taggedPtr()), dataTempRegister);
        call(dataTempRegister, tag);
    }

    void test32(ResultCondition cond, RegisterID reg, TrustedImm32 mask, RegisterID dest)
    {
        if (mask.m_value == -1)
            m_assembler.mr(dataTempRegister, reg);
        else if (mask.m_value >= 0 && isUInt16(mask.m_value))
            m_assembler.andi_(dataTempRegister, reg, uint16_t(mask.m_value));
        else {
            moveImmToScratch(uint32_t(mask.m_value), dataTempRegister);
            m_assembler.and_(dataTempRegister, reg, dataTempRegister);
        }
        // cmpwi tests only the low 32 bits (EQ for Zero/NonZero, LT for the
        // 32-bit sign), so it serves all four ResultConditions.
        m_assembler.cmpwi(0, dataTempRegister, 0);
        setFromResultCondition(cond, dest);
    }
    void test32(ResultCondition cond, RegisterID reg, RegisterID mask, RegisterID dest)
    {
        m_assembler.and_(dataTempRegister, reg, mask);
        m_assembler.cmpwi(0, dataTempRegister, 0);
        setFromResultCondition(cond, dest);
    }
    void test32(ResultCondition cond, Address address, TrustedImm32 mask, RegisterID dest)
    {
        load32(address, memoryTempRegister);
        test32(cond, memoryTempRegister, mask, dest);
    }

    // or16 / add8 — small-width arithmetic with memory destination (typed array writes).
    void add8(TrustedImm32 imm, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dataTempRegister, r.offset, r.base);
        m_assembler.addi(dataTempRegister, dataTempRegister, int16_t(imm.m_value));
        m_assembler.stb(dataTempRegister, r.offset, r.base);
    }
    void add8(RegisterID src, BaseIndex address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lbz(dataTempRegister, r.offset, r.base);
        m_assembler.add(dataTempRegister, dataTempRegister, src);
        m_assembler.stb(dataTempRegister, r.offset, r.base);
    }

    void store8(TrustedImm32 imm, void* address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        moveToAbsolute(address);
        m_assembler.stb(dataTempRegister, 0, memoryTempRegister);
    }
    void store64(TrustedImm64 imm, void* address)
    {
        moveImmToScratch(imm.m_value, dataTempRegister);
        store64(dataTempRegister, address);
    }

    DataLabelPtr moveWithPatch(TrustedImmPtr initialValue, RegisterID dest)
    {
        DataLabelPtr label(this);
        moveFixedLi64(dest, uint64_t(initialValue.asIntptr()));
        return label;
    }
    // Fixed 2-instruction (lis/ori) patchable 32-bit immediate load.
    DataLabel32 moveWithPatch(TrustedImm32 initialValue, RegisterID dest)
    {
        DataLabel32 label(this);
        m_assembler.lis(dest, int16_t(uint16_t(uint32_t(initialValue.m_value) >> 16)));
        m_assembler.ori(dest, dest, uint16_t(initialValue.m_value));
        return label;
    }

    void branchConvertDoubleToInt32(FPRegisterID src, RegisterID dest, JumpList& failureCases, FPRegisterID fpTemp, bool negZeroCheck = true)
    {
        // Convert the (integer-valued) double to int32 via round-toward-zero,
        // then round-trip back to double and compare: any mismatch or unordered
        // result (NaN, or an out-of-int32-range value that fctiwz saturated to
        // INT32_MIN/MAX) means the double was not an exact int32 -> failure.
        m_assembler.fctiwz(fpTempRegister, src);
        m_assembler.mfvsrwz(dest, fpTempRegister);
        convertInt32ToDouble(dest, fpTemp);
        failureCases.append(branchDouble(DoubleNotEqualOrUnordered, src, fpTemp));

        // Negative zero: dest == 0 but src has its sign bit set (src == -0.0).
        if (negZeroCheck) {
            Jump valueIsNonZero = branchTest32(NonZero, dest);
            moveDoubleTo64(src, dataTempRegister);
            failureCases.append(branch64(LessThan, dataTempRegister, TrustedImm64(0)));
            valueIsNonZero.link(this);
        }
    }

    // Byte-width test/compare and bitwise NOT.
    void test8(ResultCondition cond, Address address, TrustedImm32 mask, RegisterID dest)
    {
        load8(address, dataTempRegister); // zero-extended byte
        m_assembler.andi_(dataTempRegister, dataTempRegister, uint16_t(mask.m_value & 0xFF));
        m_assembler.cmpwi(0, dataTempRegister, 0);
        setFromResultCondition(cond, dest);
    }
    void compare8(RelationalCondition cond, Address left, TrustedImm32 right, RegisterID dest)
    {
        load8(left, dataTempRegister); // zero-extended byte
        emitCompare32(cond, dataTempRegister, TrustedImm32(right.m_value & 0xFF));
        setFromCondition(cond, dest);
    }
    void compare8(RelationalCondition cond, Address left, RegisterID right, RegisterID dest)
    {
        load8(left, dataTempRegister); // zero-extended byte
        emitCompare32(cond, dataTempRegister, right);
        setFromCondition(cond, dest);
    }
    void not32(RegisterID src, RegisterID dest)
    {
        m_assembler.nor(dest, src, src);   // ~src
        zeroExtend32ToWordInternal(dest);
    }

    // Imm64 overloads — Imm64 is privately derived from TrustedImm64, so callers
    // pass it explicitly for "untrusted" 64-bit immediates (range-check checked).
    Jump branch64(RelationalCondition cond, RegisterID left, Imm64 right)
    {
        return branch64(cond, left, right.asTrustedImm64());
    }

    // Negate-with-branch-on-overflow.
    // srcDest = -srcDest (32-bit). Signed overflow iff srcDest was INT32_MIN.
    Jump branchNeg32(ResultCondition cond, RegisterID srcDest)
    {
        if (cond == Overflow) {
            m_assembler.rldicl(dataTempRegister, srcDest, 0, 32);   // orig, zero-extended low32
            m_assembler.neg(srcDest, srcDest);
            zeroExtend32ToWordInternal(srcDest);
            moveImmToScratch(int64_t(0x80000000u), memoryTempRegister);
            m_assembler.cmpld(0, dataTempRegister, memoryTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(12, 2));     // EQ (orig==INT32_MIN) => overflow
        }
        m_assembler.neg(srcDest, srcDest);
        zeroExtend32ToWordInternal(srcDest);
        return branchTestImpl32(resultConditionForArith(cond), srcDest);
    }
    // srcDest = -srcDest (64-bit). Signed overflow iff srcDest was INT64_MIN.
    Jump branchNeg64(ResultCondition cond, RegisterID srcDest)
    {
        if (cond == Overflow) {
            m_assembler.mr(dataTempRegister, srcDest);              // orig
            m_assembler.neg(srcDest, srcDest);
            moveImmToScratch(int64_t(0x8000000000000000ull), memoryTempRegister);
            m_assembler.cmpd(0, dataTempRegister, memoryTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(12, 2));     // EQ (orig==INT64_MIN) => overflow
        }
        m_assembler.neg(srcDest, srcDest);
        return branchTest64Impl(resultConditionForArith(cond), srcDest);
    }
    // dest = a * b (64-bit). Signed overflow iff the 128-bit product's high
    // word differs from the sign-extension of the low word (mulhd != low>>63).
    Jump branchMul64(ResultCondition cond, RegisterID a, RegisterID b, RegisterID dest)
    {
        if (cond == Overflow) {
            m_assembler.mulld(dataTempRegister, a, b);              // low 64
            m_assembler.mulhd(memoryTempRegister, a, b);            // high 64 (signed)
            m_assembler.mr(dest, dataTempRegister);                 // result = low
            m_assembler.sradi(dataTempRegister, dataTempRegister, 63);  // sign of low
            m_assembler.cmpd(0, memoryTempRegister, dataTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));      // high != sign => overflow
        }
        m_assembler.mulld(dest, a, b);
        return branchTest64Impl(resultConditionForArith(cond), dest);
    }
    Jump branchMul64(ResultCondition cond, RegisterID src, RegisterID dest) { return branchMul64(cond, src, dest, dest); }

    Jump branchAdd32(ResultCondition cond, Address address, RegisterID dest)
    {
        load32(address, dataTempRegister);
        return branchAdd32(cond, dataTempRegister, dest, dest);
    }

    // transfer64 / transferVector / storePair32 — memory shuffles.
    void transferVector(Address src, Address dest)
    {
        loadVector(src, fpTempRegister);
        storeVector(fpTempRegister, dest);
    }
    void transferVector(Address src, BaseIndex dest)
    {
        loadVector(src, fpTempRegister);
        getEffectiveAddress(dest, memoryTempRegister);
        m_assembler.stxvd2x(fpTempRegister, PPC64Registers::r0, memoryTempRegister);
    }
    void transferVector(BaseIndex src, BaseIndex dest)
    {
        getEffectiveAddress(src, memoryTempRegister);
        m_assembler.lxvd2x(fpTempRegister, PPC64Registers::r0, memoryTempRegister);
        getEffectiveAddress(dest, memoryTempRegister);
        m_assembler.stxvd2x(fpTempRegister, PPC64Registers::r0, memoryTempRegister);
    }
    void storePair32(RegisterID src1, TrustedImm32 imm2, Address address)
    {
        store32(src1, address);
        store32(imm2, address.withOffset(4));
    }
    void storePair32(TrustedImm32 imm1, RegisterID src2, Address address)
    {
        store32(imm1, address);
        store32(src2, address.withOffset(4));
    }
    void storePair32(TrustedImm32 imm1, TrustedImm32 imm2, Address address)
    {
        store32(imm1, address);
        store32(imm2, address.withOffset(4));
    }

    // Sign-extend 8/16-bit values; byte-swap halfword.
    void signExtend8To32(RegisterID src, RegisterID dest)
    {
        m_assembler.extsb(dest, src);
        zeroExtend32ToWordInternal(dest);   // 32-bit result, upper word cleared
    }
    void signExtend16To32(RegisterID src, RegisterID dest)
    {
        m_assembler.extsh(dest, src);
        zeroExtend32ToWordInternal(dest);
    }
    // Register byte-reverse: brh/brw/brd on POWER10, else rotate/mask sequences.
    // The 16- and 32-bit results are zero-extended.
    // Accesses to memory whose byte order is fixed by its format rather than
    // by the host (wasm linear memory). Only big-endian builds call these;
    // the byte-reversed forms are X-form, so the address goes through
    // memoryTempRegister and the data, for floats, through dataTempRegister
    // and the ELFv2 red zone (the FPRs hold doubles).
    template<typename AddressType>
    RegisterID littleEndianAddress(AddressType address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        if (!r.offset)
            return r.base;
        m_assembler.addi(memoryTempRegister, r.base, r.offset);
        return memoryTempRegister;
    }
    template<typename AddressType>
    void load16LittleEndian(AddressType address, RegisterID dest)
    {
        m_assembler.lhbrx(dest, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void load16SignedExtendTo32LittleEndian(AddressType address, RegisterID dest)
    {
        load16LittleEndian(address, dest);
        m_assembler.extsh(dest, dest);
        zeroExtend32ToWordInternal(dest);
    }
    template<typename AddressType>
    void load16SignedExtendTo64LittleEndian(AddressType address, RegisterID dest)
    {
        load16LittleEndian(address, dest);
        m_assembler.extsh(dest, dest);
    }
    template<typename AddressType>
    void load32LittleEndian(AddressType address, RegisterID dest)
    {
        m_assembler.lwbrx(dest, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void load32SignedExtendTo64LittleEndian(AddressType address, RegisterID dest)
    {
        load32LittleEndian(address, dest);
        m_assembler.extsw(dest, dest);
    }
    template<typename AddressType>
    void load64LittleEndian(AddressType address, RegisterID dest)
    {
        m_assembler.ldbrx(dest, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void loadFloatLittleEndian(AddressType address, FPRegisterID dest)
    {
        load32LittleEndian(address, dataTempRegister);
        m_assembler.stw(dataTempRegister, -8, stackPointerRegister);
        m_assembler.lfs(dest, -8, stackPointerRegister);
    }
    template<typename AddressType>
    void loadDoubleLittleEndian(AddressType address, FPRegisterID dest)
    {
        load64LittleEndian(address, dataTempRegister);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    template<typename AddressType>
    void store16LittleEndian(RegisterID src, AddressType address)
    {
        m_assembler.sthbrx(src, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void store32LittleEndian(RegisterID src, AddressType address)
    {
        m_assembler.stwbrx(src, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void store64LittleEndian(RegisterID src, AddressType address)
    {
        m_assembler.stdbrx(src, PPC64Registers::r0, littleEndianAddress(address));
    }
    template<typename AddressType>
    void storeFloatLittleEndian(FPRegisterID src, AddressType address)
    {
        m_assembler.stfs(src, -8, stackPointerRegister);
        m_assembler.lwz(dataTempRegister, -8, stackPointerRegister);
        store32LittleEndian(dataTempRegister, address);
    }
    template<typename AddressType>
    void storeDoubleLittleEndian(FPRegisterID src, AddressType address)
    {
        m_assembler.mfvsrd(dataTempRegister, src);
        store64LittleEndian(dataTempRegister, address);
    }

    void byteSwap16(RegisterID reg)
    {
        if (supportsISA3_1()) {
            m_assembler.brh(reg, reg);
            m_assembler.rldicl(reg, reg, 0, 48);
            return;
        }
        m_assembler.rlwinm(dataTempRegister, reg, 8, 16, 23);   // (reg<<8) & 0x0000ff00
        m_assembler.rlwinm(memoryTempRegister, reg, 24, 24, 31); // (reg>>8) & 0x000000ff
        m_assembler.or_(reg, dataTempRegister, memoryTempRegister);
    }
    void byteSwap32(RegisterID reg)
    {
        if (supportsISA3_1()) {
            m_assembler.brw(reg, reg);
            m_assembler.rldicl(reg, reg, 0, 32);
            return;
        }
        m_assembler.rlwinm(dataTempRegister, reg, 8, 0, 31);    // rotl(reg,8)
        m_assembler.rlwimi(dataTempRegister, reg, 24, 0, 7);
        m_assembler.rlwimi(dataTempRegister, reg, 24, 16, 23);
        m_assembler.mr(reg, dataTempRegister);
    }
    void byteSwap64(RegisterID reg)
    {
        if (supportsISA3_1()) {
            m_assembler.brd(reg, reg);
            return;
        }
        // reg = HIGH:LOW; result = bswap32(LOW):bswap32(HIGH).
        m_assembler.rldicl(dataTempRegister, reg, 32, 32);      // dataTemp = HIGH (zero-extended)
        m_assembler.rldicl(memoryTempRegister, reg, 0, 32);     // memoryTemp = LOW (zero-extended)
        // bswap32 HIGH in place (dataTemp)
        m_assembler.rlwinm(reg, dataTempRegister, 8, 0, 31);
        m_assembler.rlwimi(reg, dataTempRegister, 24, 0, 7);
        m_assembler.rlwimi(reg, dataTempRegister, 24, 16, 23);
        m_assembler.mr(dataTempRegister, reg);                  // dataTemp = bswap32(HIGH)
        // bswap32 LOW in place (memoryTemp)
        m_assembler.rlwinm(reg, memoryTempRegister, 8, 0, 31);
        m_assembler.rlwimi(reg, memoryTempRegister, 24, 0, 7);
        m_assembler.rlwimi(reg, memoryTempRegister, 24, 16, 23);
        // reg = bswap32(LOW); result = (bswap32(LOW) << 32) | bswap32(HIGH)
        m_assembler.sldi(reg, reg, 32);
        m_assembler.or_(reg, reg, dataTempRegister);
    }

    // Fold an Address/BaseIndex into a single register holding the full
    // effective address, as required by the indexed larx/stcx. forms (which
    // take rA+rB and no displacement). We pass rA=0, rB=this register.
    template<typename AddressType>
    RegisterID prepareAtomicAddress(AddressType address, RegisterID scratch)
    {
        ResolvedAddress r = resolveAddress(address, scratch);
        if (r.offset) {
            m_assembler.addi(scratch, r.base, r.offset);
            return scratch;
        }
        return r.base;
    }

    // Atomic weak compare-and-swap (larx/stcx.). Load-reserve,
    // compare against `expected`, and if equal attempt a store-conditional of
    // `newValue`. The returned Jump follows the StatusCondition:
    //   Success -> taken iff the value matched AND the store-conditional held
    //              (the CAS completed).
    //   Failure -> taken iff the CAS did not complete (value mismatch or the
    //              reservation was lost).
    // Mirrors the ARM64 ldaxr/stlxr contract. Seq-cst ordering: a leading full
    // `sync` and an `isync` on the success (acquire) path. `expected` is
    // clobbered — zero-extended to `width` to match the zero-extended larx
    // result. The load value uses dataTempRegister; the address uses
    // memoryTempRegister.
    template<typename AddressType>
    Jump branchAtomicWeakCAS(unsigned width, StatusCondition cond, RegisterID expected, RegisterID newValue, AddressType address)
    {
        RegisterID addr = prepareAtomicAddress(address, memoryTempRegister);
        RegisterID tmp = dataTempRegister;
        constexpr RegisterID zero = PPC64Registers::r0; // rA=0 -> literal 0 in larx/stcx.

        switch (width) {
        case 8:  and64(TrustedImm32(0xFF), expected); break;
        case 16: and64(TrustedImm32(0xFFFF), expected); break;
        case 32: zeroExtend32ToWord(expected, expected); break;
        default: break; // 64-bit: full width
        }

        m_assembler.sync(); // seq-cst leading barrier

        switch (width) {
        case 8:  m_assembler.lbarx(tmp, zero, addr); break;
        case 16: m_assembler.lharx(tmp, zero, addr); break;
        case 32: m_assembler.lwarx(tmp, zero, addr); break;
        default: m_assembler.ldarx(tmp, zero, addr); break;
        }

        m_assembler.cmpld(0, expected, tmp); // unsigned 64-bit; both zero-extended
        Jump mismatch = makeBranch(NotEqual);

        switch (width) {
        case 8:  m_assembler.stbcx_(newValue, zero, addr); break;
        case 16: m_assembler.sthcx_(newValue, zero, addr); break;
        case 32: m_assembler.stwcx_(newValue, zero, addr); break;
        default: m_assembler.stdcx_(newValue, zero, addr); break;
        }
        // stcx. sets CR0[EQ]=1 iff the store succeeded (reservation held).

        // Acquire on every outcome, as ARM64's ldaxr gives even when the value
        // does not match: a failed CAS has still read the location, and a
        // caller may act on that (the strong CAS does the same).
        if (cond == Success) {
            Jump storeFailed = makeBranch(NotEqual); // CR0[EQ] clear -> stcx. failed
            m_assembler.isync();                     // acquire on success
            Jump success = jump();
            mismatch.link(this);
            storeFailed.link(this);
            m_assembler.isync();                     // acquire on failure
            return success;
        }
        // Failure form: taken on value mismatch or a lost reservation.
        Jump stored = makeBranch(Equal); // CR0[EQ] set -> stcx. succeeded
        mismatch.link(this);             // mismatch joins the store-failed fall-through
        m_assembler.isync();             // acquire on failure
        Jump failure = jump();
        stored.link(this);
        m_assembler.isync();             // acquire on the success path
        return failure;
    }

    Jump branchAtomicWeakCAS8(StatusCondition cond, RegisterID expected, RegisterID newValue, Address address)    { return branchAtomicWeakCAS(8, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS8(StatusCondition cond, RegisterID expected, RegisterID newValue, BaseIndex address)  { return branchAtomicWeakCAS(8, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS16(StatusCondition cond, RegisterID expected, RegisterID newValue, Address address)   { return branchAtomicWeakCAS(16, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS16(StatusCondition cond, RegisterID expected, RegisterID newValue, BaseIndex address) { return branchAtomicWeakCAS(16, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS32(StatusCondition cond, RegisterID expected, RegisterID newValue, Address address)   { return branchAtomicWeakCAS(32, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS32(StatusCondition cond, RegisterID expected, RegisterID newValue, BaseIndex address) { return branchAtomicWeakCAS(32, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS64(StatusCondition cond, RegisterID expected, RegisterID newValue, Address address)   { return branchAtomicWeakCAS(64, cond, expected, newValue, address); }
    Jump branchAtomicWeakCAS64(StatusCondition cond, RegisterID expected, RegisterID newValue, BaseIndex address) { return branchAtomicWeakCAS(64, cond, expected, newValue, address); }

    // Strong compare-and-swap. Unlike branchAtomicWeakCAS this retries on a
    // lost reservation, so it only returns once the CAS genuinely succeeded or
    // genuinely saw a mismatching value. `expectedAndResult` supplies the
    // comparand and receives the previous memory value; `result` receives 1 or
    // 0 according to whether the outcome matched `cond`. Mirrors the ARM64
    // ldaxr/stlxr version except on a mismatch, which is reported straight
    // from the larx (see below).
    template<typename AddressType>
    void atomicStrongCAS(unsigned width, StatusCondition cond, RegisterID expectedAndResult, RegisterID newValue, AddressType address, RegisterID result)
    {
        RegisterID addr = prepareAtomicAddress(address, memoryTempRegister);
        RegisterID tmp = dataTempRegister;
        constexpr RegisterID zero = PPC64Registers::r0;

        // Match the zero-extended larx result so cmpld compares like for like.
        switch (width) {
        case 8:  and64(TrustedImm32(0xFF), expectedAndResult); break;
        case 16: and64(TrustedImm32(0xFFFF), expectedAndResult); break;
        case 32: zeroExtend32ToWord(expectedAndResult, expectedAndResult); break;
        default: break;
        }

        m_assembler.sync(); // seq-cst leading barrier
        Label reloop(this);
        switch (width) {
        case 8:  m_assembler.lbarx(tmp, zero, addr); break;
        case 16: m_assembler.lharx(tmp, zero, addr); break;
        case 32: m_assembler.lwarx(tmp, zero, addr); break;
        default: m_assembler.ldarx(tmp, zero, addr); break;
        }
        m_assembler.cmpld(0, expectedAndResult, tmp);
        Jump failure = makeBranch(NotEqual);

        switch (width) {
        case 8:  m_assembler.stbcx_(newValue, zero, addr); break;
        case 16: m_assembler.sthcx_(newValue, zero, addr); break;
        case 32: m_assembler.stwcx_(newValue, zero, addr); break;
        default: m_assembler.stdcx_(newValue, zero, addr); break;
        }
        makeBranch(NotEqual).linkTo(reloop, this); // reservation lost -> retry
        m_assembler.isync();                       // acquire
        move(TrustedImm32(cond == Success), result);
        Jump done = jump();

        failure.link(this);
        // Mismatch: the larx was an atomic read of a value other than the
        // comparand, which is all a failed strong CAS has to observe, so report
        // it without storing anything (as atomicStrongCASNoStatus does). The
        // ARM64 shape stores the loaded value back and retries the whole CAS if
        // that store-conditional fails, but by then it has already replaced the
        // comparand with the observed value, so the retry compares the value
        // against itself, matches, and stores newValue: a mismatching CAS that
        // writes, whenever the store-back's stcx. fails.
        move(tmp, expectedAndResult); // report the value we actually saw
        m_assembler.isync();
        move(TrustedImm32(cond == Failure), result);

        done.link(this);
    }

    // Three-operand form: same CAS, but the caller only wants the previous
    // value in expectedAndResult and no success flag. BBQ uses both shapes.
    template<typename AddressType>
    void atomicStrongCASNoStatus(unsigned width, RegisterID expectedAndResult, RegisterID newValue, AddressType address)
    {
        RegisterID addr = prepareAtomicAddress(address, memoryTempRegister);
        RegisterID tmp = dataTempRegister;
        constexpr RegisterID zero = PPC64Registers::r0;

        switch (width) {
        case 8:  and64(TrustedImm32(0xFF), expectedAndResult); break;
        case 16: and64(TrustedImm32(0xFFFF), expectedAndResult); break;
        case 32: zeroExtend32ToWord(expectedAndResult, expectedAndResult); break;
        default: break;
        }

        m_assembler.sync();
        Label reloop(this);
        switch (width) {
        case 8:  m_assembler.lbarx(tmp, zero, addr); break;
        case 16: m_assembler.lharx(tmp, zero, addr); break;
        case 32: m_assembler.lwarx(tmp, zero, addr); break;
        default: m_assembler.ldarx(tmp, zero, addr); break;
        }
        m_assembler.cmpld(0, expectedAndResult, tmp);
        Jump mismatch = makeBranch(NotEqual);
        switch (width) {
        case 8:  m_assembler.stbcx_(newValue, zero, addr); break;
        case 16: m_assembler.sthcx_(newValue, zero, addr); break;
        case 32: m_assembler.stwcx_(newValue, zero, addr); break;
        default: m_assembler.stdcx_(newValue, zero, addr); break;
        }
        makeBranch(NotEqual).linkTo(reloop, this); // strong: retry a lost reservation
        mismatch.link(this);
        m_assembler.isync();
        move(tmp, expectedAndResult);
    }

    template<typename AddressType> void atomicStrongCAS8(RegisterID expectedAndResult, RegisterID newValue, AddressType address)  { atomicStrongCASNoStatus(8, expectedAndResult, newValue, address); }
    template<typename AddressType> void atomicStrongCAS16(RegisterID expectedAndResult, RegisterID newValue, AddressType address) { atomicStrongCASNoStatus(16, expectedAndResult, newValue, address); }
    template<typename AddressType> void atomicStrongCAS32(RegisterID expectedAndResult, RegisterID newValue, AddressType address) { atomicStrongCASNoStatus(32, expectedAndResult, newValue, address); }
    template<typename AddressType> void atomicStrongCAS64(RegisterID expectedAndResult, RegisterID newValue, AddressType address) { atomicStrongCASNoStatus(64, expectedAndResult, newValue, address); }

    template<typename AddressType> void atomicStrongCAS8(StatusCondition cond, RegisterID expectedAndResult, RegisterID newValue, AddressType address, RegisterID result)  { atomicStrongCAS(8, cond, expectedAndResult, newValue, address, result); }
    template<typename AddressType> void atomicStrongCAS16(StatusCondition cond, RegisterID expectedAndResult, RegisterID newValue, AddressType address, RegisterID result) { atomicStrongCAS(16, cond, expectedAndResult, newValue, address, result); }
    template<typename AddressType> void atomicStrongCAS32(StatusCondition cond, RegisterID expectedAndResult, RegisterID newValue, AddressType address, RegisterID result) { atomicStrongCAS(32, cond, expectedAndResult, newValue, address, result); }
    template<typename AddressType> void atomicStrongCAS64(StatusCondition cond, RegisterID expectedAndResult, RegisterID newValue, AddressType address, RegisterID result) { atomicStrongCAS(64, cond, expectedAndResult, newValue, address, result); }

    // --- Atomic read-modify-write, x86 "lock xadd" / "xchg" shape ---------
    // `reg` supplies the operand and receives the previous memory value,
    // zero-extended from the access width. Sequential consistency uses the
    // same leading sync / trailing isync idiom as branchAtomicWeakCAS above.
    // `reg` is never written inside the larx/stcx. loop, so a lost
    // reservation can simply retry.
    template<typename AddressType>
    void atomicXchgAdd(unsigned width, RegisterID reg, AddressType address)
    {
        RegisterID addr = prepareAtomicAddress(address, memoryTempRegister);
        RegisterID tmp = dataTempRegister;
        constexpr RegisterID zero = PPC64Registers::r0; // rA=0 -> literal 0 in larx/stcx.

        m_assembler.sync(); // seq-cst leading barrier
        Label reloop(this);
        switch (width) {
        case 8:  m_assembler.lbarx(tmp, zero, addr); break;
        case 16: m_assembler.lharx(tmp, zero, addr); break;
        case 32: m_assembler.lwarx(tmp, zero, addr); break;
        default: m_assembler.ldarx(tmp, zero, addr); break;
        }
        // Hold the *sum* in tmp rather than the old value: that keeps reg
        // intact for a retry, and the old value is recovered exactly as
        // sum - reg once the store sticks (true at every width, since larx
        // zero-extends and the subtraction is done at full 64-bit width).
        m_assembler.add(tmp, tmp, reg);
        switch (width) {
        case 8:  m_assembler.stbcx_(tmp, zero, addr); break;
        case 16: m_assembler.sthcx_(tmp, zero, addr); break;
        case 32: m_assembler.stwcx_(tmp, zero, addr); break;
        default: m_assembler.stdcx_(tmp, zero, addr); break;
        }
        makeBranch(NotEqual).linkTo(reloop, this); // CR0[EQ] clear -> reservation lost
        m_assembler.isync();                       // acquire
        m_assembler.subf(reg, reg, tmp);           // reg = tmp - reg = old value
    }

    template<typename AddressType>
    void atomicXchg(unsigned width, RegisterID reg, AddressType address)
    {
        RegisterID addr = prepareAtomicAddress(address, memoryTempRegister);
        RegisterID tmp = dataTempRegister;
        constexpr RegisterID zero = PPC64Registers::r0;

        m_assembler.sync();
        Label reloop(this);
        switch (width) {
        case 8:  m_assembler.lbarx(tmp, zero, addr); break;
        case 16: m_assembler.lharx(tmp, zero, addr); break;
        case 32: m_assembler.lwarx(tmp, zero, addr); break;
        default: m_assembler.ldarx(tmp, zero, addr); break;
        }
        switch (width) {
        case 8:  m_assembler.stbcx_(reg, zero, addr); break;
        case 16: m_assembler.sthcx_(reg, zero, addr); break;
        case 32: m_assembler.stwcx_(reg, zero, addr); break;
        default: m_assembler.stdcx_(reg, zero, addr); break;
        }
        makeBranch(NotEqual).linkTo(reloop, this);
        m_assembler.isync();
        move(tmp, reg); // previous value
    }

    void atomicXchgAdd8(RegisterID reg, Address address)    { atomicXchgAdd(8, reg, address); }
    void atomicXchgAdd8(RegisterID reg, BaseIndex address)  { atomicXchgAdd(8, reg, address); }
    void atomicXchgAdd16(RegisterID reg, Address address)   { atomicXchgAdd(16, reg, address); }
    void atomicXchgAdd16(RegisterID reg, BaseIndex address) { atomicXchgAdd(16, reg, address); }
    void atomicXchgAdd32(RegisterID reg, Address address)   { atomicXchgAdd(32, reg, address); }
    void atomicXchgAdd32(RegisterID reg, BaseIndex address) { atomicXchgAdd(32, reg, address); }
    void atomicXchgAdd64(RegisterID reg, Address address)   { atomicXchgAdd(64, reg, address); }
    void atomicXchgAdd64(RegisterID reg, BaseIndex address) { atomicXchgAdd(64, reg, address); }

    void atomicXchg8(RegisterID reg, Address address)    { atomicXchg(8, reg, address); }
    void atomicXchg8(RegisterID reg, BaseIndex address)  { atomicXchg(8, reg, address); }
    void atomicXchg16(RegisterID reg, Address address)   { atomicXchg(16, reg, address); }
    void atomicXchg16(RegisterID reg, BaseIndex address) { atomicXchg(16, reg, address); }
    void atomicXchg32(RegisterID reg, Address address)   { atomicXchg(32, reg, address); }
    void atomicXchg32(RegisterID reg, BaseIndex address) { atomicXchg(32, reg, address); }
    void atomicXchg64(RegisterID reg, Address address)   { atomicXchg(64, reg, address); }
    void atomicXchg64(RegisterID reg, BaseIndex address) { atomicXchg(64, reg, address); }

    // --- Air LoadAcq/StoreRel (B3 Load/Store with a fence range) --------
    // Stand-ins for ARM64's ldar/stlr, which are RCsc: together they give
    // sequentially consistent loads and stores. This is the leading-sync C11
    // seq_cst mapping for Power: a full sync before every access, then an
    // lwsync after a load to make it an acquire. A store needs the sync too,
    // not just an lwsync: with only an lwsync, a store-release can still be
    // ordered after a later store-release to another location as seen from a
    // thread that stores and then load-acquires ("R" litmus:
    // T0: x=1; y=1  T1: y=2; r=x  ends with y==2 && r==0), which ldar/stlr
    // forbid.
    void loadAcq8(Address address, RegisterID dest) { m_assembler.sync(); load8(address, dest); m_assembler.lwsync(); }
    void loadAcq8SignedExtendTo32(Address address, RegisterID dest) { m_assembler.sync(); load8SignedExtendTo32(address, dest); m_assembler.lwsync(); }
    void loadAcq16(Address address, RegisterID dest) { m_assembler.sync(); load16(address, dest); m_assembler.lwsync(); }
    void loadAcq16SignedExtendTo32(Address address, RegisterID dest) { m_assembler.sync(); load16SignedExtendTo32(address, dest); m_assembler.lwsync(); }
    void loadAcq32(Address address, RegisterID dest) { m_assembler.sync(); load32(address, dest); m_assembler.lwsync(); }
    void loadAcq64(Address address, RegisterID dest) { m_assembler.sync(); load64(address, dest); m_assembler.lwsync(); }
    void storeRel8(RegisterID src, Address address) { m_assembler.sync(); store8(src, address); }
    void storeRel16(RegisterID src, Address address) { m_assembler.sync(); store16(src, address); }
    void storeRel32(RegisterID src, Address address) { m_assembler.sync(); store32(src, address); }
    void storeRel64(RegisterID src, Address address) { m_assembler.sync(); store64(src, address); }

    // --- Air LoadLink/StoreCond (B3 atomics LL/SC loops) ----------------
    // The Air forms use SimpleAddr (a bare register address, offset 0).
    // storeCond* results follow the ARM64 stxr convention the lowering
    // expects: 0 = success, nonzero = reservation lost.
    //
    // The Acq/Rel variants stand in for ARM64's ldaxr/stlxr, which are RCsc:
    // besides acquire and release, a store-release is ordered before any later
    // load-acquire. lwsync alone does not give that -- it orders every pair
    // except store -> load -- so an atomic store followed by an atomic load of
    // another location (each one an LL/SC loop here) could be reordered, which
    // breaks the seq-cst atomics wasm and JS require (store-buffering litmus).
    // So use the leading-sync C11 seq_cst mapping for Power, the one
    // branchAtomicWeakCAS uses: a full sync before the load-reserve orders it,
    // and the whole loop, after everything earlier, including a preceding
    // stcx. The lwsync after the load-reserve keeps acquire ordering for what
    // follows, and the lwsync before the store-conditional keeps storeCondRel a
    // release on its own, independent of how the reserve was taken.
    // larx/stcx. take a bare register address, so these ignore address.offset;
    // callers (Air SimpleAddr, BBQ's Address(pointer)) always pass 0.
    void loadLink8(Address address, RegisterID dest)  { ASSERT(!address.offset); m_assembler.lbarx(dest, PPC64Registers::r0, address.base); }
    void loadLink16(Address address, RegisterID dest) { ASSERT(!address.offset); m_assembler.lharx(dest, PPC64Registers::r0, address.base); }
    void loadLink32(Address address, RegisterID dest) { ASSERT(!address.offset); m_assembler.lwarx(dest, PPC64Registers::r0, address.base); }
    void loadLink64(Address address, RegisterID dest) { ASSERT(!address.offset); m_assembler.ldarx(dest, PPC64Registers::r0, address.base); }
    void loadLinkAcq8(Address address, RegisterID dest)  { m_assembler.sync(); loadLink8(address, dest); m_assembler.lwsync(); }
    void loadLinkAcq16(Address address, RegisterID dest) { m_assembler.sync(); loadLink16(address, dest); m_assembler.lwsync(); }
    void loadLinkAcq32(Address address, RegisterID dest) { m_assembler.sync(); loadLink32(address, dest); m_assembler.lwsync(); }
    void loadLinkAcq64(Address address, RegisterID dest) { m_assembler.sync(); loadLink64(address, dest); m_assembler.lwsync(); }
    void emitStoreCondResult(RegisterID result)
    {
        // CR0.EQ set by st*cx. on success; produce 0 on success, 1 on failure.
        m_assembler.mfcr(result);
        m_assembler.rlwinm(result, result, 3, 31, 31); // CR0 bit 2 (EQ) -> LSB
        m_assembler.xori(result, result, 1);
    }
    void storeCond8(RegisterID src, Address address, RegisterID result)  { ASSERT(!address.offset); m_assembler.stbcx_(src, PPC64Registers::r0, address.base); emitStoreCondResult(result); }
    void storeCond16(RegisterID src, Address address, RegisterID result) { ASSERT(!address.offset); m_assembler.sthcx_(src, PPC64Registers::r0, address.base); emitStoreCondResult(result); }
    void storeCond32(RegisterID src, Address address, RegisterID result) { ASSERT(!address.offset); m_assembler.stwcx_(src, PPC64Registers::r0, address.base); emitStoreCondResult(result); }
    void storeCond64(RegisterID src, Address address, RegisterID result) { ASSERT(!address.offset); m_assembler.stdcx_(src, PPC64Registers::r0, address.base); emitStoreCondResult(result); }
    void storeCondRel8(RegisterID src, Address address, RegisterID result)  { m_assembler.lwsync(); storeCond8(src, address, result); }
    void storeCondRel16(RegisterID src, Address address, RegisterID result) { m_assembler.lwsync(); storeCond16(src, address, result); }
    void storeCondRel32(RegisterID src, Address address, RegisterID result) { m_assembler.lwsync(); storeCond32(src, address, result); }
    void storeCondRel64(RegisterID src, Address address, RegisterID result) { m_assembler.lwsync(); storeCond64(src, address, result); }

    // Artificial value dependency (Air Depend32/64): dest = 0 with a data
    // dependency on src, used to order dependent loads. Same trick as ARM64's
    // eor x,x,x.
    void depend32(RegisterID src, RegisterID dest) { m_assembler.xor_(dest, src, src); zeroExtend32ToWordInternal(dest); }
    void depend64(RegisterID src, RegisterID dest) { m_assembler.xor_(dest, src, src); }

    // Float16 bit-pattern moves (see loadFloat16 for the in-FPR format).
    void move16ToFloat16(RegisterID src, FPRegisterID dest)
    {
        zeroExtend16To32(src, dataTempRegister);
        m_assembler.mtvsrd(dest, dataTempRegister);
    }
    void moveFloat16To16(FPRegisterID src, RegisterID dest)
    {
        m_assembler.mfvsrd(dest, src);
        zeroExtend16To32(dest, dest);
    }

    // moveDoubleConditionallyDouble — FP-conditional FP-move. Mirrors
    // moveConditionallyDouble but moves an FPR (fmr) instead of a GPR (mr).
    void moveDoubleConditionallyDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right, FPRegisterID src, FPRegisterID dest)
    {
        // dest = cond ? src : dest
        DoubleBranchBits bits = doubleBranchBitsFor(cond);
        m_assembler.fcmpu(0, left, right);
        emitDoubleConditionCRop(bits);
        emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi); // skip the move when cond is false
        m_assembler.fmr(dest, src);
    }
    void moveDoubleConditionallyDouble(DoubleCondition cond, FPRegisterID left, FPRegisterID right, FPRegisterID thenCase, FPRegisterID elseCase, FPRegisterID dest)
    {
        // dest = cond ? thenCase : elseCase, handling dest aliasing either input.
        DoubleBranchBits bits = doubleBranchBitsFor(cond);
        m_assembler.fcmpu(0, left, right);
        emitDoubleConditionCRop(bits);
        if (dest == thenCase) {
            emitSkipOneInstruction(bits.bo, bits.bi);   // skip when cond true (keep thenCase)
            m_assembler.fmr(dest, elseCase);
        } else {
            m_assembler.fmr(dest, elseCase);            // default to elseCase (no-op if dest==elseCase)
            emitSkipOneInstruction(bits.bo ^ 0x8, bits.bi); // skip when cond false
            m_assembler.fmr(dest, thenCase);
        }
    }

    // 64-bit integer add/sub on values held in FP registers (dest = left +/-
    // right on the raw bit patterns). Used by DFGSpeculativeJIT64 to NaN-box /
    // unbox doubles (add/subtract DoubleEncodeOffset). PPC FPRs can't do integer
    // arithmetic, so shuttle the bits through the assembler scratch GPRs. Both
    // operands are read before dest is written, so dest may alias left or right.
    void sub64(FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        moveDoubleTo64(left, dataTempRegister);
        moveDoubleTo64(right, memoryTempRegister);
        sub64(dataTempRegister, memoryTempRegister, dataTempRegister); // dataTemp = left - right
        move64ToDouble(dataTempRegister, dest);
    }
    void add64(FPRegisterID left, FPRegisterID right, FPRegisterID dest)
    {
        moveDoubleTo64(left, dataTempRegister);
        moveDoubleTo64(right, memoryTempRegister);
        add64(dataTempRegister, memoryTempRegister, dataTempRegister); // dataTemp = left + right
        move64ToDouble(dataTempRegister, dest);
    }

    // store32 to BaseIndex with immediate source.
    void store32(TrustedImm32 imm, BaseIndex address)
    {
        moveImmToScratch(int64_t(imm.m_value), dataTempRegister);
        store32(dataTempRegister, address);
    }
    // storeDouble to absolute pointer.
    // and32 with Address source.

    void pushPair(RegisterID src1, RegisterID src2)
    {
        m_assembler.stdu(src1, -16, stackPointerRegister);
        m_assembler.std(src2, 8, stackPointerRegister);
    }
    void popPair(RegisterID dest1, RegisterID dest2)
    {
        m_assembler.ld(dest1, 0, stackPointerRegister);
        m_assembler.ld(dest2, 8, stackPointerRegister);
        m_assembler.addi(stackPointerRegister, stackPointerRegister, 16);
    }

    // A naturally-aligned 64-bit ld is single-copy atomic on PPC64; add an
    // lwsync after it to give load-acquire ordering (matches loadAcq64 use).
    void atomicLoad64(Address address, RegisterID dest)
    {
        load64(address, dest);
        m_assembler.lwsync();
    }
    void atomicLoad64(BaseIndex address, RegisterID dest)
    {
        load64(address, dest);
        m_assembler.lwsync();
    }
    void atomicLoad64(const void* address, RegisterID dest)
    {
        load64(address, dest);
        m_assembler.lwsync();
    }

    // 64-bit shifts with memory source / 3-arg forms.
    void lshift64(Address src, RegisterID shiftAmount, RegisterID dest)
    {
        // Load the value into memoryTempRegister; lshift64(reg,reg,reg) uses
        // dataTempRegister as its shift-count scratch, so they must not alias.
        load64(src, memoryTempRegister);
        lshift64(memoryTempRegister, shiftAmount, dest);
    }

    // In-memory add: [address] += imm, then branch on the 32-bit result. The
    // store MUST be emitted before the branch. Assumes an in-range (non-scratch
    // base) Address, which is the DFG/baseline norm; memoryTempRegister is then
    // free to hold the immediate.
    Jump branchAdd32(ResultCondition cond, TrustedImm32 imm, Address address)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dataTempRegister, r.offset, r.base);
        m_assembler.extsw(dataTempRegister, dataTempRegister);
        moveImmToScratch(imm.m_value, memoryTempRegister);
        m_assembler.add(dataTempRegister, dataTempRegister, memoryTempRegister);
        m_assembler.stw(dataTempRegister, r.offset, r.base);
        if (cond == Overflow) {
            m_assembler.extsw(memoryTempRegister, dataTempRegister);
            m_assembler.cmpd(0, dataTempRegister, memoryTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        }
        m_assembler.rldicl(dataTempRegister, dataTempRegister, 0, 32);
        return branchTestImpl32(resultConditionForArith(cond), dataTempRegister);
    }
    Jump branchAdd32(ResultCondition cond, TrustedImm32 imm, AbsoluteAddress address)
    {
        moveToAbsolute(address.m_ptr);                          // memoryTemp = &address
        m_assembler.lwz(dataTempRegister, 0, memoryTempRegister);
        m_assembler.extsw(dataTempRegister, dataTempRegister);  // dataTemp = value
        moveImmToScratch(imm.m_value, memoryTempRegister);      // memoryTemp = imm
        m_assembler.add(dataTempRegister, dataTempRegister, memoryTempRegister);
        moveToAbsolute(address.m_ptr);                          // re-materialize constant addr
        m_assembler.stw(dataTempRegister, 0, memoryTempRegister);
        if (cond == Overflow) {
            m_assembler.extsw(memoryTempRegister, dataTempRegister);
            m_assembler.cmpd(0, dataTempRegister, memoryTempRegister);
            return Jump(m_assembler.emitUnlinkedBranch(4, 2));
        }
        m_assembler.rldicl(dataTempRegister, dataTempRegister, 0, 32);
        return branchTestImpl32(resultConditionForArith(cond), dataTempRegister);
    }

    // int/uint -> single-precision float. Mirrors convertInt32ToDouble but uses
    // the round-to-single convert (fcfids/fcfidus). Move the integer into the
    // FPR/VSR: mtvsrwa sign-extends a 32-bit source, mtvsrwz zero-extends it,
    // mtvsrd moves a full 64-bit source.
    void convertInt32ToFloat(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrwa(dest, src);
        m_assembler.fcfids(dest, dest);
    }
    void convertUInt32ToFloat(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrwz(dest, src);
        m_assembler.fcfidus(dest, dest);
    }
    void convertInt64ToFloat(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrd(dest, src);
        m_assembler.fcfids(dest, dest);
    }
    void convertUInt64ToFloat(RegisterID src, FPRegisterID dest)
    {
        m_assembler.mtvsrd(dest, src);
        m_assembler.fcfidus(dest, dest);
    }

    Jump branchTest8(ResultCondition cond, ExtendedAddress address, TrustedImm32 mask = TrustedImm32(-1))
    {
        // ExtendedAddress is a base register plus a (possibly large) offset.
        move(TrustedImmPtr(reinterpret_cast<void*>(address.offset)), memoryTempRegister);
        m_assembler.add(memoryTempRegister, memoryTempRegister, address.base);
        m_assembler.lbz(dataTempRegister, 0, memoryTempRegister);
        return finishNarrowTest(cond, mask, 0xFF, /*halfword*/ false);
    }

    // As on x86_64/ARM64, the base register is a uint16_t index into the table
    // at `offset`: dest = *(uint16_t*)(offset + base * 2).
    void load16(ExtendedAddress address, RegisterID dest)
    {
        move(TrustedImmPtr(reinterpret_cast<void*>(address.offset)), memoryTempRegister);
        m_assembler.sldi(dataTempRegister, address.base, 1);
        m_assembler.lhzx(dest, memoryTempRegister, dataTempRegister);
    }

    // PPC64 performs unaligned integer loads in hardware for cacheable
    // memory, so these are ordinary zero-extending loads.
    void load16Unaligned(Address address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dest, r.offset, r.base);
    }
    void load16Unaligned(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lhz(dest, r.offset, r.base);
    }
    void load32WithUnalignedHalfWords(BaseIndex address, RegisterID dest)
    {
        ResolvedAddress r = resolveAddress(address, memoryTempRegister);
        m_assembler.lwz(dest, r.offset, r.base);
    }

    // dest = (src >> lsb) & ((1 << width) - 1).
    void extractUnsignedBitfield32(RegisterID src, TrustedImm32 lsb, TrustedImm32 width, RegisterID dest)
    {
        int l = lsb.m_value & 31, w = width.m_value;
        m_assembler.rlwinm(dest, src, (32 - l) & 31, 32 - w, 31);
    }
    void extractUnsignedBitfield64(RegisterID src, TrustedImm32 lsb, TrustedImm32 width, RegisterID dest)
    {
        int l = lsb.m_value & 63, w = width.m_value;
        m_assembler.rldicl(dest, src, (64 - l) & 63, 64 - w);
    }

    DataLabelPtr storePtrWithPatch(TrustedImmPtr initialValue, Address address)
    {
        DataLabelPtr label = moveWithPatch(initialValue, dataTempRegister);
        store64(dataTempRegister, address);
        return label;
    }
    DataLabelPtr storePtrWithPatch(Address address)
    {
        return storePtrWithPatch(TrustedImmPtr(nullptr), address);
    }

    template<PtrTag startTag, PtrTag destTag>
    static void replaceWithJump(CodeLocationLabel<startTag> instructionStart, CodeLocationLabel<destTag> destination)
    {
        PPC64Assembler::replaceWithJump(instructionStart.dataLocation(), destination.dataLocation());
    }
    template<PtrTag tag>
    static void replaceWithVMHalt(CodeLocationLabel<tag> instructionStart)
    {
        PPC64Assembler::replaceWithVMHalt(instructionStart.dataLocation());
    }
    template<PtrTag startTag>
    static void replaceWithNops(CodeLocationLabel<startTag> instructionStart, size_t memoryToFillWithNopsInBytes)
    {
        PPC64Assembler::fillNops(instructionStart.dataLocation(), memoryToFillWithNopsInBytes);
        PPC64Assembler::cacheFlush(instructionStart.dataLocation(), memoryToFillWithNopsInBytes);
    }
    template<PtrTag callTag, PtrTag destTag>
    static void repatchCall(CodeLocationCall<callTag> call, CodeLocationLabel<destTag> destination)
    {
        PPC64Assembler::relinkCCall(call.dataLocation(), destination.taggedPtr());
    }
    template<PtrTag callTag, PtrTag destTag>
    static void repatchCall(CodeLocationCall<callTag> call, CodePtr<destTag> destination)
    {
        PPC64Assembler::relinkCCall(call.dataLocation(), destination.taggedPtr());
    }
    template<PtrTag resultTag, PtrTag locationTag>
    static CodePtr<resultTag> readCallTarget(CodeLocationCall<locationTag> call)
    {
        return CodePtr<resultTag>(PPC64Assembler::readCallTarget(call.dataLocation()));
    }

    friend class LinkBuffer;
    template<PtrTag tag>
    static void linkCall(void* code, Call call, CodePtr<tag> function)
    {
        // Every call() / nearCall() emits the same 8-insn emitUnlinkedCall slot;
        // only a tail call is a jump. call() keeps r12 = target (C callees need
        // it, and readCallTarget reads it back) via applyCallSlot. nearCall()
        // targets JIT code and gets the r12-less form that can be retargeted
        // with a single bl write (applyNearCallSlot). Never link a call slot
        // with linkPointer: that writes a bctrl-less li64 (fall-through to pc=0).
        if (call.isFlagSet(Call::Tail))
            PPC64Assembler::linkJump(code, call.m_label, function.untaggedPtr());
        else if (call.isFlagSet(Call::Near))
            PPC64Assembler::linkNearCall(code, call.m_label, function.untaggedPtr());
        else
            PPC64Assembler::linkCall(code, call.m_label, function.untaggedPtr());
    }
};

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && CPU(PPC64)
