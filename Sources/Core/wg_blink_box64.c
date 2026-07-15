// wg_blink_box64.c — box64 implementation of WineGlass's CPU bridge API.
//
// This exposes the SAME wg_blink_* symbols as wg_blink_bridge.c, but drives box64
// (x86-64 emu + ARM64 DynaRec) instead of blink. Link this INSTEAD of
// wg_blink_bridge.c + wg_blink_impl.o + blink.a to run the whole WineGlass engine
// on the box64 translator with zero engine call-site changes.
//
// Interception model: WineGlass maps HLT (0xF4) import thunks at 0xDEAD0000. With
// box64_unittest_mode=1, box64's interpreter treats HLT as a clean stop
// (emu->quit=1), so wg_blink_run returns WG_BLINK_HALT with RIP at the thunk — the
// engine's existing handle_blink_thunk dispatch then works unchanged.
//
// Memory model: box64 is IDENTITY-mapped (guest VA == host VA). The engine
// populates guest memory via wg_blink_write_mem at arbitrary guest addresses, so we
// demand-map touched 64 KiB chunks on first access. Guest addresses must be
// host-mappable — Win64 PE image base (0x140000000) and the thunk base sit above
// the macOS/iOS 4 GiB __PAGEZERO, which is fine.
//
// Compiled SEPARATELY with box64 headers on the include path (they shadow system
// headers), exactly like wg_blink_impl.o. See memory/box64-pivot.md.

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <signal.h>

#include "box64context.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"
#include "box64cpu.h"
#include "regs.h"

// Mirror wg_blink_bridge.h's opaque handle + result enum (avoid including it, which
// would pull system headers box64's headers shadow).
typedef struct WGBlinkInstance WGBlinkInstance;
typedef enum { WG_BLINK_OK, WG_BLINK_HALT, WG_BLINK_SYSCALL, WG_BLINK_ERROR } WGBlinkResult;

extern FILE* ftrace;
extern uintptr_t box64_pagesize;
extern int box64_unittest_mode;

// Blink JIT instrumentation symbols the engine reads (for logging). box64 doesn't
// produce these; define them as inert no-ops so the engine links against box64.
unsigned long long wg_lockcas_total = 0, wg_lockcas_acq = 0;
unsigned long long wg_cx16_total = 0, wg_cx16_ok = 0, wg_store_total = 0;
void (*wg_spinlock_acquire_hook)(unsigned long long, unsigned long long) = 0;

// ---- demand-paged identity memory -----------------------------------------
// A hash set of mapped 64 KiB chunk ids (addr>>16). New chunks are mmap'd at their
// own VA. Fixed-capacity open addressing; 1<<21 slots supports ~2M chunks (128 GiB).
#define WG_CHUNK_SHIFT 16
#define WG_CHUNK_SIZE  (1u << WG_CHUNK_SHIFT)
#define WG_HT_SLOTS    (1u << 21)
static uint64_t* s_ht = NULL;          // 0 = empty; stores chunk_id+1
static void ht_init(void) {
    if (!s_ht) s_ht = (uint64_t*)calloc(WG_HT_SLOTS, sizeof(uint64_t));
}
static bool ht_has_or_add(uint64_t chunk) {
    ht_init();
    uint64_t key = chunk + 1;
    uint64_t h = (chunk * 0x9E3779B97F4A7C15ULL) & (WG_HT_SLOTS - 1);
    for (uint32_t i = 0; i < WG_HT_SLOTS; ++i) {
        uint64_t slot = s_ht[h];
        if (slot == 0) { s_ht[h] = key; return false; }  // added (was absent)
        if (slot == key) return true;                    // already present
        h = (h + 1) & (WG_HT_SLOTS - 1);
    }
    return true; // table full — treat as present (best effort)
}
// Apple Silicon reserves the low 4 GB, so guest addresses there can never be
// identity-mapped. Treat them as not-present (read 0 / discard write) — the engine's
// real guest structures are relocated above 4 GB on the box64 path; only stray
// diagnostic probes (Visage-specific hardcoded low addresses) land here.
#define WG_LOW_LIMIT 0x100000000ULL

// Ensure [addr, addr+len) is backed by host memory, mapping new 64 KiB chunks.
// Returns false if any part is unmappable (below 4 GB or a failed reservation).
static bool ensure_mapped(uint64_t addr, uint64_t len) {
    if (!len) return true;
    if (addr < WG_LOW_LIMIT) return false;             // low 4 GB: never mappable
    uint64_t c0 = addr >> WG_CHUNK_SHIFT;
    uint64_t c1 = (addr + len - 1) >> WG_CHUNK_SHIFT;
    bool ok = true;
    for (uint64_t c = c0; c <= c1; ++c) {
        if (ht_has_or_add(c)) continue;
        void* base = (void*)(uintptr_t)(c << WG_CHUNK_SHIFT);
        void* got = mmap(base, WG_CHUNK_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (got == MAP_FAILED) {
            static int warned = 0;
            if (warned++ < 20)
                fprintf(stderr, "[box64] ensure_mapped: mmap @%p failed (guest 0x%llx) — box64-reserved?\n",
                        base, (unsigned long long)addr);
            ok = false;
        }
    }
    return ok;
}

// ---- instance --------------------------------------------------------------
typedef struct wg_inst {
    x64emu_t* emu;
    int       last_stop;      // 0 clean, -1 halt, -4 segfault, -8 #GP
    uint64_t  fault_addr;
    bool      is32;
} wg_inst;

static box64context_t* g_ctx = NULL;
static x64emu_t* volatile s_run_emu = NULL;   // emu currently in Run(), for the SIGSEGV diag

static void wg_box64_segv(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    x64emu_t* e = s_run_emu;
    fprintf(stderr, "[box64] SIG%d fault@%p  guest RIP=0x%llx RSP=0x%llx RAX=0x%llx RCX=0x%llx\n",
            sig, si ? si->si_addr : 0,
            e ? (unsigned long long)e->ip.q[0] : 0,
            e ? (unsigned long long)e->regs[_SP].q[0] : 0,
            e ? (unsigned long long)e->regs[_AX].q[0] : 0,
            e ? (unsigned long long)e->regs[_CX].q[0] : 0);
    _exit(139);
}

static void ensure_context(void) {
    if (g_ctx) return;
    if (!ftrace) ftrace = stderr;
    if (!box64_pagesize) box64_pagesize = (uintptr_t)sysconf(_SC_PAGESIZE);
    box64_unittest_mode = 1;                 // HLT -> clean emu->quit (thunk stop)
    g_ctx = NewBox64Context(0);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = wg_box64_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);   // override box64's
}

#define EMU(i) (((wg_inst*)(i))->emu)

static WGBlinkInstance* mk(bool is32) {
    ensure_context();
    wg_inst* w = (wg_inst*)calloc(1, sizeof(wg_inst));
    w->emu = NewX64Emu(g_ctx, 0, (uintptr_t)NULL, 0, 0);
    SetupX64Emu(w->emu, NULL);
    w->is32 = is32;
    if (is32) w->emu->segs[_CS] = 0x23;
    return (WGBlinkInstance*)w;
}
WGBlinkInstance* wg_blink_create(void)   { return mk(false); }
WGBlinkInstance* wg_blink_create32(void) { return mk(true); }
void wg_blink_destroy(WGBlinkInstance* i) {
    wg_inst* w = (wg_inst*)i;
    if (w && w->emu) FreeX64Emu(&w->emu);
    free(w);
}
void wg_blink_switch_to_32bit(WGBlinkInstance* i) { EMU(i)->segs[_CS] = 0x23; ((wg_inst*)i)->is32 = true; }
bool wg_blink_load_binary(WGBlinkInstance* i, const char* p) { (void)i; (void)p; return false; }

bool wg_blink_setup_stack(WGBlinkInstance* i, uint64_t entry_rip) {
    // box64 executes stack pushes DIRECTLY on the host, so the guest stack must be a
    // real, mapped, high-address region (blink used 0x7FFF0000, unmappable on Apple
    // Silicon). Place it in the proven-free 0x1_4000_0000..0x1_FFFF_FFFF window, clear
    // of the image / thunks / tramp / warm-up.
    const uint64_t stack_top = 0x1A0000000ULL;
    const uint64_t stack_sz  = 0x1000000ULL;     // 16 MB (UE4 asks up to ~11MB)
    ensure_mapped(stack_top - stack_sz, stack_sz);
    EMU(i)->regs[_SP].q[0] = stack_top - 0x100;   // small gap below the top
    EMU(i)->ip.q[0] = entry_rip;
    return true;
}
bool wg_blink_load_code(WGBlinkInstance* i, uint64_t addr, const uint8_t* code,
                        uint32_t size, uint64_t entry_rip) {
    if (ensure_mapped(addr, size))
        memcpy((void*)(uintptr_t)addr, code, size);
    if (entry_rip) EMU(i)->ip.q[0] = entry_rip;   // 0 = "don't change RIP" (thunk/tramp loads)
    return true;
}

// ---- execution -------------------------------------------------------------
WGBlinkResult wg_blink_run(WGBlinkInstance* i, int max_instructions) {
    (void)max_instructions;               // box64 runs to the next HLT/fault, not a slice
    wg_inst* w = (wg_inst*)i;
    box64_unittest_mode = 1;
    // RIP==0 is the engine's "process exited" signal (ExitProcess sets it). blink
    // halts there; box64 would fetch at host 0 and SIGSEGV — so halt cleanly instead.
    if (w->emu->ip.q[0] == 0) { w->last_stop = -1; return WG_BLINK_HALT; }
    w->emu->quit = 0;
    s_run_emu = w->emu;
    Run(w->emu, 0);                        // interpreter (JIT via EmuRun is a later step)
    if (w->emu->quit) {
        // box64 leaves RIP AT the HLT (thunk) address on quit — matches blink's
        // semantics, so the engine's handle_blink_thunk identifies the thunk directly.
        w->last_stop = -1;
        return WG_BLINK_HALT;
    }
    w->last_stop = 0;
    return WG_BLINK_OK;
}
WGBlinkResult wg_blink_step(WGBlinkInstance* i) {
    Run(EMU(i), 1);
    return WG_BLINK_OK;
}

// ---- registers -------------------------------------------------------------
uint64_t wg_blink_get_reg(WGBlinkInstance* i, int idx)          { return EMU(i)->regs[idx].q[0]; }
void     wg_blink_set_reg(WGBlinkInstance* i, int idx, uint64_t v){ EMU(i)->regs[idx].q[0] = v; }
void     wg_blink_set_xmm_low(WGBlinkInstance* i, int idx, uint64_t lo){ EMU(i)->xmm[idx].q[0] = lo; }
uint64_t wg_blink_get_rip(WGBlinkInstance* i)                   { return EMU(i)->ip.q[0]; }
void     wg_blink_set_rip(WGBlinkInstance* i, uint64_t rip)     { EMU(i)->ip.q[0] = rip; }
uint64_t wg_blink_get_flags(WGBlinkInstance* i)                 { return EMU(i)->eflags.x64; }
void     wg_blink_set_flags(WGBlinkInstance* i, uint64_t f)     { EMU(i)->eflags.x64 = f; }

void     wg_blink_set_fs_base(WGBlinkInstance* i, uint64_t b)   { EMU(i)->segs_offs[_FS] = b; }
void     wg_blink_set_gs_base(WGBlinkInstance* i, uint64_t b)   { EMU(i)->segs_offs[_GS] = b; }
uint64_t wg_blink_get_fs_base(WGBlinkInstance* i)              { return EMU(i)->segs_offs[_FS]; }
uint64_t wg_blink_get_gs_base(WGBlinkInstance* i)              { return EMU(i)->segs_offs[_GS]; }

int      wg_blink_get_stop_reason(WGBlinkInstance* i)          { return ((wg_inst*)i)->last_stop; }
uint64_t wg_blink_get_fault_addr(WGBlinkInstance* i)           { return ((wg_inst*)i)->fault_addr; }

// ---- memory (demand-paged identity) ---------------------------------------
bool wg_blink_write_mem(WGBlinkInstance* i, uint64_t addr, const void* buf, uint32_t len) {
    (void)i;
    if (!ensure_mapped(addr, len)) return true;   // unmappable (low 4GB) — discard, like not-present
    memcpy((void*)(uintptr_t)addr, buf, len); return true;
}
bool wg_blink_read_mem(WGBlinkInstance* i, uint64_t addr, void* buf, uint32_t len) {
    (void)i;
    if (!ensure_mapped(addr, len)) { memset(buf, 0, len); return true; }  // not-present -> zeros
    memcpy(buf, (void*)(uintptr_t)addr, len); return true;
}
uint64_t wg_blink_mem_copy(WGBlinkInstance* i, uint64_t dst, uint64_t src, uint64_t n) {
    (void)i;
    if (!ensure_mapped(dst, n) || !ensure_mapped(src, n)) return dst;
    memcpy((void*)(uintptr_t)dst, (void*)(uintptr_t)src, n); return dst;
}
uint64_t wg_blink_mem_set(WGBlinkInstance* i, uint64_t dst, int c, uint64_t n) {
    (void)i;
    if (!ensure_mapped(dst, n)) return dst;
    memset((void*)(uintptr_t)dst, c, n); return dst;
}

// ---- real-threads (a Machine/emu per guest thread) -------------------------
void* wg_blink_new_thread_machine(WGBlinkInstance* i) {
    ensure_context();
    x64emu_t* m = NewX64Emu(g_ctx, 0, (uintptr_t)NULL, 0, 0);
    SetupX64Emu(m, EMU(i));           // clone base state from the parent
    return m;
}
void wg_blink_adopt_machine(void* machine) { (void)machine; }        // box64 emu is passed explicitly
void wg_blink_free_thread_machine(void* machine) {
    x64emu_t* m = (x64emu_t*)machine; if (m) FreeX64Emu(&m);
}

// ---- info ------------------------------------------------------------------
bool        wg_blink_has_jit(void)      { return true; }
const char* wg_blink_version(void)      { return "box64 (WineGlass backend)"; }
void        wg_blink_force_jit(int on)  { (void)on; }
int         wg_jit_smoke_test(void)     { return 42; }
