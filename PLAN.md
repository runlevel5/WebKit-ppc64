# JavaScriptCore on PPC64LE — port notes

JSC's JIT tiers and WebAssembly on 64-bit little-endian POWER. **POWER8 (Power ISA 2.07) is the
baseline**; POWER9 (ISA 3.0) and POWER10 (ISA 3.1) instructions are used only behind runtime checks.
Non-goals: 32-bit PPC, AIX, pointer authentication. Big-endian ELFv2 is a second target on the
`ppc64-be` branch (see *Big-endian*); ELFv1 stays C_LOOP.

- Branch `ppc64` on the fork `github.com/runlevel5/WebKit-ppc64`, based on the **WebKitGTK 2.54.1**
  tag (`webkitgtk-2.54.1` = `628d6ce1850e`, release branch `webkitglib/2.54`) since 2026-10-15.
  The earlier `main`-based line is `ppc64-main-base` (`476f932e2234`); upstreaming to `main` starts there.
- Sister port: SpiderMonkey PPC64 in `~/Work/firefox` (read-only; its `PLAN.md` documents the
  traps it hit). Reference backends here: ARM64 for semantics, RISCV64 for the shape of a new port.

## Status

On by default, all tested on POWER9 and POWER10:

- LLInt (offlineasm backend), Baseline, DFG, FTL (B3/Air).
- WebAssembly: IPInt (including SIMD), BBQ, OMG; SIMD, tail calls, wide arithmetic; signal-based
  bounds checks (`useWasmFastMemory`, `useWasmFaultSignalHandler`).
- Yarr RegExp JIT (all features ARM64 has, including the SIMD Boyer-Moore and multi-pattern
  searches), sampling profiler (with `--sampleCCode`), signal-based VM traps, jump islands,
  branch compaction.
- Float16 JIT paths on POWER9+; POWER9/POWER10 fast paths (see ISA levels below).

Latest gates (2.54.1 suites):

| Host, ISA level | JSTests/stress | JSTests/wasm.yaml | testmasm | testb3 / testair |
|---|---|---|---|---|
| power9, 3.0 | 82,176 / 0 | 19,812 / 0 | — | — |
| power9, capped 2.07 | 82,176 / 0 | 19,812 / 0 | — | — |
| power10, 3.1 | 82,176 / 0 | 19,812 / 0 | 386 / 0 | pass |
| power10, capped 3.0 / 2.07 | — | — | 386 / 0 | pass (2.07) |

Also clean at some point on POWER9: ASSERT build, ASan + UBSan (no ASan reports; failures were
sanitizer stack/memory overhead), concurrency stress, TestWTF / TestJavaScriptCore / testapi,
RegExp yaml 85 / 0, RegExpTest.data 2,085 checks.

Benchmarks (POWER9, before the 2.54.1 move; `power9:/home/tle/bench/SUMMARY.txt` has the rest):

| | DFG / LLInt | FTL / DFG | M1 Pro reference (DFG/LLInt, FTL/DFG) |
|---|---|---|---|
| microbenchmarks | 5.3× | 1.17× | 4.0×, 1.20× |
| Octane | 12.4× | 1.43× | 12.3×, 1.68× |
| LongSpider | 13.5× | 1.55× | 10.8×, 1.41× |

JetStream2 (1 GB pool, packed allocations): Baseline 33.0, DFG 84.3, FTL 89.8. POWER10 fast paths
vs the POWER8 sequences: Math.max −32%, 3-operand Math.min −50%, int `%` −21%, big-endian
`getFloat64` −40%. Epiphany Speedometer 3.1 (Fedora RPM, Xvfb software rendering): 3.45 → 4.85.

## Design notes

### Registers

| Register | Role |
|---|---|
| r0 | Assembler scratch. As a base in D-form/`addi`, RA = 0 means literal zero, so never a base. |
| r1 / r2 / r13 | sp / TOC / TLS (never touched) |
| r3–r10 | Arguments, `regT0–7`; the 8 volatile allocatable GPRs |
| r11, r12 | MacroAssembler `dataTempRegister` / `memoryTempRegister`; not allocatable. Any operation may clobber them (immediates, wide offsets, absolute addresses). r12 also carries the callee address for an ELFv2 global entry. |
| r14–r19 | `regCS0–5`, allocatable callee-saves (in `dfgCalleeSaveRegisters`) |
| r20–r23 | `regCS6–9`: metadataTable, jitData, numberTag, notCellMask |
| r19 | Yarr `regT3` (always saved) |
| r24–r28 | Yarr scratch (saved by Yarr, like x86_64's rbx/r12–r15); Air also allocates r24–r30 |
| r29, r30 | JIT-side `nonArgGPR0/1`, `wasmScratchGPR`: ELFv2 callee-saved but scratch to JSC (LLInt/IPInt temporaries, saved by `pushCalleeSaves` at C→VM entry, never allocated by Air) |
| r0, r27–r30 | offlineasm's own scratch; offlineasm `ws0/ws1` stay r11/r12 |
| r31 | frame pointer (`cfr`) |
| f0 | `fpTempRegister`; f1–f13 FP arguments; f14–f21 VM callee-saves |

ELFv2 callee-saves are r14–r31 and f14–f31; the VM models only r14–r23 and f14–f21. Anything that
crosses a C↔JS boundary must preserve what offlineasm uses beyond that set: vmEntry saves r24
(csr10) and r25–r30; the FTL IC slow-path clobber set is `registersToSaveForCCall`; BBQ must not
allocate f22–f31.

### ELFv2 calls

- A callee writes CR at **sp+8**, LR at **sp+16**, TOC at **sp+24** of the *caller's* frame; the
  parameter save area starts at sp+32. **Any C call made with live data in [sp, sp+32) is a bug**:
  lower sp first (`makeSpaceOnStackForCCall`, `maxFrameExtentForSlowPathCall` = 96). This was the
  most frequent bug class (~20 sites). Diagnostic: a frame header holding code addresses; the
  clobbered offset names the save (8 CR, 16 LR, 24 TOC).
- Argument *position* counts both banks: an FP argument skips its GPR. Integers at positions 0–7 go
  in r3–r10, FP arguments in f1–f13, and anything in memory at `sp + 32 + 8 * position`. 96 bytes
  covers every in-register call; `setupArguments` static_asserts the operation fits.
- The caller sign/zero-extends narrow integer arguments to 64 bits (callees use an `int32_t` as an
  index directly). `CCallHelpers` widens by C type at the ELFv2 position, except `GPRReg`-typed
  entries in FTL's synthesized `void(ArgumentTypes...)` signatures, which name registers, not
  parameters. B3 CCallSpecial still sign-extends every Int32 (wrong for `uint32_t` ≥ 2^31).
- C calls go through CTR with r12 = target (global entry builds the TOC from it). `farJump` to a C
  function must set r12 too. JS→JS calls use a bare `bl`/`bctrl` and never touch r2.
- `__builtin_frame_address` is not used (`USE(BUILTIN_FRAME_ADDRESS)` is off): wasm operations need
  the frame stored into the instance's `temporaryCallFrame` before the call.

### Link register

`linkRegister` is an r0 placeholder: LR is an SPR. Every return-address save/restore must use
`mflr`/`mtlr` (`moveFromLR`/`moveToLR`), never a register move — the arity-check entries, CTI thunk
prologue/epilogue, DataIC handlers and tail-call shuffles all broke on this. Baseline DataIC handlers
are entered as leaves with the return address in LR and must spill it before a nested call
(`emitDataICPrepareForCall`). OSR entry reads LR from `context.spr(lr)`.

### Memory model

- Fences are real: `lwsync` (acq/rel), `sync` (seq_cst), `isync` after a failed CAS compare.
- LL/SC is RCsc (leading `sync`), matching ARM64's ldaxr/stlxr. `storeRel*` is `sync; st`;
  `loadAcq*` is `ld; lwsync`. Strong CAS never stores back after a mismatch.
- Cross-modifying code: Power allows concurrent modification only of `b`/`nop`-class words. The
  icache flush is hand-written (`dcbst; sync; icbi; sync; isync`, block size from
  `AT_DCACHEBSIZE`/`AT_ICACHEBSIZE`): `__builtin___clear_cache` compiles to nothing on ppc64le.
  `crossModifyingCodeFence()` is `isync`.

### Code layout and patching

- Branch reach: `bc` ±32 KB, `b`/`bl` ±32 MB. Jump/call slots are fixed 8 instructions; the far form
  is `nop; li64 r12; mtctr; bctr[l]`, so word 0 is always a `nop` or `b` and retargeting within
  reach is one aligned store. Near calls to JIT code are `nops; bl`; `call()` slots keep `li64 r12`.
  `call()` is `Linkable`, only `nearCall()` is `LinkableNear`; both link through `applyCallSlot`.
- Jump islands: `nearJumpRange` 32 MB, 2 MB island area per 32 MB region (65,536 islands; ~28 MB of
  code per region). An island is a whole jump slot, so islands never chain. Only concurrent wasm
  retargets use islands; other repatching happens on the owning or a stopped thread. Concurrent
  retargets never free islands (upstream behaviour). The executable pool is 1 GB with allocations
  packed at the bottom (locality only).
- Branch compaction shrinks unpatched jumps (b / bc / bc+b / full slot). 64-bit immediates take 1–5
  instructions; `moveWithPatch` keeps the fixed 5-instruction li64, patched at its start.
- VM-trap halt is `ld r0,0(0)` (SIGSEGV; `trap` would be SIGTRAP, which VMTraps does not handle).

### SIMD

- A v128 is the full VSR aliasing FPR fN (VSR 0–31); doubleword 0 is the scalar. VMX-only operations
  stage through v0–v5 (VSR 32–37) inside one MacroAssembler call; nothing is live in VSR 32–63
  between calls. Callee-saves stay 64-bit (f14–f31 doubleword 0), the ARM64 d8–d15 model.
- Layout is lxvd2x/stxvd2x: doubleword 0 = bytes 0–7 as a little-endian u64; loads/stores need no
  permute. Wasm lane i of an S-byte element is ISA element `i ^ (8/S − 1)`. VMX pack/unpack see the
  doublewords swapped (add `xxswapd`); "high"/"low" in VMX names are big-endian.
- f32 lives in double format in FPRs, so f32x4 lane 0 is *not* the scalar. Scalar FP spills use
  `stfd`/`lfd`, never `stxvx`.
- IPInt SIMD: offlineasm v0/v1/v2 = VMX 0/1/14, VMX 2–7 scratch, same lane layout. Handler slots are
  512 bytes (`IPInt::initialize()` validates the stride).
- POWER8 leaves some result words undefined (`xvcvdpsp`, `xvcvdpsxws/uxws`, `xscvdpspn`); the
  sequences read only defined words.

### ISA levels

`MacroAssemblerPPC64::isaLevel()` reads `AT_HWCAP2` (ARCH_3_00 → 300, ARCH_3_1 → 310), capped by the
option `maximumPPC64ISALevel` (`JSC_maximumPPC64ISALevel=207|300|310`). Gated instructions:

- 3.0: `cnttzw/d`, `modsw/uw/sd/ud` (DFG, BBQ, Air `Mod32/UMod32/Mod64/UMod64`),
  `xsmaxjdp/xsminjdp`, `xscvhpdp/xscvdphp` (Float16), `xxinsertw`/`vinsertb/h` (wasm
  replace_lane), `cmpeqb` (Yarr classes of up to 8 Latin-1 characters; 16-bit strings exclude
  characters above 0xff first, since it compares only the low byte).
- 3.1: `brh/brw/brd`, `setbc/setbcr`, `vmulld`, `vextract{b,h,w,d}m`.

`xsmaxjdp/xsminjdp` return a signalling NaN unquieted: fine for JS (NaNs are purified), wrong for
wasm, so B3 keeps the FMin/FMax expansion when `Procedure::isWasm()` and BBQ keeps its sequence
(`JSTests/wasm/stress/min-max-signalling-nan-is-quieted.js`). Divide and remainder results are
undefined for x/0 and INT_MIN/−1: callers guard both. Float16 lives in bits 48:63 of VSR
doubleword 0.

### Other invariants

- 32-bit values are kept zero-extended in 64-bit registers (offlineasm `i` ops and
  `move(TrustedImm32)` clear the upper word; PPC `addi`/`lis` sign-extend).
- **IPInt is mandatory for wasm.** BBQ/OMG are tier-ups from IPInt; `useWasmIPInt=false` means "tier
  up immediately", not "skip IPInt".
- Arch gates that omit PPC64 compile cleanly and emit nothing or the x86 shape — the most common
  silent bug. Air opcode gates live on individual form sub-lines in `AirOpcode.opcodes`.
  `PPC64_UNSUPPORTED()` marks deliberately absent operations; hitting one means a gate leaked.
- Naming: `CPU(PPC64)` is 64-bit PowerPC in either byte order, `CPU(PPC64LE)` the little-endian
  subset; byte-order-specific code uses `CPU(BIG_ENDIAN)`. The offlineasm backend, Air arch tags and
  `isPPC64()` all say PPC64. CMake enables the JIT for `WTF_CPU_PPC64LE` or for `WTF_CPU_PPC64`
  with `WTF_PPC64_ELFV2` (`_CALL_ELF == 2`); ELFv1 stays C_LOOP.
- offlineasm: resolve PtrTags symbolically (never re-implement a WTF hash in Ruby); `ppc64.rb` is a
  CMake dependency of LLIntAssembly.h.
- GC scans the 288-byte red zone below sp. Pages are 64 KB (memory-threshold tests are tuned for 4 KB).

## Big-endian

Branch `ppc64-be` on top of `ppc64`; host `ppc64elfv2` (Arch POWER, POWER9 KVM guest). The ABI and
every instruction sequence are shared with LE; only the byte order of memory differs, and nearly
every bug was one shape: **a 32-bit access to the first four bytes of an 8-byte slot** (a boxed
int32's payload, the low word of a pointer-sized field, a `Move32` spill into an 8-byte stack slot).

- JSValue halves: `TagOffset` 0 / `PayloadOffset` 4. Fills of int32 locals use `payloadFor`; the
  callee slot on JSVALUE64 is a whole pointer (`addressFor`). The LLInt's
  `loadp Callee + PayloadOffset[cfr]` on 64-bit was a latent upstream bug.
- Boxed-int32 metadata read with `load32` needs `+ PayloadOffset`: Map/Set ordered-hash-table
  counts (DFG, FTL, the IC `size` getter); DataView `isLittleEndian` is inverted on a BE host and
  the CSE key must carry byteSize/isSigned (the AbstractHeap payload is 48 bits).
- Low word of a 64-bit field: `JSRopeString::offsetOfFlags()`, Yarr `branch32` against frame slots
  written with `storePtr` (`frameSlotLowWordAddress`), `FreeCell::scrambledBits` halves in the
  inline allocator, `FreeList` secret.
- Packed words built by shifting: object header (`structureID << 32 | typeInfo`), rope
  `CompactFibers`, `JSBigInt` header (`1 << 32`), `OpInfoWrapper` (integers stored zero-extended in
  `int64` and read back truncated — the `int32` union member is gone), `ArrayMode` (a bare
  `Array::Type` no longer converts to the private word constructor).
- Air: GP spill slots are at least 8 bytes and `lowerStackArgs` moves a narrower spill access to the
  low half, except Patch (stackmap) arguments, which the OSR exit / B3 stackmap code restores with a
  whole-slot `load64`.
- offlineasm: `btbz addr, mask` was lowered to `andi` on a 32-bit load; `ppc64LowerByteTestsOnMemory`
  loads the byte first (`MasqueradesAsUndefined`, typed-array mode byte).
- Yarr: surrogate-pair loads are 32-bit pairs of UTF-16 code units — `rotateRight32(16)` after the
  load; multi-character compares (`maxCharactersAtOnce = 1`) and SIMD searches are off on BE.
- Uint8Array hex encode/decode lane shifts and the DataView stress tests assumed an LE host.
- WebAssembly runs on BE in every tier. Two rules carry it: wasm memory is little-endian, so every
  access wider than a byte byte-reverses — offlineasm `load*le`/`store*le`/`bswap*` under
  `if BIG_ENDIAN` (ppc64 lowers them to `l*brx`/`st*brx`, r0 and the red zone as scratch), the
  `*LittleEndian` helpers in MacroAssemblerPPC64 for BBQ, patchpoints in OMG (B3 has no
  byte-reversed memory ops), LL/SC and CAS loops swapping around the operation; and a 32-bit value
  lives at `PayloadOffset` (+4) of its 8-byte frame slot everywhere — IPInt pushes whole words, BBQ
  gives i32/f32 8-byte slots on BE (its 4-byte slots put a payload on a saved callee-save), the
  `IPIntStackEntry`/`IPIntLocal` unions pad i32/f32, OMG and Air address the payload half of
  stack-passed and global 32-bit values, the entry thunks already did. Module data is
  little-endian by format: header version, f32/f64 constants, `memory.atomic.wait` operands,
  data-segment copies into i16/i32/i64 arrays. Two LE-invisible bugs fell out: the JS→wasm call
  stub stored the boxed callee pointer at `slot + PayloadOffset`, and the frame builder wrote
  converted i32/f32 arguments as 4 bytes at the slot start. SIMD stays off on BE (v128 lane order
  is not reversed). Test aid: wabt needs a working wasm, so compile WAT on power10 and ship the
  bytes (`/tmp/gen.js` → JSON → `/tmp/be-ops.js`). IPInt pushes an int32 zero-extended (ELFv2
  returns it sign-extended), so 64-bit results (`ref.cast`, memory64 `memory.grow`) push whole
  words. Gate (`wasm.yaml`, `--memory-limited`): 15,313 pass / 766 fail, all expected — 567
  SIMD-named variants, and 199 that are BE-divergent by design: tests reading wasm memory through
  JS typed arrays or DataView (host order: context-switch, grow-memory-2, load-offset,
  memory-reuse, trap-from-start*, test_memory, memory_copy_shared, simple-inline-stacktrace*, v8
  import-memory/instance-gc/instantiate-module-basic), modules that need SIMD
  (tail-call-should-not-clobber-caller-origin, omg-tail-call-clobber-scratch-register,
  ipint-multimem-oob), and `wasm-slow-memory` variants of wabt-using tests, whose emscripten glue
  writes `environ_sizes_get` through `HEAPU32` in host order; wasm reads a huge size, grows its
  memory to ~2 GB, and slow memory's zeroed malloc makes that resident past the 2 GB limit.
- Off on BE: Yarr multi-character compares and SIMD searches.
- Debugging: `--dumpFTLDisassembly=true` prints per-Air-instruction address ranges without a
  disassembler, usable with gdb `x/Ni`; `--collectContinuously=true` makes heap corruption
  deterministic. Release `RELEASE_ASSERT`s fold into one `abort()` block per function, so a -g1 line
  number can name a different assert in the same function.
- Gate: `~/be-jit-gate.sh` (TZ=America/Los_Angeles, `JSCTEST_timeout=120`, `ulimit -c 0`; core dumps
  filled the disk once). The tree there is rsync'd, not git: a source edited while ninja runs ends up
  older than its object — `touch` it. Result: 82,106 pass, 88 fail, all nine
  failing tests needing `WebAssembly`.
- Releases ship two JIT patches, LE (`webkitgtk-2.54.1..ppc64`) and BE (`ppc64..ppc64-be`, applied
  on top of the LE one); `ppc64-be` is gated on LE too (power10: stress 82,194/0, wasm 19,812/0).

## Testing

Rule: never weaken, skip or special-case a test to make it pass.

- **power9** (`ssh power9`): `/home/tle/stressgate.sh` (stress, IPINT, `-c 24`, compares against the
  empty `/home/tle/stress-baseline-fails.txt`), `/home/tle/wasmrun.sh` (wasm.yaml →
  `/home/tle/wsuite`), `/home/tle/isagates.sh 300 207` (stress + wasm per ISA level →
  `stress-<lvl>`, `wsuite-<lvl>`), `/home/tle/assertgate.sh <BUILD>`, `/home/tle/sangate.sh`
  (both run `~/Work/WebKit/WebKitBuild/<BUILD>`; the ASSERT and SAN builds exist only in
  `~/Work/WebKit-main-base`, so configure them in the 2.54.1 tree first)
  (ASAN_OPTIONS `detect_leaks=0:handle_segv=0:handle_sigbus=0:handle_sigill=0:allocator_may_return_null=1`,
  6 GB memory limit, ~29 h), `/home/tle/concgate.sh` (7 DFG/FTL/wasm compiler threads, 8 GC markers).
  Yarr: `/home/tle/yarr/{allgates,quick,fuzz}.sh`, `rxdata.js`, `rxlong.js`/`rxlong-plain.js`.
  SIMD: `/home/tle/simd/` (encoding check, per-op harness, differential test in `diff/`).
  VM traps: `/home/tle/traps/`. Test-binary driver: `/home/tle/drive.sh`.
- **power10**: `/home/fedora/p10gates.sh` (testair/testb3 at 310 and 207, then stress + wasm at 310).
- Runner settings: `--memory-limited` is mandatory (otherwise an OOM test kills the workers),
  `JSCTEST_memoryLimit=2147483648` (64 KB pages), `TZ=America/Los_Angeles`, `TMPDIR` off `/tmp`.
  Launch from a script file: `pkill -f run-jsc-stress` matches your own ssh command line.
- testmasm/testb3/testair need a DEVELOPER_MODE build. testmasm has PPC64 litmus tests (MP, SB, R,
  weak/strong CAS), cross-modifying-call, many-mixed-C-arguments, Float16 and ISA-level tests.
  TestWTF needs `TZ=America/Los_Angeles`; pipe gtest through `stdbuf -oL`.
- `sampling-profiler-*` tests occasionally time out under full load; rerun before investigating.
- There is no PPC64 disassembler: check encodings with `as -mpower8/-mpower9/-mpower10` + `objdump`,
  and read JIT code with gdb (`/home/tle/yarr/dis.sh`) or `perf` + `--useJITDump`
  (`/home/tle/bench/jitdump2map.py` converts the dump, which `perf inject` ignores).
- Debugging that worked: valgrind memcheck runs the JIT (`--smc-check=all-non-file`); an ASSERT build
  turns silent corruption into a named assertion; gdb record/reverse watchpoints found the linkage
  clobbers; for perturbation-sensitive bugs write to a `volatile` global and read the core.

## Hosts and builds

- **power9** — `tle@192.168.1.247`, POWER9, 32 threads, 63 GB, Fedora 45. `~/Work/WebKit` is the
  2.54.1 tree (no `.git`, synced by rsync) with builds `IPINT` (Release GCC, the gate build),
  `CLANG` (Release clang 23), `GTK-VSX`. The old `main`-based tree `~/Work/WebKit-main-base` holds
  `ASSERT`, `SAN` and `DEV` builds.
- **power10** — `fedora@140.211.169.158`, 8 vCPU, 15 GB, ~20 GB free, Fedora 44, GCC 16, ccache.
  Sparse shallow clone at `~/Work/WebKit` (JSTests, Source, Tools, PerformanceTests/{JetStream2,
  Speedometer2.1}, LayoutTests/imported/w3c/web-platform-tests/{wasm,resources},
  LayoutTests/imported/w3c/resources — wasm.yaml needs the WPT paths). Builds `IPINT` and `DEV`.
  Files are synced from the Mac with rsync.
- **power8** — `fedora@140.211.11.222`; the host key has not been verified, so nothing has run there.

Build recipe (JSCOnly, as on power9):

```sh
rsync -az --delete --exclude .git --exclude WebKitBuild /Users/tle/Work/WebKit/ power9:/home/tle/Work/WebKit/
sudo dnf install -y perl wabt          # full core Perl: piecemeal modules fail late and opaquely
TMPDIR=/home/tle/tmp CXXFLAGS="-Wno-error=sfinae-incomplete" \
  cmake -S . -B WebKitBuild/IPINT -GNinja -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_JIT=ON -DENABLE_FTL_JIT=ON -DENABLE_WEBASSEMBLY=ON -DUSE_SYSTEM_MALLOC=ON
TMPDIR=/home/tle/tmp ninja -C WebKitBuild/IPINT jsc
```

Add `-DDEVELOPER_MODE=ON -DDEVELOPER_MODE_FATAL_WARNINGS=OFF` for the test binaries,
`-DENABLE_ASSERTS=ON` for ASSERT, `-DENABLE_SANITIZERS="address;undefined"` for SAN.
`USE_SYSTEM_MALLOC=ON` is required (libpas has no PPC64 port; mimalloc on 64 KB pages untried).

Build traps — both look like compile errors that contradict the file you just wrote:

- **Stale PCH**: GCC does not invalidate `cmake_pch.hxx.gch` when headers change, and rsync's
  rename gives an edited header a new inode, so `#pragma once` fails (redefinition errors). `rm` the `.gch`.
- **Silent rsync failure**: a `for … rsync` loop hides ssh timeouts. Retry and verify with md5.
- `/tmp` on power9 is a quota'd tmpfs: exceeding it gives an empty ninja log and exit 1.
- Fast loops: offlineasm alone in ~15 s, and a single-file syntax check in ~14 s against a copy of
  `cmake_pch.hxx` with no `.gch` beside it (scripts were in `/tmp/k/` and must be regenerated after
  a build).

## Releases and packaging

- Tag `webkitgtk-<version>-ppc64` on the fork, with three release assets. The big-endian patch is
  incremental: it applies after the LE JIT patch (`ppc64-be` is `ppc64` plus the BE commits).

  ```sh
  git diff webkitgtk-2.54.1 <le-commit> -- Source/ThirdParty/skia > webkitgtk-2.54.1-ppc64-skia-vsx.patch
  git diff webkitgtk-2.54.1 <le-commit> -- Source/ ':!Source/ThirdParty/skia' > webkitgtk-2.54.1-ppc64-jit.patch
  git diff <le-commit> <be-commit> -- Source/ > webkitgtk-2.54.1-ppc64-be-jit.patch
  ```

  Check both apply with `patch -p1` to the release tarball (`power9:~/Projects/webkitgtk-rpm/`) before
  uploading. Recutting moves the tag (`git push -f origin refs/tags/...`), `gh release upload --clobber`,
  and `gh release edit --target`.
- Skia VSX patch: CPU raster backend paths gated on `SK_CPU_PPC && __VSX__ && SK_CPU_LENDIAN`;
  blitters, sampling and swizzling are bit-exact, the raster pipeline differs only by SIMD rounding;
  ~3× faster.
- Spec repo `github.com/runlevel5/fedora-ppc64` (`~/Work/fedora-ppc64`): branches `rawhide` and `f45`,
  one directory per package, `Release: <fedora>.ppc64.<n>`, COPR builds with `make_srpm`
  (`.copr/Makefile` runs `spectool` + `rpmbuild -bs`).
- Local RPM build on power9: `~/Projects/build-fedora-ppc64.sh` (from the spec repo clone in
  `~/Projects/fedora-ppc64`, 16 jobs — more has hung the box; output `~/rpmbuild-fp64`). The Fedora
  dist-git clone `~/Projects/webkitgtk-rpm` is local only; never push it.
- Rebasing onto the next 2.54.x: push a backup branch first, fetch the tag with
  `git fetch --no-tags https://github.com/WebKit/WebKit.git refs/tags/<tag>:refs/tags/<tag>`
  (the fork has no release tags), rebase, re-run the gates, `push --force-with-lease`.

## Upstream bugs found

- **Bytecode-cache race**: concurrent writers serialize a Proxy `set` handler as an empty stub
  (any arch). Repro: 20 parallel `JSC_diskCachePath=wN jsc proxy-set-failure-inline-cache.js`
  writers, then `JSC_forceDiskCache=true` readers → `Bad value: 0!`. Not yet reported.
- **Yarr interpreter case folding**: without /u, ſ (U+017F) and K (U+212A) must not match s/k; with
  /u, İ (U+0130) has no simple folding. The interpreter gets these right and the JIT does not, on
  ARM64 too.
- **Yarr JIT/interpreter**: /u and /v matches starting inside a surrogate pair disagree; same on ARM64.
- **ARM64 non-LSE `atomicStrongCAS`** overwrites `expectedAndResult` before its store-back retry, so a
  spuriously failing `stlxr` can make a mismatching CAS store (PPC64's version avoids this).
- **GCC `enumName`** parses a fixed-length `__PRETTY_FUNCTION__` prefix GCC does not keep stable.
- **ASan use-after-return** breaks the conservative stack scan; now off in `ENABLE_SANITIZERS`.
- JSCOnly does not export `wtf/linux/HighPriorityThreads.h`, which a 2.54.1 TestWTF test includes.
- Candidates from the port: `disableAllWasmJITOptions()` re-enabling the LLInt; the FP-return
  `OperationResult` change; `emitAtomicOpGeneric()`'s `#else #error`; IPInt loading `ref.test`
  heap types zero-extended; `testElideSimpleMove` unguarded without a disassembler.

## Open work and known limits

- **POWER8 silicon** — the largest risk. Everything ran on POWER9/POWER10, with POWER8 paths tested
  only by capping the ISA level, which cannot catch an unguarded newer instruction.
- Measured and not worth doing: POWER9 extract lane (`mfvsrld` runs as fast as
  `xxswapd; mfvsrd`) and `cmprb` for one or two RegExp ranges (`[a-zA-Z]` 1.56 → 1.54 ns/char).
- Not worth doing: POWER10 prefixed `pli`/`pla`. Measured on POWER10, a pointer constant
  (`li; sldi; oris; ori` vs `pli; sldi; ori`), an int32 (`lis; ori` vs `pli`) and a far call
  (`li64; mtctr; bctrl` vs `pla r12; mtctr; bctrl`) run at the same speed, and only the far call
  is smaller (12 bytes, mostly in cold slow paths). Prefixed instructions must not cross a 64-byte
  boundary, which LinkBuffer's branch compaction and the allocator's alignment do not preserve.
- Yarr: `maximumRegExpTestInlineCodesize` (500 bytes) is untuned for PPC64's larger code (~4 of 23
  sample patterns inline). The RegExp fuzzer (`fuzz.sh 1 300 3000`) shows JIT/interpreter
  disagreements on ~41 seeds that predate the PPC64 Yarr work (identical JIT output before and
  after it); they are not yet classified beyond the known case-folding interpreter bug.
- `--sampleCCode` walks C frames by the ELFv2 back chain, so only the C frames above the first JS
  frame are recorded, and internal (hidden) functions show as `(unknown C PC)`: `dladdr` sees only
  exported symbols.
- Narrow integer *register* arguments on FTL slow-path calls are not widened (see ELFv2 calls).
- ASSERT build: `stack-overflow-llint-large-params-and-large-locals.js` (`no-llint`) needs a 35.6 MB
  single allocation, more than one island region holds.
- Not measured: wasm throughput, island exhaustion on a large wasm app, POWER8 anything.
- mimalloc on 64 KB pages untried; `USE_SYSTEM_MALLOC=ON` throughout.
- Upstreaming to WebKit `main` from `ppc64-main-base`.
