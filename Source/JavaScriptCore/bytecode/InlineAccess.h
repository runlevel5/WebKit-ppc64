/*
 * Copyright (C) 2016-2018 Apple Inc. All rights reserved.
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

#if ENABLE(JIT)

#include <JavaScriptCore/CodeLocation.h>
#include <JavaScriptCore/PropertyOffset.h>

namespace JSC {

class CodeBlock;
class InlineCacheHandler;
class JSArray;
class Structure;
class PropertyInlineCache;
class VM;

class InlineAccess {
public:

    // This is the maximum between inline and out of line self access cases.
    static constexpr size_t sizeForPropertyAccess()
    {
#if CPU(X86_64)
        return 26;
#elif CPU(ARM64)
        return 40;
#elif CPU(ARM_THUMB2)
        return 48;
#elif CPU(RISCV64)
        return 44;
#elif CPU(PPC64)
        // lwz + (lis, ori) + cmpw + an 8-word patchable branch slot = 48 bytes for the
        // structure check, then ld (inline offset) or ld + ld (butterfly), so 56 bytes
        // covers both self access forms with 16-bit offsets.
        return 56;
#else
#error "unsupported platform"
#endif
    }

    // This is the maximum between inline and out of line property replace cases.
    static constexpr size_t sizeForPropertyReplace()
    {
#if CPU(X86_64)
        return 26;
#elif CPU(ARM64)
        return 40;
#elif CPU(ARM_THUMB2)
        return 48;
#elif CPU(RISCV64)
        return 52;
#elif CPU(PPC64)
        // The 48-byte structure check, then std (inline) or ld + std (butterfly).
        return 56;
#else
#error "unsupported platform"
#endif
    }

    // This is the maximum between array length, string length, and regular self access sizes.
    ALWAYS_INLINE static constexpr size_t sizeForLengthAccess()
    {
#if CPU(X86_64)
        size_t size = 43;
#elif CPU(ARM64)
        size_t size = 44;
#elif CPU(ARM_THUMB2)
        size_t size = 30;
#elif CPU(RISCV64)
        size_t size = 60;
#elif CPU(PPC64)
        // Array length: lbz, andi., cmpwi, the 8-word slot, ld, lwz, or = 56 bytes.
        // String length does not fit (its two internal branches are 8-word slots until
        // branch compaction, and the size check runs before compaction).
        size_t size = 60;
#else
#error "unsupported platform"
#endif
        return std::max(size, sizeForPropertyAccess());
    }

    static bool generateSelfPropertyAccess(PropertyInlineCache&, Structure*, PropertyOffset);
    static bool canGenerateSelfPropertyReplace(PropertyInlineCache&, PropertyOffset);
    static bool generateSelfPropertyReplace(PropertyInlineCache&, Structure*, PropertyOffset);
    static bool isCacheableArrayLength(PropertyInlineCache&, JSArray*);
    static bool isCacheableStringLength(PropertyInlineCache&);
    static bool generateArrayLength(PropertyInlineCache&, JSArray*);
    static bool generateSelfInAccess(PropertyInlineCache&, Structure*);
    static bool generateStringLength(PropertyInlineCache&);

    // This is helpful when determining the size of an IC on
    // various platforms. When adding a new type of IC, implement
    // its placeholder code here, and log the size. That way we
    // can intelligently choose sizes on various platforms.
    JS_EXPORT_PRIVATE NO_RETURN_DUE_TO_CRASH static void dumpCacheSizesAndCrash();
};

} // namespace JSC

#endif // ENABLE(JIT)
