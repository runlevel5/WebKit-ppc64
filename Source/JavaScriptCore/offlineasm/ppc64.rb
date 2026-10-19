# Copyright (C) 2026 Trung Lê and contributors. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.

# PPC64 (ELFv2) register conventions
#
# GPR assignments:
#   r0   => scratch (holds LR save during function prologue; never a named offlineasm reg)
#   r1   => sp
#   r2   => TOC pointer (reserved by ABI, not used by JSC)
#   r3   => t0, a0, wa0, r0 (return / 1st arg)
#   r4   => t1, a1, wa1, r1 (2nd arg)
#   r5   => t2, a2, wa2
#   r6   => t3, a3, wa3
#   r7   => t4, a4, wa4
#   r8   => t5, a5, wa5
#   r9   => t6, a6, wa6
#   r10  => t7, a7, wa7
#   r11  => ws0, t9  (volatile scratch; t9=ws0 per LLInt const ws0 = t9)
#   r12  => ws1, t10 (volatile scratch; t10=ws1 per LLInt const ws1 = t10)
#   r13  => TLS pointer (reserved)
#   r14  => csr0
#   r15  => csr1
#   r16  => csr2
#   r17  => csr3
#   r18  => csr4
#   r19  => csr5
#   r20  => csr6  (metadataTable)
#   r21  => csr7  (PB / jitData)
#   r22  => csr8  (numberTag)
#   r23  => csr9  (notCellMask)
#   r24  => csr10
#   r25  => ws2, t11 (callee-saved; preserved by C; t11=ws2 per LLInt const ws2 = t11)
#   r26  => ws3, t12 (callee-saved; preserved by C; t12=ws3 per LLInt const ws3 = t12)
#   r31  => cfr   (frame pointer, callee-saved)
#
# FPR assignments (ELFv2: f0 volatile scratch; f1-f13 volatile, f1-f8 carry
# FP arguments and f1 the FP result; f14-f31 callee-save):
#   f1   => ft0, fa0, wfa0, fr
#   f2   => ft1, fa1, wfa1
#   f3   => ft2, fa2, wfa2
#   f4   => ft3, fa3, wfa3
#   f5   => ft4,      wfa4
#   f6   => ft5,      wfa5
#   f7   => ft6,      wfa6
#   f8   => ft7,      wfa7
# fa*/wfa*/fr are not register names in offlineasm: LowLevelInterpreter.asm
# defines them as consts aliasing ft0-ft7 (the generic `else` arm of its
# register-convention chain, which PPC64 takes), and consts are substituted
# before lowering.  So the argument registers are whatever ft0-ft7 lower to,
# and ft0-ft7 must be f1-f8 for ELFv2 and for JSC's own PPC64 convention
# (FPRInfo.h: argumentFPR0-7 = f1-f8, returnValueFPR = f1, fpRegT0-7 =
# f1-f8).  This is the arm64 layout (q0-q7 => ft0-ft7 = fa0-fa7, fr = q0)
# and satisfies the contract in LowLevelInterpreter.asm: ft0-ft5 pairwise
# distinct, ft1-ft5 never fr, ftX never faY for X != Y.
#   f14  => csfr0
#   f15  => csfr1
#   f16  => csfr2
#   f17  => csfr3
#   f18  => csfr4
#   f19  => csfr5
#   f20  => csfr6
#   f21  => csfr7

# Extra scratch GPRs available for Tmp allocation (r25-r30 are callee-save but
# not otherwise named in offlineasm; r0 is volatile ABI scratch).
PPC64_EXTRA_GPRS = [
    SpecialRegister.new("r27"),
    SpecialRegister.new("r28"),
    SpecialRegister.new("r29"),
    SpecialRegister.new("r30"),
]
# Extra scratch FPRs for Tmp allocation.  These were f24-f27, which ELFv2
# makes callee-saved and which nothing saves on the way into JS (vmEntry's
# pushCalleeSaves covers r24-r30, and JSC's FP callee saves are f14-f21), so
# every FP temporary -- e.g. the one ppc64_truncd2i needs -- clobbered a
# C caller's f24.  f9-f12 are volatile and, with ft0-ft7 on f1-f8, named by
# nothing else.  They are ELFv2 argument registers 9-12, but a Tmp lives only
# inside the expansion of one offlineasm instruction, never across a call.
PPC64_EXTRA_FPRS = [
    SpecialRegister.new("f9"),
    SpecialRegister.new("f10"),
    SpecialRegister.new("f11"),
    SpecialRegister.new("f12"),
]

# -------------------------------------------------------------------------
# PtrTag-based call classification.
#
# Offlineasm resolves `constexpr FooPtrTag` to a bare Immediate before the
# backend lowering runs, so the lowering cannot see tag NAMES.  Capture the
# name -> value mapping as ConstExpr nodes are resolved (transform.rb
# Node#resolve -> ConstExpr#resolveOffsets) and classify call sites
# symbolically.  Never recompute WTF's makePtrTagHash here: a value that
# matches no real tag would classify a C call as JS and skip the r2 (TOC)
# restore it needs.
#
# JS-entry tags: the callee is LLInt/JIT code following the JSC calling
# convention -- the caller has already arranged sp for the callee's frame,
# the callee never runs an ELFv2 GEP, never touches r2, and stores the
# return PC itself.  Emit a bare mtctr/bctrl; any extra stack adjustment
# here would shift the callee's cfr and corrupt the frame header slots.
#
# Everything else (HostFunctionPtrTag, CustomAccessorPtrTag, and the
# untagged `call reg` sites from the cCall*/slow-path macros) is a C call
# under the full ELFv2 ABI: the callee may run its GEP off r12 and
# overwrite r2 (host functions live in the jsc BINARY -- a different
# module with a different TOC), and writes its saved lr at 16(caller_sp).
# Emit a 32-byte linkage frame, set r12, and restore r2 after return.
# -------------------------------------------------------------------------
$ppc64ConstExprValues = {}
module PPC64ConstExprCapture
    def resolveOffsets(constantsMap)
        result = super
        $ppc64ConstExprValues[@value] = result.value if result.is_a?(Immediate)
        result
    end
end
class ConstExpr
    prepend PPC64ConstExprCapture
end

PPC64_JS_ENTRY_TAG_NAMES = %w[
    JSEntryPtrTag JSEntrySlowPathPtrTag WasmEntryPtrTag LLIntToWasmEntryPtrTag
    wasmIPIntTailCallWasmEntryPtrTag
].freeze

PPC64_C_CALL_TAG_NAMES = %w[
    HostFunctionPtrTag CustomAccessorPtrTag OperationPtrTag NoPtrTag
].freeze

# Returns [:js, tagName] or [:c, tagName-or-nil]; raises on a tag value that
# matches neither list so a new tag fails at offlineasm time instead of
# miscompiling to the wrong calling convention.
def ppc64ClassifyCallTag(tagValue, whereString)
    return [:c, nil] unless tagValue
    jsName = PPC64_JS_ENTRY_TAG_NAMES.find { |n| $ppc64ConstExprValues[n] == tagValue }
    cName  = PPC64_C_CALL_TAG_NAMES.find { |n| $ppc64ConstExprValues[n] == tagValue }
    if jsName and cName
        raise "ppc64: PtrTag hash collision between #{jsName} and #{cName} (#{tagValue}) at #{whereString}"
    end
    return [:js, jsName] if jsName
    return [:c, cName] if cName
    known = $ppc64ConstExprValues.select { |_, v| v == tagValue }.keys
    raise "ppc64: call with unclassified PtrTag value #{tagValue} (#{known.empty? ? 'no name captured' : known.join('/')}) at #{whereString}"
end

# -------------------------------------------------------------------------
# RegisterID operand → PPC64 register number (bare numeral).
# PPC GAS does not accept "rN" register names by default (without -mregnames),
# so we emit bare numerals like "1" for r1, matching GCC's convention.
# -------------------------------------------------------------------------
class RegisterID
    def ppc64Operand
        case @name
        when "t0", "a0", "wa0", "r0"
            "3"
        when "t1", "a1", "wa1", "r1"
            "4"
        when "t2", "a2", "wa2"
            "5"
        when "t3", "a3", "wa3"
            "6"
        when "t4", "a4", "wa4"
            "7"
        when "t5", "a5", "wa5"
            "8"
        when "t6", "a6", "wa6"
            "9"
        when "t7", "a7", "wa7"
            "10"
        when "ws0", "t9"     # t9 = ws0 per LowLevelInterpreter.asm const ws0 = t9
            "11"
        when "ws1", "t10"    # t10 = ws1 per LowLevelInterpreter.asm const ws1 = t10
            "12"
        when "ws2", "t11"    # t11 = ws2; callee-saved, C preserves it across calls
            "25"
        when "ws3", "t12"    # t12 = ws3; callee-saved, C preserves it across calls
            "26"
        when "csr0"
            "14"
        when "csr1"
            "15"
        when "csr2"
            "16"
        when "csr3"
            "17"
        when "csr4"
            "18"
        when "csr5"
            "19"
        when "csr6"
            "20"
        when "csr7"
            "21"
        when "csr8"
            "22"
        when "csr9"
            "23"
        when "csr10"
            "24"
        when "cfr"
            "31"
        when "sp"
            "1"
        when "lr"
            # lr is a Special Purpose Register on PPC64 — cannot be named as a
            # plain operand in arithmetic instructions.  Callers that know lr is
            # the operand must emit mflr/mtlr manually.
            raise "ppc64: lr is a SPR; handle with mflr/mtlr at call site (#{codeOriginString})"
        else
            raise "ppc64: unknown GPR name '#{@name}' at #{codeOriginString}"
        end
    end
end

# -------------------------------------------------------------------------
# FPRegisterID operand → PPC64 FP register name string
# -------------------------------------------------------------------------
class FPRegisterID
    def ppc64Operand
        case @name
        # See the FPR table at the top of the file: ft0-ft7 are also
        # fa0-fa3/wfa0-wfa7/fr (via LowLevelInterpreter.asm consts), so they
        # must be the ELFv2 FP argument registers f1-f8.
        when "ft0"
            "1"
        when "ft1"
            "2"
        when "ft2"
            "3"
        when "ft3"
            "4"
        when "ft4"
            "5"
        when "ft5"
            "6"
        when "ft6"
            "7"
        when "ft7"
            "8"
        when "csfr0"
            "14"
        when "csfr1"
            "15"
        when "csfr2"
            "16"
        when "csfr3"
            "17"
        when "csfr4"
            "18"
        when "csfr5"
            "19"
        when "csfr6"
            "20"
        when "csfr7"
            "21"
        else
            raise "ppc64: unknown FPR name '#{@name}' at #{codeOriginString}"
        end
    end
end

# -------------------------------------------------------------------------
# VecRegisterID operand → VSX register number (VSR 0-63, bare numeral).
#
# offlineasm's v0..v7 (and their _b/_h/_i/_q lane-view aliases, which name
# the same register) map onto the VMX half of the VSX file, VSR 32-63 =
# VMX v0-v31, never onto VSR 0-31, which alias the FPRs.  So a vector
# transport can never clobber an ft/fa/csfr register.  IPInt cannot depend
# on the two classes aliasing or not aliasing: x86 maps v0 to xmm0 (= ft0)
# while arm64 maps v0 to v16 (disjoint from ft0 = d0), so any code relying
# on either would already be broken on one of them.
#
# ELFv2: VMX v0-v19 are volatile, v20-v31 non-volatile, and
# v2-v13 carry vector arguments/results.  Like arm64 (v16-v23, outside its
# argument registers v0-v7), the mapping stays inside the volatile,
# non-argument set: VMX v0, v1 (scratch) and v14-v19.
#   v0 → VSR 32 (VMX v0)    v1 → VSR 33 (VMX v1)
#   v2..v7 → VSR 46..51 (VMX v14..v19)
# The register is the same for every lane view; lane width only matters
# to arithmetic, which this backend does not lower (see pushv/loadv).
# -------------------------------------------------------------------------
class VecRegisterID
    def ppc64Operand
        base = @name.sub(/_[bhiq]\z/, "")
        case base
        when "v0" then "32"
        when "v1" then "33"
        when "v2" then "46"
        when "v3" then "47"
        when "v4" then "48"
        when "v5" then "49"
        when "v6" then "50"
        when "v7" then "51"
        else
            raise "ppc64: unknown vector register name '#{@name}' at #{codeOriginString}"
        end
    end
end

# SpecialRegister (e.g. PPC64_EXTRA_GPRS entries with names like "r27").
# Strip the leading "r"/"f" so we emit bare numerals (GAS convention).
class SpecialRegister
    def ppc64Operand
        @name.sub(/^[rf]/, "")
    end
end

# -------------------------------------------------------------------------
# Immediate — validate range and emit decimal value
# -------------------------------------------------------------------------

# DS-form instructions (ld, std, lwa, stdu, …) require the displacement
# to be a 16-bit signed multiple of 4.
def ppc64ValidateDSOffset(value, origin)
    unless (-32768..32764).include?(value) && (value % 4 == 0)
        raise "ppc64: DS-form offset #{value} not in [-32768..32764] aligned to 4 at #{origin}"
    end
end

# D-form instructions (lwz, stw, lbz, stb, …) require a 16-bit signed displacement.
def ppc64ValidateDOffset(value, origin)
    unless (-32768..32767).include?(value)
        raise "ppc64: D-form offset #{value} not in [-32768..32767] at #{origin}"
    end
end

class Immediate
    def ppc64Operand
        "#{value}"
    end

    # True if the value fits in a 16-bit signed immediate (for addi/li/etc.)
    def ppc6416BitSignedImmediate?
        (-32768..32767).include?(value)
    end

    # True if the value fits in a 16-bit unsigned immediate (for ori/andi./etc.)
    def ppc6416BitUnsignedImmediate?
        (0..65535).include?(value)
    end
end

# -------------------------------------------------------------------------
# Address operand → "offset(base)" string
# DS-form check (for ld/std) is deferred to the instruction emitter.
# -------------------------------------------------------------------------
class Address
    def ppc64Operand
        # base.ppc64Operand returns a bare numeral; emit GAS-syntax "OFFSET(BASE)".
        "#{offset.value}(#{base.ppc64Operand})"
    end
end

class BaseIndex
    def ppc64Operand
        raise "ppc64: BaseIndex addressing not directly supported; must be lowered"
    end
end

# The load-reserve/store-conditional primitives (l{b,h,w,d}arx, st{b,h,w,d}cx.)
# are X-form indexed only: there is no displacement field.  Accept `[reg]`
# (an Address with a zero offset) or a bare register and return the base
# register operand; anything else has no encoding on PPC.
def ppc64ReservationAddress(operand, opcode, origin)
    if operand.is_a?(Address)
        unless operand.offset.value.zero?
            raise "ppc64: #{opcode} needs a zero displacement (l*arx/st*cx. are indexed-only), got #{operand.offset.value} at #{origin}"
        end
        operand.base.ppc64Operand
    elsif operand.is_a?(RegisterID) || operand.is_a?(SpecialRegister)
        operand.ppc64Operand
    else
        raise "ppc64: #{opcode} requires a register address operand at #{origin}"
    end
end

# -------------------------------------------------------------------------
# Preprocessing pass — normalise forms that PPC64 cannot encode directly.
# -------------------------------------------------------------------------
def ppc64LowerMalformedAddresses(list)
    # Lower BaseIndex addressing modes that PPC64 cannot encode directly.
    # Pattern: base + index * (1 << scaleShift) + offset
    # Emitted as: tmp = index << scaleShift; tmp += offset; tmp += base; use Address(tmp,0)
    #
    # Also lower jmp/call with Address operand:
    #   jmp [base+offset]  →  loadp [base+offset], tmp; jmp tmp
    #   call [base+offset] →  loadp [base+offset], tmp; call tmp
    newList = []
    list.each { |node|
        next (newList << node) unless node.is_a?(Instruction)

        ops = node.operands

        # jmp/call with an Address target — load the pointer into a Tmp first.
        # Preserve any extra operands (e.g. PtrTag) so the lowering can still
        # distinguish C calls from JS calls based on HostFunctionPtrTag.
        if (node.opcode == "jmp" || node.opcode == "call") && ops[0].is_a?(Address)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "loadp", [ops[0], tmp])
            newList << Instruction.new(co, node.opcode, [tmp] + ops[1..], node.annotation)
            next
        end

        # orh src, Address — read-modify-write: load halfword, OR, store back
        # PPC has no memory-OR instruction; expand to loadh / ori / storeh with a Tmp.
        if node.opcode == "orh" && ops[1].is_a?(Address)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "loadh",  [ops[1], tmp])
            newList << Instruction.new(co, "ori",    [ops[0], tmp])
            newList << Instruction.new(co, "storeh", [tmp, ops[1]])
            next
        end

        # Compare-and-branch with an Address as first operand:
        # riscLowerTest expands btpz addr, label → bpeq addr, 0, label.
        # PPC cmp* requires a register; load from the address into a Tmp first.
        if node.opcode =~ /\Ab[ipqb]/ && ops[0].is_a?(Address)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            load_op = case node.opcode[1]
                      when "p" then "loadp"
                      when "q" then "loadq"
                      when "i" then "loadi"
                      when "b" then "loadb"
                      else          "loadp"
                      end
            newList << Instruction.new(co, load_op, [ops[0], tmp])
            newList << Instruction.new(co, node.opcode, [tmp] + ops[1..], node.annotation)
            next
        end

        # Compare-and-branch with an Address as second operand (3-operand form):
        # e.g. bpbeq reg, addr, label.  cmp/cmpl/cmpd/cmpld take two GPRs,
        # so load the memory operand into a Tmp first.
        # The /\Ab[ipqb]/ regex excludes badd*/bsub* (read-modify-write), bt* (test-and-branch),
        # and bd*/bf* (FP), so this only fires on integer compare-and-branch.
        if node.opcode =~ /\Ab[ipqb]/ && ops.length >= 3 && ops[1].is_a?(Address)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            load_op = case node.opcode[1]
                      when "p" then "loadp"
                      when "q" then "loadq"
                      when "i" then "loadi"
                      when "b" then "loadb"
                      else          "loadp"
                      end
            newList << Instruction.new(co, load_op, [ops[1], tmp])
            newList << Instruction.new(co, node.opcode, [ops[0], tmp] + ops[2..], node.annotation)
            next
        end

        # truncated2is / cd2i need an FPR scratch: fctiwz must not clobber
        # the source FPR (callers keep using the double), and the int32
        # result moves out with mfvsrwz.  Expand to a pseudo carrying a
        # fresh FPR Tmp.
        if node.opcode == "truncated2is" || node.opcode == "cd2i"
            co   = node.codeOrigin
            ftmp = Tmp.new(co, :fpr)
            newList << Instruction.new(co, "ppc64_truncd2i", [ops[0], ops[1], ftmp], node.annotation)
            next
        end

        # transferp/transferq src_addr, dst_addr — memory-to-memory pointer copy via Tmp.
        if node.opcode == "transferp" || node.opcode == "transferq"
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "loadp",  [ops[0], tmp])
            newList << Instruction.new(co, "storep", [tmp, ops[1]])
            next
        end

        # baddis imm, addr, label — read-modify-write counter then branch if (signed)
        # negative.  PPC has no fused arith-and-branch on memory; decompose into:
        #   loadi  addr, tmp
        #   addi   imm,  tmp        ; plain add, no flags
        #   storei tmp,  addr
        #   bilt   tmp, 0, label    ; cmpwi+blt (signed 32-bit < 0)
        # Only baddis (32-bit signed) is used by LLInt; the pointer/quad and
        # bsub variants are not implemented.
        if node.opcode == "baddis" && ops[1].is_a?(Address)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "loadi",  [ops[1], tmp])
            newList << Instruction.new(co, "addi",   [ops[0], tmp])
            newList << Instruction.new(co, "storei", [tmp, ops[1]])
            newList << Instruction.new(co, "bilt",   [tmp, Immediate.new(co, 0), ops[2]], node.annotation)
            next
        end

        # load* with lr as destination: PPC LR is an SPR — must use mtlr.
        # Expand: load addr, lr  →  load addr, tmp; move tmp, lr
        if node.opcode =~ /\Aload/ && ops[-1].is_a?(RegisterID) && ops[-1].name == "lr"
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, node.opcode, ops[0..-2] + [tmp], node.annotation)
            newList << Instruction.new(co, "move", [tmp, ops[-1]])
            next
        end

        # store* with lr as source: must use mflr first.
        # Expand: store lr, addr  →  move lr, tmp; store tmp, addr
        if node.opcode =~ /\Astore/ && ops[0].is_a?(RegisterID) && ops[0].name == "lr"
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "move", [ops[0], tmp])
            newList << Instruction.new(co, node.opcode, [tmp] + ops[1..], node.annotation)
            next
        end

        # loadv/storev: lxvd2x/stxvd2x are X-form (RA|0 + RB) with no
        # displacement field.  Fold any non-zero displacement into a Tmp
        # (the BaseIndex path below already produces Address(tmp, 0)), so
        # the emitter only ever sees Address(base, 0).
        if (node.opcode == "loadv" || node.opcode == "storev")
            addr_pos = (node.opcode == "loadv") ? 0 : 1
            addr = ops[addr_pos]
            if addr.is_a?(Address) && addr.offset.value != 0
                co  = node.codeOrigin
                tmp = Tmp.new(co, :gpr)
                # leap is a single addi for a 16-bit displacement (the
                # common case: local and stack slots) and handles larger
                # ones itself.
                newList << Instruction.new(co, "leap", [addr, tmp])
                new_ops = ops.dup
                new_ops[addr_pos] = Address.new(co, tmp, Immediate.new(co, 0))
                newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
                next
            end
        end

        # DS-form opcodes (ld/lwa/std) require 4-byte-aligned offsets.
        # Also catch any offset outside 16-bit signed range.
        # Split: ppc64_li64 offset, tmp; addp base, tmp; Address(tmp, 0)
        large_addr_pos = ops.each_with_index.find { |op, _|
            next false unless op.is_a?(Address) && op.offset.is_a?(Immediate)
            v = op.offset.value
            ds_form = %w[loadp loadq loadis storep storeq].include?(node.opcode)
            (v < -32768 || v > 32767) || (ds_form && (v % 4) != 0)
        }&.last

        if large_addr_pos
            addr = ops[large_addr_pos]
            co   = node.codeOrigin
            tmp  = Tmp.new(co, :gpr)
            # ppc64_li64 handles any 64-bit value; addp adds the base
            newList << Instruction.new(co, "ppc64_li64", [addr.offset, tmp])
            newList << Instruction.new(co, "addp", [addr.base, tmp])
            new_addr = Address.new(co, tmp, Immediate.new(co, 0))
            new_ops  = ops.dup
            new_ops[large_addr_pos] = new_addr
            newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
            next
        end

        # Detect which operand position holds a BaseIndex, if any.
        bi_pos = ops.each_with_index.find { |op, _| op.is_a?(BaseIndex) }&.last

        unless bi_pos
            newList << node
            next
        end

        bi  = ops[bi_pos]
        co  = node.codeOrigin
        tmp = Tmp.new(co, :gpr)

        # tmp = index
        newList << Instruction.new(co, "movep", [bi.index, tmp])
        # tmp = tmp << scaleShift
        if bi.scaleShift > 0
            newList << Instruction.new(co, "lshiftq",
                [Immediate.new(co, bi.scaleShift), tmp])
        end
        # tmp += const offset
        if bi.offset.value != 0
            newList << Instruction.new(co, "addp", [bi.offset, tmp])
        end
        # tmp += base
        newList << Instruction.new(co, "addp", [bi.base, tmp])

        # Replace the BaseIndex with Address(tmp, 0)
        new_addr = Address.new(co, tmp, Immediate.new(co, 0))
        new_ops  = ops.dup
        new_ops[bi_pos] = new_addr

        newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
    }
    newList
end

# -------------------------------------------------------------------------
# Compare-and-branch / compare-and-set with an IMMEDIATE first operand.
#
# offlineasm allows `bineq 0, t0, label` (IPInt's `if` handler is written
# exactly so), but PPC's cmp*i forms only take the immediate second, and
# ppc64EmitCompareAndBranch/ppc64EmitConditionalSet print ops[0] as a
# register: the 0 above would come out as `cmpw 0, 3`, a silent compare
# against r0.  Materialise such an operand into a Tmp first.
# Only real comparisons qualify: badd*/bsub*/bmul* (arith-and-branch, where
# an immediate first operand is the addend), bt* (bit tests), bo (overflow)
# and the FP bf*/bd* forms are left alone.
# -------------------------------------------------------------------------
PPC64_INT_COMPARE_SUFFIXES = %w[eq neq lt gt lteq gteq le ge b a be ae beq aeq ult ugt ule uge].freeze

def ppc64IsIntCompare(opcode)
    return false unless opcode =~ /\A([bc])([ipqb])([a-z]+)\z/
    kind, suffix = $1, $3
    return false if kind == "b" && opcode =~ /\Ab(add|sub|mul|t|o)/
    PPC64_INT_COMPARE_SUFFIXES.include?(suffix)
end

def ppc64LowerImmediateFirstCompares(list)
    newList = []
    list.each { |node|
        if node.is_a?(Instruction) && ppc64IsIntCompare(node.opcode) &&
           node.operands.length == 3 && node.operands[0].is_a?(Immediate)
            co  = node.codeOrigin
            tmp = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "move", [node.operands[0], tmp])
            newList << Instruction.new(co, node.opcode, [tmp] + node.operands[1..], node.annotation)
        else
            newList << node
        end
    }
    newList
end

def ppc64LowerLargeImmediates(list)
    # Convert any Immediate that doesn't fit in 16 bits into a pre-load via
    # a Tmp (scratch) register.  Only arithmetic "move imm, dest" paths need
    # this; load/store offsets are handled separately.
    newList = []
    list.each { |node|
        if node.is_a?(Instruction)
            case node.opcode
            when /^(move|movei|moveq|movep)$/
                src = node.operands[0]
                dst = node.operands[1]
                if src.is_a?(Immediate) && !src.ppc6416BitSignedImmediate?
                    # Large immediate: use a ppc64_li64 pseudo that the
                    # lowering stage expands to lis/ori/rldicr/oris/ori.
                    newList << Instruction.new(node.codeOrigin, "ppc64_li64", node.operands)
                else
                    newList << node
                end
            when /^and[ipq]$/, /^or[ipq]$/, /^xor[ipq]$/
                # PPC andi./ori/xori take 16-bit UNSIGNED immediates.  If the immediate
                # doesn't fit, materialize it via ppc64_li64 into a Tmp and rewrite
                # to use the register-form op.
                ops = node.operands
                imm_idx = ops.index { |o| o.is_a?(Immediate) }
                if imm_idx && !ops[imm_idx].ppc6416BitUnsignedImmediate?
                    co  = node.codeOrigin
                    tmp = Tmp.new(co, :gpr)
                    newList << Instruction.new(co, "ppc64_li64", [ops[imm_idx], tmp])
                    new_ops = ops.dup
                    new_ops[imm_idx] = tmp
                    newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
                else
                    newList << node
                end
            when /^add[ipq]$/
                # PPC addi RT,RA,SI takes a 16-bit SIGNED immediate.
                # Materialize larger constants via Tmp and use register-form add.
                ops = node.operands
                imm_idx = ops.index { |o| o.is_a?(Immediate) }
                if imm_idx && !ops[imm_idx].ppc6416BitSignedImmediate?
                    co  = node.codeOrigin
                    tmp = Tmp.new(co, :gpr)
                    newList << Instruction.new(co, "ppc64_li64", [ops[imm_idx], tmp])
                    new_ops = ops.dup
                    new_ops[imm_idx] = tmp
                    newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
                else
                    newList << node
                end
            when /^store[bhipq]$/
                # PPC st{b,h,w,d} require a GPR source.  An Immediate operand[0]
                # is invalid encoding; materialize via Tmp.
                ops = node.operands
                if ops[0].is_a?(Immediate)
                    co  = node.codeOrigin
                    tmp = Tmp.new(co, :gpr)
                    newList << Instruction.new(co, "ppc64_li64", [ops[0], tmp])
                    newList << Instruction.new(co, node.opcode, [tmp, ops[1]], node.annotation)
                else
                    newList << node
                end
            when /^sub[ipq]$/
                # subi imm, dest is lowered to addi dest, dest, -imm.  Materialize when
                # NEGATED value doesn't fit in 16-bit signed (e.g. imm = 0x40000000).
                ops = node.operands
                imm_idx = ops.index { |o| o.is_a?(Immediate) }
                if imm_idx
                    neg = -ops[imm_idx].value
                    if neg < -32768 || neg > 32767
                        co  = node.codeOrigin
                        tmp = Tmp.new(co, :gpr)
                        newList << Instruction.new(co, "ppc64_li64", [ops[imm_idx], tmp])
                        new_ops = ops.dup
                        new_ops[imm_idx] = tmp
                        newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
                        next
                    end
                end
                newList << node
            when /^b[ipqb][a-z]+\z/
                # Compare-and-branch (3-operand form) lowering uses cmpdi/cmpwi which take
                # 16-bit signed immediates, or cmpldi/cmplwi which take 16-bit unsigned.
                # If ops[1] is an out-of-range Immediate, materialize and let the
                # lowering pick the register-form compare.
                # Skip 2-operand bt*z forms (only one register operand).
                ops = node.operands
                if ops.length == 3 && ops[1].is_a?(Immediate)
                    imm = ops[1]
                    # Determine signedness by mnemonic suffix.  Unsigned variants:
                    # bub, bua, bube, buae, bib, bia, bibe, biaeq, bpb, bpa, bpbe, bpbeq, bpaeq,
                    # bbb, bba, bbbeq, bbaeq, bqb, bqa, bqbe, bqbeq, bquge, ...
                    # Heuristic: opcode contains "b" or "a" (above/below) → unsigned.
                    unsigned = node.opcode =~ /b(b|a)e?q?\z/ || node.opcode =~ /\bbu/
                    fits = unsigned ? imm.ppc6416BitUnsignedImmediate? : imm.ppc6416BitSignedImmediate?
                    unless fits
                        co  = node.codeOrigin
                        tmp = Tmp.new(co, :gpr)
                        newList << Instruction.new(co, "ppc64_li64", [imm, tmp])
                        new_ops = ops.dup
                        new_ops[1] = tmp
                        newList << Instruction.new(co, node.opcode, new_ops, node.annotation)
                        next
                    end
                end
                newList << node
            else
                newList << node
            end
        else
            newList << node
        end
    }
    newList
end

# -------------------------------------------------------------------------
# Overflow / sign branch lowering (badd*o, bsub*o, badd*s, bsub*s).
#
# PPC64 has no equivalent of x86 "add; jo/js", and the XER route is a trap:
# the dot form (addo.) copies XER[SO] — the STICKY summary-overflow bit —
# into CR0, not this instruction's OV, so one earlier overflow anywhere
# makes every later bso fire; addic. never touches OV at all; and addo/
# subfo detect 64-bit overflow, which two int32 operands can never produce,
# so int32 overflow would go undetected.  We avoid XER entirely:
#
#   int32 overflow (baddio/bsubio): do the arithmetic exactly in 64 bits on
#   sign-extended copies; overflow iff the 64-bit result differs from its
#   own low-word sign extension (extsw + cmpd).
#
#   int64 overflow (badd{p,q}o/bsub{p,q}o): sign algebra — for r = a+b,
#   overflow iff (a^r) & (b^r) < 0; for r = a-b, iff (a^b) & (a^r) < 0.
#
#   sign branches (b*s): plain add/sub, then branch if result negative
#   (32-bit: bilt → cmpwi tests the low word; 64-bit: bqlt → cmpdi).
#
# Everything is emitted as generic offlineasm ops on fresh Tmps so the
# register assigner picks provably-dead registers (same pattern as the
# jmp/call Address expansion — never hardcode a scratch in lowering).
# Address-destination forms (baddis imm, addr, lbl) are left untouched for
# the rmw expansion in ppc64LowerMalformedAddresses.  The residual direct
# lowerings raise, so an unhandled operand shape fails loudly at offlineasm
# time instead of miscompiling.
# -------------------------------------------------------------------------
def ppc64LowerOverflowBranches(list)
    newList = []
    list.each {
        | node |
        unless node.is_a?(Instruction)
            newList << node
            next
        end
        ops = node.operands
        co  = node.codeOrigin
        case node.opcode
        when "baddio", "bsubio"
            unless ops[1].is_a?(RegisterID)
                newList << node
                next
            end
            arith = (node.opcode == "baddio") ? "addq" : "subq"
            wide = Tmp.new(co, :gpr)   # exact 64-bit result
            sext = Tmp.new(co, :gpr)   # sign-extended src / re-extension of result
            newList << Instruction.new(co, "sxi2q", [ops[1], wide], node.annotation)
            if ops[0].is_a?(Immediate)
                newList << Instruction.new(co, arith, [ops[0], wide])
            else
                newList << Instruction.new(co, "sxi2q", [ops[0], sext])
                newList << Instruction.new(co, arith, [sext, wide])
            end
            # x86 addl / arm64 "adds Wn" zero-extend the 32-bit result, and
            # LLInt relies on that (binaryOp re-boxes with a plain
            # "orq numberTag"): a sign-extended negative result would leave
            # 0xFFFFFFFF in the upper word and malform the JSValue box.
            newList << Instruction.new(co, "zxi2q", [wide, ops[1]])
            newList << Instruction.new(co, "sxi2q", [wide, sext])
            newList << Instruction.new(co, "bqneq", [wide, sext, ops[2]])
        when "baddpo", "baddqo", "bsubpo", "bsubqo"
            unless ops[1].is_a?(RegisterID)
                newList << node
                next
            end
            isAdd = node.opcode.start_with?("badd")
            a = Tmp.new(co, :gpr)
            b = Tmp.new(co, :gpr)
            newList << Instruction.new(co, "move", [ops[1], a], node.annotation)  # a = old dst
            newList << Instruction.new(co, "move", [ops[0], b])                   # b = src (reg or imm)
            newList << Instruction.new(co, isAdd ? "addq" : "subq", [b, ops[1]])  # dst = r
            if isAdd
                newList << Instruction.new(co, "xorq", [ops[1], a])               # a = a^r
                newList << Instruction.new(co, "xorq", [ops[1], b])               # b = b^r
            else
                newList << Instruction.new(co, "xorq", [a, b])                    # b = a^b (before a is clobbered)
                newList << Instruction.new(co, "xorq", [ops[1], a])               # a = a^r
            end
            newList << Instruction.new(co, "andq", [b, a])                        # a &= b
            newList << Instruction.new(co, "bqlt", [a, Immediate.new(co, 0), ops[2]])
        when "baddis", "bsubis"
            unless ops[1].is_a?(RegisterID)
                newList << node   # baddis imm, Address, lbl → rmw expansion later
                next
            end
            newList << Instruction.new(co, (node.opcode == "baddis") ? "addi" : "subi", [ops[0], ops[1]], node.annotation)
            newList << Instruction.new(co, "bilt", [ops[1], Immediate.new(co, 0), ops[2]])
        when "baddps", "baddqs", "bsubps", "bsubqs"
            unless ops[1].is_a?(RegisterID)
                newList << node
                next
            end
            newList << Instruction.new(co, node.opcode.start_with?("badd") ? "addq" : "subq", [ops[0], ops[1]], node.annotation)
            newList << Instruction.new(co, "bqlt", [ops[1], Immediate.new(co, 0), ops[2]])
        else
            newList << node
        end
    }
    newList
end

# Per-backend hook called by riscLowerMisplacedAddresses BEFORE its generic
# rewrite.  We claim jmp/call so the generic path doesn't strip extra operands
# (notably PtrTag, which we use to distinguish C from JS calls in lowering).
# At this point our own ppc64LowerMalformedAddresses has already replaced
# any Address target with a Tmp, so the call/jmp is safe to leave as-is.
class Instruction
    def self.lowerMisplacedAddressesPPC64(node, newList)
        if node.opcode == "jmp" || node.opcode == "call"
            newList << node
            return [true, newList]
        end
        [false, newList]
    end
end

class Sequence
    def getModifiedListPPC64(result = @list)
        # Expand bt*z/bt*nz (test-and-branch) into and + bieq/bineq
        result = riscLowerTest(result)
        # Expand bmulio → smulli + rshifti + bineq
        result = riscLowerHardBranchOps(result)
        # Expand overflow/sign branches (badd*o/bsub*o/badd*s/bsub*s) into
        # XER-free sequences on fresh Tmps.  Must run before the large-immediate
        # pass (it emits move/addq with the original immediates) and before the
        # malformed-address passes (it leaves Address forms for the rmw path).
        result = ppc64LowerOverflowBranches(result)
        # Compares written with the immediate first: put it in a register
        # (before the large-immediate pass, which then sizes the move).
        result = ppc64LowerImmediateFirstCompares(result)
        # Lower large immediates AFTER riscLowerTest, since riscLowerTest synthesises
        # and{i,p,q} with the original immediate (e.g. btqnz t, ~1, lbl → andq t, ~1, tmp).
        result = ppc64LowerLargeImmediates(result)
        # PPC-specific malformed addresses: BaseIndex → Address, jmp/call Address,
        # transferp, lr SPR routing, baddis rmw, DS-form alignment.
        result = ppc64LowerMalformedAddresses(result)
        # Second pass: BaseIndex expansion may have produced new jmp/call Address nodes.
        result = ppc64LowerMalformedAddresses(result)
        # Generic RISC pass: lower Address operands in arith/test/branch/compare ops by
        # loading them into Tmps.  Covers cases not handled by the PPC-specific pass.
        result = riscLowerMisplacedAddresses(result)
        result = assignRegistersToTemporaries(result, :gpr, PPC64_EXTRA_GPRS)
        result = assignRegistersToTemporaries(result, :fpr, PPC64_EXTRA_FPRS)
        ppc64MarkNearBranches(result)
        result
    end
end

# -------------------------------------------------------------------------
# Short conditional branches where they provably reach.
#
# A PPC conditional branch (B-form bc) reaches only +/-32 KB, so by default
# every compare-and-branch is emitted as an inverted bc over an unconditional
# `b` (+/-32 MB) -- see ppc64EmitLongBranch.  That costs an instruction per
# branch, and IPInt handlers must fit fixed 256-byte slots.
#
# Here, on the final instruction list, a branch to a LOCAL label is marked
# near when an upper bound on the bytes between it and the label is well
# inside the bc range.  The bound charges every instruction node 64 bytes (no
# single offlineasm instruction lowers to more than about ten PPC
# instructions; the C-call stanza is seven, a 64-bit constant five) and
# every global label its alignment (IPInt handler slots are .balign 256) or
# 16 bytes.  Only LocalLabelReference targets qualify, and only when the
# label is found in the same list.  If the bound were ever wrong the result
# would still be loud, not silent: GAS rejects an out-of-range bc
# displacement at assembly time.
# -------------------------------------------------------------------------
PPC64_NEAR_BRANCH_BYTES = 24 * 1024
PPC64_MAX_BYTES_PER_INSTRUCTION = 64

def ppc64NodeByteBound(node)
    if node.is_a?(Instruction)
        PPC64_MAX_BYTES_PER_INSTRUCTION
    elsif node.is_a?(Label)
        align = node.instance_variable_get(:@alignTo)
        (align && align.respond_to?(:value)) ? [align.value.to_i, 16].max : 16
    else
        0
    end
end

def ppc64MarkNearBranches(list)
    labelIndex = {}
    prefix = [0]
    list.each_with_index { |node, i|
        labelIndex[node] = i if node.is_a?(LocalLabel)
        prefix << prefix[-1] + ppc64NodeByteBound(node)
    }
    list.each_with_index { |node, i|
        next unless node.is_a?(Instruction)
        targets = node.operands.select { |op| op.is_a?(LocalLabelReference) }
        next unless targets.length == 1
        j = labelIndex[targets[0].label]
        next unless j
        lo, hi = [i, j].min, [i, j].max
        if prefix[hi + 1] - prefix[lo] < PPC64_NEAR_BRANCH_BYTES
            node.instance_variable_set(:@ppc64NearBranch, true)
        end
    }
end

# Zero-extend a 32-bit result into the full 64-bit register (rldicl SH=0,
# MB=32 = clrldi).  See the width-contract comment at the arithmetic section:
# offlineasm "i" ops must match x86/arm64 semantics where 32-bit writes
# zero the upper word.
def ppc64ZeroExtend32(dst)
    $asm.puts "rldicl #{dst}, #{dst}, 0, 32"
end

# -------------------------------------------------------------------------
# Conditional branch to a label that may be more than ±32 KB away.  bc has a
# 14-bit word displacement (±32 KB), b has 24 bits (±32 MB): invert the
# condition and skip over a `b`, at the cost of one extra instruction.
# -------------------------------------------------------------------------
$ppc64LongBranchCounter = 0
PPC64_BRANCH_INVERSE = {
    "blt" => "bge", "bge" => "blt",
    "bgt" => "ble", "ble" => "bgt",
    "beq" => "bne", "bne" => "beq",
    "bso" => "bns", "bns" => "bso",
}.freeze

def ppc64EmitLongBranch(cond, target)
    if $ppc64CurrentNearBranch
        # Proven in range by ppc64MarkNearBranches: a single bc.
        raise "ppc64: unknown conditional #{cond}" unless PPC64_BRANCH_INVERSE[cond]
        $asm.puts "#{cond} #{target}"
        return
    end
    inverted = PPC64_BRANCH_INVERSE[cond] or raise "ppc64: unknown conditional #{cond}"
    $ppc64LongBranchCounter += 1
    skip = ".Lppc64_lb_#{$ppc64LongBranchCounter}"
    $asm.puts "#{inverted} #{skip}"
    $asm.puts "b #{target}"
    $asm.puts "#{skip}:"
end

# -------------------------------------------------------------------------
# Helper: emit a 64-bit integer load into a named PPC64 register.
# Uses the standard 5-instruction sequence:
#   lis   dest, value@highest
#   ori   dest, dest, value@higher
#   rldicr dest, dest, 32, 31
#   oris  dest, dest, value@h
#   ori   dest, dest, value@l
# -------------------------------------------------------------------------
def ppc64EmitLI64(dest, value)
    if value.is_a?(Integer)
        # Shorter forms for constants that fit in 32 bits, producing exactly
        # the same 64-bit register value as the general sequence below.
        # lis sign-extends its 16-bit field into the upper word, so
        #   signed 32-bit values:   lis hi ; ori lo        (1-2 instructions)
        #   unsigned 32-bit values: lis hi ; ori lo ; clrldi 32  (2-3)
        # (clrldi = rldicl d,d,0,32 zeroes the upper word).
        v = value & 0xffffffffffffffff
        sv = v >= (1 << 63) ? v - (1 << 64) : v
        if (-0x80000000..0xffffffff).include?(sv)
            lo = sv & 0xffff
            hi = (sv >> 16) & 0xffff
            hi -= 0x10000 if hi >= 0x8000
            $asm.puts "lis #{dest}, #{hi}"
            $asm.puts "ori #{dest}, #{dest}, #{lo}" if lo != 0
            $asm.puts "clrldi #{dest}, #{dest}, 32" if sv > 0x7fffffff
            return
        end
    end
    $asm.puts "lis #{dest}, #{value}@highest"
    $asm.puts "ori #{dest}, #{dest}, #{value}@higher"
    $asm.puts "rldicr #{dest}, #{dest}, 32, 31"
    $asm.puts "oris #{dest}, #{dest}, #{value}@h"
    $asm.puts "ori #{dest}, #{dest}, #{value}@l"
end

# -------------------------------------------------------------------------
# Helper: emit a conditional branch.  Emits a compare then a bc instruction.
# size: :i (32-bit cmpw) or :q/:p (64-bit cmpd); signed: true/false
# -------------------------------------------------------------------------
def ppc64EmitCompareAndBranch(cond, size, signed, ops)
    # ops: [src1, src2, label] or [src1, imm, label]
    raise "ppc64: compare with an immediate first operand reached the emitter (#{ops[0].value}); ppc64LowerImmediateFirstCompares should have moved it to a register" if ops[0].is_a?(Immediate)
    ra  = ops[0].ppc64Operand
    rbl = ops[1]
    lbl = ops[2].asmLabel

    if size == :i || size == :b
        # byte values are zero-extended to full register by lbz; compare as 32-bit
        cmpOp  = signed ? "cmpwi"  : "cmplwi"
        cmpOpR = signed ? "cmpw"   : "cmplw"
    else  # :q or :p
        cmpOp  = signed ? "cmpdi"  : "cmpldi"
        cmpOpR = signed ? "cmpd"   : "cmpld"
    end

    if rbl.is_a?(Immediate)
        $asm.puts "#{cmpOp} #{ra}, #{rbl.value}"
    else
        $asm.puts "#{cmpOpR} #{ra}, #{rbl.ppc64Operand}"
    end

    case cond
    when :eq  then ppc64EmitLongBranch("beq", lbl)
    when :neq then ppc64EmitLongBranch("bne", lbl)
    when :lt  then ppc64EmitLongBranch("blt", lbl)
    when :gt  then ppc64EmitLongBranch("bgt", lbl)
    when :le  then ppc64EmitLongBranch("ble", lbl)
    when :ge  then ppc64EmitLongBranch("bge", lbl)
    else
        raise "ppc64: unknown compare condition #{cond}"
    end
end

# -------------------------------------------------------------------------
# Helper: FP conditional set (cd*/cf* opcodes).
# fcmpu cr0, fA, fB sets CR0: LT(bit31), GT(bit30), EQ(bit29), FU(bit28).
# Same bit layout as integer CR0 — reuse rlwinm rotations.
# "ordered" conditions exclude NaN (FU=0). "un" variants include NaN.
# -------------------------------------------------------------------------
def ppc64EmitFPConditionalSet(cond, is_double, ops)
    fA  = ops[0].ppc64Operand
    fB  = ops[1].ppc64Operand
    dst = ops[2].ppc64Operand
    # fcmpu (unordered) sets CR0.FU (= SO) for NaN; does NOT raise exceptions.
    $asm.puts "fcmpu cr0, #{fA}, #{fB}"
    case cond
    when :lt
        # LT=1 only when ordered and fA < fB. NaN → LT=0.
        $asm.puts "mfcr #{dst}"
        $asm.puts "rlwinm #{dst}, #{dst}, 1, 31, 31"
    when :gt
        # GT=1 only when ordered and fA > fB. NaN → GT=0.
        $asm.puts "mfcr #{dst}"
        $asm.puts "rlwinm #{dst}, #{dst}, 2, 31, 31"
    when :eq
        # EQ=1 only when ordered and fA == fB. NaN → EQ=0.
        $asm.puts "mfcr #{dst}"
        $asm.puts "rlwinm #{dst}, #{dst}, 3, 31, 31"
    when :lteq
        # LT | EQ and not NaN. Use branches: if GT or FU → false.
        # CR0.SO (bso) = FU (NaN). bgt = GT.
        $asm.puts "li #{dst}, 0"
        $asm.puts "bso 1f"    # NaN → false
        $asm.puts "bgt 1f"    # GT  → false
        $asm.puts "li #{dst}, 1"
        $asm.puts "1:"
    when :gteq
        # GT | EQ and not NaN. If LT or FU → false.
        $asm.puts "li #{dst}, 0"
        $asm.puts "bso 1f"    # NaN → false
        $asm.puts "blt 1f"    # LT  → false
        $asm.puts "li #{dst}, 1"
        $asm.puts "1:"
    when :neq
        # Not equal and not NaN. If EQ or FU → false.
        $asm.puts "li #{dst}, 0"
        $asm.puts "bso 1f"    # NaN → false
        $asm.puts "beq 1f"    # EQ  → false
        $asm.puts "li #{dst}, 1"
        $asm.puts "1:"
    when :neq_un
        # Not equal or NaN (unordered variant). EQ=0 OR FU=1 → true.
        $asm.puts "mfcr #{dst}"
        $asm.puts "rlwinm #{dst}, #{dst}, 3, 31, 31"  # EQ bit
        $asm.puts "xori #{dst}, #{dst}, 1"             # NOT EQ (NaN→EQ=0→NOT EQ=1)
    else
        raise "ppc64: unknown FP conditional set condition #{cond}"
    end
end

# Helper: FP branch (bd*/bf* opcodes).
# fcmpu cr0, fA, fB; CR0: LT(31), GT(30), EQ(29), FU/SO(28).
# ops: [fA, fB, label]
def ppc64EmitFPBranch(cond, ops)
    fA  = ops[0].ppc64Operand
    fB  = ops[1].ppc64Operand
    lbl = ops[2].asmLabel
    $asm.puts "fcmpu cr0, #{fA}, #{fB}"
    case cond
    when :lt                                    # ordered <: NaN→LT=0
        ppc64EmitLongBranch("blt", lbl)
    when :gt                                    # ordered >: NaN→GT=0
        ppc64EmitLongBranch("bgt", lbl)
    when :eq                                    # ordered =: NaN→EQ=0
        ppc64EmitLongBranch("beq", lbl)
    when :lteq                                  # ordered ≤: NaN→no branch
        ppc64EmitLongBranch("blt", lbl)
        ppc64EmitLongBranch("beq", lbl)
    when :gteq                                  # ordered ≥: NaN→no branch
        ppc64EmitLongBranch("bgt", lbl)
        ppc64EmitLongBranch("beq", lbl)
    when :neq                                   # ordered ≠: LT or GT; NaN→no branch
        ppc64EmitLongBranch("blt", lbl)
        ppc64EmitLongBranch("bgt", lbl)
    when :neq_un                                # not-equal or NaN: bne = NOT EQ
        ppc64EmitLongBranch("bne", lbl)
    when :ltun                                  # < or NaN
        ppc64EmitLongBranch("blt", lbl)
        ppc64EmitLongBranch("bso", lbl)
    when :gtun                                  # > or NaN
        ppc64EmitLongBranch("bgt", lbl)
        ppc64EmitLongBranch("bso", lbl)
    when :ltequn                                # ≤ or NaN: ble = NOT GT
        ppc64EmitLongBranch("ble", lbl)
    when :gtequn                                # ≥ or NaN: bge = NOT LT
        ppc64EmitLongBranch("bge", lbl)
    else
        raise "ppc64: unknown FP branch condition #{cond}"
    end
end

# -------------------------------------------------------------------------
# Helper: conditional set (c* opcodes): compare, then mfcr and rlwinm to move
# one CR0 bit to the low bit.  After mfcr, CR0's LT, GT, EQ and SO are bits
# 31, 30, 29 and 28 counting from the least significant, so rotating the low
# word left by 1, 2 or 3 brings LT, GT or EQ to bit 0.
# -------------------------------------------------------------------------
def ppc64EmitConditionalSet(cond, size, signed, ops)
    raise "ppc64: compare with an immediate first operand reached the emitter (#{ops[0].value}); ppc64LowerImmediateFirstCompares should have moved it to a register" if ops[0].is_a?(Immediate)
    ra  = ops[0].ppc64Operand
    rbl = ops[1]
    dst = ops[2].ppc64Operand

    if size == :i
        cmpOp  = signed ? "cmpwi" : "cmplwi"
        cmpOpR = signed ? "cmpw"  : "cmplw"
    else
        cmpOp  = signed ? "cmpdi" : "cmpldi"
        cmpOpR = signed ? "cmpd"  : "cmpld"
    end

    if rbl.is_a?(Immediate)
        $asm.puts "#{cmpOp} #{ra}, #{rbl.value}"
    else
        $asm.puts "#{cmpOpR} #{ra}, #{rbl.ppc64Operand}"
    end

    $asm.puts "mfcr #{dst}"
    case cond
    when :eq
        $asm.puts "rlwinm #{dst}, #{dst}, 3, 31, 31"
    when :neq
        $asm.puts "rlwinm #{dst}, #{dst}, 3, 31, 31"
        $asm.puts "xori #{dst}, #{dst}, 1"
    when :lt
        $asm.puts "rlwinm #{dst}, #{dst}, 1, 31, 31"
    when :gt
        $asm.puts "rlwinm #{dst}, #{dst}, 2, 31, 31"
    when :le  # NOT gt
        $asm.puts "rlwinm #{dst}, #{dst}, 2, 31, 31"
        $asm.puts "xori #{dst}, #{dst}, 1"
    when :ge  # NOT lt
        $asm.puts "rlwinm #{dst}, #{dst}, 1, 31, 31"
        $asm.puts "xori #{dst}, #{dst}, 1"
    else
        raise "ppc64: unknown conditional set condition #{cond}"
    end
end

# -------------------------------------------------------------------------
# Helper: 3-operand integer emit.
# offlineasm 2-op: "addp src, dest" → dest = dest + src → add dest, dest, src
# offlineasm 3-op: "addp src1, src2, dest" → dest = src1 + src2
# -------------------------------------------------------------------------
def ppc64EmitArith(opcode3reg, opcode3imm, node)
    ops = node.operands
    case ops.length
    when 2
        dst = ops[1].ppc64Operand
        src = ops[0]
        if src.is_a?(Immediate)
            $asm.puts "#{opcode3imm} #{dst}, #{dst}, #{src.value}"
        else
            $asm.puts "#{opcode3reg} #{dst}, #{dst}, #{src.ppc64Operand}"
        end
    when 3
        # offlineasm: dst = src1 + src2.  Caller is responsible for choosing
        # only commutative ops here (add{i,p,q}); for non-commutative subtract,
        # see the dedicated subi/subp/subq case in lowerPPC64.
        dst = ops[2].ppc64Operand
        if ops[0].is_a?(Immediate) && ops[1].is_a?(Immediate)
            raise "ppc64: both operands immediate in #{node.opcode} at #{node.codeOriginString}"
        elsif ops[0].is_a?(Immediate)
            # imm-first: swap, since add is commutative
            $asm.puts "#{opcode3imm} #{dst}, #{ops[1].ppc64Operand}, #{ops[0].value}"
        elsif ops[1].is_a?(Immediate)
            $asm.puts "#{opcode3imm} #{dst}, #{ops[0].ppc64Operand}, #{ops[1].value}"
        else
            $asm.puts "#{opcode3reg} #{dst}, #{ops[0].ppc64Operand}, #{ops[1].ppc64Operand}"
        end
    else
        raise "ppc64: unexpected operand count #{ops.length} for #{node.opcode}"
    end
end

# -------------------------------------------------------------------------
# Instruction lowering — main dispatch
# -------------------------------------------------------------------------
class Instruction
    def lowerPPC64
        $asm.comment codeOriginString
        operands = self.operands
        $ppc64CurrentNearBranch = (instance_variable_get(:@ppc64NearBranch) == true)
        case opcode

        # ------------------------------------------------------------------
        # Annotate / no-ops
        # ------------------------------------------------------------------
        when "tagReturnAddress", "untagReturnAddress", "untagReturnAddressWithoutMoving",
             "nop", "breakpoint"
            $asm.puts "nop"

        # Memory barriers.  PPC64's memory model is weaker than x86's and
        # ARM64's, so these must be real fences: e.g. the one in
        # skipIfIsRememberedOrInEden, the GC write-barrier fast path, orders
        # the store to the object against the load of m_cellState.
        #
        # memfence/fence match ARM64's "dmb sy"/"dmb ish" and x86's locked
        # read-modify-write: a full barrier, which on PPC is sync (hwsync, L=0)
        # -- lwsync is not enough because it does not order store-then-load.
        # loadStoreFence only has to order loads and stores against each other,
        # which is exactly lwsync.
        when "memfence", "fence"
            $asm.puts "sync"

        when "loadStoreFence"
            $asm.puts "lwsync"

        when "checkStackPointerAlignment"
            # Not checked on PPC64.

        # ------------------------------------------------------------------
        # Move
        # ------------------------------------------------------------------
        when "move", "movep", "moveq", "movei"
            src = operands[0]
            dst = operands[1]
            if src.is_a?(RegisterID) && src.name == "lr"
                $asm.puts "mflr #{dst.ppc64Operand}"
            elsif dst.is_a?(RegisterID) && dst.name == "lr"
                $asm.puts "mtlr #{src.ppc64Operand}"
            elsif src.is_a?(Immediate)
                v = src.value
                if src.ppc6416BitSignedImmediate?
                    $asm.puts "li #{dst.ppc64Operand}, #{v}"
                else
                    ppc64EmitLI64(dst.ppc64Operand, v)
                end
            else
                $asm.puts "mr #{dst.ppc64Operand}, #{src.ppc64Operand}"
            end

        when "ppc64_li64"
            # Emitted by preprocessing for large immediates.
            dst = operands[1].ppc64Operand
            val = operands[0].value
            ppc64EmitLI64(dst, val)

        # ------------------------------------------------------------------
        # Arithmetic — add
        #
        # Width contract for every offlineasm "i" (32-bit) op below: the
        # 32-bit result is ZERO-EXTENDED into the 64-bit register, exactly
        # like x86 32-bit ops and arm64 Wn writes.  LLInt asm relies on this
        # implicitly — e.g. op_switch_imm computes `subi min, scrutinee` on a
        # value whose upper word still holds JSValue tag bits and then uses
        # the full register as a jump-table index; without the implicit
        # truncation the numberTag survives and the table load dereferences
        # (tag << 2) + base.  PPC 64-bit arith (add/subf/neg/nor/or/xor/and)
        # leaves the upper word dirty and sraw sign-extends, so each i-op
        # clears its result via ppc64ZeroExtend32.
        # slw/srw/slwi/srwi/andi./lwz/lhz/lbz/cntlzw zero-extend natively.
        # Inputs may be dirty: 32-bit PPC ops read only the low word, and the
        # low word of a 64-bit add/sub/mul/logic result equals the 32-bit
        # result, so output-cleaning is a complete contract.
        # ------------------------------------------------------------------
        when "addp", "addq", "addi"
            ppc64EmitArith("add", "addi", self)
            ppc64ZeroExtend32(operands.last.ppc64Operand) if opcode == "addi"

        when "addis"
            # offlineasm addis = "add integers and set a flag" (x86/arm64
            # width-table op) — NOT PPC's add-immediate-shifted.  Not used by
            # LLInt, so not implemented.
            raise "ppc64: offlineasm addis (add-and-set-flags) not implemented at #{codeOriginString}"

        # ------------------------------------------------------------------
        # Arithmetic — subtract
        # subp src, dest  →  dest = dest - src  →  subf dest, src, dest
        # subp A, B, dest →  dest = A - B       →  subf dest, B, A
        # ------------------------------------------------------------------
        when "subp", "subq", "subi"
            ops = operands
            case ops.length
            when 2
                dst = ops[1].ppc64Operand
                src = ops[0]
                if src.is_a?(Immediate)
                    # dest -= imm  →  addi dest, dest, -imm
                    $asm.puts "addi #{dst}, #{dst}, #{-src.value}"
                else
                    $asm.puts "subf #{dst}, #{src.ppc64Operand}, #{dst}"
                end
            when 3
                dst  = ops[2].ppc64Operand
                src1 = ops[0].ppc64Operand   # dest = src1 - src2
                src2 = ops[1]
                if src2.is_a?(Immediate)
                    $asm.puts "addi #{dst}, #{src1}, #{-src2.value}"
                else
                    $asm.puts "subf #{dst}, #{src2.ppc64Operand}, #{src1}"
                end
            else
                raise "ppc64: unexpected operand count for #{opcode}"
            end
            ppc64ZeroExtend32(operands.last.ppc64Operand) if opcode == "subi"

        # ------------------------------------------------------------------
        # Arithmetic — multiply
        # mulq / mulp  →  mulld (64-bit multiply low)
        # muli          →  mullw or mulli (immediate)
        # ------------------------------------------------------------------
        # Multiply. Two forms: 2-operand `mul src, dst` (dst *= src) and
        # 3-operand `mul src1, src2, dst` (dst = src1 * src2).  The dest is
        # always the LAST operand (argumentProfileLoop's `mulp sizeof, t0, t2`
        # writes t2).
        when "mulp", "mulq"
            ops = operands
            dst = ops.last.ppc64Operand
            if ops.length == 3
                a, b = ops[0], ops[1]
                if a.is_a?(Immediate) && b.is_a?(Immediate)
                    raise "ppc64: mulq with two immediates at #{codeOriginString}"
                elsif a.is_a?(Immediate)
                    raise "ppc64: mulq immediate #{a.value} out of 16-bit range at #{codeOriginString}" unless a.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{b.ppc64Operand}, #{a.value}"
                elsif b.is_a?(Immediate)
                    raise "ppc64: mulq immediate #{b.value} out of 16-bit range at #{codeOriginString}" unless b.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{a.ppc64Operand}, #{b.value}"
                else
                    $asm.puts "mulld #{dst}, #{a.ppc64Operand}, #{b.ppc64Operand}"
                end
            else
                src = ops[0]
                if src.is_a?(Immediate)
                    raise "ppc64: mulq immediate #{src.value} out of 16-bit range at #{codeOriginString}" unless src.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{dst}, #{src.value}"
                else
                    $asm.puts "mulld #{dst}, #{dst}, #{src.ppc64Operand}"
                end
            end

        when "muli"
            ops = operands
            dst = ops.last.ppc64Operand
            if ops.length == 3
                a, b = ops[0], ops[1]
                if a.is_a?(Immediate) && b.is_a?(Immediate)
                    raise "ppc64: muli with two immediates at #{codeOriginString}"
                elsif a.is_a?(Immediate)
                    raise "ppc64: muli immediate out of range at #{codeOriginString}" unless a.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{b.ppc64Operand}, #{a.value}"
                elsif b.is_a?(Immediate)
                    raise "ppc64: muli immediate out of range at #{codeOriginString}" unless b.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{a.ppc64Operand}, #{b.value}"
                else
                    # mullw writes the full 64-bit product of the low words —
                    # the high word holds product overflow bits, not zeros.
                    $asm.puts "mullw #{dst}, #{a.ppc64Operand}, #{b.ppc64Operand}"
                end
            else
                src = ops[0]
                if src.is_a?(Immediate)
                    raise "ppc64: muli immediate out of range at #{codeOriginString}" unless src.ppc6416BitSignedImmediate?
                    $asm.puts "mulli #{dst}, #{dst}, #{src.value}"
                else
                    $asm.puts "mullw #{dst}, #{dst}, #{src.ppc64Operand}"
                end
            end
            ppc64ZeroExtend32(dst)

        # ------------------------------------------------------------------
        # Arithmetic — divide
        #
        # Operand order is fixed by offlineasm/arm64.rb emitARM64Div (the
        # reference lowering; x86 has no div* opcode, it uses cdqi/idivi):
        #   2-operand `div src, dst`        →  dst = dst / src
        #   3-operand `div src2, src1, dst` →  dst = src1 / src2
        # divi/divq are UNSIGNED (arm64 udiv), divis/divqs are SIGNED (sdiv).
        # PPC's form is `div* RT, RA, RB` with RA = dividend, RB = divisor.
        # IPInt only ever emits the 2-operand form on PPC64 (see the
        # i32/i64 div_s/div_u/rem_s/rem_u handlers in
        # llint/InPlaceInterpreter64.asm), which is also the unambiguous one.
        #
        # divw/divwu leave bits 0:31 of RT undefined.  int32 values are kept
        # zero-extended in the 64-bit register, so the 32-bit forms
        # re-establish that, as div32/uDiv32 in MacroAssemblerPPC64.h do.
        # divd/divdu are full-width; no fixup needed.
        #
        # PPC has no divide-immediate; an Immediate operand has no encoding.
        # Division by zero and the INT_MIN/-1 overflow are guarded by the
        # caller (btiz / bineq -1) — PPC leaves RT undefined for both and
        # only traps if the OE form is used, which we do not emit.
        # ------------------------------------------------------------------
        when "divi", "divis", "divq", "divqs"
            insn = { "divi"  => "divwu", "divis" => "divw",
                     "divq"  => "divdu", "divqs" => "divd" }[opcode]
            ops = operands
            case ops.length
            when 2
                divisor, dividend, dst = ops[0], ops[1], ops[1]
            when 3
                divisor, dividend, dst = ops[0], ops[1], ops[2]
            else
                raise "ppc64: unexpected operand count for #{opcode} at #{codeOriginString}"
            end
            [divisor, dividend].each { | op |
                raise "ppc64: #{opcode} has no immediate form at #{codeOriginString}" if op.is_a?(Immediate)
            }
            $asm.puts "#{insn} #{dst.ppc64Operand}, #{dividend.ppc64Operand}, #{divisor.ppc64Operand}"
            ppc64ZeroExtend32(dst.ppc64Operand) if opcode == "divi" or opcode == "divis"

        # ------------------------------------------------------------------
        # Arithmetic — negate
        # ------------------------------------------------------------------
        when "negp", "negq"
            dst = operands[0].ppc64Operand
            $asm.puts "neg #{dst}, #{dst}"

        when "negi"
            dst = operands[0].ppc64Operand
            $asm.puts "neg #{dst}, #{dst}"
            ppc64ZeroExtend32(dst)

        # ------------------------------------------------------------------
        # Bitwise: and / or / xor / not
        # ------------------------------------------------------------------
        # Critical correctness note: with bare-numeral register encoding, GAS
        # parses an integer in a register slot as a register number.  So
        # emitting `and dst, src, 1` is silently assembled as `and dst, src, r1`
        # (= AND with sp).  Every 3-operand form below MUST detect Immediate
        # operands and route to andi./ori/xori (which take a real immediate
        # field) — never let a bare integer end up as the third arg of `and`.

        when "andp", "andq", "andi"
            ops = operands
            dst = ops.last.ppc64Operand
            if ops.length == 3
                if ops[0].is_a?(Immediate) && ops[1].is_a?(Immediate)
                    raise "ppc64: andp with two immediates at #{codeOriginString}"
                elsif ops[0].is_a?(Immediate)
                    $asm.puts "andi. #{dst}, #{ops[1].ppc64Operand}, #{ops[0].value}"
                elsif ops[1].is_a?(Immediate)
                    $asm.puts "andi. #{dst}, #{ops[0].ppc64Operand}, #{ops[1].value}"
                else
                    $asm.puts "and #{dst}, #{ops[0].ppc64Operand}, #{ops[1].ppc64Operand}"
                end
            else
                src = ops[0]
                if src.is_a?(Immediate)
                    $asm.puts "andi. #{dst}, #{dst}, #{src.value}"
                else
                    $asm.puts "and #{dst}, #{src.ppc64Operand}, #{dst}"
                end
            end
            ppc64ZeroExtend32(dst) if opcode == "andi"

        when "orp", "orq", "ori"
            ops = operands
            dst = ops.last.ppc64Operand
            if ops.length == 3
                if ops[0].is_a?(Immediate) && ops[1].is_a?(Immediate)
                    raise "ppc64: orp with two immediates at #{codeOriginString}"
                elsif ops[0].is_a?(Immediate)
                    $asm.puts "ori #{dst}, #{ops[1].ppc64Operand}, #{ops[0].value}"
                elsif ops[1].is_a?(Immediate)
                    $asm.puts "ori #{dst}, #{ops[0].ppc64Operand}, #{ops[1].value}"
                else
                    $asm.puts "or #{dst}, #{ops[0].ppc64Operand}, #{ops[1].ppc64Operand}"
                end
            else
                src = ops[0]
                if src.is_a?(Immediate)
                    $asm.puts "ori #{dst}, #{dst}, #{src.value}"
                else
                    $asm.puts "or #{dst}, #{src.ppc64Operand}, #{dst}"
                end
            end
            ppc64ZeroExtend32(dst) if opcode == "ori"

        when "xorp", "xorq", "xori"
            ops = operands
            dst = ops.last.ppc64Operand
            if ops.length == 3
                if ops[0].is_a?(Immediate) && ops[1].is_a?(Immediate)
                    raise "ppc64: xorp with two immediates at #{codeOriginString}"
                elsif ops[0].is_a?(Immediate)
                    $asm.puts "xori #{dst}, #{ops[1].ppc64Operand}, #{ops[0].value}"
                elsif ops[1].is_a?(Immediate)
                    $asm.puts "xori #{dst}, #{ops[0].ppc64Operand}, #{ops[1].value}"
                else
                    $asm.puts "xor #{dst}, #{ops[0].ppc64Operand}, #{ops[1].ppc64Operand}"
                end
            else
                src = ops[0]
                if src.is_a?(Immediate)
                    $asm.puts "xori #{dst}, #{dst}, #{src.value}"
                else
                    $asm.puts "xor #{dst}, #{src.ppc64Operand}, #{dst}"
                end
            end
            ppc64ZeroExtend32(dst) if opcode == "xori"

        when "notp", "notq"
            dst = operands[0].ppc64Operand
            $asm.puts "nor #{dst}, #{dst}, #{dst}"

        when "noti"
            dst = operands[0].ppc64Operand
            $asm.puts "nor #{dst}, #{dst}, #{dst}"
            ppc64ZeroExtend32(dst)

        # ------------------------------------------------------------------
        # Shifts
        # lshiftp/q/i src, dest  →  sldi/slwi dest, dest, src
        # rshiftp/q/i src, dest  →  srdi/srwi (logical) or srawi (arithmetic)
        # urshiftp/q/i src, dest →  srdi/srwi (logical unsigned)
        # ------------------------------------------------------------------
        # Shift-count masking: x86/arm64 hardware mask the count (&31 for
        # 32-bit shifts, &63 for 64-bit) and JS semantics depend on it
        # (5 << 32 === 5).  PPC sl/sr/sra use a wider count field where
        # 32..63 (resp. 64..127) shifts out everything.  Mask register
        # counts through r0 — architecturally safe as a pure data scratch
        # in a self-contained two-instruction sequence (r0 is only special
        # as a load/store base / addi operand), and r0 has no offlineasm
        # register mapping.  Immediate counts are masked at generation time.
        when "lshiftp", "lshiftq"
            dst = operands[1].ppc64Operand
            src = operands[0]
            if src.is_a?(Immediate)
                $asm.puts "sldi #{dst}, #{dst}, #{src.value & 63}"
            else
                $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 58"
                $asm.puts "sld #{dst}, #{dst}, 0"
            end

        when "lshifti"
            dst = operands[1].ppc64Operand
            src = operands[0]
            if src.is_a?(Immediate)
                $asm.puts "slwi #{dst}, #{dst}, #{src.value & 31}"
            else
                $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 59"
                $asm.puts "slw #{dst}, #{dst}, 0"
            end

        when "rshiftp", "rshiftq"
            # Arithmetic (signed) right shift
            dst = operands[1].ppc64Operand
            src = operands[0]
            if src.is_a?(Immediate)
                $asm.puts "sradi #{dst}, #{dst}, #{src.value & 63}"
            else
                $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 58"
                $asm.puts "srad #{dst}, #{dst}, 0"
            end

        when "rshifti"
            # 2-op: rshifti imm_or_reg, dst  →  dst >>= imm/reg (arithmetic)
            # 3-op: rshifti src, imm_or_reg, dst  →  dst = src >> imm/reg
            if operands.size == 3
                src = operands[0].ppc64Operand
                shamt = operands[1]
                dst = operands[2].ppc64Operand
                if shamt.is_a?(Immediate)
                    $asm.puts "srawi #{dst}, #{src}, #{shamt.value & 31}"
                else
                    $asm.puts "rldicl 0, #{shamt.ppc64Operand}, 0, 59"
                    $asm.puts "sraw #{dst}, #{src}, 0"
                end
            else
                dst = operands[1].ppc64Operand
                src = operands[0]
                if src.is_a?(Immediate)
                    $asm.puts "srawi #{dst}, #{dst}, #{src.value & 31}"
                else
                    $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 59"
                    $asm.puts "sraw #{dst}, #{dst}, 0"
                end
            end
            # sraw/srawi sign-extend the 32-bit result to 64; x86 sarl and
            # arm64 asr Wn zero-extend.
            ppc64ZeroExtend32(operands.last.ppc64Operand)

        when "smulli"
            # smulli src1, src2, dst_lo, dst_hi
            # riscLowerHardBranchOps generates: smulli [src, dst, dst, tmp]
            # where dst is both the second factor AND the low-result register.
            # Compute mulhw FIRST (while dst still holds the original factor),
            # then mullw to overwrite dst with the low result.
            src1   = operands[0].ppc64Operand
            src2   = operands[1].ppc64Operand
            dst_lo = operands[2].ppc64Operand
            dst_hi = operands[3].ppc64Operand
            $asm.puts "mulhw #{dst_hi}, #{src1}, #{src2}"
            $asm.puts "mullw #{dst_lo}, #{src1}, #{src2}"

        when "urshiftp", "urshiftq"
            # Logical (unsigned) right shift
            dst = operands[1].ppc64Operand
            src = operands[0]
            if src.is_a?(Immediate)
                $asm.puts "srdi #{dst}, #{dst}, #{src.value & 63}"
            else
                $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 58"
                $asm.puts "srd #{dst}, #{dst}, 0"
            end

        when "urshifti"
            dst = operands[1].ppc64Operand
            src = operands[0]
            if src.is_a?(Immediate)
                $asm.puts "srwi #{dst}, #{dst}, #{src.value & 31}"
            else
                $asm.puts "rldicl 0, #{src.ppc64Operand}, 0, 59"
                $asm.puts "srw #{dst}, #{dst}, 0"
            end

        # ------------------------------------------------------------------
        # Rotates
        #
        # Operand order matches x86 handleX86Shift / arm64 emitARM64TAC:
        #   2-operand `Xrotate amount, dst`      →  dst = rot(dst, amount)
        #   3-operand `Xrotate src, amount, dst` →  dst = rot(src, amount)
        # IPInt only emits the 2-operand form (i32/i64 rotl/rotr in
        # llint/InPlaceInterpreter64.asm); the 3-operand form follows arm64.
        #
        # 32-bit: rlwnm/rlwinm rotate RS{32:63} left and mask with
        # MB..ME = 0..31, i.e. the whole low word, AND clear RA{0:31} — so
        # they re-establish the zero-extended-int32 invariant for free (no
        # ppc64ZeroExtend32 needed).  Same trick as rotateRight32 in
        # assembler/MacroAssemblerPPC64.h.
        # 64-bit: rldcl/rldicl with MB = 0 is a plain 64-bit rotate.
        #
        # Rotate-right is expressed as rotate-left by (width - n).  rlwnm
        # reads only RB{59:63} (5 bits) and rldcl only RB{58:63} (6 bits),
        # so `subfic 0, amount, width` wraps correctly for amount == 0
        # (width → 0) and for amount >= width.  x86 rol/ror and arm64 ror
        # mask the count the same way, and Wasm i32.rotl requires it.
        # r0 is a pure data scratch here (never a load/store base).
        # ------------------------------------------------------------------
        when "lrotatei", "rrotatei", "lrotateq", "rrotateq"
            quad = (opcode == "lrotateq" or opcode == "rrotateq")
            right = (opcode == "rrotatei" or opcode == "rrotateq")
            width = quad ? 64 : 32
            ops = operands
            case ops.length
            when 2
                amount, src, dst = ops[0], ops[1].ppc64Operand, ops[1].ppc64Operand
            when 3
                amount, src, dst = ops[1], ops[0].ppc64Operand, ops[2].ppc64Operand
            else
                raise "ppc64: unexpected operand count for #{opcode} at #{codeOriginString}"
            end
            if amount.is_a?(Immediate)
                n = amount.value & (width - 1)
                n = (width - n) & (width - 1) if right
                if quad
                    $asm.puts "rldicl #{dst}, #{src}, #{n}, 0"
                else
                    $asm.puts "rlwinm #{dst}, #{src}, #{n}, 0, 31"
                end
            else
                shift = amount.ppc64Operand
                if right
                    $asm.puts "subfic 0, #{shift}, #{width}"
                    shift = "0"
                end
                if quad
                    $asm.puts "rldcl #{dst}, #{src}, #{shift}, 0"
                else
                    $asm.puts "rlwnm #{dst}, #{src}, #{shift}, 0, 31"
                end
            end

        # ------------------------------------------------------------------
        # Bit-count
        #
        # lzcnt*/tzcnt* are the Wasm-facing spellings of the same operation
        # (arm64 lowers lzcnti to plain `clz`, x86 to bsr + fixup); both
        # families take [src, dst] in that order, so they share the cases
        # below.  ctz(0) and clz(0) must return the type width — Wasm
        # i32.clz/i32.ctz are total functions.
        # cntlzw/cntlzd write the full 64-bit RA (result is 0..width, so the
        # upper word is naturally zero) — no ppc64ZeroExtend32 needed.
        # ------------------------------------------------------------------
        when "countLeadingZerosp", "countLeadingZerosq", "lzcntq"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "cntlzd #{dst}, #{src}"

        when "countLeadingZerosi", "lzcnti"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "cntlzw #{dst}, #{src}"

        # cnttzd/cnttzw are POWER9 (ISA 3.0), outside the POWER8 baseline.
        # Use the isolate-lowest-bit identity instead:
        # cnttz(x) = popcnt((x & -x) - 1); for x == 0 the mask becomes all
        # ones and popcnt yields the type width.
        # r0 is a pure data scratch here (never a load/store base).
        # The decrement must be addic, NOT addi: addi reads RA = r0 as a
        # literal zero (the (RA|0) rule); addic reads the register.
        when "countTrailingZerosp", "countTrailingZerosq", "tzcntq"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "neg 0, #{src}"
            $asm.puts "and 0, 0, #{src}"
            $asm.puts "addic 0, 0, -1"
            $asm.puts "popcntd #{dst}, 0"

        when "countTrailingZerosi", "tzcnti"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "neg 0, #{src}"
            $asm.puts "and 0, 0, #{src}"
            $asm.puts "addic 0, 0, -1"
            $asm.puts "rldicl 0, 0, 0, 32"
            $asm.puts "popcntw #{dst}, 0"

        # ------------------------------------------------------------------
        # Zero/Sign extension
        # ------------------------------------------------------------------
        when "zxi2p", "zxi2q"
            # Zero-extend 32-bit → 64-bit: rldicl with SH=0, MB=32
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "rldicl #{dst}, #{src}, 0, 32"

        when "sxi2p", "sxi2q"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "extsw #{dst}, #{src}"

        when "sxb2q", "sxb2p"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "extsb #{dst}, #{src}"

        when "sxb2i"
            # movsbl semantics: sign-extend byte to 32, zero-extend to 64.
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "extsb #{dst}, #{src}"
            ppc64ZeroExtend32(dst)

        when "sxh2q", "sxh2p"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "extsh #{dst}, #{src}"

        when "sxh2i"
            # movswl semantics: sign-extend half to 32, zero-extend to 64.
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "extsh #{dst}, #{src}"
            ppc64ZeroExtend32(dst)

        # ------------------------------------------------------------------
        # Loads
        # ------------------------------------------------------------------
        when "loadp", "loadq"
            # 64-bit load (DS-form, offset must be 4-aligned)
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            if addr.is_a?(Address)
                ppc64ValidateDSOffset(addr.offset.value, codeOriginString)
                $asm.puts "ld #{dst}, #{addr.ppc64Operand}"
            else
                raise "ppc64: loadp/loadq requires Address operand at #{codeOriginString}"
            end

        when "loadi", "loadis"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            if addr.is_a?(Address)
                if opcode == "loadis"
                    # sign-extend 32-bit (DS-form)
                    ppc64ValidateDSOffset(addr.offset.value, codeOriginString)
                    $asm.puts "lwa #{dst}, #{addr.ppc64Operand}"
                else
                    # zero-extend 32-bit (D-form, any 16-bit offset)
                    ppc64ValidateDOffset(addr.offset.value, codeOriginString)
                    $asm.puts "lwz #{dst}, #{addr.ppc64Operand}"
                end
            else
                raise "ppc64: loadi/loadis requires Address operand at #{codeOriginString}"
            end

        when "load8", "loadb"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lbz #{dst}, #{addr.ppc64Operand}"

        when "loadbs", "load8SignedExtendTo32", "loadbsi"
            # movsbl semantics: sign-extend byte to 32, zero-extend to 64
            # (loadbsq below keeps the full 64-bit sign extension).
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lbz #{dst}, #{addr.ppc64Operand}"
            $asm.puts "extsb #{dst}, #{dst}"
            ppc64ZeroExtend32(dst)

        when "loadbsq"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lbz #{dst}, #{addr.ppc64Operand}"
            $asm.puts "extsb #{dst}, #{dst}"

        when "load16", "loadh"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lhz #{dst}, #{addr.ppc64Operand}"

        when "loadhs", "load16SignedExtendTo32", "loadhsi"
            # movswl semantics: lha sign-extends to 64, so clear the upper
            # word (loadhsq below keeps the full 64-bit sign extension).
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lha #{dst}, #{addr.ppc64Operand}"
            ppc64ZeroExtend32(dst)

        when "loadhsq"
            # lha sign-extends 16-bit to 64-bit in 64-bit mode
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "lha #{dst}, #{addr.ppc64Operand}"

        # ------------------------------------------------------------------
        # Stores
        # ------------------------------------------------------------------
        when "storep", "storeq"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            if addr.is_a?(Address)
                ppc64ValidateDSOffset(addr.offset.value, codeOriginString)
                $asm.puts "std #{src}, #{addr.ppc64Operand}"
            else
                raise "ppc64: storep/storeq requires Address operand at #{codeOriginString}"
            end

        when "storei"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "stw #{src}, #{addr.ppc64Operand}"

        when "store8", "storeb"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "stb #{src}, #{addr.ppc64Operand}"

        when "store16", "storeh"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            ppc64ValidateDOffset(addr.offset.value, codeOriginString)
            $asm.puts "sth #{src}, #{addr.ppc64Operand}"

        # ------------------------------------------------------------------
        # Load-linked / store-conditional (LL/SC), used by the IPInt/LLInt
        # weakCASExchange* macros.  Mirrors MacroAssemblerPPC64.h's
        # loadLinkAcq{8,16,32,64} / storeCondRel{8,16,32,64}.
        #
        # offlineasm operand contract (must match arm64.rb, which is the
        # reference lowering — see arm64.rb "loadlinkacq*"/"storecondrel*"):
        #   loadlinkacq<w>  [mem], dest          → ldaxr{b,h,,} dest, [mem]
        #   storecondrel<w> result, value, [mem] → stlxr{b,h,,} result, value, [mem]
        # i.e. the result register comes FIRST on the store, and `result` is
        # 0 on SUCCESS, non-zero when the reservation was lost.
        #
        # PPC has no offset form of the reserve/conditional-store primitives:
        # they are X-form indexed (RA|0, RB), so we emit rA=0 (literal zero,
        # not r0's contents) and put the address in rB.  The .asm callers all
        # use `[reg]` with a zero displacement; a non-zero displacement would
        # need a scratch register we do not have at lowering time, so reject
        # it loudly (arm64.rb's arm64SimpleAddressOperand does the same).
        #
        # Ordering.  The contract is ARM64's ldaxr/stlxr, which are RCsc: an
        # earlier store-release is ordered before a later load-acquire, so
        # atomics are sequentially consistent with each other.  lwsync alone
        # cannot give that -- it never orders a store before a later load --
        # so a Dekker pattern (atomic store A; atomic RMW on B) could
        # reorder.  Hence a full `sync` (hwsync) BEFORE every load-reserve,
        # the leading-sync seq_cst mapping also used by C++11 compilers on
        # Power and by MacroAssemblerPPC64's branchAtomicWeakCAS; `lwsync`
        # after the load-reserve gives acquire, and `lwsync` before the
        # store-conditional gives release.  The
        # sync sits inside the retry loop (offlineasm lowers per
        # instruction), which costs time on contention but not correctness:
        # neither sync nor lwsync clears a reservation.
        # All of these are in the POWER8 baseline.
        # ------------------------------------------------------------------
        when "loadlinkacqb", "loadlinkacqh", "loadlinkacqi", "loadlinkacqq"
            addr = ppc64ReservationAddress(operands[0], opcode, codeOriginString)
            dst  = operands[1].ppc64Operand
            larx = case opcode
                   when "loadlinkacqb" then "lbarx"
                   when "loadlinkacqh" then "lharx"
                   when "loadlinkacqi" then "lwarx"
                   else                     "ldarx"
                   end
            # l{b,h,w}arx zero-extend into the full 64-bit register, matching
            # ARM64's ldaxr{b,h} Wt writes — the callers compare the result
            # against a 64-bit `expected` with bqneq, so this matters.
            $asm.puts "sync"                              # order prior stores (RCsc)
            $asm.puts "#{larx} #{dst}, 0, #{addr}"
            $asm.puts "lwsync"                            # acquire

        when "storecondrelb", "storecondrelh", "storecondreli", "storecondrelq"
            dst  = operands[0].ppc64Operand             # result: 0 = success
            src  = operands[1].ppc64Operand             # value to store
            addr = ppc64ReservationAddress(operands[2], opcode, codeOriginString)
            stcx = case opcode
                   when "storecondrelb" then "stbcx."
                   when "storecondrelh" then "sthcx."
                   when "storecondreli" then "stwcx."
                   else                      "stdcx."
                   end
            $asm.puts "lwsync"                            # release
            $asm.puts "#{stcx} #{src}, 0, #{addr}"
            # st*cx. sets CR0[EQ]=1 on success; the offlineasm contract is
            # 0 on success.  mfcr zeroes the upper word and rlwinm clears
            # bits 0:31, so the result is already properly zero-extended.
            $asm.puts "mfcr #{dst}"
            $asm.puts "rlwinm #{dst}, #{dst}, 3, 31, 31"  # CR0[EQ] (bit 29) → bit 31
            $asm.puts "xori #{dst}, #{dst}, 1"            # 0 = success, 1 = reservation lost

        # ------------------------------------------------------------------
        # Load effective address
        # leap base + offset, dest
        # ------------------------------------------------------------------
        when "leap"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            if addr.is_a?(Address)
                off = addr.offset.value
                base = addr.base.ppc64Operand
                if (-32768..32767).include?(off)
                    $asm.puts "addi #{dst}, #{base}, #{off}"
                else
                    raise "ppc64: leap offset #{off} out of 16-bit range at #{codeOriginString}"
                end
            elsif addr.is_a?(LabelReference)
                # Load the symbol's address from its GOT entry.
                # Using @toc@ha+@toc@l(rD) with `ld` would compute an offset to
                # the SYMBOL itself (DS-form load FROM the symbol), which only
                # works if the symbol is within ±2 GB of TOC AND we want its
                # first 8 bytes — not what `leap _g_config, dst` means.
                # @got@ha+@got@l(rD) with `ld` makes the linker create a GOT
                # entry containing the symbol's address; the load reads that
                # entry, giving us &symbol regardless of where the symbol lives.
                lbl = addr.asmLabel
                $asm.puts "addis #{dst}, 2, #{lbl}@got@ha"
                $asm.puts "ld #{dst}, #{lbl}@got@l(#{dst})"
                if addr.offset != 0
                    $asm.puts "addi #{dst}, #{dst}, #{addr.offset}"
                end
            else
                raise "ppc64: leap requires Address or LabelReference operand at #{codeOriginString}"
            end

        # ------------------------------------------------------------------
        # Stack: push and pop
        #
        # push reg           — single register (8-byte, may temporarily misalign)
        # push reg1, reg2    — paired push (16-byte, maintains alignment)
        #   "push cfr, lr"  — the common prologue form on PPC64:
        #       mflr 0 ; stdu r1, -16(r1) ; std r31, 0(r1) ; std r0, 8(r1)
        #   "push regA, regB" (no lr):
        #       stdu r1, -16(r1) ; std regA, 0(r1) ; std regB, 8(r1)
        #
        # pop reg1, reg2     — paired pop (reverse of push)
        # pop reg            — single register
        #
        # Frame layout after "push cfr, lr; move sp, cfr":
        #   cfr[0]  = old cfr  (CallerFrameAndPC::callerFrame)
        #   cfr[8]  = saved lr (CallerFrameAndPC::returnPC)
        # ------------------------------------------------------------------
        when "push"
            if operands.length > 2 && operands.length.even?
                # `push a, b, c, d` = `push a, b` then `push c, d`, exactly as
                # arm64.rb's each_slice(2) stp sequence lays it out (c at sp+0,
                # d at sp+8, a at sp+16, b at sp+24); the matching
                # `pop d, c, b, a` below undoes it pair by pair.
                operands.each_slice(2) { |pair|
                    Instruction.new(codeOrigin, "push", pair, annotation).lowerPPC64
                }
            elsif operands.length == 1
                reg = operands[0]
                if reg.is_a?(RegisterID) && reg.name == "lr"
                    # push lr: save lr to r0, allocate 8 bytes, store
                    $asm.puts "mflr 0"
                    $asm.puts "stdu 1, -8(1)"
                    $asm.puts "std 0, 0(1)"
                else
                    $asm.puts "stdu 1, -8(1)"
                    $asm.puts "std #{reg.ppc64Operand}, 0(1)"
                end
            elsif operands.length == 2
                r1op = operands[0]
                r2op = operands[1]
                lr1  = r1op.is_a?(RegisterID) && r1op.name == "lr"
                lr2  = r2op.is_a?(RegisterID) && r2op.name == "lr"
                if lr1 || lr2
                    # One of the operands is lr; save it into r0 first.
                    $asm.puts "mflr 0"
                    $asm.puts "stdu 1, -16(1)"
                    $asm.puts "std #{lr1 ? '0' : r1op.ppc64Operand}, 0(1)"
                    $asm.puts "std #{lr2 ? '0' : r2op.ppc64Operand}, 8(1)"
                else
                    $asm.puts "stdu 1, -16(1)"
                    $asm.puts "std #{r1op.ppc64Operand}, 0(1)"
                    $asm.puts "std #{r2op.ppc64Operand}, 8(1)"
                end
            else
                raise "ppc64: push with #{operands.length} operands not supported"
            end

        when "pop"
            if operands.length > 2 && operands.length.even?
                # Inverse of the multi-operand push above (arm64 each_slice(2)).
                operands.each_slice(2) { |pair|
                    Instruction.new(codeOrigin, "pop", pair, annotation).lowerPPC64
                }
            elsif operands.length == 1
                reg = operands[0]
                if reg.is_a?(RegisterID) && reg.name == "lr"
                    $asm.puts "ld 0, 0(1)"
                    $asm.puts "addi 1, 1, 8"
                    $asm.puts "mtlr 0"
                else
                    $asm.puts "ld #{reg.ppc64Operand}, 0(1)"
                    $asm.puts "addi 1, 1, 8"
                end
            elsif operands.length == 2
                r1op = operands[0]
                r2op = operands[1]
                lr1  = r1op.is_a?(RegisterID) && r1op.name == "lr"
                lr2  = r2op.is_a?(RegisterID) && r2op.name == "lr"
                # pop R1, R2: load R1 from [sp+8], load R2 from [sp+0], sp += 16
                # (Reverse of ARM64 ldp: R1 gets [sp+8], R2 gets [sp+0])
                # This restores "push cfr, lr": cfr from [sp+0], lr from [sp+8]
                # Note: pop is reversed relative to push per offlineasm convention.
                slot0 = r2op  # r2op was pushed to [sp+0]
                slot8 = r1op  # r1op was pushed to [sp+8]
                if slot0.is_a?(RegisterID) && slot0.name == "lr"
                    $asm.puts "ld 0, 0(1)"
                else
                    $asm.puts "ld #{slot0.ppc64Operand}, 0(1)"
                end
                if slot8.is_a?(RegisterID) && slot8.name == "lr"
                    $asm.puts "ld 0, 8(1)"
                else
                    $asm.puts "ld #{slot8.ppc64Operand}, 8(1)"
                end
                $asm.puts "addi 1, 1, 16"
                if slot0.is_a?(RegisterID) && slot0.name == "lr"
                    $asm.puts "mtlr 0"
                elsif slot8.is_a?(RegisterID) && slot8.name == "lr"
                    $asm.puts "mtlr 0"
                end
            else
                raise "ppc64: pop with #{operands.length} operands not supported"
            end

        # ------------------------------------------------------------------
        # Stack: 128-bit vector push and pop
        #
        # `pushv f` / `popv f` move a whole 16-byte slot, matching x86
        # (sub $16,%rsp ; movdqu %xmm,(%rsp)) and arm64 (str q, [sp,#-16]!).
        # Both backends place the scalar half of the register at sp+0, and
        # IPInt's pushFloat32/pushFloat64/pushVec all feed this slot, so the
        # PPC form must land the FPR-visible doubleword at sp+0 too.
        #
        # VSR n aliases FPR n (VSX: the FPR is doubleword 0 of the VSR), and
        # lxvd2x/stxvd2x transfer doubleword 0 to/from EA+0 with only
        # per-doubleword byte reversal under LE — so `stxvd2x n, 0, 1` after
        # `addi 1, 1, -16` writes the FPR value at sp+0 and preserves the
        # other 8 bytes.
        #
        # stxvd2x/lxvd2x tolerate misalignment (sp stays 16-byte aligned
        # here regardless).  RA is written as literal 0 — the (RA|0) rule
        # makes that "no base register", not r0.
        # ------------------------------------------------------------------
        #
        # VecRegisterID operands (v0..v7 → VSR 32+, see class VecRegisterID)
        # take the same path: IPInt uses them as a pure 16-byte transport
        # (local.get/set/tee, global.get/set, select), and a stxvd2x/lxvd2x
        # pair is bit-exact for any 16 bytes because the per-doubleword
        # byte swap is undone on the way back.  The EA+0 doubleword is VSR
        # doubleword 0 for VSR 32+ exactly as for VSR 0-31, so a slot moved
        # through a v register and then read with lfd/ld at +0 sees the
        # same 8 bytes.
        when "pushv"
            operands.each { | op |
                raise "ppc64: pushv expects an FPR/vector register, got #{op.class} #{op.respond_to?(:name) ? op.name : op.inspect} at #{codeOriginString}" unless op.is_a?(FPRegisterID) or op.is_a?(VecRegisterID) or op.is_a?(SpecialRegister)
                $asm.puts "addi 1, 1, -16"
                $asm.puts "stxvd2x #{op.ppc64Operand}, 0, 1"
            }

        when "popv"
            operands.each { | op |
                raise "ppc64: popv expects an FPR/vector register, got #{op.class} #{op.respond_to?(:name) ? op.name : op.inspect} at #{codeOriginString}" unless op.is_a?(FPRegisterID) or op.is_a?(VecRegisterID) or op.is_a?(SpecialRegister)
                $asm.puts "lxvd2x #{op.ppc64Operand}, 0, 1"
                $asm.puts "addi 1, 1, 16"
            }

        # ------------------------------------------------------------------
        # 128-bit load/store: loadv addr, vreg / storev vreg, addr
        #
        # Same 16-byte transport as pushv/popv, at an arbitrary address.
        # ppc64LowerMalformedAddresses has already folded any displacement
        # or index into the base, so only Address(base, 0) reaches here,
        # emitted as the X-form `lxvd2x XT, 0, base` (RA=0 means "no
        # register", EA = (base)).  The register operand may be a v register
        # (VSR 32+) or an FPR (VSR 0-31; mINT/argumINT move float argument
        # registers this way), never anything else.
        # ------------------------------------------------------------------
        when "loadv", "storev"
            isLoad = (opcode == "loadv")
            reg  = isLoad ? operands[1] : operands[0]
            addr = isLoad ? operands[0] : operands[1]
            unless reg.is_a?(VecRegisterID) or reg.is_a?(FPRegisterID)
                raise "ppc64: #{opcode} expects a vector/FPR register, got #{reg.class} at #{codeOriginString}"
            end
            unless addr.is_a?(Address) && addr.offset.value == 0
                raise "ppc64: #{opcode} address must be lowered to [reg] first, got #{addr.class} at #{codeOriginString}"
            end
            $asm.puts "#{isLoad ? 'lxvd2x' : 'stxvd2x'} #{reg.ppc64Operand}, 0, #{addr.base.ppc64Operand}"

        # ------------------------------------------------------------------
        # Control flow
        # ------------------------------------------------------------------
        when "ret"
            $asm.puts "blr"

        when "jmp"
            op = operands[0]
            if op.is_a?(RegisterID) || op.is_a?(SpecialRegister)
                # Tail call.  If the target is a C function (e.g.
                # `jmp a3, CustomAccessorPtrTag`), its GEP computes the
                # callee's TOC from r12 — set it or the callee runs with a
                # garbage r2.  (The eventual ld-2-from-24(r1) is the ORIGINAL
                # caller's duty; a tail call doesn't return here.)  Only do
                # this for C-classified tags: r12 is ws1 in our register map
                # and may hold a live value on JS/wasm jump paths.
                # Unlike `call`, unknown jmp tags stay on the plain path: jmp
                # targets (exception handlers, dispatch, gates) are same-module
                # asm labels that need no GEP.
                tagValue = (operands.length > 1 && operands[1].is_a?(Immediate)) ? operands[1].value : nil
                tailCName = tagValue ? PPC64_C_CALL_TAG_NAMES.find { |n| $ppc64ConstExprValues[n] == tagValue } : nil
                if tailCName
                    $asm.comment "tail call C (#{tailCName})"
                    $asm.puts "mr 12, #{op.ppc64Operand}"
                    $asm.puts "mtctr 12"
                else
                    $asm.puts "mtctr #{op.ppc64Operand}"
                end
                $asm.puts "bctr"
            elsif op.is_a?(LabelReference) || op.is_a?(LocalLabelReference)
                $asm.puts "b #{op.asmLabel}"
            else
                raise "ppc64: jmp operand type #{op.class} not supported at #{codeOriginString}"
            end

        when "call"
            op = operands[0]
            # Classify by PtrTag (symbolically, via the captured constexpr
            # map — see ppc64ClassifyCallTag above).  JS-entry tags → bare
            # bctrl; C tags and untagged register calls → full ELFv2 stanza.
            tagValue = (operands.length > 1 && operands[1].is_a?(Immediate)) ? operands[1].value : nil
            kind, tagName = ppc64ClassifyCallTag(tagValue, codeOriginString)
            if op.is_a?(RegisterID) || op.is_a?(SpecialRegister)
                $asm.comment "call #{kind}#{tagName ? " (#{tagName})" : ""}"
                # The caller sets r12 = callee entry so the
                # callee's GEP can compute its TOC (harmless for JS callees).
                if kind == :c
                    # C callee follows full ABI: it'll write lr at caller_sp+16
                    # (need linkage area) and may clobber r2 (need save/restore
                    # — host functions live in the jsc binary, a different
                    # module with a different TOC).
                    $asm.puts "stdu 1, -32(1)"
                    $asm.puts "std 2, 24(1)"
                    $asm.puts "mr 12, #{op.ppc64Operand}"
                    $asm.puts "mtctr 12"
                    $asm.puts "bctrl"
                    $asm.puts "ld 2, 24(1)"
                    $asm.puts "addi 1, 1, 32"
                else
                    # JS callee: makeJavaScriptCall-style sp dance is already
                    # arranged by the caller; an extra stdu would shift the
                    # callee's cfr off and the codeBlock/callee/argCount slots
                    # would be read from wrong offsets.  Skip frame allocation.
                    $asm.puts "mr 12, #{op.ppc64Operand}"
                    $asm.puts "mtctr 12"
                    $asm.puts "bctrl"
                end
            elsif op.is_a?(LocalLabelReference)
                # A local label is an asm subroutine in the same function body
                # (e.g. JSPI's .jspi_execute_evacuated_slice), written for ARM64
                # `bl` semantics: sp unchanged at entry, the callee's
                # functionPrologue pushes below it. It is never a C function, so
                # it does not write the ELFv2 linkage area, and code like the
                # JSPI sentinel frame relies on sp being exactly what the caller
                # left ("the sentinel must be right below the
                # PinballHandlerContext"). A bare bl.
                $asm.puts "bl #{op.asmLabel}"
            elsif op.is_a?(LabelReference)
                # Direct call to a C function (slow path / runtime helper).
                # The ELFv2 callee writes its saved lr at 16(r1) of
                # the CALLER's sp.  We must reserve at least 32 bytes of
                # "linkage area" so that store does not clobber caller data.
                # This matches GCC's `stdu 1,-32(1); bl ...; addi 1,1,32`.
                $asm.puts "stdu 1, -32(1)"
                $asm.puts "bl #{op.asmLabel}"
                $asm.puts "addi 1, 1, 32"
            else
                raise "ppc64: call operand type #{op.class} not supported at #{codeOriginString}"
            end

        # ------------------------------------------------------------------
        # PC-relative address load (for dispatch table setup)
        #
        # pcrtoaddr label, dest:
        #   mflr 0           # save return address
        #   bl 1f             # LR = address of 1: (next instruction)
        #   1: mflr dest      # dest = address of 1:
        #   mtlr 0           # restore return address
        #   addis dest, dest, (label - 1b)@ha
        #   addi dest, dest, (label - 1b)@l
        # ------------------------------------------------------------------
        when "pcrtoaddr"
            lbl  = operands[0].asmLabel
            dest = operands[1].ppc64Operand
            $asm.puts "mflr 0"
            $asm.puts "bl 1f"
            $asm.puts "1:"
            $asm.puts "mflr #{dest}"
            $asm.puts "mtlr 0"
            $asm.puts "addis #{dest}, #{dest}, (#{lbl} - 1b)@ha"
            $asm.puts "addi #{dest}, #{dest}, (#{lbl} - 1b)@l"

        # globaladdr label, dest — like pcrtoaddr but for globally visible symbols
        when "globaladdr"
            lbl  = operands[0].asmLabel
            dest = operands[1].ppc64Operand
            $asm.puts "mflr 0"
            $asm.puts "bl 1f"
            $asm.puts "1:"
            $asm.puts "mflr #{dest}"
            $asm.puts "mtlr 0"
            $asm.puts "addis #{dest}, #{dest}, (#{lbl} - 1b)@ha"
            $asm.puts "addi #{dest}, #{dest}, (#{lbl} - 1b)@l"

        # ------------------------------------------------------------------
        # Branch-on-test-bit-zero / non-zero
        # btiz  src, label  — branch if src == 0 (test integer zero)
        # btinz src, label  — branch if src != 0
        # btpz  src, label  — pointer-width zero test
        # btpnz src, label
        # btqz  src, label  — quad-word zero test
        # btqnz src, label
        # btz   src, label  — generic zero test (treat as btpz)
        # btnz  src, label  — generic non-zero test
        # ------------------------------------------------------------------
        when "btiz", "btpz", "btqz", "btz"
            reg = operands[0].ppc64Operand
            lbl = operands[1].asmLabel
            $asm.puts "cmpdi #{reg}, 0"
            ppc64EmitLongBranch("beq", lbl)

        when "btinz", "btpnz", "btqnz", "btnz"
            reg = operands[0].ppc64Operand
            lbl = operands[1].asmLabel
            $asm.puts "cmpdi #{reg}, 0"
            ppc64EmitLongBranch("bne", lbl)

        # Test bit N: btbz reg, imm, label — branch if bit N of reg is zero
        when "btbz"
            reg = operands[0].ppc64Operand
            bit = operands[1].value
            lbl = operands[2].asmLabel
            $asm.puts "rldicl. 0, #{reg}, 64 - #{bit}, 63"
            ppc64EmitLongBranch("beq", lbl)

        when "btbnz"
            reg = operands[0].ppc64Operand
            bit = operands[1].value
            lbl = operands[2].asmLabel
            $asm.puts "rldicl. 0, #{reg}, 64 - #{bit}, 63"
            ppc64EmitLongBranch("bne", lbl)

        # Test signed bit (same as btbz/btbnz for PPC64):
        when "btbiz"
            reg = operands[0].ppc64Operand
            bit = operands[1].value
            lbl = operands[2].asmLabel
            $asm.puts "rlwinm. 0, #{reg}, 0, #{bit}, #{bit}"
            ppc64EmitLongBranch("beq", lbl)

        when "btbinz"
            reg = operands[0].ppc64Operand
            bit = operands[1].value
            lbl = operands[2].asmLabel
            $asm.puts "rlwinm. 0, #{reg}, 0, #{bit}, #{bit}"
            ppc64EmitLongBranch("bne", lbl)

        # ------------------------------------------------------------------
        # Integer compare-and-branch
        # b{i,q,p}{eq,neq,lt,gt,le,ge} src1, src2, label
        # signed variants: bilt, bigt, bile, bige
        # unsigned: bult, bugt, bule, buge (use cmpldi)
        # b{i,q,p}beq, b{i,q,p}baeq — above/below (unsigned)
        # ------------------------------------------------------------------
        when "bieq", "bbeq"
            ppc64EmitCompareAndBranch(:eq, :i, true, operands)
        when "bineq", "bbneq"
            ppc64EmitCompareAndBranch(:neq, :i, true, operands)
        when "bilt", "bblt"
            ppc64EmitCompareAndBranch(:lt, :i, true, operands)
        when "bigt", "bbgt"
            ppc64EmitCompareAndBranch(:gt, :i, true, operands)
        when "bile", "bilteq", "bblteq"
            ppc64EmitCompareAndBranch(:le, :i, true, operands)
        when "bige", "bigteq", "bbgteq"
            ppc64EmitCompareAndBranch(:ge, :i, true, operands)
        when "bult", "bib", "bbb"
            ppc64EmitCompareAndBranch(:lt, :i, false, operands)
        when "bugt", "bia", "bba"
            ppc64EmitCompareAndBranch(:gt, :i, false, operands)
        when "bule", "bibe", "bibeq", "bbbeq"
            ppc64EmitCompareAndBranch(:le, :i, false, operands)
        when "buge", "biaeq", "bbaeq"
            ppc64EmitCompareAndBranch(:ge, :i, false, operands)

        when "bqeq", "bpeq"
            ppc64EmitCompareAndBranch(:eq, :q, true, operands)
        when "bqneq", "bpneq"
            ppc64EmitCompareAndBranch(:neq, :q, true, operands)
        when "bqlt", "bplt"
            ppc64EmitCompareAndBranch(:lt, :q, true, operands)
        when "bqgt", "bpgt"
            ppc64EmitCompareAndBranch(:gt, :q, true, operands)
        when "bqle", "bple", "bqlteq", "bplteq"
            ppc64EmitCompareAndBranch(:le, :q, true, operands)
        when "bqge", "bpge", "bqgteq", "bpgteq"
            ppc64EmitCompareAndBranch(:ge, :q, true, operands)
        when "bqult", "bpult", "bqb", "bpb"
            ppc64EmitCompareAndBranch(:lt, :q, false, operands)
        when "bqugt", "bpugt", "bqa", "bpa"
            ppc64EmitCompareAndBranch(:gt, :q, false, operands)
        when "bqule", "bpule", "bqbe", "bpbe", "bpbeq", "bqbeq"
            ppc64EmitCompareAndBranch(:le, :q, false, operands)
        when "bquge", "bpuge", "bqaeq", "bpaeq"
            ppc64EmitCompareAndBranch(:ge, :q, false, operands)

        when "bbieq", "bbbeq", "bbaeq"
            ppc64EmitCompareAndBranch(:eq, :b, true, operands)
        when "bbineq", "bbbneq", "bbaneq"
            ppc64EmitCompareAndBranch(:neq, :b, true, operands)

        # ------------------------------------------------------------------
        # Add-with-branch-on-overflow
        # ------------------------------------------------------------------
        when "baddis", "baddio", "baddps", "baddqs", "baddpo", "baddqo",
             "bsubis", "bsubio", "bsubps", "bsubqs", "bsubpo", "bsubqo"
            # All register/immediate forms are expanded by
            # ppc64LowerOverflowBranches; baddis-with-Address by the rmw
            # expansion in ppc64LowerMalformedAddresses.  Reaching here means
            # an operand shape neither pass claims — fail loudly rather than
            # emit a wrong XER-based sequence.
            raise "ppc64: unlowered overflow/sign branch #{opcode} at #{codeOriginString}"

        when "baddinz", "baddiz"
            # 32-bit: CR0 must reflect the low word only.  The dot form (add.)
            # compares the full 64-bit register, which is wrong once bit 32
            # carries (e.g. lwz-loaded 0xFFFFFFFF + 1: low word is zero but
            # the 64-bit result is 0x100000000).  Use plain add + cmpwi.
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addi #{dst}, #{dst}, #{src.value}"
            else
                $asm.puts "add #{dst}, #{dst}, #{src.ppc64Operand}"
            end
            # Match x86 addl / arm64 adds Wn: 32-bit result is zero-extended.
            $asm.puts "rldicl #{dst}, #{dst}, 0, 32"
            $asm.puts "cmpwi #{dst}, 0"
            ppc64EmitLongBranch(opcode == "baddiz" ? "beq" : "bne", lbl)

        when "baddpnz", "baddqnz"
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addic. #{dst}, #{dst}, #{src.value}"
            else
                $asm.puts "add. #{dst}, #{dst}, #{src.ppc64Operand}"
            end
            ppc64EmitLongBranch("bne", lbl)

        when "baddpz", "baddqz"
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addic. #{dst}, #{dst}, #{src.value}"
            else
                $asm.puts "add. #{dst}, #{dst}, #{src.ppc64Operand}"
            end
            ppc64EmitLongBranch("beq", lbl)

        # ------------------------------------------------------------------
        # Subtract-with-branch
        # bsubinz src, dst, label  →  dst -= src; branch if dst != 0
        # bsubiz  src, dst, label  →  dst -= src; branch if dst == 0
        # (pointer/quad variants are same as int on 64-bit)
        # ------------------------------------------------------------------
        when "bsubinz", "bsubiz"
            # 32-bit: plain sub + cmpwi (see baddinz note on dot-form vs low word).
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addi #{dst}, #{dst}, #{-src.value}"
            else
                $asm.puts "subf #{dst}, #{src.ppc64Operand}, #{dst}"
            end
            # Match x86 subl: 32-bit result is zero-extended.
            $asm.puts "rldicl #{dst}, #{dst}, 0, 32"
            $asm.puts "cmpwi #{dst}, 0"
            ppc64EmitLongBranch(opcode == "bsubiz" ? "beq" : "bne", lbl)

        when "bsubpnz", "bsubqnz"
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addic. #{dst}, #{dst}, #{-src.value}"
            else
                $asm.puts "subf. #{dst}, #{src.ppc64Operand}, #{dst}"
            end
            ppc64EmitLongBranch("bne", lbl)

        when "bsubpz", "bsubqz"
            src = operands[0]
            dst = operands[1].ppc64Operand
            lbl = operands[2].asmLabel
            if src.is_a?(Immediate)
                $asm.puts "addic. #{dst}, #{dst}, #{-src.value}"
            else
                $asm.puts "subf. #{dst}, #{src.ppc64Operand}, #{dst}"
            end
            ppc64EmitLongBranch("beq", lbl)

        # ------------------------------------------------------------------
        # Conditional set: c{i,p,q,b}{eq,neq,lt,gt,lteq,gteq,a,b,aeq,beq}
        # Sets dst = 1 if condition true, 0 otherwise.
        # Uses mfcr + rlwinm bit extraction (no scratch register).
        # ------------------------------------------------------------------
        when "cieq", "cbeq", "cpeq", "cqeq"
            sz = (opcode == "cieq" || opcode == "cbeq") ? :i : :q
            ppc64EmitConditionalSet(:eq, sz, true, operands)
        when "cineq", "cbneq", "cpneq", "cqneq"
            sz = (opcode == "cineq" || opcode == "cbneq") ? :i : :q
            ppc64EmitConditionalSet(:neq, sz, true, operands)
        when "cilt", "cblt", "cplt", "cqlt"
            sz = (opcode == "cilt" || opcode == "cblt") ? :i : :q
            ppc64EmitConditionalSet(:lt, sz, true, operands)
        when "cigt", "cbgt", "cpgt", "cqgt"
            sz = (opcode == "cigt" || opcode == "cbgt") ? :i : :q
            ppc64EmitConditionalSet(:gt, sz, true, operands)
        when "cilteq", "cblteq", "cplteq", "cqlteq"
            sz = (opcode == "cilteq" || opcode == "cblteq") ? :i : :q
            ppc64EmitConditionalSet(:le, sz, true, operands)
        when "cigteq", "cbgteq", "cpgteq", "cqgteq"
            sz = (opcode == "cigteq" || opcode == "cbgteq") ? :i : :q
            ppc64EmitConditionalSet(:ge, sz, true, operands)
        # Unsigned variants
        when "cib", "cbb", "cpb", "cqb"
            sz = (opcode == "cib" || opcode == "cbb") ? :i : :q
            ppc64EmitConditionalSet(:lt, sz, false, operands)
        when "cia", "cba", "cpa", "cqa"
            sz = (opcode == "cia" || opcode == "cba") ? :i : :q
            ppc64EmitConditionalSet(:gt, sz, false, operands)
        when "cibeq", "cbbeq", "cpbeq", "cqbeq"
            sz = (opcode == "cibeq" || opcode == "cbbeq") ? :i : :q
            ppc64EmitConditionalSet(:le, sz, false, operands)
        when "ciaeq", "cbaeq", "cpaeq", "cqaeq"
            sz = (opcode == "ciaeq" || opcode == "cbaeq") ? :i : :q
            ppc64EmitConditionalSet(:ge, sz, false, operands)

        # FP conditional set — double
        when "cdlt"
            ppc64EmitFPConditionalSet(:lt, true, operands)
        when "cdgt"
            ppc64EmitFPConditionalSet(:gt, true, operands)
        when "cdeq"
            ppc64EmitFPConditionalSet(:eq, true, operands)
        when "cdlteq"
            ppc64EmitFPConditionalSet(:lteq, true, operands)
        when "cdgteq"
            ppc64EmitFPConditionalSet(:gteq, true, operands)
        when "cdneq"
            ppc64EmitFPConditionalSet(:neq, true, operands)
        when "cdnequn"
            ppc64EmitFPConditionalSet(:neq_un, true, operands)

        # FP conditional set — float
        when "cflt"
            ppc64EmitFPConditionalSet(:lt, false, operands)
        when "cfgt"
            ppc64EmitFPConditionalSet(:gt, false, operands)
        when "cfeq"
            ppc64EmitFPConditionalSet(:eq, false, operands)
        when "cflteq"
            ppc64EmitFPConditionalSet(:lteq, false, operands)
        when "cfgteq"
            ppc64EmitFPConditionalSet(:gteq, false, operands)
        when "cfneq"
            ppc64EmitFPConditionalSet(:neq, false, operands)
        when "cfnequn"
            ppc64EmitFPConditionalSet(:neq_un, false, operands)

        # ------------------------------------------------------------------
        # Floating-point operations (basic)
        #
        # Float (f32) values live in FPRs in DOUBLE format on PPC: lfs
        # widens on load, stfs narrows on store, and the A-form "s"
        # arithmetic (fadds/fsubs/fmuls/fdivs/fsqrts) takes double-format
        # operands and rounds the result to single precision.  So the *f
        # arithmetic must use the "s" forms -- fadd would round to double and
        # leave a value that is not a representable float in the register,
        # which a later comparison or conversion would observe.  This is
        # what MacroAssemblerPPC64::addFloat & co. emit too.
        #
        # f0 is reserved as the backend's FP scratch: no offlineasm FPR
        # maps to it (ft0-ft7 are f1-f8, csfr0-7 f14-f21, Tmps f9-f12) and
        # it is volatile in ELFv2.
        # ------------------------------------------------------------------
        when "addf", "addd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fadd#{opcode == "addf" ? "s" : ""} #{dst}, #{dst}, #{src}"

        when "subf", "subd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fsub#{opcode == "subf" ? "s" : ""} #{dst}, #{dst}, #{src}"

        when "mulf", "muld"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fmul#{opcode == "mulf" ? "s" : ""} #{dst}, #{dst}, #{src}"

        when "divf", "divd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fdiv#{opcode == "divf" ? "s" : ""} #{dst}, #{dst}, #{src}"

        when "absf", "absd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fabs #{dst}, #{src}"

        when "negf", "negd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fneg #{dst}, #{src}"

        when "sqrtf"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fsqrts #{dst}, #{src}"

        when "sqrtd"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fsqrt #{dst}, #{src}"

        when "loadf"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            $asm.puts "lfs #{dst}, #{addr.ppc64Operand}"

        when "loadd"
            addr = operands[0]
            dst  = operands[1].ppc64Operand
            $asm.puts "lfd #{dst}, #{addr.ppc64Operand}"

        when "storef"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            $asm.puts "stfs #{src}, #{addr.ppc64Operand}"

        when "stored"
            src  = operands[0].ppc64Operand
            addr = operands[1]
            $asm.puts "stfd #{src}, #{addr.ppc64Operand}"

        when "moved"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            $asm.puts "fmr #{dst}, #{src}"

        # Bitwise and/or on FP registers (f32/f64 min/max merge the sign of
        # +0/-0 this way).  xxland/xxlor operate on the full
        # VSR, and FPR n is doubleword 0 of VSR n, so the VSR number is the
        # FPR number.  This is correct for floats too: the double-format
        # image of a float keeps its sign in bit 0 and equal floats have
        # equal images, so and/or of two equal-magnitude images is the
        # image of the and/or'd float.
        when "andf", "andd", "orf", "ord"
            dst = operands[1].ppc64Operand
            src = operands[0].ppc64Operand
            ins = opcode.start_with?("and") ? "xxland" : "xxlor"
            $asm.puts "#{ins} #{dst}, #{dst}, #{src}"

        # double <-> float.  frsp rounds to single precision (the FPR keeps
        # the double-format image).  A float in an FPR already IS a double, so
        # widening is value-preserving with any move -- but fmr would also
        # preserve a signalling NaN, and x86 cvtss2sd, arm64 fcvt and wasm's
        # f64.promote_f32 (whose result must be an arithmetic NaN) all quiet
        # it.  frsp does exactly that: on a value that is already a float it
        # is exact (the input is representable), and it quiets SNaNs keeping
        # the payload.
        when "cd2f", "cf2d"
            $asm.puts "frsp #{operands[1].ppc64Operand}, #{operands[0].ppc64Operand}"

        # Round to integral value, result stays floating point.  frip/frim/
        # friz are ceil/floor/trunc.  "round" is offlineasm's
        # round-half-to-even (arm64 frintn, x86 roundss/sd $0); PPC's frin
        # rounds half AWAY from zero, so use xsrdpic instead, which rounds
        # per FPSCR[RN] -- round-to-nearest-even, which JSC never changes.
        # All four are exact for float inputs (an integer nearest a float is
        # itself a float).
        when "ceilf", "ceild"
            $asm.puts "frip #{operands[1].ppc64Operand}, #{operands[0].ppc64Operand}"
        when "floorf", "floord"
            $asm.puts "frim #{operands[1].ppc64Operand}, #{operands[0].ppc64Operand}"
        when "truncatef", "truncated"
            $asm.puts "friz #{operands[1].ppc64Operand}, #{operands[0].ppc64Operand}"
        when "roundf", "roundd"
            $asm.puts "xsrdpic #{operands[1].ppc64Operand}, #{operands[0].ppc64Operand}"

        # Raw bit moves between a GPR and a float (arm64 fmov Sd, Wn / Wd, Sn).
        # The FPR holds the double-format image, so these are conversions,
        # not copies: xscvspdpn/xscvdpspn convert between the
        # single-format word and the double image WITHOUT signalling, so NaN
        # payloads (including signalling NaNs) and denormals survive exactly.
        # xscvspdpn reads word 0 of the VSR, hence the sldi; mfvsrwz reads
        # word 1 and zero-extends, matching the 32-bit-write convention.
        # This is GCC -mcpu=power8's own memcpy(float<->uint32) sequence.
        # Scratch: r0 for fi2f, f0 (VSR 0) for ff2i.
        when "fi2f"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "sldi 0, #{src}, 32"
            $asm.puts "mtvsrd #{dst}, 0"
            $asm.puts "xscvspdpn #{dst}, #{dst}"
        when "ff2i"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "xscvdpspn 0, #{src}"
            $asm.puts "mfvsrwz #{dst}, 0"

        # FP -> integer, truncating toward zero (arm64 fcvtz{u,s}).  Callers
        # range-check first (wasm trunc traps, trunc_sat saturates
        # explicitly, as on x86), so only in-range values reach these and
        # the out-of-range behaviour of fcti* never matters.  The convert
        # happens in f0 so the source survives; the integer moves out with
        # mfvsrwz (zero-extending, 32-bit results) or mfvsrd (64-bit).
        # Float sources need nothing extra: the FPR already holds the double
        # image.
        when "truncatef2i", "truncated2i"
            $asm.puts "fctiwuz 0, #{operands[0].ppc64Operand}"
            $asm.puts "mfvsrwz #{operands[1].ppc64Operand}, 0"
        when "truncatef2is"
            $asm.puts "fctiwz 0, #{operands[0].ppc64Operand}"
            $asm.puts "mfvsrwz #{operands[1].ppc64Operand}, 0"
        when "truncatef2q", "truncated2q"
            $asm.puts "fctiduz 0, #{operands[0].ppc64Operand}"
            $asm.puts "mfvsrd #{operands[1].ppc64Operand}, 0"
        when "truncatef2qs", "truncated2qs"
            $asm.puts "fctidz 0, #{operands[0].ppc64Operand}"
            $asm.puts "mfvsrd #{operands[1].ppc64Operand}, 0"

        # Integer-to-float conversions.
        # Semantics per the ARM64 reference backend: the "s" suffix means
        # SIGNED source (scvtf), no suffix means UNSIGNED (ucvtf); "ci2*"
        # converts the low 32 bits ONLY (x86 cvtsi2sd Ed / arm64 scvtf Wn),
        # "cq2*" converts all 64.  The width part is critical: LLInt feeds
        # ci2ds boxed int32 JSValues whose upper bits hold the numberTag —
        # converting all 64 bits turns boxed 1 into -(2^49-1).
        #
        # mtvsrwa (sign-extend word), mtvsrwz (zero-extend word) and mtvsrd
        # move GPR→VSR directly; FPR f_n aliases VSR n, so fcfid* can consume
        # the result in place.
        when "ci2ds"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrwa #{dst}, #{src}"
            $asm.puts "fcfid #{dst}, #{dst}"
        when "ci2d"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrwz #{dst}, #{src}"
            $asm.puts "fcfid #{dst}, #{dst}"
        when "ci2fs"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrwa #{dst}, #{src}"
            $asm.puts "fcfids #{dst}, #{dst}"
        when "ci2f"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrwz #{dst}, #{src}"
            $asm.puts "fcfids #{dst}, #{dst}"
        when "cq2ds"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrd #{dst}, #{src}"
            $asm.puts "fcfid #{dst}, #{dst}"
        when "cq2d"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrd #{dst}, #{src}"
            $asm.puts "fcfidu #{dst}, #{dst}"
        when "cq2fs"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrd #{dst}, #{src}"
            $asm.puts "fcfids #{dst}, #{dst}"
        when "cq2f"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "mtvsrd #{dst}, #{src}"
            $asm.puts "fcfidus #{dst}, #{dst}"

        when "cd2i", "truncated2is"
            # Rewritten to ppc64_truncd2i (with an FPR Tmp) by
            # ppc64LowerMalformedAddresses; reaching here means the
            # rewrite pass was bypassed.
            raise "ppc64: unlowered #{opcode} at #{codeOriginString}"

        when "ppc64_truncd2i"
            # fctiwz: truncate toward zero to int32; the result lands in bits
            # 32:63 of the target FPR with bits 0:31 undefined.  mfvsrwz moves
            # exactly that word zero-extended — matching x86 cvttsd2si Ed /
            # arm64 fcvtzs Wn register semantics.
            src  = operands[0].ppc64Operand
            dst  = operands[1].ppc64Operand
            ftmp = operands[2].ppc64Operand
            $asm.puts "fctiwz #{ftmp}, #{src}"
            $asm.puts "mfvsrwz #{dst}, #{ftmp}"

        # Double-to-float truncation and double-to-double
        when "fd2q"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "stdu 1, -16(1)"
            $asm.puts "stfd #{src}, 0(1)"
            $asm.puts "ld #{dst}, 0(1)"
            $asm.puts "addi 1, 1, 16"

        when "fq2d"
            src = operands[0].ppc64Operand
            dst = operands[1].ppc64Operand
            $asm.puts "stdu 1, -16(1)"
            $asm.puts "std #{src}, 0(1)"
            $asm.puts "lfd #{dst}, 0(1)"
            $asm.puts "addi 1, 1, 16"

        # ------------------------------------------------------------------
        # FP compare and branch  (fcmpu cr0: LT=bit31, GT=bit30, EQ=bit29, FU=SO=bit28)
        # Ordered variants (no "un"): NaN must NOT trigger the branch.
        # Unordered variants ("un"/"equn"): NaN MUST trigger the branch.
        # ------------------------------------------------------------------

        # ordered equal: EQ=1 only when not NaN
        when "bdeq", "bfeq"
            ppc64EmitFPBranch(:eq, operands)

        # ordered not-equal: branch on LT or GT; NaN sets FU only → no branch
        when "bdneq", "bfneq"
            ppc64EmitFPBranch(:neq, operands)

        # not-equal or NaN: bne = NOT EQ covers both cases
        when "bdnequn", "bfnequn"
            ppc64EmitFPBranch(:neq_un, operands)

        # ordered: NaN → no branch (NaN sets FU not LT/GT/EQ)
        when "bdlt", "bflt"
            ppc64EmitFPBranch(:lt, operands)
        when "bdgt", "bfgt"
            ppc64EmitFPBranch(:gt, operands)

        # ordered ≤/≥: use blt+beq / bgt+beq so NaN (FU only) does not trigger
        when "bdlteq", "bflteq"
            ppc64EmitFPBranch(:lteq, operands)
        when "bdgteq", "bfgteq"
            ppc64EmitFPBranch(:gteq, operands)

        # unordered (NaN → branch): ble = NOT GT for ltequn; bge = NOT LT for gtequn
        when "bdltequn", "bfltequn"
            ppc64EmitFPBranch(:ltequn, operands)
        when "bdgtequn", "bfgtequn"
            ppc64EmitFPBranch(:gtequn, operands)

        # unordered lt/gt: simple condition + bso for NaN path
        when "bdltun", "bfltun"
            ppc64EmitFPBranch(:ltun, operands)
        when "bdgtun", "bfgtun"
            ppc64EmitFPBranch(:gtun, operands)

        # unordered equal (NaN → branch): bso + beq
        when "bdequn", "bfequn"
            fA = operands[0].ppc64Operand; fB = operands[1].ppc64Operand; l = operands[2].asmLabel
            $asm.puts "fcmpu cr0, #{fA}, #{fB}"
            ppc64EmitLongBranch("bso", l)   # NaN → branch
            ppc64EmitLongBranch("beq", l)   # equal → branch

        # ------------------------------------------------------------------
        # Misc
        # ------------------------------------------------------------------
        when "crash"
            $asm.puts ".int 0xbadbeef0"   # illegal instruction trap

        when "break"
            $asm.puts ".int 0xbadbeef0"

        when "unimplemented"
            $asm.puts ".int 0xbadbeef0"

        else
            lowerDefault
        end
    end
end
