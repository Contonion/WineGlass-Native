# WineGlass — Pragmatic Re-Architecture (translator + native Win32 + Wine DLLs)

## 🎯 THE DIRECTIVE (do not forget)
> **Swap blink out for box64. Get Visage BOOTING — at all costs.** Use WineGlass's existing
> Win32 API/thunk layer, and pull in whatever we need from **Wine** to help/speed it up.
> Public GitHub fork for this work. Positive attitude, no stopping until it runs.
> Endgame: **Visage on iOS via a TRANSLATOR (box64), not an emulator (blink).** Download any
> files needed to make this happen.

**Concrete order of battle:**
1. [x] Public repo `Contonion/WineGlass-Native`, box64 vendored (submodule).
2. [ ] Build box64 on macOS arm64 (or its DynaRec in isolation) — see what off-Linux needs.
3. [ ] `wg_cpu.h` backend interface (mirror WGBlinkVM_*); make blink + box64 swappable.
4. [ ] Wrap box64's ARM64 DynaRec as `wg_cpu_box64`; route its call-outs → our Win32 thunks.
5. [ ] Boot a trivial PE, then Visage, on `Tests/build_mac_window.sh` (D3D11→Metal path).
6. [ ] Pull Wine sources for reference/DLLs where our Win32 coverage is thin.
7. [ ] Keep every decision iOS-JIT (StikDebug) compatible.


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
