// wg_cpu.c — dispatches the backend-neutral wg_cpu_* API through the active
// backend vtable (blink today, box64 for the translator pivot). Selection is
// sticky once a CPU is created; pick it early via wg_cpu_use() or WG_CPU=box64.

#include "wg_cpu.h"
#include <stdlib.h>
#include <string.h>

static const wg_cpu_backend_t* g_backend = NULL;

static const wg_cpu_backend_t* resolve_default(void) {
    const char* sel = getenv("WG_CPU");
    if (sel && (strcmp(sel, "box64") == 0 || strcmp(sel, "BOX64") == 0))
        return wg_cpu_box64_backend();
    return wg_cpu_blink_backend();
}

static const wg_cpu_backend_t* B(void) {
    if (!g_backend) g_backend = resolve_default();
    return g_backend;
}

void wg_cpu_use(wg_cpu_backend_id id) {
    g_backend = (id == WG_CPU_BOX64) ? wg_cpu_box64_backend() : wg_cpu_blink_backend();
}

wg_cpu_backend_id wg_cpu_active(void) {
    return (B() == wg_cpu_box64_backend()) ? WG_CPU_BOX64 : WG_CPU_BLINK;
}

const char* wg_cpu_backend_name(void) { return B()->name; }

// ---- Forwarders ------------------------------------------------------------
wg_cpu_t* wg_cpu_create(void)                 { return B()->create(); }
wg_cpu_t* wg_cpu_create32(void)               { return B()->create32(); }
void      wg_cpu_destroy(wg_cpu_t* c)         { B()->destroy(c); }

bool wg_cpu_load_binary(wg_cpu_t* c, const char* p)        { return B()->load_binary(c, p); }
bool wg_cpu_setup_stack(wg_cpu_t* c, uint64_t e)           { return B()->setup_stack(c, e); }
void wg_cpu_switch_to_32bit(wg_cpu_t* c)                   { B()->switch_to_32bit(c); }
bool wg_cpu_load_code(wg_cpu_t* c, uint64_t a, const uint8_t* code, uint32_t sz, uint64_t rip) {
    return B()->load_code(c, a, code, sz, rip);
}

wg_cpu_result wg_cpu_run(wg_cpu_t* c, int n)  { return B()->run(c, n); }
wg_cpu_result wg_cpu_step(wg_cpu_t* c)        { return B()->step(c); }

uint64_t wg_cpu_get_reg(wg_cpu_t* c, int i)              { return B()->get_reg(c, i); }
void     wg_cpu_set_reg(wg_cpu_t* c, int i, uint64_t v)  { B()->set_reg(c, i, v); }
void     wg_cpu_set_xmm_low(wg_cpu_t* c, int i, uint64_t lo) { B()->set_xmm_low(c, i, lo); }
uint64_t wg_cpu_get_rip(wg_cpu_t* c)                     { return B()->get_rip(c); }
void     wg_cpu_set_rip(wg_cpu_t* c, uint64_t rip)       { B()->set_rip(c, rip); }
uint64_t wg_cpu_get_flags(wg_cpu_t* c)                   { return B()->get_flags(c); }
void     wg_cpu_set_flags(wg_cpu_t* c, uint64_t f)       { B()->set_flags(c, f); }

void     wg_cpu_set_fs_base(wg_cpu_t* c, uint64_t b)     { B()->set_fs_base(c, b); }
void     wg_cpu_set_gs_base(wg_cpu_t* c, uint64_t b)     { B()->set_gs_base(c, b); }
uint64_t wg_cpu_get_fs_base(wg_cpu_t* c)                 { return B()->get_fs_base(c); }
uint64_t wg_cpu_get_gs_base(wg_cpu_t* c)                 { return B()->get_gs_base(c); }

int      wg_cpu_get_stop_reason(wg_cpu_t* c)             { return B()->get_stop_reason(c); }
uint64_t wg_cpu_get_fault_addr(wg_cpu_t* c)              { return B()->get_fault_addr(c); }

bool     wg_cpu_write_mem(wg_cpu_t* c, uint64_t a, const void* b, uint32_t l) { return B()->write_mem(c, a, b, l); }
bool     wg_cpu_read_mem(wg_cpu_t* c, uint64_t a, void* b, uint32_t l)        { return B()->read_mem(c, a, b, l); }
uint64_t wg_cpu_mem_copy(wg_cpu_t* c, uint64_t d, uint64_t s, uint64_t n)     { return B()->mem_copy(c, d, s, n); }
uint64_t wg_cpu_mem_set(wg_cpu_t* c, uint64_t d, int ch, uint64_t n)          { return B()->mem_set(c, d, ch, n); }

void* wg_cpu_new_thread_machine(wg_cpu_t* c)  { return B()->new_thread_machine(c); }
void  wg_cpu_adopt_machine(void* m)           { B()->adopt_machine(m); }
void  wg_cpu_free_thread_machine(void* m)     { B()->free_thread_machine(m); }

bool        wg_cpu_has_jit(void)  { return B()->has_jit(); }
const char* wg_cpu_version(void)  { return B()->version(); }
