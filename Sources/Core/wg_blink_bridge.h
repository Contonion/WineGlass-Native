#ifndef WG_BLINK_BRIDGE_H
#define WG_BLINK_BRIDGE_H

// Bridge between WineGlass engine and blink's x86-64 emulation.
// This replaces our hand-rolled interpreter with blink's full
// x86-64 implementation including JIT (via MAP_JIT on iOS).

#include <stdint.h>
#include <stdbool.h>

typedef struct WGBlinkInstance WGBlinkInstance;

WGBlinkInstance *wg_blink_create(void);     // 64-bit mode
WGBlinkInstance *wg_blink_create32(void);  // 32-bit protected mode
void             wg_blink_destroy(WGBlinkInstance *inst);

bool wg_blink_load_binary(WGBlinkInstance *inst, const char *path);
bool wg_blink_setup_stack(WGBlinkInstance *inst, uint64_t entry_rip);
void wg_blink_switch_to_32bit(WGBlinkInstance *inst);

// Load raw x86-64 code at a specific address and set RIP
bool wg_blink_load_code(WGBlinkInstance *inst, uint64_t addr,
                         const uint8_t *code, uint32_t size,
                         uint64_t entry_rip);

// Execute instructions
typedef enum {
    WG_BLINK_OK,
    WG_BLINK_HALT,
    WG_BLINK_SYSCALL,
    WG_BLINK_ERROR,
} WGBlinkResult;

WGBlinkResult wg_blink_run(WGBlinkInstance *inst, int max_instructions);
WGBlinkResult wg_blink_step(WGBlinkInstance *inst);

// Register access
uint64_t wg_blink_get_reg(WGBlinkInstance *inst, int reg_index);
void     wg_blink_set_reg(WGBlinkInstance *inst, int reg_index, uint64_t val);
// Set low 64 bits of an XMM register (idx 0..15) — for FP return values (XMM0).
void     wg_blink_set_xmm_low(WGBlinkInstance *inst, int idx, uint64_t lo);
uint64_t wg_blink_get_rip(WGBlinkInstance *inst);
void     wg_blink_set_rip(WGBlinkInstance *inst, uint64_t rip);

// Last blink stop reason (0 clean, -1 halt, -4 segfault, -8 #GP) and the
// faulting guest address from the last memory fault.
int      wg_blink_get_stop_reason(WGBlinkInstance *inst);
uint64_t wg_blink_get_fault_addr(WGBlinkInstance *inst);

// Set the FS (32-bit TEB) / GS (64-bit TEB) segment base linear address.
void     wg_blink_set_fs_base(WGBlinkInstance *inst, uint64_t base);
void     wg_blink_set_gs_base(WGBlinkInstance *inst, uint64_t base);

// Full thread-context save/restore helpers (cooperative x86-64 switch).
uint64_t wg_blink_get_flags(WGBlinkInstance *inst);
void     wg_blink_set_flags(WGBlinkInstance *inst, uint64_t f);
uint64_t wg_blink_get_fs_base(WGBlinkInstance *inst);
uint64_t wg_blink_get_gs_base(WGBlinkInstance *inst);

// Memory access
bool wg_blink_write_mem(WGBlinkInstance *inst, uint64_t addr,
                         const void *buf, uint32_t len);
bool wg_blink_read_mem(WGBlinkInstance *inst, uint64_t addr,
                        void *buf, uint32_t len);
// Fast in-guest memcpy/memset (direct host page walk; no malloc). Return dst, or
// 0 if a page was unmapped (caller should fall back).
uint64_t wg_blink_mem_copy(WGBlinkInstance *inst, uint64_t dst, uint64_t src, uint64_t n);
uint64_t wg_blink_mem_set(WGBlinkInstance *inst, uint64_t dst, int c, uint64_t n);

// Info
bool wg_blink_has_jit(void);
const char *wg_blink_version(void);

// Real-threads support: a blink Machine per guest thread over the shared System.
// wg_blink_new_thread_machine() is called on the parent thread; the spawned
// pthread calls wg_blink_adopt_machine() to make it current, then seeds regs via
// the normal wg_blink_set_* accessors and drives its own wg_blink_run loop.
void *wg_blink_new_thread_machine(WGBlinkInstance *inst);
void  wg_blink_adopt_machine(void *machine);
void  wg_blink_free_thread_machine(void *machine);

// Force blink's JIT on (1) or off (0) before the first VM is created.
void  wg_blink_force_jit(int on);
// Standalone MAP_JIT capability probe (wg_jit_probe.c): returns 42 if the
// device permits executing JIT code right now, negative on failure.
int   wg_jit_smoke_test(void);

#endif
