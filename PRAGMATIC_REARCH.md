# WineGlass — Pragmatic Re-Architecture (translator + native Win32 + Wine DLLs)

## 🎯 THE DIRECTIVE (do not forget)
> **Swap blink out for box64. Get Visage BOOTING — at all costs.** Use WineGlass's existing
> Win32 API/thunk layer, and pull in whatever we need from **Wine** to help/speed it up.
> Public GitHub fork for this work. Positive attitude, no stopping until it runs.
> Endgame: **Visage on iOS via a TRANSLATOR (box64), not an emulator (blink).** Download any
> files needed to make this happen.

**Concrete order of battle:**
1. [x] Public repo `Contonion/WineGlass-Native`, box64 vendored (submodule).
2. [x] Build box64 on macOS arm64 — **DONE / proven portable.** box64's emu + ARM64
   DynaRec compile 100% clean on Apple/clang (the iOS toolchain). Only the Linux
   OS-integration layer (ELF loader, wrapped .so libs, Linux syscalls/threads/signals)
   fails — exactly the parts WineGlass replaces with its PE loader + Win32 thunks.
   macOS port shims (all in Vendor/box64, downstream fork):
     - flags: `-D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Dsincos=__sincos`
     - `os.h`: JUMPBUFF → sigjmp_buf (Android-style) on __APPLE__
     - vendored `src/include/elf.h` (musl, macOS has none)
     - `arm64_asm.h` CNAME + @PAGE/@PAGEOFF; ported all 4 arm64 .S to Mach-O
     - `arm64_printer.c` uintptr_t/uint64_t (Mach-O distinct types)
     - `dynarec_arm64_arch.c` adjust_arch: macOS mcontext stub (TODO: real port)
     - `core.c` prctl→pthread_setname_np; new `src/os/os_macos.c` (Darwin backend)
     - `debug.h` allocator: box_calloc→libc on __APPLE__ (no glibc __libc_*)
   Spike harness `Vendor/box64/box64_spike.c`: drives NewBox64Context + NewX64Emu +
   interpreter Run (BOX64_DYNAREC=0, no JIT/PROT_EXEC needed) on our own mmap'd guest
   memory (identity-mapped above macOS 4GB __PAGEZERO). Links vs a libbox64all.a of the
   clean core objects + spike_stubs.c (zero-stubs for the unused ELF/librarian layer).
3. [x] `wg_cpu.h` backend interface — **DONE.** Backend-neutral vtable API (Sources/Core/
   wg_cpu.h/.c) with blink adapter (wg_cpu_blink.c, wraps existing wg_blink_*) and box64
   backend (wg_cpu_box64.c, real box64 API — struct accesses validated). Selectable via
   `WG_CPU=box64` or wg_cpu_use(). PROVEN: a driver using ONLY wg_cpu_* drove box64 to
   run mov/imul/add/dec → RAX=66. blink path untouched (zero engine call-site churn yet).
4. [~] Wrap box64's DynaRec as `wg_cpu_box64` (create/run/regs/mem DONE) + route its
   call-outs → our Win32 thunks. **Interception SEAM PROVEN** (box64_bridge_spike.c): guest
   CALL → box64 bridge → our C wrapper (reads Win64-ABI args from emu, returns via RAX) →
   RAX=43 ✅. Enter guest via DynaCall(emu, rip, 0); Win32 handled INLINE (no halt-dispatch).
   REMAINING: register every WineGlass Win32 import as a bridge with a generic wrapper that
   routes to our existing handlers, and wire into the engine (tasks #35-37).
5. [~] Boot a trivial PE, then Visage, on the mac harness. **Full engine now BUILDS +
   LINKS on box64** (Tests/build_mac_window_box64.sh, 6MB binary, box64 symbols confirmed,
   pagezero shrunk). BLOCKER to a real boot = the address-space relocation below.
6. [ ] Pull Wine sources for reference/DLLs where our Win32 coverage is thin.
7. [ ] Keep every decision iOS-JIT (StikDebug) compatible.

## ⛔ THE REMAINING BLOCKER: relocate the guest address space above 4 GB (for box64)
box64 is IDENTITY-mapped (guest VA == host VA). **Apple Silicon reserves the low 4 GB**
(mmap MAP_FIXED at 0xDEAD0000 gets SIGKILL'd even with -pagezero_size 0x4000 — proven).
So for box64, EVERY fixed guest address must live above 4 GB. But WineGlass's engine is
**32-bit-centric** even in 64-bit mode (legacy of the Steam/32-bit origin):
- `WG_THUNK_BASE = 0xDEAD0000` (wg_dll_mapper.h) — below 4 GB, and stored/compared as
  `uint32_t` (s_heap_ptr is uint32_t; next_thunk; casts at wg_engine.c:1877/8265, 3728).
- 32-bit PEs load at image base 0x400000 with stacks/heaps < 2 GB — **fundamentally can't**
  use box64 identity on Apple Silicon (low 4 GB reserved). So box64 targets **64-bit PEs
  only** on Apple hw (Visage is 64-bit — fine; 32-bit Steam etc. would need a different path).
- 64-bit path: image base must be high (0x140000000 ok); WG_HEAP64_BASE=0x100000000 ok;
  thunks + any uint32_t address state must move above 4 GB.

**Plan (do WITH the game + device iteration — invasive, risks the working blink path):**
  a. Gate a box64 build flag (WG_CPU_BOX64). Under it, set WG_THUNK_BASE to a high value
     (e.g. 0x2_DEAD_0000, above WG_HEAP64_END=0x2800000000) and widen s_heap_ptr / next_thunk
     / thunk compares from uint32_t to uint64_t on the 64-bit path.
  b. Ensure the PE loader rebases the 64-bit image to a high base (≥0x140000000) with relocs,
     and TEB/PEB/stack allocate above 4 GB.
  c. DynaRec JIT: bridge NewBrick + the dynablock code cache to MAP_JIT + pthread_jit_write_
     protect_np (WineGlass already has this for blink). Interpreter path (BOX64_DYNAREC=0)
     works today and is enough for first boot.
  d. Boot WGTest-class 64-bit PE (build a minimal one), then Visage; debug to the menu.

## ✅ PROVEN (all on the mac harness — see memory/box64-pivot.md)
1. box64 emu+DynaRec compile clean on Apple/clang (only Linux OS-layer fails — we replace it).
2. Execution spike: box64 ran mov/imul/add/dec → RAX=66 via our own harness (no ELF loader).
3. wg_cpu backend interface: a driver using only wg_cpu_* drove box64 → RAX=66.
4. Interception: box64 bridge → our C handler → RAX=43; AND box64 drives WineGlass's own
   HLT-thunk model (wg_blink_box64.c) → dispatch → RAX=43. Zero engine call-site changes.
5. Full WineGlass engine builds + links on box64 (Tests/build_mac_window_box64.sh).

**Key model facts learned (for the wg_cpu_box64 wiring):**
- Registers: `emu->regs[16]` (idx `_AX=0,_CX,_DX,_BX,_SP,_BP,_SI,_DI,_R8..R15`), `emu->ip`,
  `emu->xmm[16]`, `emu->eflags`. Accessors `R_RAX`/`R_RIP`/`R_RSP` (reg.q is a `[1]` array).
- Entry: `Run(emu,step)` = interpreter (step≠0 single-steps); `DynaRun(emu)`/`DynaCall(emu,addr,0)`
  = JIT, falls back to interpreter when `BOX64_DYNAREC=0`. Terminates on `emu->quit`; a
  guest `RET` to `my_context->exit_bridge` ends a `DynaCall`. `PushExit(emu)` sets that up.
- **Memory is IDENTITY-mapped** (guest VA == host VA), MAP_FIXED at guest addresses — like
  Path B without the skew. On macOS/iOS guest mem must live above the 4GB `__PAGEZERO`
  (Win64 image base 0x140000000 already does). DynaRec code cache needs MAP_JIT + W^X on
  Apple (the StikDebug path WineGlass already has for blink) — spike sidesteps via interpreter.


## Goal
Replace **blink** (interpreter, with a basic JIT) with a **mature x86-64→ARM64 translator**
(**box64** first, FEX-emu as an alternative), keep WineGlass's **existing iOS-native Win32
thunk layer**, and **cherry-pick individual Wine DLLs** only where our coverage is thin.

Develop and test on **macOS** (fast iteration), but keep every design decision compatible
with **iOS** running under **JIT via StikDebug** (dev-mode W^X toggle).

## Why (what this fixes)
The blink path hit two hard walls on UE4 (Visage):
1. **Speed** — blink's JIT is basic; UE4's startup constructs ~500K UObjects, taking tens of
   minutes emulated. A FEX/box64-class translator is typically 2–5× faster.
2. **Correctness** — WineGlass's hand-rolled region-3 allocator corrupts FMallocBinned2's heap
   under sustained churn (whole-block reuse) or exhausts address space (exact-only). box64/FEX
   ship a proven memory model; this class of bug largely disappears.

box64 is the recommended first target: it's more embeddable and is proven on Android
(Winlator = box64 + Wine), which is the closest analog to iOS's constraints.

## iOS constraints & the workarounds we design around
| iOS blocker | Workaround baked into the design |
|---|---|
| No arbitrary JIT | Dev-mode JIT via **StikDebug** (W^X toggle). box64/FEX use it. |
| No `dlopen` of native code | **Static-link** all native components into the signed app binary. |
| No `fork`/`exec` of processes | Single-process; stub/thread-ify `CreateProcess` (one app target). |
| No separate `wineserver` process | If/when full Wine is used: run wineserver **in-process** (thread). |
| Sandbox filesystem | Map the Wine prefix / C:\ into the app's `Documents/` container. |

## Architecture
```
Windows .exe (x86-64)
   ↓ WineGlass PE loader (keep)
   ↓
box64/FEX  ── x86→ARM64 JIT (REPLACES blink)
   ↓ intercept Win32 calls
WineGlass Win32 thunk layer (keep — iOS-native: Metal, sockets, files)
   + cherry-picked Wine DLLs where coverage is thin (e.g. rich shell/ole/gdiplus)
   ↓
Native macOS/iOS (Metal, BSD sockets, sandbox FS)
```

## Roadmap
1. **Vendor box64** (git submodule) + build it standalone on macOS arm64.
2. **Embed box64 as a library** (not a standalone loader): expose create-VM / load-PE /
   run / register+memory access, mirroring the current `WGBlinkVM_*` bridge so the engine
   can swap backends behind one interface (`wg_cpu_*`).
3. **Route Win32 interception** from box64's call-out mechanism into WineGlass's existing
   thunk dispatch (`handle_blink_thunk` → rename to `handle_win32_thunk`, backend-agnostic).
4. **Bring up on the mac window harness** (`Tests/build_mac_window.sh`) with the same
   D3D11→Metal path; get a simple PE, then Visage, running.
5. **Cherry-pick Wine DLLs** only where our stubs are insufficient (identified by the
   auto-stub log). Prefer keeping our native thunks for perf-critical / iOS-specific paths.
6. **iOS packaging**: static-link box64 + WineGlass; verify JIT under StikDebug.

## Backend-swap interface (to add)
Introduce `wg_cpu.h` with a vtable-style backend:
`create/destroy, load_code, reserve/map, run/step, get_reg/set_reg/set_xmm, read/write_mem,
set_rip, on_thunk_callback`. blink stays as `wg_cpu_blink`; box64 becomes `wg_cpu_box64`.
This lets us A/B the two on the mac harness and de-risk the swap.

## Status
- Repo forked from Contonion/WineGlass (blink-based, incl. this session's JIT + memory work).
- The blink path reaches: clean boot → menu Slate UI (189 assets) → UObject drain (the wall).
  See memory/visage-boot-frontier.md for the exact state and the clean run command.
- NEXT: step 1 (vendor + build box64 on macOS arm64).
