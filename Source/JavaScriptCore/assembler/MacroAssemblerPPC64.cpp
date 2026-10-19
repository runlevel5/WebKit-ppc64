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

#include "config.h"
#include "MacroAssembler.h"

#if ENABLE(ASSEMBLER) && CPU(PPC64)

#include "Options.h"
#include "ProbeContext.h"
#include <wtf/InlineASM.h>

#if OS(LINUX)
#include <sys/auxv.h>
#endif

// AT_HWCAP2 bits for Power ISA 3.0 and 3.1 (asm/cputable.h's PPC_FEATURE2_ARCH_3_00 / _3_1).
#define PPC64_FEATURE2_ARCH_3_00 0x00800000
#define PPC64_FEATURE2_ARCH_3_1 0x00040000

namespace JSC {

unsigned MacroAssemblerPPC64::isaLevel()
{
    static const unsigned hardwareLevel = [] {
#if OS(LINUX)
        unsigned long hwcap2 = getauxval(AT_HWCAP2);
        if (hwcap2 & PPC64_FEATURE2_ARCH_3_1)
            return 310u;
        if (hwcap2 & PPC64_FEATURE2_ARCH_3_00)
            return 300u;
#endif
        return 207u;
    }();
    return std::min(hardwareLevel, Options::maximumPPC64ISALevel());
}

using namespace PPC64Registers;

#if COMPILER(GCC_COMPATIBLE)

extern "C" void SYSV_ABI ctiMasmProbeTrampoline();
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(ctiMasmProbeTrampoline, void, ());
JSC_ANNOTATE_JIT_OPERATION_PROBE(ctiMasmProbeTrampoline);

extern "C" void SYSV_ABI executeJSCJITProbe(Probe::State*) REFERENCED_FROM_ASM;

// Probe::State layout as numeric offsets for the handwritten assembly below,
// checked against offsetof() by static_asserts.
#define PTR_SIZE 8
#define PROBE_PROBE_FUNCTION_OFFSET (0 * PTR_SIZE)
#define PROBE_ARG_OFFSET (1 * PTR_SIZE)
#define PROBE_INITIALIZE_STACK_FUNCTION_OFFSET (2 * PTR_SIZE)
#define PROBE_INITIALIZE_STACK_ARG_OFFSET (3 * PTR_SIZE)

#define PROBE_FIRST_GPR_OFFSET (4 * PTR_SIZE)
#define PROBE_CPU_GPR_OFFSET(N) (PROBE_FIRST_GPR_OFFSET + (N) * PTR_SIZE)

// SPRs follow the 32 GPRs, in FOR_EACH_SP_REGISTER order: pc, cr, xer, lr, ctr.
#define PROBE_FIRST_SPR_OFFSET (PROBE_FIRST_GPR_OFFSET + 32 * PTR_SIZE)
#define PROBE_CPU_PC_OFFSET  (PROBE_FIRST_SPR_OFFSET + 0 * PTR_SIZE)
#define PROBE_CPU_CR_OFFSET  (PROBE_FIRST_SPR_OFFSET + 1 * PTR_SIZE)
#define PROBE_CPU_XER_OFFSET (PROBE_FIRST_SPR_OFFSET + 2 * PTR_SIZE)
#define PROBE_CPU_LR_OFFSET  (PROBE_FIRST_SPR_OFFSET + 3 * PTR_SIZE)
#define PROBE_CPU_CTR_OFFSET (PROBE_FIRST_SPR_OFFSET + 4 * PTR_SIZE)

// The FPR area holds the full 128-bit VSR 0-31 (v128_t), stored with
// stxvd2x, so a wasm v128 survives a probe and Probe::Context::vector()
// reads it in the JIT's lane layout; fpr() is doubleword 0, the scalar.
#define VECTOR_SIZE 16
#define PROBE_FIRST_FPR_OFFSET (PROBE_FIRST_SPR_OFFSET + 5 * PTR_SIZE)
#define PROBE_CPU_FPR_OFFSET(N) (PROBE_FIRST_FPR_OFFSET + (N) * VECTOR_SIZE)

#define PROBE_SIZE (PROBE_FIRST_FPR_OFFSET + 32 * VECTOR_SIZE)
#define PROBE_SAVED_RETURN_PC_OFFSET PROBE_SIZE
#define PROBE_ALIGNED_STACK_SIZE (PROBE_SIZE + PTR_SIZE)

#define PROBE_OFFSETOF(x) offsetof(struct Probe::State, x)
static_assert(PROBE_OFFSETOF(probeFunction) == PROBE_PROBE_FUNCTION_OFFSET);
static_assert(PROBE_OFFSETOF(arg) == PROBE_ARG_OFFSET);
static_assert(PROBE_OFFSETOF(initializeStackFunction) == PROBE_INITIALIZE_STACK_FUNCTION_OFFSET);
static_assert(PROBE_OFFSETOF(initializeStackArg) == PROBE_INITIALIZE_STACK_ARG_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.gprs[r0]) == PROBE_CPU_GPR_OFFSET(0));
static_assert(PROBE_OFFSETOF(cpu.gprs[r31]) == PROBE_CPU_GPR_OFFSET(31));
static_assert(PROBE_OFFSETOF(cpu.sprs[pc]) == PROBE_CPU_PC_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.sprs[cr]) == PROBE_CPU_CR_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.sprs[xer]) == PROBE_CPU_XER_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.sprs[lr]) == PROBE_CPU_LR_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.sprs[ctr]) == PROBE_CPU_CTR_OFFSET);
static_assert(PROBE_OFFSETOF(cpu.fprs.vectors[f0]) == PROBE_CPU_FPR_OFFSET(0));
static_assert(PROBE_OFFSETOF(cpu.fprs.vectors[f31]) == PROBE_CPU_FPR_OFFSET(31));
static_assert(sizeof(Probe::State) == PROBE_SIZE);
static_assert(!(PROBE_ALIGNED_STACK_SIZE & 0xf));

// probe() reserves this record and stores the probed-code values of the
// trampoline's working registers (LR + r14/r15/r16) so the trampoline can
// reconstruct them into the CPU state.
struct IncomingProbeRecord {
    UCPURegister lr;
    UCPURegister r12;   // probe() clobbers r12 (ELFv2 GEP entry) before the call
    UCPURegister r14;
    UCPURegister r15;
    UCPURegister r16;
    UCPURegister padding;
};

#define IN_LR_OFFSET  (0 * PTR_SIZE)
#define IN_R12_OFFSET (1 * PTR_SIZE)
#define IN_R14_OFFSET (2 * PTR_SIZE)
#define IN_R15_OFFSET (3 * PTR_SIZE)
#define IN_R16_OFFSET (4 * PTR_SIZE)
#define IN_SIZE       (6 * PTR_SIZE)

static_assert(offsetof(IncomingProbeRecord, lr) == IN_LR_OFFSET);
static_assert(offsetof(IncomingProbeRecord, r12) == IN_R12_OFFSET);
static_assert(offsetof(IncomingProbeRecord, r14) == IN_R14_OFFSET);
static_assert(offsetof(IncomingProbeRecord, r15) == IN_R15_OFFSET);
static_assert(offsetof(IncomingProbeRecord, r16) == IN_R16_OFFSET);
static_assert(sizeof(IncomingProbeRecord) == IN_SIZE);
static_assert(!(IN_SIZE & 0xf));

// Stashed just below the outgoing sp so the trampoline's final instructions
// can restore the working registers sp-relative.
#define OUT_R14_OFFSET (0 * PTR_SIZE)
#define OUT_R15_OFFSET (1 * PTR_SIZE)
#define OUT_R16_OFFSET (2 * PTR_SIZE)
#define OUT_SIZE       (4 * PTR_SIZE)   // padded to 16-byte alignment
static_assert(!(OUT_SIZE & 0xf));

asm (
    ".text" "\n"
    ".globl " SYMBOL_STRING(ctiMasmProbeTrampoline) "\n"
    HIDE_SYMBOL(ctiMasmProbeTrampoline) "\n"
    SYMBOL_STRING(ctiMasmProbeTrampoline) ":" "\n"

    // ELFv2 global entry: our C-tagged call sets r12 = entry, so compute
    // the TOC to make the local bl to executeJSCJITProbe valid.
    "addis 2, 12, .TOC.-" SYMBOL_STRING(ctiMasmProbeTrampoline) "@ha" "\n"
    "addi  2, 2, .TOC.-" SYMBOL_STRING(ctiMasmProbeTrampoline) "@l" "\n"
    ".localentry " SYMBOL_STRING(ctiMasmProbeTrampoline) ", .-" SYMBOL_STRING(ctiMasmProbeTrampoline) "\n"

    // On entry sp points at the IncomingProbeRecord. r14=probeFunction,
    // r15=arg. Keep the incoming-record pointer in r16.
    "mr 16, 1" "\n"
    "addi 1, 1, " STRINGIZE_VALUE_OF(-PROBE_ALIGNED_STACK_SIZE) "\n"

    "std 14, " STRINGIZE_VALUE_OF(PROBE_PROBE_FUNCTION_OFFSET) "(1)" "\n"
    "std 15, " STRINGIZE_VALUE_OF(PROBE_ARG_OFFSET) "(1)" "\n"

    // Save all GPRs. r14/r15/r16 come from the IncomingProbeRecord; the
    // real sp is (incoming sp + IN_SIZE).
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(0)) "(1)" "\n"
    "std 2, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(2)) "(1)" "\n"
    "std 3, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(3)) "(1)" "\n"
    "std 4, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(4)) "(1)" "\n"
    "std 5, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(5)) "(1)" "\n"
    "std 6, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(6)) "(1)" "\n"
    "std 7, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(7)) "(1)" "\n"
    "std 8, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(8)) "(1)" "\n"
    "std 9, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(9)) "(1)" "\n"
    "std 10, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(10)) "(1)" "\n"
    "std 11, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(11)) "(1)" "\n"
    // r12 was clobbered by probe() for the GEP entry; the probed value is in
    // the IncomingProbeRecord.
    "ld 0, " STRINGIZE_VALUE_OF(IN_R12_OFFSET) "(16)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(12)) "(1)" "\n"
    "std 13, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(13)) "(1)" "\n"
    "std 17, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(17)) "(1)" "\n"
    "std 18, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(18)) "(1)" "\n"
    "std 19, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(19)) "(1)" "\n"
    "std 20, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(20)) "(1)" "\n"
    "std 21, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(21)) "(1)" "\n"
    "std 22, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(22)) "(1)" "\n"
    "std 23, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(23)) "(1)" "\n"
    "std 24, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(24)) "(1)" "\n"
    "std 25, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(25)) "(1)" "\n"
    "std 26, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(26)) "(1)" "\n"
    "std 27, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(27)) "(1)" "\n"
    "std 28, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(28)) "(1)" "\n"
    "std 29, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(29)) "(1)" "\n"
    "std 30, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(30)) "(1)" "\n"
    "std 31, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(31)) "(1)" "\n"

    "ld 0, " STRINGIZE_VALUE_OF(IN_R14_OFFSET) "(16)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(14)) "(1)" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(IN_R15_OFFSET) "(16)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(15)) "(1)" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(IN_R16_OFFSET) "(16)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(16)) "(1)" "\n"

    "addi 0, 16, " STRINGIZE_VALUE_OF(IN_SIZE) "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(1)) "(1)" "\n"

    // SPRs. Two distinct link values:
    //   cpu.pc = the LR the trampoline was entered with (our bctrl's return
    //            address) — the point probed execution resumes at.
    //   cpu.lr = the probed code's own LR, saved into the IncomingProbeRecord
    //            by probe() before the call.
    "mflr 0" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_PC_OFFSET) "(1)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_SAVED_RETURN_PC_OFFSET) "(1)" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(IN_LR_OFFSET) "(16)" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_LR_OFFSET) "(1)" "\n"
    "mfcr 0" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_CR_OFFSET) "(1)" "\n"
    "mfxer 0" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_XER_OFFSET) "(1)" "\n"
    "mfctr 0" "\n"
    "std 0, " STRINGIZE_VALUE_OF(PROBE_CPU_CTR_OFFSET) "(1)" "\n"

    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(0)) "\n"
    "stxvd2x 0, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(1)) "\n"
    "stxvd2x 1, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(2)) "\n"
    "stxvd2x 2, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(3)) "\n"
    "stxvd2x 3, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(4)) "\n"
    "stxvd2x 4, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(5)) "\n"
    "stxvd2x 5, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(6)) "\n"
    "stxvd2x 6, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(7)) "\n"
    "stxvd2x 7, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(8)) "\n"
    "stxvd2x 8, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(9)) "\n"
    "stxvd2x 9, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(10)) "\n"
    "stxvd2x 10, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(11)) "\n"
    "stxvd2x 11, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(12)) "\n"
    "stxvd2x 12, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(13)) "\n"
    "stxvd2x 13, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(14)) "\n"
    "stxvd2x 14, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(15)) "\n"
    "stxvd2x 15, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(16)) "\n"
    "stxvd2x 16, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(17)) "\n"
    "stxvd2x 17, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(18)) "\n"
    "stxvd2x 18, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(19)) "\n"
    "stxvd2x 19, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(20)) "\n"
    "stxvd2x 20, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(21)) "\n"
    "stxvd2x 21, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(22)) "\n"
    "stxvd2x 22, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(23)) "\n"
    "stxvd2x 23, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(24)) "\n"
    "stxvd2x 24, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(25)) "\n"
    "stxvd2x 25, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(26)) "\n"
    "stxvd2x 26, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(27)) "\n"
    "stxvd2x 27, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(28)) "\n"
    "stxvd2x 28, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(29)) "\n"
    "stxvd2x 29, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(30)) "\n"
    "stxvd2x 30, 1, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(31)) "\n"
    "stxvd2x 31, 1, 0" "\n"

    // r16 = &State (kept across the call). executeJSCJITProbe(&State),
    // with a 32-byte ELFv2 linkage area below the State.
    "mr 16, 1" "\n"
    "mr 3, 1" "\n"
    "addi 1, 1, -32" "\n"
    "bl " SYMBOL_STRING(executeJSCJITProbe) "\n"
    "nop" "\n"
    "addi 1, 1, 32" "\n"

    // If the probe moved sp below the State, relocate the State just above
    // the new sp so the restore reads valid data.
    "ld 3, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(1)) "(16)" "\n"
    "addi 4, 16, " STRINGIZE_VALUE_OF(PROBE_ALIGNED_STACK_SIZE) "\n"
    "cmpld 3, 4" "\n"
    "bge " LOCAL_LABEL_STRING(ppc64ProbeStateIsSafe) "\n"

    "addi 5, 3, " STRINGIZE_VALUE_OF(-PROBE_ALIGNED_STACK_SIZE) "\n"
    "rldicr 5, 5, 0, 59" "\n"
    "mr 1, 5" "\n"
    "mr 6, 16" "\n"
    "mr 7, 5" "\n"
    "addi 8, 16, " STRINGIZE_VALUE_OF(PROBE_ALIGNED_STACK_SIZE) "\n"
    LOCAL_LABEL_STRING(ppc64ProbeCopyLoop) ":" "\n"
    "ld 9, 0(6)" "\n"
    "std 9, 0(7)" "\n"
    "addi 6, 6, 8" "\n"
    "addi 7, 7, 8" "\n"
    "cmpld 6, 8" "\n"
    "blt " LOCAL_LABEL_STRING(ppc64ProbeCopyLoop) "\n"
    "mr 16, 5" "\n"

    LOCAL_LABEL_STRING(ppc64ProbeStateIsSafe) ":" "\n"

    // initializeStackFunction(&State) if present.
    "ld 4, " STRINGIZE_VALUE_OF(PROBE_INITIALIZE_STACK_FUNCTION_OFFSET) "(16)" "\n"
    "cmpldi 4, 0" "\n"
    "beq " LOCAL_LABEL_STRING(ppc64ProbeRestore) "\n"
    "mr 3, 16" "\n"
    "mr 12, 4" "\n"
    "mtctr 12" "\n"
    // Reserve the linkage area FIRST: sp currently equals &State, so writing
    // the TOC save at 24(sp) before adjusting would clobber the State's
    // initializeStackArg field (also at offset 24).
    "addi 1, 1, -32" "\n"
    "std 2, 24(1)" "\n"
    "bctrl" "\n"
    "ld 2, 24(1)" "\n"
    "addi 1, 1, 32" "\n"

    LOCAL_LABEL_STRING(ppc64ProbeRestore) ":" "\n"
    // Base for the restore is r16 = &State.
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(0)) "\n"
    "lxvd2x 0, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(1)) "\n"
    "lxvd2x 1, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(2)) "\n"
    "lxvd2x 2, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(3)) "\n"
    "lxvd2x 3, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(4)) "\n"
    "lxvd2x 4, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(5)) "\n"
    "lxvd2x 5, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(6)) "\n"
    "lxvd2x 6, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(7)) "\n"
    "lxvd2x 7, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(8)) "\n"
    "lxvd2x 8, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(9)) "\n"
    "lxvd2x 9, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(10)) "\n"
    "lxvd2x 10, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(11)) "\n"
    "lxvd2x 11, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(12)) "\n"
    "lxvd2x 12, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(13)) "\n"
    "lxvd2x 13, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(14)) "\n"
    "lxvd2x 14, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(15)) "\n"
    "lxvd2x 15, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(16)) "\n"
    "lxvd2x 16, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(17)) "\n"
    "lxvd2x 17, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(18)) "\n"
    "lxvd2x 18, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(19)) "\n"
    "lxvd2x 19, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(20)) "\n"
    "lxvd2x 20, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(21)) "\n"
    "lxvd2x 21, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(22)) "\n"
    "lxvd2x 22, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(23)) "\n"
    "lxvd2x 23, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(24)) "\n"
    "lxvd2x 24, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(25)) "\n"
    "lxvd2x 25, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(26)) "\n"
    "lxvd2x 26, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(27)) "\n"
    "lxvd2x 27, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(28)) "\n"
    "lxvd2x 28, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(29)) "\n"
    "lxvd2x 29, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(30)) "\n"
    "lxvd2x 30, 16, 0" "\n"
    "li 0, " STRINGIZE_VALUE_OF(PROBE_CPU_FPR_OFFSET(31)) "\n"
    "lxvd2x 31, 16, 0" "\n"

    // SPRs. LR <- cpu.lr; CTR <- cpu.pc (target of the final bctr).
    "ld 0, " STRINGIZE_VALUE_OF(PROBE_CPU_CR_OFFSET) "(16)" "\n"
    "mtcr 0" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(PROBE_CPU_XER_OFFSET) "(16)" "\n"
    "mtxer 0" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(PROBE_CPU_LR_OFFSET) "(16)" "\n"
    "mtlr 0" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(PROBE_CPU_PC_OFFSET) "(16)" "\n"
    "mtctr 0" "\n"

    // Restore GPRs except r1(sp) and r14/r15/r16 (the outgoing dance),
    // r0 loaded last.
    "ld 2, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(2)) "(16)" "\n"
    "ld 3, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(3)) "(16)" "\n"
    "ld 4, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(4)) "(16)" "\n"
    "ld 5, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(5)) "(16)" "\n"
    "ld 6, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(6)) "(16)" "\n"
    "ld 7, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(7)) "(16)" "\n"
    "ld 8, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(8)) "(16)" "\n"
    "ld 9, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(9)) "(16)" "\n"
    "ld 10, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(10)) "(16)" "\n"
    "ld 11, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(11)) "(16)" "\n"
    "ld 12, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(12)) "(16)" "\n"
    "ld 13, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(13)) "(16)" "\n"
    "ld 17, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(17)) "(16)" "\n"
    "ld 18, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(18)) "(16)" "\n"
    "ld 19, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(19)) "(16)" "\n"
    "ld 20, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(20)) "(16)" "\n"
    "ld 21, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(21)) "(16)" "\n"
    "ld 22, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(22)) "(16)" "\n"
    "ld 23, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(23)) "(16)" "\n"
    "ld 24, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(24)) "(16)" "\n"
    "ld 25, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(25)) "(16)" "\n"
    "ld 26, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(26)) "(16)" "\n"
    "ld 27, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(27)) "(16)" "\n"
    "ld 28, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(28)) "(16)" "\n"
    "ld 29, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(29)) "(16)" "\n"
    "ld 30, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(30)) "(16)" "\n"
    "ld 31, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(31)) "(16)" "\n"
    "ld 0, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(0)) "(16)" "\n"

    // Build the OutgoingProbeRecord below the final sp, using r14 as temp.
    "ld 15, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(1)) "(16)" "\n"   // r15 = final sp
    "addi 15, 15, " STRINGIZE_VALUE_OF(-OUT_SIZE) "\n"
    "ld 14, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(14)) "(16)" "\n"
    "std 14, " STRINGIZE_VALUE_OF(OUT_R14_OFFSET) "(15)" "\n"
    "ld 14, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(15)) "(16)" "\n"
    "std 14, " STRINGIZE_VALUE_OF(OUT_R15_OFFSET) "(15)" "\n"
    "ld 14, " STRINGIZE_VALUE_OF(PROBE_CPU_GPR_OFFSET(16)) "(16)" "\n"
    "std 14, " STRINGIZE_VALUE_OF(OUT_R16_OFFSET) "(15)" "\n"

    "mr 1, 15" "\n"
    "ld 14, " STRINGIZE_VALUE_OF(OUT_R14_OFFSET) "(1)" "\n"
    "ld 15, " STRINGIZE_VALUE_OF(OUT_R15_OFFSET) "(1)" "\n"
    "ld 16, " STRINGIZE_VALUE_OF(OUT_R16_OFFSET) "(1)" "\n"
    "addi 1, 1, " STRINGIZE_VALUE_OF(OUT_SIZE) "\n"

    "bctr" "\n"     // jump to cpu.pc(); LR already = cpu.lr()
);

void MacroAssembler::probe(Probe::Function function, void* arg)
{
    // Save the probed values of every register the setup below clobbers,
    // BEFORE clobbering them: r12 (the call's ELFv2 GEP entry), r14/r15/r16
    // (the trampoline argument carriers), and LR. r11 (dataTempRegister) is
    // deliberately left untouched — moveImmToScratch writes straight to its
    // destination and call() only uses r12 — so the trampoline saves the
    // live (probed) r11 directly. The trampoline reconstructs r12/r14/r15/r16
    // and LR from this record into the CPU state.
    sub64(TrustedImm32(sizeof(IncomingProbeRecord)), sp);
    store64(PPC64Registers::r12, Address(sp, offsetof(IncomingProbeRecord, r12)));
    store64(PPC64Registers::r14, Address(sp, offsetof(IncomingProbeRecord, r14)));
    store64(PPC64Registers::r15, Address(sp, offsetof(IncomingProbeRecord, r15)));
    store64(PPC64Registers::r16, Address(sp, offsetof(IncomingProbeRecord, r16)));
    // mflr into r14 (already saved) to avoid touching any other register.
    moveFromLR(PPC64Registers::r14);
    store64(PPC64Registers::r14, Address(sp, offsetof(IncomingProbeRecord, lr)));

    move(TrustedImmPtr(tagCFunction<OperationPtrTag>(ctiMasmProbeTrampoline)), PPC64Registers::r16);
    move(TrustedImmPtr(reinterpret_cast<void*>(function)), PPC64Registers::r14);
    move(TrustedImmPtr(arg), PPC64Registers::r15);
    // Bare indirect call: r12 = entry so the trampoline's ELFv2 GEP computes
    // its TOC. We deliberately do NOT use the C-call r2 guard here — it would
    // write std r2, 24(sp), and sp currently points at the IncomingProbeRecord
    // whose offset 24 holds the saved r15. The caller's r2 needs no saving
    // because the trampoline restores cpu.r2 (the probed value) on the way out.
    m_assembler.mr(PPC64Registers::r12, PPC64Registers::r16);
    m_assembler.mtctr(PPC64Registers::r12);
    m_assembler.bctrl();

    // The trampoline's final bctr targets cpu.pc(); for an unmodified probe
    // that is this return address, with sp/LR restored. Nothing to undo.
}

#else // COMPILER(GCC_COMPATIBLE)

void MacroAssembler::probe(Probe::Function, void*)
{
    UNREACHABLE_FOR_PLATFORM();
}

#endif // COMPILER(GCC_COMPATIBLE)

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && CPU(PPC64)
