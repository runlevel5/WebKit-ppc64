/*
 * Copyright (C) 2015-2016 Apple Inc. All rights reserved.
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

#if ENABLE(B3_JIT)

#include "B3Effects.h"
#include "B3Value.h"
#include <type_traits>
#include <wtf/FunctionTraits.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 {

class JS_EXPORT_PRIVATE CCallValue final : public Value {
public:
    static bool accepts(Kind kind) { return kind == CCall; }

    ~CCallValue() final;

    void appendArgs(const Vector<Value*>&);
    
    Effects effects;

    B3_SPECIALIZE_VALUE_FOR_VARARGS_CHILDREN
    B3_SPECIALIZE_VALUE_FOR_FINAL_SIZE_VARARGS_CHILDREN

private:
    friend class Procedure;
    friend class Value;

    template<typename... Arguments>
    static Opcode opcodeFromConstructor(Arguments...) { return CCall; }

    template<typename... Arguments>
    CCallValue(Type type, Origin origin, Arguments... arguments)
        : Value(CheckedOpcode, CCall, type, VarArgs, origin, static_cast<Value*>(arguments)...)
        , effects(Effects::forCall())
    {
        RELEASE_ASSERT(numChildren() >= 1);
    }

    template<typename... Arguments>
    CCallValue(Type type, Origin origin, const Effects& effects, Arguments... arguments)
        : Value(CheckedOpcode, CCall, type, VarArgs, origin, static_cast<Value*>(arguments)...)
        , effects(effects)
    {
        RELEASE_ASSERT(numChildren() >= 1);
    }
};

#if CPU(PPC64)
// ELFv2 has the caller extend a narrow integer argument by its C type. A B3 CCall
// only knows B3 types, so Air's CCallSpecial sign-extends every Int32 argument,
// which is wrong for a uint32_t. Creators that know the C type use this to pass such
// an argument zero-extended as an Int64.
template<typename OperationType, size_t index>
constexpr bool cCallArgumentIsUnsigned32()
{
    using Traits = FunctionTraits<OperationType>;
    if constexpr (index < Traits::arity) {
        using RawArgument = std::remove_cvref_t<typename Traits::template ArgumentType<index>>;
        using Argument = typename std::conditional_t<std::is_enum_v<RawArgument>, std::underlying_type<RawArgument>, std::type_identity<RawArgument>>::type;
        return std::is_integral_v<Argument> && std::is_unsigned_v<Argument> && sizeof(Argument) == sizeof(uint32_t);
    } else
        return false;
}
#endif

} } // namespace JSC::B3

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
