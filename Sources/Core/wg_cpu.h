#ifndef WG_CPU_H
#define WG_CPU_H

// wg_cpu.h — backend-neutral CPU interface for WineGlass.
//
// WineGlass translates x86/x86-64 to ARM64. Historically that was blink
// (interpreter + basic JIT). This interface lets the engine drive EITHER blink
// or box64 (a mature x86-64→ARM64 DynaRec) behind one API, so we can A/B them on
// the mac harness and swap the backend without touching the ~1400 engine call
// sites. The method set mirrors the proven wg_blink_bridge.h surface.
//
// Backends:
//   WG_CPU_BLINK  — wraps the existing wg_blink_* implementation (default today).
//   WG_CPU_BOX64  — box64's x86-64 emu + ARM64 DynaRec (see memory/box64-pivot.md).
//
// Both backends present the SAME guest model: 64-bit registers by x86 index,
// linear guest memory (identity or paged — hidden behind read/write_mem), and a
// run/step loop that stops on HLT/halt (our Win32 thunks) or error.

#include <stdint.h>
#include <stdbool.h>

typedef struct wg_cpu wg_cpu_t;

typedef enum {
    WG_CPU_BLINK = 0,
    WG_CPU_BOX64 = 1,
} wg_cpu_backend_id;

// Result of a run/step, matching WGBlinkResult semantics.
typedef enum {
    WG_CPU_OK = 0,
    WG_CPU_HALT,     // hit a HLT thunk (Win32 interception point) — engine dispatches
    WG_CPU_SYSCALL,
    WG_CPU_ERROR,
} wg_cpu_result;

// x86-64 register indices (match box64 emu->regs[] and blink's ordering).
enum {
    WG_REG_RAX = 0, WG_REG_RCX, WG_REG_RDX, WG_REG_RBX,
    WG_REG_RSP,     WG_REG_RBP, WG_REG_RSI, WG_REG_RDI,
    WG_REG_R8,      WG_REG_R9,  WG_REG_R10, WG_REG_R11,
    WG_REG_R12,     WG_REG_R13, WG_REG_R14, WG_REG_R15,
};

// ---- Backend selection -----------------------------------------------------
// Choose the active backend BEFORE creating a CPU. Defaults to WG_CPU_BLINK, or
// WG_CPU_BOX64 if the env var WG_CPU=box64 is set (for A/B on the harness).
void              wg_cpu_use(wg_cpu_backend_id id);
wg_cpu_backend_id wg_cpu_active(void);
const char*       wg_cpu_backend_name(void);

// ---- Lifecycle -------------------------------------------------------------
wg_cpu_t* wg_cpu_create(void);       // 64-bit mode
wg_cpu_t* wg_cpu_create32(void);     // 32-bit protected mode
void      wg_cpu_destroy(wg_cpu_t* cpu);

bool wg_cpu_load_binary(wg_cpu_t* cpu, const char* path);
bool wg_cpu_setup_stack(wg_cpu_t* cpu, uint64_t entry_rip);
void wg_cpu_switch_to_32bit(wg_cpu_t* cpu);

// Map/copy raw guest code at a specific guest address and set RIP.
bool wg_cpu_load_code(wg_cpu_t* cpu, uint64_t addr,
                      const uint8_t* code, uint32_t size, uint64_t entry_rip);

// ---- Execution -------------------------------------------------------------
wg_cpu_result wg_cpu_run(wg_cpu_t* cpu, int max_instructions);
wg_cpu_result wg_cpu_step(wg_cpu_t* cpu);

// ---- Registers -------------------------------------------------------------
uint64_t wg_cpu_get_reg(wg_cpu_t* cpu, int reg_index);
void     wg_cpu_set_reg(wg_cpu_t* cpu, int reg_index, uint64_t val);
void     wg_cpu_set_xmm_low(wg_cpu_t* cpu, int idx, uint64_t lo);  // FP return (XMM0)
uint64_t wg_cpu_get_rip(wg_cpu_t* cpu);
void     wg_cpu_set_rip(wg_cpu_t* cpu, uint64_t rip);
uint64_t wg_cpu_get_flags(wg_cpu_t* cpu);
void     wg_cpu_set_flags(wg_cpu_t* cpu, uint64_t f);

// FS (32-bit TEB) / GS (64-bit TEB) segment base linear address.
void     wg_cpu_set_fs_base(wg_cpu_t* cpu, uint64_t base);
void     wg_cpu_set_gs_base(wg_cpu_t* cpu, uint64_t base);
uint64_t wg_cpu_get_fs_base(wg_cpu_t* cpu);
uint64_t wg_cpu_get_gs_base(wg_cpu_t* cpu);

// Last stop reason (0 clean, -1 halt, -4 segfault, -8 #GP) + faulting guest addr.
int      wg_cpu_get_stop_reason(wg_cpu_t* cpu);
uint64_t wg_cpu_get_fault_addr(wg_cpu_t* cpu);

// ---- Memory ----------------------------------------------------------------
bool     wg_cpu_write_mem(wg_cpu_t* cpu, uint64_t addr, const void* buf, uint32_t len);
bool     wg_cpu_read_mem(wg_cpu_t* cpu, uint64_t addr, void* buf, uint32_t len);
uint64_t wg_cpu_mem_copy(wg_cpu_t* cpu, uint64_t dst, uint64_t src, uint64_t n);
uint64_t wg_cpu_mem_set(wg_cpu_t* cpu, uint64_t dst, int c, uint64_t n);

// ---- Real-threads support (a Machine/emu per guest thread) -----------------
void* wg_cpu_new_thread_machine(wg_cpu_t* cpu);
void  wg_cpu_adopt_machine(void* machine);
void  wg_cpu_free_thread_machine(void* machine);

// ---- Info ------------------------------------------------------------------
bool        wg_cpu_has_jit(void);
const char* wg_cpu_version(void);

// ---- Backend vtable (implemented by wg_cpu_blink.c / wg_cpu_box64.c) --------
// wg_cpu.c dispatches the public API above through the active backend's vtable.
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

// Each backend exposes its vtable; wg_cpu.c holds the active pointer.
const wg_cpu_backend_t* wg_cpu_blink_backend(void);
const wg_cpu_backend_t* wg_cpu_box64_backend(void);

#endif // WG_CPU_H
