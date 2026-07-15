// wg_cpu_box64.c — box64 backend for the wg_cpu interface.
//
// Implements the wg_cpu vtable against box64's x86-64 emu + ARM64 DynaRec, using
// the initialization sequence proven by Vendor/box64/box64_spike.c (see
// memory/box64-pivot.md). Compiled SEPARATELY with box64's headers on the include
// path (they shadow system headers), exactly like wg_blink_impl.c.
//
// Memory model: box64 is IDENTITY-mapped (guest VA == host VA). WineGlass's PE
// loader maps the image at its own VA (above the macOS/iOS 4GB __PAGEZERO), so
// read/write_mem are direct host memcpy. Win32 interception: box64's INT3/bridge
// mechanism (AddBridge) routes guest calls into native handlers — the next wiring
// step points those at WineGlass's Win32 thunk dispatch.

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#include "box64context.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"
#include "box64cpu.h"
#include "regs.h"

// Public wg_cpu vtable type — declared without pulling wg_cpu.h (which would drag
// system headers box64's headers shadow). Keep this struct byte-identical to the
// wg_cpu_backend_t in wg_cpu.h.
typedef struct wg_cpu wg_cpu_t;
typedef enum { WG_CPU_OK=0, WG_CPU_HALT, WG_CPU_SYSCALL, WG_CPU_ERROR } wg_cpu_result;

typedef struct {
    const char* name;
    wg_cpu_t* (*create)(void);
    wg_cpu_t* (*create32)(void);
    void      (*destroy)(wg_cpu_t*);
    bool      (*load_binary)(wg_cpu_t*, const char*);
    bool      (*setup_stack)(wg_cpu_t*, uint64_t);
    void      (*switch_to_32bit)(wg_cpu_t*);
    bool      (*load_code)(wg_cpu_t*, uint64_t, const uint8_t*, uint32_t, uint64_t);
    wg_cpu_result (*run)(wg_cpu_t*, int);
    wg_cpu_result (*step)(wg_cpu_t*);
    uint64_t  (*get_reg)(wg_cpu_t*, int);
    void      (*set_reg)(wg_cpu_t*, int, uint64_t);
    void      (*set_xmm_low)(wg_cpu_t*, int, uint64_t);
    uint64_t  (*get_rip)(wg_cpu_t*);
    void      (*set_rip)(wg_cpu_t*, uint64_t);
    uint64_t  (*get_flags)(wg_cpu_t*);
    void      (*set_flags)(wg_cpu_t*, uint64_t);
    void      (*set_fs_base)(wg_cpu_t*, uint64_t);
    void      (*set_gs_base)(wg_cpu_t*, uint64_t);
    uint64_t  (*get_fs_base)(wg_cpu_t*);
    uint64_t  (*get_gs_base)(wg_cpu_t*);
    int       (*get_stop_reason)(wg_cpu_t*);
    uint64_t  (*get_fault_addr)(wg_cpu_t*);
    bool      (*write_mem)(wg_cpu_t*, uint64_t, const void*, uint32_t);
    bool      (*read_mem)(wg_cpu_t*, uint64_t, void*, uint32_t);
    uint64_t  (*mem_copy)(wg_cpu_t*, uint64_t, uint64_t, uint64_t);
    uint64_t  (*mem_set)(wg_cpu_t*, uint64_t, int, uint64_t);
    void*     (*new_thread_machine)(wg_cpu_t*);
    void      (*adopt_machine)(void*);
    void      (*free_thread_machine)(void*);
    bool      (*has_jit)(void);
    const char* (*version)(void);
} wg_cpu_backend_t;

// box64 globals normally set by its main(); we drive it embedded, so seed them once.
extern FILE* ftrace;
extern uintptr_t box64_pagesize;

typedef struct wg_box64 {
    x64emu_t* emu;
    int       last_stop;
    uint64_t  fault_addr;
} wg_box64;

static box64context_t* g_ctx = NULL;

static void ensure_context(void) {
    if (g_ctx) return;
    if (!ftrace) ftrace = stderr;                 // box64 trace sink (main() would set this)
    if (!box64_pagesize) box64_pagesize = (uintptr_t)sysconf(_SC_PAGESIZE);
    g_ctx = NewBox64Context(0);                   // sets up custommem + exit_bridge + bridges
}

// ---- lifecycle -------------------------------------------------------------
static wg_cpu_t* bx_create(void) {
    ensure_context();
    wg_box64* w = (wg_box64*)calloc(1, sizeof(wg_box64));
    // Stack is provided later via setup_stack/load_code; start with a modest owned stack.
    w->emu = NewX64Emu(g_ctx, 0, (uintptr_t)NULL, 0, 0);
    SetupX64Emu(w->emu, NULL);
    return (wg_cpu_t*)w;
}
static wg_cpu_t* bx_create32(void) { return bx_create(); } // 32-bit: SetMachineMode later
static void bx_destroy(wg_cpu_t* c) {
    wg_box64* w = (wg_box64*)c;
    if (w && w->emu) FreeX64Emu(&w->emu);
    free(w);
}

// ---- code / memory (identity-mapped: guest VA == host VA) ------------------
static bool bx_load_code(wg_cpu_t* c, uint64_t addr, const uint8_t* code, uint32_t sz, uint64_t rip) {
    wg_box64* w = (wg_box64*)c;
    // Map the guest region at its own VA if not already present (MAP_FIXED, RW).
    uintptr_t page = box64_pagesize ? box64_pagesize : 0x4000;
    uintptr_t base = addr & ~(page - 1);
    size_t    span = ((addr + sz + page - 1) & ~(page - 1)) - base;
    void* m = mmap((void*)base, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED && (uintptr_t)mmap((void*)base, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == (uintptr_t)MAP_FAILED)
        return false;
    memcpy((void*)(uintptr_t)addr, code, sz);
    w->emu->ip.q[0] = rip;
    return true;
}
static bool bx_write_mem(wg_cpu_t* c, uint64_t a, const void* b, uint32_t l) {
    (void)c; memcpy((void*)(uintptr_t)a, b, l); return true;
}
static bool bx_read_mem(wg_cpu_t* c, uint64_t a, void* b, uint32_t l) {
    (void)c; memcpy(b, (void*)(uintptr_t)a, l); return true;
}
static uint64_t bx_mem_copy(wg_cpu_t* c, uint64_t d, uint64_t s, uint64_t n) {
    (void)c; memcpy((void*)(uintptr_t)d, (void*)(uintptr_t)s, n); return d;
}
static uint64_t bx_mem_set(wg_cpu_t* c, uint64_t d, int ch, uint64_t n) {
    (void)c; memset((void*)(uintptr_t)d, ch, n); return d;
}

static bool bx_setup_stack(wg_cpu_t* c, uint64_t entry_rip) {
    wg_box64* w = (wg_box64*)c; w->emu->ip.q[0] = entry_rip; return true;
}
static bool bx_load_binary(wg_cpu_t* c, const char* p) { (void)c; (void)p; return false; } // PE loader stays in WineGlass
static void bx_switch_to_32bit(wg_cpu_t* c) {
    wg_box64* w = (wg_box64*)c;
    w->emu->segs[_CS] = 0x23;   // box64 flags 32-bit via CS selector 0x23
}

// ---- execution -------------------------------------------------------------
static wg_cpu_result bx_run(wg_cpu_t* c, int max) {
    wg_box64* w = (wg_box64*)c;
    (void)max;
    // DynaCall enters guest code at the current RIP as a call: it pushes exit_bridge
    // as the return address and runs EmuRun (interpreter when BOX64_DYNAREC=0, else
    // JIT), which processes INT3 native-call transitions. Win32 imports registered as
    // bridges are handled INLINE by their wrappers (proven by box64_bridge_spike.c) —
    // no halt-and-dispatch loop needed; the guest runs to its return/ExitProcess.
    DynaCall(w->emu, w->emu->ip.q[0], 0);
    w->last_stop = w->emu->quit ? -1 : 0;
    return WG_CPU_HALT;
}
static wg_cpu_result bx_step(wg_cpu_t* c) {
    wg_box64* w = (wg_box64*)c;
    Run(w->emu, 1);
    return WG_CPU_OK;
}

// ---- registers -------------------------------------------------------------
static uint64_t bx_get_reg(wg_cpu_t* c, int i)             { return ((wg_box64*)c)->emu->regs[i].q[0]; }
static void     bx_set_reg(wg_cpu_t* c, int i, uint64_t v) { ((wg_box64*)c)->emu->regs[i].q[0] = v; }
static void     bx_set_xmm_low(wg_cpu_t* c, int i, uint64_t lo) { ((wg_box64*)c)->emu->xmm[i].q[0] = lo; }
static uint64_t bx_get_rip(wg_cpu_t* c)                    { return ((wg_box64*)c)->emu->ip.q[0]; }
static void     bx_set_rip(wg_cpu_t* c, uint64_t rip)      { ((wg_box64*)c)->emu->ip.q[0] = rip; }
static uint64_t bx_get_flags(wg_cpu_t* c)                  { return ((wg_box64*)c)->emu->eflags.x64; }
static void     bx_set_flags(wg_cpu_t* c, uint64_t f)      { ((wg_box64*)c)->emu->eflags.x64 = f; }

// FS/GS segment bases: box64 stores segment bases in emu->segs_offs (per-selector).
static void     bx_set_fs_base(wg_cpu_t* c, uint64_t b)    { ((wg_box64*)c)->emu->segs_offs[_FS] = b; }
static void     bx_set_gs_base(wg_cpu_t* c, uint64_t b)    { ((wg_box64*)c)->emu->segs_offs[_GS] = b; }
static uint64_t bx_get_fs_base(wg_cpu_t* c)                { return ((wg_box64*)c)->emu->segs_offs[_FS]; }
static uint64_t bx_get_gs_base(wg_cpu_t* c)                { return ((wg_box64*)c)->emu->segs_offs[_GS]; }

static int      bx_get_stop_reason(wg_cpu_t* c)           { return ((wg_box64*)c)->last_stop; }
static uint64_t bx_get_fault_addr(wg_cpu_t* c)            { return ((wg_box64*)c)->fault_addr; }

static void*    bx_new_thread_machine(wg_cpu_t* c) { (void)c; return NULL; } // TODO: per-thread emu
static void     bx_adopt_machine(void* m)          { (void)m; }
static void     bx_free_thread_machine(void* m)    { (void)m; }

static bool        bx_has_jit(void) { return true; }
static const char* bx_version(void) { return "box64"; }

const wg_cpu_backend_t* wg_cpu_box64_backend(void) {
    static const wg_cpu_backend_t vt = {
        .name = "box64",
        .create = bx_create, .create32 = bx_create32, .destroy = bx_destroy,
        .load_binary = bx_load_binary, .setup_stack = bx_setup_stack,
        .switch_to_32bit = bx_switch_to_32bit, .load_code = bx_load_code,
        .run = bx_run, .step = bx_step,
        .get_reg = bx_get_reg, .set_reg = bx_set_reg, .set_xmm_low = bx_set_xmm_low,
        .get_rip = bx_get_rip, .set_rip = bx_set_rip,
        .get_flags = bx_get_flags, .set_flags = bx_set_flags,
        .set_fs_base = bx_set_fs_base, .set_gs_base = bx_set_gs_base,
        .get_fs_base = bx_get_fs_base, .get_gs_base = bx_get_gs_base,
        .get_stop_reason = bx_get_stop_reason, .get_fault_addr = bx_get_fault_addr,
        .write_mem = bx_write_mem, .read_mem = bx_read_mem,
        .mem_copy = bx_mem_copy, .mem_set = bx_mem_set,
        .new_thread_machine = bx_new_thread_machine, .adopt_machine = bx_adopt_machine,
        .free_thread_machine = bx_free_thread_machine,
        .has_jit = bx_has_jit, .version = bx_version,
    };
    return &vt;
}
