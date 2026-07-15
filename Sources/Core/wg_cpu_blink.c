// wg_cpu_blink.c — blink backend for the wg_cpu interface.
// Thin adapter: wg_cpu_t* IS a WGBlinkInstance*, and every method forwards to the
// existing, proven wg_blink_* bridge. This keeps the current engine behaviour
// identical while the wg_cpu abstraction is introduced, so box64 can be A/B'd
// against blink without disturbing the blink path.

#include "wg_cpu.h"
#include "wg_blink_bridge.h"

// WGBlinkResult and wg_cpu_result share the same ordering (OK/HALT/SYSCALL/ERROR),
// as do the x86 register indices, so the casts below are value-preserving.
#define I(cpu) ((WGBlinkInstance*)(cpu))

static wg_cpu_t* b_create(void)   { return (wg_cpu_t*)wg_blink_create(); }
static wg_cpu_t* b_create32(void) { return (wg_cpu_t*)wg_blink_create32(); }
static void      b_destroy(wg_cpu_t* c) { wg_blink_destroy(I(c)); }

static bool b_load_binary(wg_cpu_t* c, const char* p) { return wg_blink_load_binary(I(c), p); }
static bool b_setup_stack(wg_cpu_t* c, uint64_t e)    { return wg_blink_setup_stack(I(c), e); }
static void b_switch_to_32bit(wg_cpu_t* c)            { wg_blink_switch_to_32bit(I(c)); }
static bool b_load_code(wg_cpu_t* c, uint64_t a, const uint8_t* code, uint32_t sz, uint64_t rip) {
    return wg_blink_load_code(I(c), a, code, sz, rip);
}

static wg_cpu_result b_run(wg_cpu_t* c, int n) { return (wg_cpu_result)wg_blink_run(I(c), n); }
static wg_cpu_result b_step(wg_cpu_t* c)       { return (wg_cpu_result)wg_blink_step(I(c)); }

static uint64_t b_get_reg(wg_cpu_t* c, int i)             { return wg_blink_get_reg(I(c), i); }
static void     b_set_reg(wg_cpu_t* c, int i, uint64_t v) { wg_blink_set_reg(I(c), i, v); }
static void     b_set_xmm_low(wg_cpu_t* c, int i, uint64_t lo) { wg_blink_set_xmm_low(I(c), i, lo); }
static uint64_t b_get_rip(wg_cpu_t* c)                    { return wg_blink_get_rip(I(c)); }
static void     b_set_rip(wg_cpu_t* c, uint64_t rip)      { wg_blink_set_rip(I(c), rip); }
static uint64_t b_get_flags(wg_cpu_t* c)                  { return wg_blink_get_flags(I(c)); }
static void     b_set_flags(wg_cpu_t* c, uint64_t f)      { wg_blink_set_flags(I(c), f); }

static void     b_set_fs_base(wg_cpu_t* c, uint64_t b)    { wg_blink_set_fs_base(I(c), b); }
static void     b_set_gs_base(wg_cpu_t* c, uint64_t b)    { wg_blink_set_gs_base(I(c), b); }
static uint64_t b_get_fs_base(wg_cpu_t* c)                { return wg_blink_get_fs_base(I(c)); }
static uint64_t b_get_gs_base(wg_cpu_t* c)                { return wg_blink_get_gs_base(I(c)); }

static int      b_get_stop_reason(wg_cpu_t* c)           { return wg_blink_get_stop_reason(I(c)); }
static uint64_t b_get_fault_addr(wg_cpu_t* c)            { return wg_blink_get_fault_addr(I(c)); }

static bool     b_write_mem(wg_cpu_t* c, uint64_t a, const void* b, uint32_t l) { return wg_blink_write_mem(I(c), a, b, l); }
static bool     b_read_mem(wg_cpu_t* c, uint64_t a, void* b, uint32_t l)        { return wg_blink_read_mem(I(c), a, b, l); }
static uint64_t b_mem_copy(wg_cpu_t* c, uint64_t d, uint64_t s, uint64_t n)     { return wg_blink_mem_copy(I(c), d, s, n); }
static uint64_t b_mem_set(wg_cpu_t* c, uint64_t d, int ch, uint64_t n)          { return wg_blink_mem_set(I(c), d, ch, n); }

static void* b_new_thread_machine(wg_cpu_t* c) { return wg_blink_new_thread_machine(I(c)); }
static void  b_adopt_machine(void* m)          { wg_blink_adopt_machine(m); }
static void  b_free_thread_machine(void* m)    { wg_blink_free_thread_machine(m); }

static bool        b_has_jit(void) { return wg_blink_has_jit(); }
static const char* b_version(void) { return wg_blink_version(); }

const wg_cpu_backend_t* wg_cpu_blink_backend(void) {
    static const wg_cpu_backend_t vt = {
        .name = "blink",
        .create = b_create, .create32 = b_create32, .destroy = b_destroy,
        .load_binary = b_load_binary, .setup_stack = b_setup_stack,
        .switch_to_32bit = b_switch_to_32bit, .load_code = b_load_code,
        .run = b_run, .step = b_step,
        .get_reg = b_get_reg, .set_reg = b_set_reg, .set_xmm_low = b_set_xmm_low,
        .get_rip = b_get_rip, .set_rip = b_set_rip,
        .get_flags = b_get_flags, .set_flags = b_set_flags,
        .set_fs_base = b_set_fs_base, .set_gs_base = b_set_gs_base,
        .get_fs_base = b_get_fs_base, .get_gs_base = b_get_gs_base,
        .get_stop_reason = b_get_stop_reason, .get_fault_addr = b_get_fault_addr,
        .write_mem = b_write_mem, .read_mem = b_read_mem,
        .mem_copy = b_mem_copy, .mem_set = b_mem_set,
        .new_thread_machine = b_new_thread_machine, .adopt_machine = b_adopt_machine,
        .free_thread_machine = b_free_thread_machine,
        .has_jit = b_has_jit, .version = b_version,
    };
    return &vt;
}
