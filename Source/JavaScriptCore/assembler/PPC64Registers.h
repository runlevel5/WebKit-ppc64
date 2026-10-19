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

#if CPU(PPC64)

// Register roles follow the 64-bit ELFv2 ABI; POWER8 (Power ISA 2.07) is the baseline.
//
// Per-register flags, as consumed by jit/RegisterSet.cpp:
//     macro(id, name, isReserved, isCalleeSaved)
//   - isReserved:    the register allocator must never pick it (SP, TOC, TLS, ...).
//   - isCalleeSaved: non-volatile per the ABI.
//
// GPRs:
//   r0       volatile, scratch only (reads as literal 0 in the base slot of D-/DS-/X-form addressing)
//   r1       stack pointer (SP)
//   r2       Table Of Contents (TOC) pointer — restored by caller after a cross-module call
//   r3..r10  argument / return registers (volatile)
//   r11      environment pointer / PLT call target (volatile) — used as MacroAssembler scratch here
//   r12      function entry address (volatile) — used as MacroAssembler scratch here
//   r13      thread pointer (TLS) — reserved by ABI
//   r14..r31 non-volatile, callee-saved
// FPRs:
//   f0       volatile; used as MacroAssembler double scratch
//   f1..f13  argument / return (volatile)
//   f14..f31 non-volatile, callee-saved

#define RegisterNames PPC64Registers

#define FOR_EACH_GP_REGISTER(macro) \
    macro(r0, "r0"_s, 1, 0)          \
    macro(r1, "r1"_s, 1, 1)          \
    macro(r2, "r2"_s, 1, 1)          \
    macro(r3, "r3"_s, 0, 0)          \
    macro(r4, "r4"_s, 0, 0)          \
    macro(r5, "r5"_s, 0, 0)          \
    macro(r6, "r6"_s, 0, 0)          \
    macro(r7, "r7"_s, 0, 0)          \
    macro(r8, "r8"_s, 0, 0)          \
    macro(r9, "r9"_s, 0, 0)          \
    macro(r10, "r10"_s, 0, 0)        \
/* MacroAssembler scratch registers (ELFv2 volatile; plt/static-chain) */ \
    macro(r11, "r11"_s, 0, 0)        \
    macro(r12, "r12"_s, 0, 0)        \
    macro(r13, "r13"_s, 1, 0)        \
    macro(r14, "r14"_s, 0, 1)        \
    macro(r15, "r15"_s, 0, 1)        \
    macro(r16, "r16"_s, 0, 1)        \
    macro(r17, "r17"_s, 0, 1)        \
    macro(r18, "r18"_s, 0, 1)        \
    macro(r19, "r19"_s, 0, 1)        \
    macro(r20, "r20"_s, 0, 1)        \
    macro(r21, "r21"_s, 0, 1)        \
    macro(r22, "r22"_s, 0, 1)        \
    macro(r23, "r23"_s, 0, 1)        \
    macro(r24, "r24"_s, 0, 1)        \
    macro(r25, "r25"_s, 0, 1)        \
    macro(r26, "r26"_s, 0, 1)        \
    macro(r27, "r27"_s, 0, 1)        \
    macro(r28, "r28"_s, 0, 1)        \
    macro(r29, "r29"_s, 0, 1)        \
    macro(r30, "r30"_s, 0, 1)        \
    macro(r31, "r31"_s, 0, 1)

#define FOR_EACH_REGISTER_ALIAS(macro) \
    macro(zero, "zero"_s, r0)            \
    macro(sp, "sp"_s, r1)                \
    macro(toc, "toc"_s, r2)              \
    macro(tp, "tp"_s, r13)               \
    macro(fp, "fp"_s, r31)

#define FOR_EACH_SP_REGISTER(macro) \
    macro(pc, "pc"_s)                \
    macro(cr, "cr"_s)                \
    macro(xer, "xer"_s)              \
    macro(lr, "lr"_s)                \
    macro(ctr, "ctr"_s)

#define FOR_EACH_FP_REGISTER(macro) \
    macro(f0, "f0"_s, 0, 0)          \
    macro(f1, "f1"_s, 0, 0)          \
    macro(f2, "f2"_s, 0, 0)          \
    macro(f3, "f3"_s, 0, 0)          \
    macro(f4, "f4"_s, 0, 0)          \
    macro(f5, "f5"_s, 0, 0)          \
    macro(f6, "f6"_s, 0, 0)          \
    macro(f7, "f7"_s, 0, 0)          \
    macro(f8, "f8"_s, 0, 0)          \
    macro(f9, "f9"_s, 0, 0)          \
    macro(f10, "f10"_s, 0, 0)        \
    macro(f11, "f11"_s, 0, 0)        \
    macro(f12, "f12"_s, 0, 0)        \
    macro(f13, "f13"_s, 0, 0)        \
    macro(f14, "f14"_s, 0, 1)        \
    macro(f15, "f15"_s, 0, 1)        \
    macro(f16, "f16"_s, 0, 1)        \
    macro(f17, "f17"_s, 0, 1)        \
    macro(f18, "f18"_s, 0, 1)        \
    macro(f19, "f19"_s, 0, 1)        \
    macro(f20, "f20"_s, 0, 1)        \
    macro(f21, "f21"_s, 0, 1)        \
    macro(f22, "f22"_s, 0, 1)        \
    macro(f23, "f23"_s, 0, 1)        \
    macro(f24, "f24"_s, 0, 1)        \
    macro(f25, "f25"_s, 0, 1)        \
    macro(f26, "f26"_s, 0, 1)        \
    macro(f27, "f27"_s, 0, 1)        \
    macro(f28, "f28"_s, 0, 1)        \
    macro(f29, "f29"_s, 0, 1)        \
    macro(f30, "f30"_s, 0, 1)        \
    macro(f31, "f31"_s, 0, 1)

// VMX (Altivec) vector registers v0-v31. They alias the upper 32 of the 64
// VSRs (VSR32-VSR63). ELFv2:
//   v0-v1   volatile
//   v2-v13  argument / return (volatile)
//   v14-v19 volatile
//   v20-v31 non-volatile (callee-saved)
#define FOR_EACH_VR_REGISTER(macro) \
    macro(v0, "v0"_s, 0, 0)          \
    macro(v1, "v1"_s, 0, 0)          \
    macro(v2, "v2"_s, 0, 0)          \
    macro(v3, "v3"_s, 0, 0)          \
    macro(v4, "v4"_s, 0, 0)          \
    macro(v5, "v5"_s, 0, 0)          \
    macro(v6, "v6"_s, 0, 0)          \
    macro(v7, "v7"_s, 0, 0)          \
    macro(v8, "v8"_s, 0, 0)          \
    macro(v9, "v9"_s, 0, 0)          \
    macro(v10, "v10"_s, 0, 0)        \
    macro(v11, "v11"_s, 0, 0)        \
    macro(v12, "v12"_s, 0, 0)        \
    macro(v13, "v13"_s, 0, 0)        \
    macro(v14, "v14"_s, 0, 0)        \
    macro(v15, "v15"_s, 0, 0)        \
    macro(v16, "v16"_s, 0, 0)        \
    macro(v17, "v17"_s, 0, 0)        \
    macro(v18, "v18"_s, 0, 0)        \
    macro(v19, "v19"_s, 0, 0)        \
    macro(v20, "v20"_s, 0, 1)        \
    macro(v21, "v21"_s, 0, 1)        \
    macro(v22, "v22"_s, 0, 1)        \
    macro(v23, "v23"_s, 0, 1)        \
    macro(v24, "v24"_s, 0, 1)        \
    macro(v25, "v25"_s, 0, 1)        \
    macro(v26, "v26"_s, 0, 1)        \
    macro(v27, "v27"_s, 0, 1)        \
    macro(v28, "v28"_s, 0, 1)        \
    macro(v29, "v29"_s, 0, 1)        \
    macro(v30, "v30"_s, 0, 1)        \
    macro(v31, "v31"_s, 0, 1)

#endif // CPU(PPC64)
