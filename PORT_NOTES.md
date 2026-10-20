# JavaScriptCore on 64-bit PowerPC — port notes

These notes record the technical decisions behind the PPC64 backend: what the port targets, how it
maps JSC's abstractions onto the Power ISA and the ELFv2 ABI, and the invariants that code touching
it must keep. They are written for reviewers who know JSC but not Power.

## Scope

- **Targets.** 64-bit PowerPC with the ELFv2 ABI, little-endian (ppc64le) and big-endian (see
  *Big-endian*). Big-endian ELFv1 keeps using the C_LOOP interpreter. 32-bit PowerPC, AIX and
  pointer authentication are out of scope.
- **ISA baseline: POWER8 (Power ISA 2.07).** POWER9 (ISA 3.0) and POWER10 (ISA 3.1) instructions are
  used only behind a runtime check (see *ISA levels*), so one binary runs on every POWER8+ machine.
- **Tiers.** LLInt and IPInt (offlineasm), Baseline, DFG, FTL (B3/Air), WebAssembly BBQ and OMG,
  the Yarr RegExp JIT and the sampling profiler. Signal-based VM traps and wasm fast memory
  (`useWasmFastMemory`, `useWasmFaultSignalHandler`) are on.
- **Reference backends.** ARM64 for semantics (link register, LL/SC, fixed-width instructions);
  RISCV64 for the shape of a new port in the MacroAssembler and offlineasm.
- **Allocator.** `USE_SYSTEM_MALLOC=ON` is required: libpas has no PPC64 port.

## Naming and build configuration

- `CPU(PPC64)` means 64-bit PowerPC in either byte order. `CPU(PPC64LE)` is the little-endian
  subset. Byte-order-specific code uses `CPU(BIG_ENDIAN)`, never `!CPU(PPC64LE)`.
- The offlineasm backend (`ppc64.rb`), the Air architecture tags and `isPPC64()` all say PPC64.
- CMake enables the JIT for `WTF_CPU_PPC64LE`, or for `WTF_CPU_PPC64` with `WTF_PPC64_ELFV2`
  (`_CALL_ELF == 2`).
- `ppc64.rb` is a CMake dependency of `LLIntAssembly.h`, so editing the backend rebuilds the
  interpreter.
- `PPC64_UNSUPPORTED()` marks MacroAssembler operations that are deliberately absent. Reaching one
  means an architecture gate upstream of it is missing PPC64.

## Registers

| Register | Role |
|---|---|
| r0 | Assembler scratch. In a D-form base or `addi`, RA = 0 reads as literal zero, so r0 is never a base register. |
| r1, r2, r13 | Stack pointer, TOC, thread pointer. Never touched. |
| r3–r10 | Arguments and `regT0–7`: the eight volatile allocatable GPRs. |
| r11, r12 | MacroAssembler `dataTempRegister` and `memoryTempRegister`; not allocatable. Any MacroAssembler operation may clobber them, because immediates, wide offsets and absolute addresses are materialized through them. r12 also carries the callee address for an ELFv2 global entry. |
| r14–r19 | `regCS0–5`, allocatable callee-saves (`dfgCalleeSaveRegisters`). |
| r20–r23 | `regCS6–9`: metadata table, JIT data, number tag, not-cell mask. |
| r24–r28 | Yarr scratch, saved by Yarr like x86_64's rbx and r12–r15. Air also allocates r24–r30. |
| r29, r30 | `nonArgGPR0/1` and `wasmScratchGPR`. Callee-saved in ELFv2 but scratch to JSC: LLInt and IPInt temporaries, saved by `pushCalleeSaves` at the C-to-VM entry, never allocated by Air. |
| r31 | Frame pointer (`cfr`). |
| f0 | `fpTempRegister`. f1–f13 carry FP arguments, f14–f21 are VM callee-saves. |

offlineasm keeps its own scratch in r0 and r27–r30, while `ws0/ws1` stay r11/r12.

ELFv2 preserves r14–r31 and f14–f31, but the VM models only r14–r23 and f14–f21. Everything that
crosses a C↔JS boundary therefore preserves what offlineasm uses beyond that set: `vmEntry` saves
r24–r30, the FTL inline-cache slow path clobbers `registersToSaveForCCall`, and BBQ does not
allocate f22–f31.

## ELFv2 calls

- **The caller's frame header is the callee's to write.** A callee stores CR at sp+8, LR at sp+16
  and the TOC at sp+24 of its caller's frame, and the parameter save area starts at sp+32. Any C
  call made while live data sits in [sp, sp+32) corrupts it, so sp is lowered first
  (`makeSpaceOnStackForCCall`, `maxFrameExtentForSlowPathCall` = 96). This was the most frequent
  bug class in the port. A frame header that holds code addresses is the symptom, and the clobbered
  offset names the save: 8 for CR, 16 for LR, 24 for the TOC.
- **Argument positions count both register banks.** An FP argument consumes a GPR position too.
  Integer arguments at positions 0–7 go in r3–r10, FP arguments in f1–f13, and memory arguments at
  `sp + 32 + 8 * position`. 96 bytes covers every in-register call, and `setupArguments`
  static-asserts that the operation fits.
- **The caller widens narrow integers to 64 bits**, because callees use an `int32_t` directly as an
  index. `CCallHelpers` widens by C type at the ELFv2 position. The exception is `GPRReg`-typed
  entries in FTL's synthesized `void(ArgumentTypes...)` signatures, which name registers rather than
  parameters. B3's `CCallSpecial` sign-extends every Int32, which is wrong for a `uint32_t` at or
  above 2^31.
- **C calls go through CTR with r12 holding the target**, so the global entry point can derive its
  TOC. A `farJump` to a C function sets r12 as well. JS-to-JS calls use a bare `bl` or `bctrl` and
  never touch r2.
- `USE(BUILTIN_FRAME_ADDRESS)` is off. Wasm operations get their frame from the instance's
  `temporaryCallFrame`, stored before the call.

## Link register

LR is a special-purpose register, so `linkRegister` is an r0 placeholder that no instruction may
name. Every save and restore of a return address uses `mflr`/`mtlr` (`moveFromLR`/`moveToLR`),
never a register move. The arity-check entries, the CTI thunk prologues and epilogues, the
data-IC handlers and the tail-call shuffles each broke on this before being fixed.

Baseline data-IC handlers are entered as leaf functions with the return address in LR. They spill it
before any nested call (`emitDataICPrepareForCall`). OSR entry reads LR from `context.spr(lr)`.

## Memory model

- Fences are real instructions: `lwsync` for acquire and release, `sync` for sequential
  consistency, and `isync` after a failed compare in a CAS loop.
- LL/SC (`l*arx`/`st*cx.`) loops are RCsc with a leading `sync`, matching ARM64's `ldaxr`/`stlxr`.
  `storeRel*` is `sync; st` and `loadAcq*` is `ld; lwsync`.
- A strong CAS never stores back after a mismatch. ARM64's non-LSE `atomicStrongCAS` does, which is
  an upstream bug listed below.
- **Cross-modifying code.** Power allows another thread to execute a word that is being modified
  only if the word is a `b` or `nop`-class instruction. Patchable sites are built around that rule
  (see the next section).
- The instruction-cache flush is hand-written as `dcbst; sync; icbi; sync; isync`, with block sizes
  from `AT_DCACHEBSIZE` and `AT_ICACHEBSIZE`. `__builtin___clear_cache` compiles to nothing on
  ppc64le. `crossModifyingCodeFence()` is `isync`.

## Code layout and patching

- **Branch reach** is ±32 KB for `bc` and ±32 MB for `b`/`bl`.
- **Jump and call slots are a fixed 8 instructions.** The far form is
  `nop; li64 r12; mtctr; bctr[l]`, so word 0 is always a `nop` or a `b`, and retargeting within
  reach is a single aligned store, which is legal under the cross-modifying rule. Near calls to JIT
  code are `nop…; bl`. `call()` slots keep the `li64 r12`. `call()` is `Linkable` and only
  `nearCall()` is `LinkableNear`; both link through `applyCallSlot`.
- **Jump islands.** `nearJumpRange` is 32 MB, with a 2 MB island area per 32 MB region (65,536
  islands, about 28 MB of code per region). An island is a whole jump slot, so islands never chain.
  Only concurrent wasm retargets use islands, because other repatching runs on the owning thread or
  a stopped one. Like upstream, concurrent retargets never free islands. The executable pool is
  1 GB with allocations packed at the bottom, purely for locality.
- **Branch compaction** shrinks unpatched jumps to `b`, `bc`, `bc` plus `b`, or leaves the full
  slot.
- **Constants.** 64-bit immediates take 1–5 instructions. `moveWithPatch` keeps the fixed
  5-instruction `li64` form and is patched at its start.
- **VM-trap halt** is `ld r0,0(0)`, which raises SIGSEGV. `trap` would raise SIGTRAP, which VMTraps
  does not handle.
- **POWER10 prefixed instructions** (`pli`, `pla`) are not used. They run at the same speed as the
  sequences they replace, save code only on far calls (mostly cold slow paths), and must not cross a
  64-byte boundary, which neither branch compaction nor the allocator's alignment preserves.

## SIMD

- A v128 is the full VSX register that aliases FPR fN (VSRs 0–31), and doubleword 0 is the scalar.
  Operations that exist only in VMX stage through v0–v5 (VSRs 32–37) inside one MacroAssembler
  call; nothing stays live in VSRs 32–63 between calls.
- Callee-saves stay 64-bit (doubleword 0 of f14–f31), the same model as ARM64's d8–d15.
- **Layout follows `lxvd2x`/`stxvd2x`.** Doubleword 0 is bytes 0–7 read as a little-endian u64, so
  loads and stores need no permute. Wasm lane i of an S-byte element is ISA element `i ^ (8/S − 1)`.
  VMX pack and unpack see the doublewords swapped, so they add an `xxswapd`. "High" and "low" in
  VMX instruction names are big-endian.
- **An f32 lives in double format in an FPR** (`lfs` widens, `stfs` narrows). So f32x4 lane 0 is
  not the scalar, and scalar FP spills use `stfd`/`lfd`, never `stxvx`.
- IPInt SIMD maps offlineasm v0/v1/v2 to VMX 0/1/14 and uses VMX 2–7 as scratch, with the same
  lane layout. Handler slots are 512 bytes, and `IPInt::initialize()` validates the stride.
- POWER8 leaves some result words undefined (`xvcvdpsp`, `xvcvdpsxws/uxws`, `xscvdpspn`). The
  sequences read only the defined words.

## ISA levels

`MacroAssemblerPPC64::isaLevel()` reads `AT_HWCAP2` (`ARCH_3_00` gives 300, `ARCH_3_1` gives 310)
and caps it by the option `maximumPPC64ISALevel` (`JSC_maximumPPC64ISALevel=207|300|310`). The cap
lets POWER9 and POWER10 machines exercise the POWER8 paths. Gated instructions:

- **ISA 3.0:**
  - `cnttzw/d` and `modsw/uw/sd/ud`, used by DFG, BBQ and Air `Mod32/UMod32/Mod64/UMod64`.
  - `xsmaxjdp/xsminjdp` and the Float16 conversions `xscvhpdp/xscvdphp`.
  - `xxinsertw` and `vinsertb/h` for wasm `replace_lane`.
  - `cmpeqb` for Yarr classes of up to eight Latin-1 characters. It compares only the low byte, so
    16-bit strings rule out characters above 0xff first.
- **ISA 3.1:** `brh/brw/brd`, `setbc/setbcr`, `vmulld`, `vextract{b,h,w,d}m`.

`xsmaxjdp/xsminjdp` return a signalling NaN without quieting it. That is fine for JavaScript, which
purifies NaNs, and wrong for wasm. So B3 keeps the FMin/FMax expansion when `Procedure::isWasm()`,
and BBQ keeps its own sequence (`JSTests/wasm/stress/min-max-signalling-nan-is-quieted.js`).

Divide and remainder results are undefined for x/0 and INT_MIN/−1, so every caller guards both.
Float16 values live in bits 48–63 of VSR doubleword 0.

## Other invariants

- **32-bit values are kept zero-extended in 64-bit registers.** offlineasm `i` operations and
  `move(TrustedImm32)` clear the upper word. This matters because PPC `addi` and `lis` sign-extend.
- **IPInt is mandatory for wasm.** BBQ and OMG are tier-ups from IPInt, so `useWasmIPInt=false`
  means "tier up immediately", not "skip IPInt".
- **An architecture gate that omits PPC64 still compiles.** It then emits nothing or the x86 shape,
  which made it the most common silent bug in the port. Air opcode gates live on individual form
  lines in `AirOpcode.opcodes`.
- **offlineasm resolves PtrTags symbolically.** It never re-implements a WTF hash in Ruby.
- **The GC scans the 288-byte red zone below sp**, because ELFv2 leaf code may keep values there.
- **Pages are 64 KB on the usual distributions.** Memory-threshold tests are tuned for 4 KB pages,
  so the stress runner's memory limit must be set accordingly.

## Big-endian

Big-endian ELFv2 shares the ABI and every instruction sequence with little-endian. Only the byte
order of memory differs. Nearly every big-endian bug had one shape: **a 32-bit access to the first
four bytes of an 8-byte slot.** That covers a boxed int32's payload, the low word of a
pointer-sized field, and a `Move32` spill into an 8-byte stack slot.

### JavaScript tiers

- **JSValue halves.** `TagOffset` is 0 and `PayloadOffset` is 4. Int32 locals are filled through
  `payloadFor`. The callee slot on JSVALUE64 is a whole pointer (`addressFor`), so the LLInt's
  `loadp Callee + PayloadOffset[cfr]` on 64-bit was a latent upstream bug.
- **Boxed int32 metadata** read with `load32` needs `+ PayloadOffset`. This covers the Map/Set
  ordered-hash-table counts in DFG, FTL and the `size` inline cache.
- **DataView.** `isLittleEndian` is inverted on a big-endian host. The CSE key must carry byte size
  and signedness, because the AbstractHeap payload is 48 bits.
- **Low word of a 64-bit field:**
  - `JSRopeString::offsetOfFlags()`;
  - Yarr `branch32` against frame slots written with `storePtr` (`frameSlotLowWordAddress`);
  - the `FreeCell::scrambledBits` halves in the inline allocator, and the `FreeList` secret.
- **Words built by shifting** are read back in host order:
  - the object header (`structureID << 32 | typeInfo`);
  - rope `CompactFibers` and the `JSBigInt` header (`1 << 32`);
  - `OpInfoWrapper`, whose integers are stored zero-extended in an `int64` and read back truncated
    (the `int32` union member is gone);
  - `ArrayMode`, where a bare `Array::Type` no longer converts through the private word constructor.
- **Air.** GP spill slots are at least 8 bytes, and `lowerStackArgs` moves a narrower spill access
  to the low-order half. Patch (stackmap) arguments are the exception, because OSR exit and the B3
  stackmap code restore them with a whole-slot `load64`.
- **offlineasm.** `btbz addr, mask` used to become `andi` on a 32-bit load.
  `ppc64LowerByteTestsOnMemory` loads the byte first (`MasqueradesAsUndefined`, the typed-array
  mode byte).
- **Yarr.** Surrogate-pair loads are 32-bit pairs of UTF-16 code units, rotated with
  `rotateRight32(16)` after the load. Multi-character compares (`maxCharactersAtOnce = 1`) and SIMD
  searches are off.
- **DFG and FTL `startsWith`/`endsWith`** pack constant search characters in host order.

### WebAssembly

WebAssembly runs in all tiers. SIMD stays off, because the v128 lane order is not reversed. Two
rules carry the rest.

**Linear memory is little-endian**, so every access wider than a byte is byte-reversed:
- offlineasm has `load*le`, `store*le` and `bswap*` under `if BIG_ENDIAN`, which ppc64 lowers to
  `l*brx`/`st*brx` with r0 and the red zone as scratch.
- BBQ uses the `*LittleEndian` helpers in MacroAssemblerPPC64.
- OMG uses patchpoints, because B3 has no byte-reversed memory operations.
- LL/SC and CAS loops swap the bytes around the operation.

**A 32-bit value lives at `PayloadOffset` (+4) of its 8-byte slot everywhere:**
- IPInt pushes whole words and pushes an int32 zero-extended, because ELFv2 returns it
  sign-extended. 64-bit results that share the int32 paths (`ref.cast`, memory64 `memory.grow`)
  push whole words.
- BBQ gives i32 and f32 8-byte frame slots. With 4-byte slots, a payload would land on a saved
  callee-save.
- The `IPIntStackEntry` and `IPIntLocal` unions pad i32 and f32.
- OMG and Air address the payload half of stack-passed values, stack results, globals, exception
  payloads and the OSR scratch buffer. With the graph-coloring allocator, stack results are pinned
  to their canonical slot, and the post-call copy moves only the payload.
- The FTL's direct calls into wasm, the JS-to-wasm entry and the wasm-to-JS exit all use the payload
  half.

**Module data is little-endian by format:** the header version, f32/f64 constants,
`memory.atomic.wait` operands, and data-segment copies into i16, i32 and i64 arrays.

Two bugs invisible on little-endian fell out of this: the JS-to-wasm call stub stored the boxed
callee pointer at `slot + PayloadOffset`, and the frame builder wrote converted i32/f32 arguments as
4 bytes at the start of the slot.

### Big-endian results

JSTests/stress passed 82,106 with 88 failures before WebAssembly was enabled on big-endian. All 88
failures came from nine tests that use WebAssembly.

JSTests/wasm.yaml passes 15,313 and fails 766. Every failure is expected:
- 567 SIMD variants.
- 170 tests that read wasm memory through JS typed arrays or DataView, which use host byte order by
  specification.
- 15 tests whose modules need SIMD.
- 14 slow-memory runs of tests that use the emscripten-built wabt library. Its JS glue writes
  `environ_sizes_get` results through `HEAPU32` in host order. Wasm reads a huge size and grows
  memory to about 2 GB, and slow memory's zero-filled allocation makes that resident past the
  runner's 2 GB limit.

The same tree passes both suites on POWER10 little-endian with no failures.

## Testing

- `run-jsc-stress-tests --memory-limited` with `JSCTEST_memoryLimit=2147483648`, over
  `JSTests/stress` and `JSTests/wasm.yaml`. `--memory-limited` is required, because without it a
  test that runs out of memory takes down the runner's workers. Date tests expect
  `TZ=America/Los_Angeles`.
- Run each suite at every ISA level with `JSC_maximumPPC64ISALevel=207` and `300`. A capped run
  cannot catch an ungated newer instruction, which only POWER8 hardware would trap.
- `testmasm`, `testb3` and `testair` need a `DEVELOPER_MODE` build. `testmasm` includes PPC64
  litmus tests (message passing, store buffering, read, weak and strong CAS), cross-modifying calls,
  calls with many mixed C arguments, Float16, and ISA-level tests.
- There is no PPC64 disassembler in JSC. Check encodings with `as -mpower8/-mpower9/-mpower10` and
  `objdump`, and read generated code with gdb or with `perf` and `--useJITDump`.
- Techniques that found the hard bugs: valgrind memcheck runs the JIT
  (`--smc-check=all-non-file`); an ASSERT build turns silent corruption into a named assertion;
  `--collectContinuously=true` makes heap corruption deterministic; gdb reverse watchpoints found
  the linkage clobbers; and `--dumpFTLDisassembly=true` prints per-Air-instruction address ranges
  that gdb `x/Ni` can read without a disassembler.

### Results

Gates on WebKitGTK 2.54.1 suites:

| Host and ISA level | JSTests/stress | JSTests/wasm.yaml | testmasm | testb3 / testair |
|---|---|---|---|---|
| POWER10, 3.1 | 82,194 / 0 | 19,826 / 0 | 386 / 0 | pass |
| POWER10, capped to 3.0 and 2.07 | — | — | 386 / 0 | pass (2.07) |
| POWER9, 3.0 | 82,176 / 0 | 19,812 / 0 | — | — |
| POWER9, capped to 2.07 | 82,176 / 0 | 19,812 / 0 | — | — |

Also clean on POWER9 during development: an ASSERT build, ASan with UBSan, a concurrency stress
configuration, TestWTF, TestJavaScriptCore and testapi.

### Performance

Measured on POWER9 against an Apple M1 Pro as a shape reference:

| | DFG / LLInt | FTL / DFG | M1 Pro (DFG / LLInt, FTL / DFG) |
|---|---|---|---|
| Microbenchmarks | 5.3× | 1.17× | 4.0×, 1.20× |
| Octane | 12.4× | 1.43× | 12.3×, 1.68× |
| LongSpider | 13.5× | 1.55× | 10.8×, 1.41× |

JetStream2 scores Baseline 33.0, DFG 84.3, FTL 89.8. The POWER10 fast paths beat the POWER8
sequences by 32% on `Math.max`, 50% on three-operand `Math.min` and 21% on integer `%`.

## Upstream bugs found

These affect other architectures too:

- **Bytecode-cache race, not yet reported.** Concurrent writers serialize a Proxy `set` handler as
  an empty stub.
  Reproduce with 20 parallel `JSC_diskCachePath=wN jsc proxy-set-failure-inline-cache.js` writers,
  then `JSC_forceDiskCache=true` readers, which fail with `Bad value: 0!`.
- **Yarr case folding.** Without /u, ſ (U+017F) and K (U+212A) must not match s and k. With /u,
  İ (U+0130) has no simple folding. The interpreter gets these right and the JIT does not, on ARM64
  as well.
- **Yarr /u and /v.** A match starting inside a surrogate pair differs between JIT and interpreter,
  on ARM64 as well.
- **ARM64 non-LSE `atomicStrongCAS`** overwrites `expectedAndResult` before its store-back retry,
  so a spuriously failing `stlxr` can make a mismatching CAS store.
- **GCC `enumName`** parses a fixed-length `__PRETTY_FUNCTION__` prefix that GCC does not keep
  stable.
- **ASan use-after-return** breaks the conservative stack scan. It is now off in
  `ENABLE_SANITIZERS`.
- **JSCOnly** does not export `wtf/linux/HighPriorityThreads.h`, which a 2.54.1 TestWTF test
  includes.
- **Smaller candidates:** `disableAllWasmJITOptions()` re-enables the LLInt; the FP-return
  `OperationResult` change; `emitAtomicOpGeneric()`'s `#else #error`; IPInt loads `ref.test` heap
  types zero-extended; `testElideSimpleMove` runs unguarded without a disassembler.

## Known limits

- **Big-endian WebAssembly SIMD and Yarr multi-character and SIMD searches are off** (see
  *Big-endian*).
- **No POWER8 hardware testing.** This is the largest risk. POWER8 paths were exercised only by
  capping the ISA level, which cannot catch a newer instruction that lacks its runtime check.
- **Yarr inlining is untuned.** `maximumRegExpTestInlineCodesize` (500 bytes) was sized for denser
  code, so only about 4 of 23 sample patterns inline on PPC64.
- **Yarr fuzzing disagreements.** The RegExp fuzzer shows JIT/interpreter disagreements on about 41
  seeds. They predate the PPC64 Yarr work, with identical JIT output before and after it, and are
  not yet classified beyond the case-folding bug above.
- **`--sampleCCode` is partial.** It walks C frames by the ELFv2 back chain, so only the C frames
  above the first JS frame are recorded. Internal functions show as `(unknown C PC)`, because
  `dladdr` sees only exported symbols.
- **FTL slow-path calls** do not widen narrow integer arguments passed in registers (see *ELFv2
  calls*).
- **One ASSERT-build test** (`stack-overflow-llint-large-params-and-large-locals.js`, `no-llint`)
  needs a 35.6 MB allocation, more than one island region holds.
- **Not yet measured:** wasm throughput, island exhaustion on a large wasm application, and
  mimalloc on 64 KB pages.
