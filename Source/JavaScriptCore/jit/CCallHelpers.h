/*
 * Copyright (C) 2011-2020 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/AssemblyHelpers.h>
#include <JavaScriptCore/DisallowMacroScratchRegisterUsage.h>
#include <JavaScriptCore/FPRInfo.h>
#include <JavaScriptCore/GPRInfo.h>
#include <JavaScriptCore/MaxFrameExtentForSlowPathCall.h>
#include <JavaScriptCore/OperationResult.h>
#include <JavaScriptCore/StackAlignment.h>
#include <wtf/FunctionTraits.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

#define POKE_ARGUMENT_OFFSET 0

class CallFrame;
class Structure;
namespace DFG {
class RegisteredStructure;
};

class CCallHelpers : public AssemblyHelpers {
    WTF_MAKE_TZONE_ALLOCATED(CCallHelpers);
public:
    CCallHelpers(CodeBlock* codeBlock = nullptr)
        : AssemblyHelpers(codeBlock)
    {
    }

    // Wrapper to encode JSCell GPR into JSValue.
    class CellValue {
    public:
        explicit CellValue(GPRReg gpr)
            : m_gpr(gpr)
        {
        }

        GPRReg gpr() const { return m_gpr; }

    private:
        GPRReg m_gpr;
    };

    // Base class for constant materializers.
    // It offers DerivedClass::materialize and poke functions.
    class ConstantMaterializer { };

#if CPU(PPC64)
    // ELFv2 gives every C argument, integer or floating point, one doubleword of
    // the parameter save area by its position in the argument list, starting
    // after the 32-byte linkage area at sp + 32. Positions 0-7 are also r3-r10:
    // an integer argument takes the GPR of its position (an FP argument skips its
    // GPR) or, from position 8 on, its doubleword. FP arguments take f1-f13 in
    // order, then their doubleword. An argument in memory is always at
    // sp + 32 + 8 * position.
    static constexpr unsigned elfV2ArgumentGPRs = 8;
    static constexpr unsigned elfV2ArgumentFPRs = 13;
    static constexpr unsigned elfV2ParameterSaveAreaOffsetInWords = 4;

    static constexpr GPRReg cArgumentRegister(GPRReg, unsigned index) { return GPRInfo::toArgumentRegister(index); }
    static constexpr FPRReg cArgumentRegister(FPRReg, unsigned index)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(index < elfV2ArgumentFPRs);
        return static_cast<FPRReg>(PPC64Registers::f1 + index);
    }

    // The bytes at sp a C call to OperationType needs reserved: the linkage area
    // the callee may store into, and, once any argument is passed in memory, the
    // whole parameter save area (one doubleword per argument position).
    template<typename OperationType>
    static constexpr size_t outgoingArgumentAreaBytes()
    {
        return outgoingArgumentAreaBytesImpl<FunctionTraits<OperationType>>(std::make_index_sequence<FunctionTraits<OperationType>::arity>());
    }

    template<typename OperationTraitsType, size_t... argIndices>
    static constexpr size_t outgoingArgumentAreaBytesImpl(std::index_sequence<argIndices...>)
    {
        constexpr std::array<bool, sizeof...(argIndices)> isFP { std::is_same_v<typename OperationTraitsType::template ArgumentType<argIndices>, double>... };
        unsigned fprs = 0;
        bool usesParameterSaveArea = false;
        for (unsigned position = 0; position < isFP.size(); ++position) {
            if (isFP[position] ? fprs++ >= elfV2ArgumentFPRs : position >= elfV2ArgumentGPRs)
                usesParameterSaveArea = true;
        }
        size_t words = elfV2ParameterSaveAreaOffsetInWords + (usesParameterSaveArea ? isFP.size() : 0);
        return words * sizeof(CPURegister);
    }
#endif

    // The most general helper for setting arguments that fit in a GPR, if you can compute each
    // argument without using any argument registers. You usually want one of the setupArguments*()
    // methods below instead of this. This thing is most useful if you have *a lot* of arguments.
    template<typename Functor>
    void setupArgument(unsigned argumentIndex, const Functor& functor)
    {
        unsigned numberOfRegs = GPRInfo::numberOfArgumentRegisters; // Disguise the constant from clang's tautological compare warning.
        if (argumentIndex < numberOfRegs) {
            functor(GPRInfo::toArgumentRegister(argumentIndex));
            return;
        }

        functor(GPRInfo::nonArgGPR0);
#if CPU(PPC64)
        // Every argument here is GPR-class, so the index is the ELFv2 position.
        poke(GPRInfo::nonArgGPR0, elfV2ParameterSaveAreaOffsetInWords + argumentIndex);
#else
        poke(GPRInfo::nonArgGPR0, POKE_ARGUMENT_OFFSET + argumentIndex - GPRInfo::numberOfArgumentRegisters);
#endif
    }

    enum class ShuffleStatus : uint8_t {
        ToMove,
        BeingMoved,
        Moved
    };

    // GCC cannot prove this recursion terminates (the status updates end it).
#if COMPILER(GCC) && !COMPILER(CLANG)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
#endif
    template<typename RegType, size_t N, typename RegPair = std::pair<RegType, RegType>>
    void emitShuffleMove(Vector<RegPair, N>& moves, Vector<ShuffleStatus, N>& status, unsigned index, RegType scratch)
    {
        status[index] = ShuffleStatus::BeingMoved;
        for (unsigned i = 0; i < moves.size(); i ++) {
            if (moves[i].first == moves[index].second) {
                ASSERT(i != index);
                switch (status[i]) {
                case ShuffleStatus::ToMove:
                    emitShuffleMove(moves, status, i, scratch);
                    break;
                case ShuffleStatus::BeingMoved: {
                    if constexpr (std::is_same_v<RegType, FPRegisterID>)
                        moveDouble(moves[i].first, scratch);
                    else
                        move(moves[i].first, scratch);
                    moves[i].first = scratch;
                    break;
                }
                case ShuffleStatus::Moved:
                    break;
                }
            }
        }
        if constexpr (std::is_same_v<RegType, FPRegisterID>)
            moveDouble(moves[index].first, moves[index].second);
        else
            move(moves[index].first, moves[index].second);
        status[index] = ShuffleStatus::Moved;
    }
#if COMPILER(GCC) && !COMPILER(CLANG)
#pragma GCC diagnostic pop
#endif

    template<typename RegType, unsigned NumberOfRegisters>
    ALWAYS_INLINE void shuffleRegisters(std::array<RegType, NumberOfRegisters> sources, std::array<RegType, NumberOfRegisters> destinations)
    {
        if (ASSERT_ENABLED) {
            RegisterSet set;
            for (RegType dest : destinations)
                set.add(dest, IgnoreVectors);
            ASSERT_WITH_MESSAGE(set.numberOfSetRegisters() == NumberOfRegisters, "Destinations should not be aliased.");
        }

        using RegPair = std::pair<RegType, RegType>;
        Vector<RegPair, NumberOfRegisters> pairs;

        // if constexpr avoids warnings when NumberOfRegisters is 0.
        if constexpr (NumberOfRegisters > 0) {
            for (unsigned i = 0; i < NumberOfRegisters; ++i) {
                if (sources[i] != destinations[i])
                    pairs.append(std::make_pair(sources[i], destinations[i]));
            }
        } else {
            // Silence some older compilers (GCC up to 9.X) about unused but set parameters.
            UNUSED_PARAM(sources);
            UNUSED_PARAM(destinations);
        }

        Vector<ShuffleStatus, NumberOfRegisters> status;
        status.fill(ShuffleStatus::ToMove, pairs.size());

        RELEASE_ASSERT(m_allowScratchRegister); // We absolutely need one for the recursive shuffle algorithm.
        auto scratch = scratchRegister();

        {
            DisallowMacroScratchRegisterUsage disallowScratch(*this);
            for (size_t i = 0; i < pairs.size(); i ++) {
                if (status[i] == ShuffleStatus::ToMove) {
                    if constexpr (std::is_same_v<RegType, RegisterID>)
                        emitShuffleMove(pairs, status, i, scratch);
                    else
                        emitShuffleMove(pairs, status, i, fpTempRegister);
                }
            }
        }
    }

    template<unsigned NumberOfJSRs>
    ALWAYS_INLINE void shuffleJSRs(std::array<JSValueRegs, NumberOfJSRs> sources, std::array<JSValueRegs, NumberOfJSRs> destinations)
    {
#if USE(JSVALUE64)
        constexpr unsigned NumberOfRegisters = NumberOfJSRs;
#else
        constexpr unsigned NumberOfRegisters = NumberOfJSRs * 2;
#endif
        std::array<GPRReg, NumberOfRegisters> sourceRegs;
        std::array<GPRReg, NumberOfRegisters> destinationRegs;

        for (unsigned i = 0; i < NumberOfJSRs; ++i) {
            sourceRegs[i] = sources[i].payloadGPR();
            destinationRegs[i] = destinations[i].payloadGPR();
#if !USE(JSVALUE64)
            sourceRegs[i + NumberOfJSRs] = sources[i].tagGPR();
            destinationRegs[i + NumberOfJSRs] = destinations[i].tagGPR();
#endif
        }
        shuffleRegisters<GPRReg, NumberOfRegisters>(sourceRegs, destinationRegs);
    }

private:
    template<typename RegType>
    using InfoTypeForReg = decltype(toInfoFromReg(RegType(-1)));

    // extraGPRArgs is used to track 64-bit argument types passed in register on 32-bit architectures.
    // extraPoke is used to track 64-bit argument types passed on the stack.
    template<unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke>
    struct ArgCollection {
        ArgCollection()
        {
            gprSources.fill(InvalidGPRReg);
            gprDestinations.fill(InvalidGPRReg);
            fprSources.fill(InvalidFPRReg);
            fprDestinations.fill(InvalidFPRReg);
            crossSources.fill(InvalidFPRReg);
            crossDestinations.fill(InvalidGPRReg);
        }

        template<unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f, unsigned g, unsigned h>
        ArgCollection(ArgCollection<a, b, c, d, e, f, g, h>& other)
        {
            gprSources = other.gprSources;
            gprDestinations = other.gprDestinations;
            fprSources = other.fprSources;
            fprDestinations = other.fprDestinations;
            crossSources = other.crossSources;
            crossDestinations = other.crossDestinations;
        }

        ArgCollection<numGPRArgs + 1, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> pushRegArg(GPRReg argument, GPRReg destination)
        {
            ArgCollection<numGPRArgs + 1, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> result(*this);

            result.gprSources[numGPRSources] = argument;
            result.gprDestinations[numGPRSources] = destination;
            return result;
        }

        ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources + 1, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> pushRegArg(FPRReg argument, FPRReg destination)
        {
            ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources + 1, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> result(*this);

            result.fprSources[numFPRSources] = argument;
            result.fprDestinations[numFPRSources] = destination;
            return result;
        }

        ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources, numCrossSources + 1, extraGPRArgs, nonArgGPRs, extraPoke> pushRegArg(FPRReg argument, GPRReg destination)
        {
            ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources, numCrossSources + 1, extraGPRArgs, nonArgGPRs, extraPoke> result(*this);

            result.crossSources[numCrossSources] = argument;
            result.crossDestinations[numCrossSources] = destination;
            return result;
        }

        ArgCollection<numGPRArgs, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke> pushExtraRegArg(GPRReg argument, GPRReg destination)
        {
            ArgCollection<numGPRArgs, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke> result(*this);

            result.gprSources[numGPRSources] = argument;
            result.gprDestinations[numGPRSources] = destination;
            return result;
        }

        ArgCollection<numGPRArgs, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs + 1, extraPoke> pushNonArg(GPRReg argument, GPRReg destination)
        {
            ArgCollection<numGPRArgs, numGPRSources + 1, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs + 1, extraPoke> result(*this);

            result.gprSources[numGPRSources] = argument;
            result.gprDestinations[numGPRSources] = destination;
            return result;
        }

        ArgCollection<numGPRArgs + 1, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> addGPRArg()
        {
            return ArgCollection<numGPRArgs + 1, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke>(*this);
        }

        ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke> addGPRExtraArg()
        {
            return ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke>(*this);
        }

        ArgCollection<numGPRArgs + 1, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> addStackArg(GPRReg)
        {
            return ArgCollection<numGPRArgs + 1, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke>(*this);
        }

        ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> addStackArg(FPRReg)
        {
            return ArgCollection<numGPRArgs, numGPRSources, numFPRArgs + 1, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke>(*this);
        }

        ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 1> addPoke()
        {
            return ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 1>(*this);
        }

#if CPU(PPC64)
        // ELFv2: an FP argument skips its GPR, so an integer argument's GPR is
        // chosen by its position among all arguments, not by the GP count.
        // FP arguments still select FPRs by FP count.
        // e.g. f(void* a, double b, void* c) passes a in r3, c in r5.
        unsigned argCount(GPRReg) { return numGPRArgs + numFPRArgs + extraGPRArgs; }
#else
        unsigned argCount(GPRReg) { return numGPRArgs + extraGPRArgs; }
#endif
        unsigned argCount(FPRReg) { return numFPRArgs; }

        // store GPR -> GPR assignments
        std::array<GPRReg, GPRInfo::numberOfRegisters> gprSources;
        std::array<GPRReg, GPRInfo::numberOfRegisters> gprDestinations;

        // store FPR -> FPR assignments
        std::array<FPRReg, FPRInfo::numberOfRegisters> fprSources;
        std::array<FPRReg, FPRInfo::numberOfRegisters> fprDestinations;

        // store FPR -> GPR assignments
        std::array<FPRReg, GPRInfo::numberOfRegisters> crossSources;
        std::array<GPRReg, GPRInfo::numberOfRegisters> crossDestinations;
    };

    template<unsigned TargetSize, typename RegType>
    std::array<RegType, TargetSize> clampArrayToSize(std::array<RegType, InfoTypeForReg<RegType>::numberOfRegisters> sourceArray)
    {
        static_assert(TargetSize <= sourceArray.size(), "TargetSize is bigger than source.size()");
        RELEASE_ASSERT(TargetSize <= InfoTypeForReg<RegType>::numberOfRegisters);

        std::array<RegType, TargetSize> result { };

        // if constexpr avoids warnings when TargetSize is 0.
        if constexpr (TargetSize > 0) {
            for (unsigned i = 0; i < TargetSize; i++) {
                ASSERT(sourceArray[i] != static_cast<int32_t>(InfoTypeForReg<RegType>::InvalidIndex));
                result[i] = sourceArray[i];
            }
        }

        return result;
    }

    ALWAYS_INLINE unsigned calculatePokeOffset(unsigned currentGPRArgument, unsigned currentFPRArgument, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke)
    {
#if CPU(PPC64)
        // The argument's ELFv2 position is the number of arguments of either bank
        // before it; its doubleword is at sp + 32 + 8 * position (see
        // elfV2ArgumentGPRs). extraGPRArgs, numCrossSources and extraPoke are
        // always 0 on this JSVALUE64 target.
        UNUSED_PARAM(numCrossSources);
        UNUSED_PARAM(extraGPRArgs);
        UNUSED_PARAM(nonArgGPRs);
        UNUSED_PARAM(extraPoke);
        ASSERT(!numCrossSources && !extraGPRArgs && !extraPoke);
        ASSERT(currentGPRArgument + currentFPRArgument >= elfV2ArgumentGPRs || currentFPRArgument >= elfV2ArgumentFPRs);
        return elfV2ParameterSaveAreaOffsetInWords + currentGPRArgument + currentFPRArgument;
#else
        // Clang claims that it cannot find the symbol for FPRReg/GPRReg::numberOfArgumentRegisters when they are passed directly to std::max... seems like a bug
        unsigned numberOfFPArgumentRegisters = FPRInfo::numberOfArgumentRegisters;
        unsigned numberOfGPArgumentRegisters = GPRInfo::numberOfArgumentRegisters;

        UNUSED_PARAM(nonArgGPRs);

        currentGPRArgument += extraGPRArgs;
        currentFPRArgument -= numCrossSources;

        IGNORE_WARNINGS_BEGIN("type-limits")
        ASSERT(currentGPRArgument >= GPRInfo::numberOfArgumentRegisters || currentFPRArgument >= FPRInfo::numberOfArgumentRegisters);
        IGNORE_WARNINGS_END

        unsigned pokeOffset = POKE_ARGUMENT_OFFSET + extraPoke;
        pokeOffset += std::max(currentGPRArgument, numberOfGPArgumentRegisters) - numberOfGPArgumentRegisters;
        pokeOffset += std::max(currentFPRArgument, numberOfFPArgumentRegisters) - numberOfFPArgumentRegisters;
        return pokeOffset;
#endif
    }

    template<typename ArgType>
    ALWAYS_INLINE void pokeForArgument(ArgType arg, unsigned currentGPRArgument, unsigned currentFPRArgument, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke)
    {
        unsigned pokeOffset = calculatePokeOffset(currentGPRArgument, currentFPRArgument, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        if constexpr (std::derived_from<ArgType, ConstantMaterializer>)
            arg.store(*this, addressForPoke(pokeOffset));
        else
            poke(arg, pokeOffset);
    }

    ALWAYS_INLINE bool stackAligned(unsigned currentGPRArgument, unsigned currentFPRArgument, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke)
    {
        unsigned pokeOffset = calculatePokeOffset(currentGPRArgument, currentFPRArgument, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        return !(pokeOffset & 1);
    }

    // In the auto-calling convention code below the order of operations is:
    //    1) spill arguments to stack slots
    //    2) shuffle incomming argument values in registers to argument registers
    //    3) fill immediate values to argument registers
    // To do this, we recurse forwards through our args collecting argument values in registers and spilling stack slots.
    // when we run out of args we then run our shuffling code to relocate registers. Finally, as we unwind from our
    // recursion we can fill immediates.

#define CURRENT_ARGUMENT_TYPE typename FunctionTraits<OperationType>::template ArgumentType<numGPRArgs + numFPRArgs>
#define RESULT_TYPE typename FunctionTraits<OperationType>::ResultType

#if USE(JSVALUE64)

    // Avoid MSVC optimization time explosion associated with __forceinline in recursive templates.
    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename RegType, typename... Args>
    ALWAYS_INLINE void marshallArgumentRegister(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, RegType arg, Args... args)
    {
#if CPU(PPC64)
        // Decided at compile time: instantiating the register path for an argument
        // that goes to memory would size the source arrays past the register file
        // (FPRInfo has 13 registers, so a 14th FP argument could not compile).
        constexpr bool isFP = std::is_same_v<RegType, FPRReg>;
        constexpr unsigned currentArgCount = isFP ? numFPRArgs : numGPRArgs + numFPRArgs + extraGPRArgs;
        if constexpr (currentArgCount < (isFP ? elfV2ArgumentFPRs : elfV2ArgumentGPRs)) {
            auto updatedArgSourceRegs = argSourceRegs.pushRegArg(arg, cArgumentRegister(arg, currentArgCount));
            setupArgumentsImpl<OperationType>(updatedArgSourceRegs, args...);
        } else {
            pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
            setupArgumentsImpl<OperationType>(argSourceRegs.addStackArg(arg), args...);
        }
#else
        using InfoType = InfoTypeForReg<RegType>;
        unsigned numArgRegisters = InfoType::numberOfArgumentRegisters;
        unsigned currentArgCount = argSourceRegs.argCount(arg);
        if (currentArgCount < numArgRegisters) {
            auto updatedArgSourceRegs = argSourceRegs.pushRegArg(arg, InfoType::toArgumentRegister(currentArgCount));
            setupArgumentsImpl<OperationType>(updatedArgSourceRegs, args...);
            return;
        }

        pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        setupArgumentsImpl<OperationType>(argSourceRegs.addStackArg(arg), args...);
#endif
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, FPRReg arg, Args... args)
    {
        static_assert(std::is_same_v<CURRENT_ARGUMENT_TYPE, double>, "We should only be passing FPRRegs to a double. We use moveDouble / loadDouble / storeDouble exclusively");
        marshallArgumentRegister<OperationType>(argSourceRegs, arg, args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, GPRReg arg, Args... args)
    {
        marshallArgumentRegister<OperationType>(argSourceRegs, arg, args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, JSValueRegs arg, Args... args)
    {
        marshallArgumentRegister<OperationType>(argSourceRegs, arg.gpr(), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, CellValue arg, Args... args)
    {
        marshallArgumentRegister<OperationType>(argSourceRegs, arg.gpr(), args...);
    }

#else // USE(JSVALUE64)
#if CPU(ARM_THUMB2)

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, FPRReg arg, Args... args)
    {
        static_assert(std::is_same_v<CURRENT_ARGUMENT_TYPE, double>, "We should only be passing FPRRegs to a double. We use moveDouble / loadDouble / storeDouble exclusively");

        // ARM (hardfp, which we require) passes FP arguments in FP registers.
        unsigned numberOfFPArgumentRegisters = FPRInfo::numberOfArgumentRegisters;
        unsigned currentFPArgCount = argSourceRegs.argCount(arg);

        if (currentFPArgCount < numberOfFPArgumentRegisters) {
            auto updatedArgSourceRegs = argSourceRegs.pushRegArg(arg, FPRInfo::toArgumentRegister(currentFPArgCount));
            setupArgumentsImpl<OperationType>(updatedArgSourceRegs, args...);
            return;
        }

        // Otherwise pass FP argument on stack.
        if (stackAligned(numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke)) {
            pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
            setupArgumentsImpl<OperationType>(argSourceRegs.addStackArg(arg).addPoke(), args...);
        } else {
            pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 1);
            setupArgumentsImpl<OperationType>(argSourceRegs.addStackArg(arg).addPoke().addPoke(), args...);
        }
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
        requires (sizeof(CURRENT_ARGUMENT_TYPE) <= 4)
    void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, GPRReg arg, Args... args)
    {
        unsigned numArgRegisters = GPRInfo::numberOfArgumentRegisters;
        unsigned currentArgCount = argSourceRegs.argCount(arg);
        if (currentArgCount < numArgRegisters) {
            auto updatedArgSourceRegs = argSourceRegs.pushRegArg(arg, GPRInfo::toArgumentRegister(currentArgCount));
            setupArgumentsImpl<OperationType>(updatedArgSourceRegs, args...);
            return;
        }

        pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        setupArgumentsImpl<OperationType>(argSourceRegs.addStackArg(arg), args...);
    }

    template<typename OperationType, typename Arg1, typename Arg2, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    void pokeArgumentsAligned(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg1 arg1, Arg2 arg2, Args... args)
    {
        unsigned numArgRegisters = GPRInfo::numberOfArgumentRegisters;
        unsigned currentArgCount = argSourceRegs.argCount(GPRInfo::regT0);

        if (currentArgCount + 1 == numArgRegisters) {
            pokeForArgument(arg1, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke);
            pokeForArgument(arg2, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs + 1, nonArgGPRs, extraPoke + 1);
            setupArgumentsImpl<OperationType>(argSourceRegs.addGPRExtraArg().addGPRArg().addPoke(), args...);
        } else if (stackAligned(numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke)) {
            pokeForArgument(arg1, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
            pokeForArgument(arg2, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 1);
            setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg().addPoke(), args...);
        } else {
            pokeForArgument(arg1, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 1);
            pokeForArgument(arg2, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke + 2);
            setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg().addPoke().addPoke(), args...);
        }
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
        requires std::same_as<CURRENT_ARGUMENT_TYPE, EncodedJSValue>
    void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, CellValue payload, Args... args)
    {
        unsigned numArgRegisters = GPRInfo::numberOfArgumentRegisters;
        unsigned currentArgCount = argSourceRegs.argCount(payload.gpr());
        unsigned alignedArgCount = roundUpToMultipleOf<2>(currentArgCount);

        if (alignedArgCount + 1 < numArgRegisters) {
            auto updatedArgSourceRegs = argSourceRegs.pushRegArg(payload.gpr(), GPRInfo::toArgumentRegister(alignedArgCount));

            if (alignedArgCount > currentArgCount)
                setupArgumentsImpl<OperationType>(updatedArgSourceRegs.addGPRExtraArg().addGPRExtraArg(), args...);
            else
                setupArgumentsImpl<OperationType>(updatedArgSourceRegs.addGPRExtraArg(), args...);

            move(TrustedImm32(JSValue::CellTag), GPRInfo::toArgumentRegister(alignedArgCount + 1));

        } else
            pokeArgumentsAligned<OperationType>(argSourceRegs, payload.gpr(), TrustedImm32(JSValue::CellTag), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
        requires std::same_as<CURRENT_ARGUMENT_TYPE, EncodedJSValue>
    void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, JSValueRegs arg, Args... args)
    {
        unsigned numArgRegisters = GPRInfo::numberOfArgumentRegisters;
        unsigned currentArgCount = argSourceRegs.argCount(arg.tagGPR());
        unsigned alignedArgCount = roundUpToMultipleOf<2>(currentArgCount);

        if (alignedArgCount + 1 < numArgRegisters) {
            // JSValueRegs is passed in two 32-bit registers on these architectures. Increase both numGPRArgs and extraGPRArgs by 1.
            // We can't just add 2 to numGPRArgs, since it is used for CURRENT_ARGUMENT_TYPE. Adding 2 would lead to a skipped argument.
            auto updatedArgSourceRegs1 = argSourceRegs.pushRegArg(arg.payloadGPR(), GPRInfo::toArgumentRegister(alignedArgCount));
            auto updatedArgSourceRegs2 = updatedArgSourceRegs1.pushExtraRegArg(arg.tagGPR(), GPRInfo::toArgumentRegister(alignedArgCount + 1));

            if (alignedArgCount > currentArgCount)
                setupArgumentsImpl<OperationType>(updatedArgSourceRegs2.addGPRExtraArg(), args...);
            else
                setupArgumentsImpl<OperationType>(updatedArgSourceRegs2, args...);
        } else
            pokeArgumentsAligned<OperationType>(argSourceRegs, arg.payloadGPR(), arg.tagGPR(), args...);
    }

#endif // CPU(ARM_THUMB2)
#endif // USE(JSVALUE64)

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires DerivedFromOrConvertibleTo<Arg, TrustedImm> // DerivedFromOrConvertibleTo instead of derived_from since DFGSpeculativeJIT has its own implementation of TrustedImmPtr
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        // Right now this only supports non-floating point immediate arguments since we never call operations with non-register values.
        // If we ever needed to support immediate floating point arguments we would need to duplicate this logic for both types, which sounds
        // gross so it's probably better to do that marshalling before the call operation...
        static_assert(!std::is_floating_point_v<CURRENT_ARGUMENT_TYPE>, "We don't support immediate floats/doubles in setupArguments");
        auto numArgRegisters = GPRInfo::numberOfArgumentRegisters;
#if CPU(PPC64)
        // ELFv2: FP arguments consume their GPR slot, so an immediate integer
        // argument's register is selected by total argument position, matching
        // ArgCollection::argCount(GPRReg).
        auto currentArgCount = numGPRArgs + numFPRArgs + extraGPRArgs;
#else
        auto currentArgCount = numGPRArgs + extraGPRArgs;
#endif
        if (currentArgCount < numArgRegisters) {
            setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg(), args...);
#if CPU(PPC64)
            // ELFv2 has the caller extend a 32-bit argument to 64 bits by its
            // signedness, but move(TrustedImm32) zero-extends. Materialize the
            // immediate with the C argument type's own extension.
            if constexpr (std::is_same_v<std::decay_t<Arg>, TrustedImm32> && std::is_integral_v<CURRENT_ARGUMENT_TYPE> && sizeof(CURRENT_ARGUMENT_TYPE) <= 4) {
                if constexpr (std::is_signed_v<CURRENT_ARGUMENT_TYPE>)
                    move(TrustedImm64(static_cast<int64_t>(arg.m_value)), GPRInfo::toArgumentRegister(currentArgCount));
                else
                    move(TrustedImm64(static_cast<int64_t>(static_cast<uint32_t>(arg.m_value))), GPRInfo::toArgumentRegister(currentArgCount));
            } else
                move(arg, GPRInfo::toArgumentRegister(currentArgCount));
#else
            move(arg, GPRInfo::toArgumentRegister(currentArgCount));
#endif
            return;
        }

#if CPU(PPC64)
        // In memory too, ELFv2 has the caller extend an int to its whole doubleword.
        if constexpr (std::is_same_v<std::decay_t<Arg>, TrustedImm32> && std::is_integral_v<CURRENT_ARGUMENT_TYPE> && sizeof(CURRENT_ARGUMENT_TYPE) <= 4) {
            if constexpr (std::is_signed_v<CURRENT_ARGUMENT_TYPE>)
                pokeForArgument(TrustedImm64(static_cast<int64_t>(arg.m_value)), numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
            else
                pokeForArgument(TrustedImm64(static_cast<int64_t>(static_cast<uint32_t>(arg.m_value))), numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        } else
            pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
#else
        pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
#endif
        setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg(), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires (std::same_as<CURRENT_ARGUMENT_TYPE, Arg> && std::integral<CURRENT_ARGUMENT_TYPE> && sizeof(CURRENT_ARGUMENT_TYPE) <= 4)
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        setupArgumentsImpl<OperationType>(argSourceRegs, TrustedImm32(arg), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires (std::same_as<CURRENT_ARGUMENT_TYPE, Arg> && std::integral<CURRENT_ARGUMENT_TYPE> && sizeof(CURRENT_ARGUMENT_TYPE) == 8)
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        setupArgumentsImpl<OperationType>(argSourceRegs, TrustedImm64(arg), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires (std::is_pointer_v<CURRENT_ARGUMENT_TYPE> && std::same_as<Arg, std::nullptr_t>)
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        setupArgumentsImpl<OperationType>(argSourceRegs, TrustedImmPtr(arg), args...);
    }

    // Special case DFG::RegisteredStructure because it's really annoying to deal with otherwise...
    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires (std::same_as<CURRENT_ARGUMENT_TYPE, Structure*> && std::same_as<Arg, DFG::RegisteredStructure>)
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        setupArgumentsImpl<OperationType>(argSourceRegs, TrustedImmPtr(arg.get()), args...);
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename Arg, typename... Args>
        requires std::derived_from<Arg, ConstantMaterializer>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Arg arg, Args... args)
    {
        static_assert(!std::is_floating_point_v<CURRENT_ARGUMENT_TYPE>, "We don't support immediate floats/doubles in setupArguments");
        auto numArgRegisters = GPRInfo::numberOfArgumentRegisters;
#if CPU(PPC64)
        // ELFv2 position, as in ArgCollection::argCount(GPRReg).
        auto currentArgCount = numGPRArgs + numFPRArgs + extraGPRArgs;
#else
        auto currentArgCount = numGPRArgs + extraGPRArgs;
#endif
        if (currentArgCount < numArgRegisters) {
            setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg(), args...);
            arg.materialize(*this, GPRInfo::toArgumentRegister(currentArgCount));
            return;
        }


        pokeForArgument(arg, numGPRArgs, numFPRArgs, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke);
        setupArgumentsImpl<OperationType>(argSourceRegs.addGPRArg(), args...);
    }

#if CPU(PPC64)
    template<typename ArgumentType>
    void widenNarrowIntegerArgument(GPRReg reg)
    {
        // Callers that do not know the C function's type pass a signature made of the argument
        // values' own types instead (FTL's callOperation: setupArguments<void(ArgumentTypes...)>).
        // A GPRReg there names the register holding the value, not a narrow integer parameter.
        if constexpr (std::is_same_v<ArgumentType, GPRReg>)
            return;
        else if constexpr (std::is_enum_v<ArgumentType>)
            widenNarrowIntegerArgument<std::underlying_type_t<ArgumentType>>(reg);
        else if constexpr (std::is_integral_v<ArgumentType> && sizeof(ArgumentType) < sizeof(CPURegister)) {
            if constexpr (std::is_signed_v<ArgumentType>) {
                if constexpr (sizeof(ArgumentType) == 1)
                    signExtend8To32(reg, reg);
                else if constexpr (sizeof(ArgumentType) == 2)
                    signExtend16To32(reg, reg);
                else
                    signExtend32ToPtr(reg, reg);
            } else {
                if constexpr (sizeof(ArgumentType) == 1)
                    zeroExtend8To32(reg, reg);
                else if constexpr (sizeof(ArgumentType) == 2)
                    zeroExtend16To32(reg, reg);
                else
                    zeroExtend32ToWord(reg, reg);
            }
        }
    }
#endif

    template<typename OperationType, unsigned gprIndex>
    constexpr void finalizeGPRArguments(std::index_sequence<>)
    {
    }

    template<typename OperationType, unsigned gprIndex, size_t arityIndex, size_t... remainingArityIndices>
    constexpr void finalizeGPRArguments(std::index_sequence<arityIndex, remainingArityIndices...>)
    {
        using NextIndexSequenceType = std::index_sequence<remainingArityIndices...>;
        using ArgumentType = typename FunctionTraits<OperationType>::template ArgumentType<arityIndex>;

        // Every non-double-typed argument should be passed in through GPRRegs, and inversely only double-typed
        // arguments should be passed through FPRRegs. This is asserted in the invocation of the lastly-called
        // setupArgumentsImpl(ArgCollection<>) overload, by matching the number of handled GPR and FPR arguments
        // with the corresponding count of properly-typed arguments for this operation.
        if constexpr (!std::is_same_v<ArgumentType, double>) {
            // RV64 calling convention requires all 32-bit values to be sign-extended into the whole register.
            // JSC JIT is tailored for other ISAs that pass these values in 32-bit-wide registers, which RISC-V
            // doesn't support, so any 32-bit value passed in argument registers has to be manually sign-extended.
            if (isRISCV64() && gprIndex < GPRInfo::numberOfArgumentRegisters
                && std::is_integral_v<ArgumentType> && sizeof(ArgumentType) == 4) {
                GPRReg argReg = GPRInfo::toArgumentRegister(gprIndex);
                signExtend32ToPtr(argReg, argReg);
            }
#if CPU(PPC64)
            // ELFv2 likewise has the caller sign- or zero-extend every narrow integer argument
            // to 64 bits, and compiled callees rely on it. JIT code leaves the upper half of a
            // 32-bit value unspecified (a DFG int32 may still carry the number tag). The register
            // is the argument position's, not gprIndex's, since an FP argument skips its GPR.
            if (arityIndex < elfV2ArgumentGPRs)
                widenNarrowIntegerArgument<ArgumentType>(GPRInfo::toArgumentRegister(arityIndex));
#endif

            finalizeGPRArguments<OperationType, gprIndex + 1>(NextIndexSequenceType());
        } else
            finalizeGPRArguments<OperationType, gprIndex>(NextIndexSequenceType());
    }

    template<typename ArgType> using GPRArgCountValue = std::conditional_t<std::is_same_v<ArgType, double>,
        std::integral_constant<unsigned, 0>, std::integral_constant<unsigned, 1>>;
    template<typename ArgType> using FPRArgCountValue = std::conditional_t<std::is_same_v<ArgType, double>,
        std::integral_constant<unsigned, 1>, std::integral_constant<unsigned, 0>>;

    template<typename OperationTraitsType, size_t... argIndices>
    static constexpr unsigned gprArgsCount(std::index_sequence<argIndices...>)
    {
        return (0 + ... + (GPRArgCountValue<typename OperationTraitsType::template ArgumentType<argIndices>>::value));
    }

    template<typename OperationTraitsType, size_t... argIndices>
    static constexpr unsigned fprArgsCount(std::index_sequence<argIndices...>)
    {
        return (0 + ... + (FPRArgCountValue<typename OperationTraitsType::template ArgumentType<argIndices>>::value));
    }

    // Base case; set up the argument registers.
    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke>
    ALWAYS_INLINE void setupArgumentsImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs)
    {
        using TraitsType = FunctionTraits<OperationType>;
        static_assert(TraitsType::arity == numGPRArgs + numFPRArgs, "One last sanity check");
#if USE(JSVALUE64)
        static_assert(TraitsType::cCallArity() == numGPRArgs + numFPRArgs + extraPoke, "Check the CCall arity");
#endif
        static_assert(gprArgsCount<TraitsType>(std::make_index_sequence<TraitsType::arity>()) == numGPRArgs);
        static_assert(fprArgsCount<TraitsType>(std::make_index_sequence<TraitsType::arity>()) == numFPRArgs);

        shuffleRegisters<GPRReg, numGPRSources>(clampArrayToSize<numGPRSources, GPRReg>(argSourceRegs.gprSources), clampArrayToSize<numGPRSources, GPRReg>(argSourceRegs.gprDestinations));
        static_assert(!numCrossSources, "shouldn't be used on this architecture.");

        shuffleRegisters<FPRReg, numFPRSources>(clampArrayToSize<numFPRSources, FPRReg>(argSourceRegs.fprSources), clampArrayToSize<numFPRSources, FPRReg>(argSourceRegs.fprDestinations));
    }

    template<typename OperationType, unsigned numGPRArgs, unsigned numGPRSources, unsigned numFPRArgs, unsigned numFPRSources, unsigned numCrossSources, unsigned extraGPRArgs, unsigned nonArgGPRs, unsigned extraPoke, typename... Args>
    ALWAYS_INLINE void setupArgumentsEntryImpl(ArgCollection<numGPRArgs, numGPRSources, numFPRArgs, numFPRSources, numCrossSources, extraGPRArgs, nonArgGPRs, extraPoke> argSourceRegs, Args... args)
    {
        if constexpr (!FunctionTraits<OperationType>::arity) {
            static_assert(!sizeof...(Args), "Basic sanity check");
            setupArgumentsImpl<OperationType>(argSourceRegs);
        } else if constexpr (std::same_as<typename FunctionTraits<OperationType>::template ArgumentType<0>, CallFrame*>) {
#if USE(JSVALUE64)
            // This only really works for 64-bit since jsvalue regs mess things up for 32-bit...
            static_assert(FunctionTraits<OperationType>::cCallArity() == sizeof...(Args) + 1, "Basic sanity check; Did you explicitly pass callFrameRegister for the first argument?");
#endif
            setupArgumentsImpl<OperationType>(argSourceRegs, GPRInfo::callFrameRegister, args...);
        } else {
#if USE(JSVALUE64)
            // This only really works for 64-bit since jsvalue regs mess things up for 32-bit...
            static_assert(FunctionTraits<OperationType>::cCallArity() == sizeof...(Args), "Basic sanity check");
#endif
            setupArgumentsImpl<OperationType>(argSourceRegs, args...);
        }

        finalizeGPRArguments<OperationType, 0>(std::make_index_sequence<FunctionTraits<OperationType>::arity>());
    }

#undef CURRENT_ARGUMENT_TYPE
#undef RESULT_TYPE

public:

#if CPU(PPC64)
    // JIT C-call sites reserve maxFrameExtentForSlowPathCall bytes at sp for the
    // callee's linkage area and any memory arguments. An operation that needs
    // more must raise it rather than have its arguments stored past the area.
#define CHECK_OUTGOING_ARGUMENT_AREA(OperationType, reservedBytes) \
    static_assert(outgoingArgumentAreaBytes<OperationType>() <= (reservedBytes), "The C call's ELFv2 parameter save area does not fit in the reserved outgoing argument area")
#else
#define CHECK_OUTGOING_ARGUMENT_AREA(OperationType, reservedBytes) static_assert(true)
#endif

    template<typename OperationType, typename... Args>
    ALWAYS_INLINE void setupArguments(Args... args)
    {
        CHECK_OUTGOING_ARGUMENT_AREA(OperationType, maxFrameExtentForSlowPathCall);
        setupArgumentsEntryImpl<OperationType>(ArgCollection<0, 0, 0, 0, 0, 0, 0, 0>(), args...);
    }

    // setupArguments for a caller that has itself reserved reservedBytes at sp for
    // the outgoing C arguments, for calls that pass more in memory than a slow
    // path call may.
    template<typename OperationType, size_t reservedBytes, typename... Args>
    ALWAYS_INLINE void setupArgumentsWithReservedArgumentArea(Args... args)
    {
        CHECK_OUTGOING_ARGUMENT_AREA(OperationType, reservedBytes);
        setupArgumentsEntryImpl<OperationType>(ArgCollection<0, 0, 0, 0, 0, 0, 0, 0>(), args...);
    }

    template<typename OperationType, typename... Args>
    ALWAYS_INLINE void setupArgumentsForIndirectCall(GPRReg functionGPR, Args... args)
    {
        CHECK_OUTGOING_ARGUMENT_AREA(OperationType, maxFrameExtentForSlowPathCall);
        setupArgumentsEntryImpl<OperationType>(ArgCollection<0, 0, 0, 0, 0, 0, 0, 0>().pushNonArg(functionGPR, GPRInfo::nonArgGPR0), args...);
    }

    template<typename OperationType, typename... Args>
    ALWAYS_INLINE void setupArgumentsForIndirectCall(Address address, Args... args)
    {
        CHECK_OUTGOING_ARGUMENT_AREA(OperationType, maxFrameExtentForSlowPathCall);
        setupArgumentsEntryImpl<OperationType>(ArgCollection<0, 0, 0, 0, 0, 0, 0, 0>().pushNonArg(address.base, GPRInfo::nonArgGPR0), args...);
    }

#undef CHECK_OUTGOING_ARGUMENT_AREA

    void setupResults(GPRReg destA, GPRReg destB = InvalidGPRReg)
    {
        GPRReg srcA = GPRInfo::returnValueGPR;
        GPRReg srcB = GPRInfo::returnValueGPR2;

        if (destA == InvalidGPRReg)
            move(srcB, destB);
        else if (destB == InvalidGPRReg)
            move(srcA, destA);
        else if (srcB != destA) {
            // Handle the easy cases - two simple moves.
            move(srcA, destA);
            move(srcB, destB);
        } else if (srcA != destB) {
            // Handle the non-swap case - just put srcB in place first.
            move(srcB, destB);
            move(srcA, destA);
        } else
            swap(destA, destB);
    }
    
    void setupResults(JSValueRegs regs)
    {
#if USE(JSVALUE64)
        move(GPRInfo::returnValueGPR, regs.gpr());
#else
        setupResults(regs.payloadGPR(), regs.tagGPR());
#endif
    }
    
    void setupResults(FPRReg destA)
    {
        if (destA != InvalidFPRReg)
            moveDouble(FPRInfo::returnValueFPR, destA);
    }

    void jumpToExceptionHandler(VM& vm)
    {
        // genericUnwind() leaves the handler CallFrame* in vm->callFrameForCatch,
        // and the address of the handler in vm->targetMachinePCForThrow.
        loadPtr(&vm.targetMachinePCForThrow, GPRInfo::regT1);
        farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
    }

#if USE(JSVALUE64)
    template<typename T>
    requires (isExceptionOperationResult<T>)
    static constexpr GPRReg operationExceptionRegister()
    {
        static_assert(assertNotOperationSignature<T>);
        if constexpr (std::is_floating_point_v<typename T::ResultType> || std::is_same_v<typename T::ResultType, void>)
            return GPRInfo::returnValueGPR;
        else
            return GPRInfo::returnValueGPR2;
    }
#else
    template<typename T>
    requires (isExceptionOperationResult<T>)
    static constexpr GPRReg operationExceptionRegister()
    {
        static_assert(assertNotOperationSignature<T>);
        if constexpr (std::is_same_v<T, ExceptionOperationResult<void>>)
            return GPRInfo::returnValueGPR;
        else
            return GPRInfo::returnValueGPR2;
    }
#endif

    template<typename T>
    requires (!isExceptionOperationResult<T>)
    static constexpr GPRReg operationExceptionRegister()
    {
        static_assert(assertNotOperationSignature<T>);
        return InvalidGPRReg;
    }

    void prepareForTailCallSlow(RegisterSet preserved = { })
    {
        GPRReg temp1 = selectScratchGPR(preserved);
        preserved.add(temp1, IgnoreVectors);
        GPRReg temp2 = selectScratchGPR(preserved);
        preserved.add(temp2, IgnoreVectors);
#if CPU(ARM64E)
        GPRReg temp3 = selectScratchGPR(preserved);
        preserved.add(temp3, IgnoreVectors);
#endif
        ASSERT(!preserved.numberOfSetFPRs());

        GPRReg newFramePointer = temp1;
        GPRReg newFrameSizeGPR = temp2;
        {
            // The old frame size is its number of arguments (or number of
            // parameters in case of arity fixup), plus the frame header size,
            // aligned
            GPRReg oldFrameSizeGPR = temp2;
            {
                GPRReg argCountGPR = oldFrameSizeGPR;
                load32(Address(framePointerRegister, CallFrameSlot::argumentCountIncludingThis * static_cast<int>(sizeof(Register)) + PayloadOffset), argCountGPR);

                {
                    GPRReg numParametersGPR = temp1;
                    {
                        GPRReg codeBlockGPR = numParametersGPR;
                        loadPtr(Address(framePointerRegister, CallFrameSlot::codeBlock * static_cast<int>(sizeof(Register))), codeBlockGPR);
                        load32(Address(codeBlockGPR, CodeBlock::offsetOfNumParameters()), numParametersGPR);
                    }

                    ASSERT(numParametersGPR != argCountGPR);
                    // argCountGPR = max(numParametersGPR, argCountGPR)
                    moveConditionally32(Above, numParametersGPR, argCountGPR, numParametersGPR, argCountGPR, argCountGPR);
                }

                add32(TrustedImm32(stackAlignmentRegisters() + CallFrame::headerSizeInRegisters - 1), argCountGPR, oldFrameSizeGPR);
                and32(TrustedImm32(-stackAlignmentRegisters()), oldFrameSizeGPR);
                // We assume < 2^28 arguments
                mul32(TrustedImm32(sizeof(Register)), oldFrameSizeGPR, oldFrameSizeGPR);
            }

            // The new frame pointer is at framePointer + oldFrameSize - newFrameSize
            ASSERT(newFramePointer != oldFrameSizeGPR);
            addPtr(framePointerRegister, oldFrameSizeGPR, newFramePointer);

            // The new frame size is just the number of arguments plus the
            // frame header size, aligned
            ASSERT(newFrameSizeGPR != newFramePointer);
            load32(Address(stackPointerRegister, CallFrameSlot::argumentCountIncludingThis * static_cast<int>(sizeof(Register)) + PayloadOffset - sizeof(CallerFrameAndPC)),
                newFrameSizeGPR);
            add32(TrustedImm32(stackAlignmentRegisters() + CallFrame::headerSizeInRegisters - 1), newFrameSizeGPR);
            and32(TrustedImm32(-stackAlignmentRegisters()), newFrameSizeGPR);
            // We assume < 2^28 arguments
            mul32(TrustedImm32(sizeof(Register)), newFrameSizeGPR, newFrameSizeGPR);
        }

        // We don't need the current frame beyond this point. Masquerade as our
        // caller.
#if CPU(ARM_THUMB2) || CPU(ARM64) || CPU(RISCV64)
        loadPtr(Address(framePointerRegister, CallFrame::returnPCOffset()), linkRegister);
        subPtr(TrustedImm32(2 * sizeof(void*)), newFrameSizeGPR);
#if CPU(ARM64E)
        GPRReg tempGPR = temp3;
        ASSERT(tempGPR != newFramePointer && tempGPR != newFrameSizeGPR);
        addPtr(TrustedImm32(sizeof(CallerFrameAndPC)), MacroAssembler::framePointerRegister, tempGPR);
        untagPtr(tempGPR, linkRegister);
        validateUntaggedPtr(linkRegister, tempGPR);
#endif
#elif CPU(X86_64)
        push(Address(framePointerRegister, sizeof(void*)));
        subPtr(TrustedImm32(sizeof(void*)), newFrameSizeGPR);
#elif CPU(PPC64)
        // linkRegister is only a placeholder on PPC64; restore the caller's return PC
        // to LR. As on ARM64, the callee prologue re-pushes fp and the return PC.
        restoreReturnAddressBeforeReturn(Address(framePointerRegister, CallFrame::returnPCOffset()));
        subPtr(TrustedImm32(2 * sizeof(void*)), newFrameSizeGPR);
#else
        UNREACHABLE_FOR_PLATFORM();
#endif
        subPtr(newFrameSizeGPR, newFramePointer);
        loadPtr(Address(framePointerRegister), framePointerRegister);

        // We need to move the newFrameSizeGPR slots above the stack pointer by
        // newFramePointer registers. We use pointer-sized chunks.
        MacroAssembler::Label copyLoop(label());

        subPtr(TrustedImm32(sizeof(void*)), newFrameSizeGPR);
        transferPtr(BaseIndex(stackPointerRegister, newFrameSizeGPR, TimesOne), BaseIndex(newFramePointer, newFrameSizeGPR, TimesOne));
        branchTest32(MacroAssembler::NonZero, newFrameSizeGPR).linkTo(copyLoop, this);

        // Ready for a jump!
        move(newFramePointer, stackPointerRegister);
    }

    static Address addressOfCalleeCalleeFromCallerPerspective(int offset)
    {
        CCallHelpers::Address calleeFrame = CCallHelpers::Address(MacroAssembler::stackPointerRegister, 0);
        return calleeFrame.withOffset(CallFrameSlot::callee * static_cast<int>(sizeof(Register)))
            // calleeFrame is from the caller's perspective
            .withOffset(-safeCast<int>(sizeof(CallerFrameAndPC)))
            .withOffset(offset);
    }

    // This function is used to store the wasm callee in case this function hasn't tiered up yet.
    // The IPInt is going to expect this so that the common entrypoint can read bytecode/metadata.
    void storeWasmCalleeToCalleeCallFrame(RegisterID value, int offset = 0)
    {
        JIT_COMMENT(*this, "< Store Callee's wasm callee");
        auto addr = CCallHelpers::addressOfCalleeCalleeFromCallerPerspective(offset);
#if USE(JSVALUE64)
        storePtr(value, addr);
#elif USE(JSVALUE32_64)
        store32(value, addr.withOffset(PayloadOffset));
        store32(TrustedImm32(JSValue::NativeCalleeTag), addr.withOffset(TagOffset));
#else
#error "Unsupported configuration"
#endif
    }

    void storeWasmCalleeToCalleeCallFrame(CalleeBits boxedCallee)
    {
        JIT_COMMENT(*this, "> ", RawPointer(boxedCallee.asNativeCallee()));
        storeWasmCalleeToCalleeCallFrame(TrustedImmPtr(boxedCallee.rawPtr()));
    }

    void storeWasmCalleeToCalleeCallFrame(TrustedImmPtr imm, int offset = 0)
    {
        JIT_COMMENT(*this, "Store Callee's wasm callee");
        auto addr = CCallHelpers::addressOfCalleeCalleeFromCallerPerspective(offset);
#if USE(JSVALUE64)
        store64(imm, addr);
#elif USE(JSVALUE32_64)
        move(imm, scratchRegister());
        store32(scratchRegister(), addr.withOffset(PayloadOffset));
        store32(TrustedImm32(JSValue::NativeCalleeTag), addr.withOffset(TagOffset));
#else
#error "Unsupported configuration"
#endif
    }

    // These operations clobber all volatile registers. They assume that there is room on the top of
    // stack to marshall call arguments.
    void logShadowChickenProloguePacket(GPRReg shadowPacket, GPRReg scratch1, GPRReg scope);

private:
    template <typename CodeBlockType>
    void logShadowChickenTailPacketImpl(GPRReg shadowPacket, JSValueRegs thisRegs, GPRReg scope, CodeBlockType codeBlock, CallSiteIndex callSiteIndex);
public:
    void logShadowChickenTailPacket(GPRReg shadowPacket, JSValueRegs thisRegs, GPRReg scope, GPRReg codeBlock, CallSiteIndex callSiteIndex);

    // Leaves behind a pointer to the Packet we should write to in shadowPacket.
    void ensureShadowChickenPacket(VM&, GPRReg shadowPacket, GPRReg scratch1NonArgGPR, GPRReg scratch2);

    void emitCTIThunkPrologue(bool returnAddressAlreadyTagged = false);
    void emitCTIThunkEpilogue();
};

} // namespace JSC

#endif // ENABLE(JIT)
