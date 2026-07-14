// Bridge to blink's x86-64 emulation engine.

#include "wg_blink_bridge.h"
#include "wg_log.h"
#include <stdlib.h>
#include <setjmp.h>

#define TAG "Blink"

// Forward declarations — symbols in blink.a (wg_blink_impl.o)
typedef struct WGBlinkVM WGBlinkVM;
extern WGBlinkVM *WGBlinkVM_Create(void);
extern WGBlinkVM *WGBlinkVM_Create32(void);
extern void WGBlinkVM_Destroy(WGBlinkVM *vm);
extern void WGBlinkVM_ForceJit(int on);

// Force blink's JIT on/off before the first VM is created. The iOS app calls
// this after the MAP_JIT smoke test passes on-device (see wg_jit_probe.c).
void wg_blink_force_jit(int on) { WGBlinkVM_ForceJit(on); }
extern int WGBlinkVM_LoadCode(WGBlinkVM *vm, unsigned long long addr,
                               const void *code, unsigned int size,
                               unsigned long long entry_rip);
extern int WGBlinkVM_Step(WGBlinkVM *vm);
extern int WGBlinkVM_Run(WGBlinkVM *vm, int max_insns);
extern unsigned long long WGBlinkVM_GetReg(WGBlinkVM *vm, int idx);
extern void WGBlinkVM_SetReg(WGBlinkVM *vm, int idx, unsigned long long val);
extern void WGBlinkVM_SetXmmLow(WGBlinkVM *vm, int idx, unsigned long long lo);
extern unsigned long long WGBlinkVM_GetRIP(WGBlinkVM *vm);
extern void WGBlinkVM_SetRIP(WGBlinkVM *vm, unsigned long long rip);
extern int WGBlinkVM_WriteMem(WGBlinkVM *vm, unsigned long long addr,
                               const void *buf, unsigned int len);
extern int WGBlinkVM_ReadMem(WGBlinkVM *vm, unsigned long long addr,
                              void *buf, unsigned int len);
extern unsigned long long WGBlinkVM_MemCopy(WGBlinkVM *vm, unsigned long long dst,
                                            unsigned long long src, unsigned long long n);
extern unsigned long long WGBlinkVM_MemSet(WGBlinkVM *vm, unsigned long long dst,
                                           int c, unsigned long long n);
extern int WGBlinkVM_SetupStack(WGBlinkVM *vm, unsigned long long entry_rip);
extern void WGBlinkVM_SwitchTo32(WGBlinkVM *vm);
extern void WGBlinkVM_SetFsBase(WGBlinkVM *vm, unsigned long long base);
extern void WGBlinkVM_SetGsBase(WGBlinkVM *vm, unsigned long long base);
extern unsigned long long WGBlinkVM_GetFlags(WGBlinkVM *vm);
extern void WGBlinkVM_SetFlags(WGBlinkVM *vm, unsigned long long f);
extern unsigned long long WGBlinkVM_GetFsBase(WGBlinkVM *vm);
extern unsigned long long WGBlinkVM_GetGsBase(WGBlinkVM *vm);
extern int WGBlinkVM_GetStopReason(WGBlinkVM *vm);
extern unsigned long long WGBlinkVM_GetFaultAddr(WGBlinkVM *vm);
extern void *WGBlinkVM_NewThreadMachine(WGBlinkVM *vm);
extern void WGBlinkVM_AdoptMachine(void *m);
extern void WGBlinkVM_FreeThreadMachine(void *m);

// From wg_blink_stubs.c — our Abort() override recovery point
extern void wg_blink_set_abort_recovery(sigjmp_buf *buf);

struct WGBlinkInstance {
    WGBlinkVM *vm;
};

WGBlinkInstance *wg_blink_create(void) {
    WGBlinkInstance *inst = calloc(1, sizeof(WGBlinkInstance));
    if (!inst) return NULL;

    // Set up recovery point — if blink's init hits an assertion
    // (which calls our overridden Abort()), we longjmp back here.
    sigjmp_buf recovery;
    wg_blink_set_abort_recovery(&recovery);

    int aborted = sigsetjmp(recovery, 0);
    if (aborted) {
        wg_blink_set_abort_recovery(NULL);
        WG_LOGE(TAG, "Blink init failed (Abort caught) — falling back to builtin interpreter");
        free(inst);
        return NULL;
    }

    inst->vm = WGBlinkVM_Create();
    wg_blink_set_abort_recovery(NULL);

    if (!inst->vm) {
        WG_LOGE(TAG, "Failed to create blink VM");
        free(inst);
        return NULL;
    }

    WG_LOGI(TAG, "Blink x86-64 VM created (JIT: %s)", wg_blink_has_jit() ? "YES" : "NO");
    return inst;
}

WGBlinkInstance *wg_blink_create32(void) {
    WGBlinkInstance *inst = calloc(1, sizeof(WGBlinkInstance));
    if (!inst) return NULL;

    sigjmp_buf recovery;
    wg_blink_set_abort_recovery(&recovery);
    int aborted = sigsetjmp(recovery, 0);
    if (aborted) {
        wg_blink_set_abort_recovery(NULL);
        WG_LOGE(TAG, "Blink 32-bit init failed");
        free(inst);
        return NULL;
    }

    inst->vm = WGBlinkVM_Create32();
    wg_blink_set_abort_recovery(NULL);

    if (!inst->vm) {
        WG_LOGE(TAG, "Failed to create 32-bit blink VM");
        free(inst);
        return NULL;
    }

    WG_LOGI(TAG, "Blink x86 (32-bit) VM created (JIT: %s)", wg_blink_has_jit() ? "YES" : "NO");
    return inst;
}

void wg_blink_destroy(WGBlinkInstance *inst) {
    if (!inst) return;
    free(inst);
}

bool wg_blink_load_binary(WGBlinkInstance *inst, const char *path) {
    return false;
}

void wg_blink_switch_to_32bit(WGBlinkInstance *inst) {
    if (!inst || !inst->vm) return;
    WGBlinkVM_SwitchTo32(inst->vm);
    WG_LOGI(TAG, "Switched to 32-bit compatibility mode");
}

bool wg_blink_setup_stack(WGBlinkInstance *inst, uint64_t entry_rip) {
    if (!inst || !inst->vm) return false;

    sigjmp_buf recovery;
    wg_blink_set_abort_recovery(&recovery);
    if (sigsetjmp(recovery, 0)) {
        wg_blink_set_abort_recovery(NULL);
        WG_LOGE(TAG, "Blink setup_stack aborted");
        return false;
    }

    int ok = WGBlinkVM_SetupStack(inst->vm, entry_rip);
    wg_blink_set_abort_recovery(NULL);
    return ok != 0;
}

bool wg_blink_load_code(WGBlinkInstance *inst, uint64_t addr,
                         const uint8_t *code, uint32_t size,
                         uint64_t entry_rip) {
    if (!inst || !inst->vm) return false;

    sigjmp_buf recovery;
    wg_blink_set_abort_recovery(&recovery);
    if (sigsetjmp(recovery, 0)) {
        wg_blink_set_abort_recovery(NULL);
        WG_LOGE(TAG, "Blink load_code aborted");
        return false;
    }

    int ok = WGBlinkVM_LoadCode(inst->vm, addr, code, size, entry_rip);
    wg_blink_set_abort_recovery(NULL);

    if (!ok) {
        WG_LOGE(TAG, "Failed to load code at 0x%llx", (unsigned long long)addr);
        return false;
    }

    // Real/large loads always log; single-page (4096B) on-demand commits are
    // faulted in by the thousands during large allocations and would flood the
    // device ring buffer (scrolling off the actual activity), so rate-limit them.
    if (size != 4096) {
        WG_LOGI(TAG, "Loaded %u bytes at 0x%llx, entry 0x%llx",
                size, (unsigned long long)addr, (unsigned long long)entry_rip);
    } else {
        static uint32_t pc = 0;
        if ((pc++ & 0x3FF) == 0)
            WG_LOGI(TAG, "Loaded 4096 bytes at 0x%llx (page commit #%u)",
                    (unsigned long long)addr, pc);
    }
    return true;
}

WGBlinkResult wg_blink_step(WGBlinkInstance *inst) {
    if (!inst || !inst->vm) return WG_BLINK_ERROR;
    int r = WGBlinkVM_Step(inst->vm);
    switch (r) {
        case 0:  return WG_BLINK_OK;
        case 1:  return WG_BLINK_HALT;
        default: return WG_BLINK_ERROR;
    }
}

WGBlinkResult wg_blink_run(WGBlinkInstance *inst, int max_instructions) {
    if (!inst || !inst->vm) return WG_BLINK_ERROR;

    sigjmp_buf recovery;
    wg_blink_set_abort_recovery(&recovery);
    if (sigsetjmp(recovery, 0)) {
        wg_blink_set_abort_recovery(NULL);
        WG_LOGE(TAG, "Blink execution aborted");
        return WG_BLINK_ERROR;
    }

    int r = WGBlinkVM_Run(inst->vm, max_instructions);
    wg_blink_set_abort_recovery(NULL);

    switch (r) {
        case 0:  return WG_BLINK_OK;
        case 1:  return WG_BLINK_HALT;
        default: return WG_BLINK_ERROR;
    }
}

uint64_t wg_blink_get_reg(WGBlinkInstance *inst, int reg_index) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_GetReg(inst->vm, reg_index);
}

void wg_blink_set_reg(WGBlinkInstance *inst, int reg_index, uint64_t val) {
    if (!inst || !inst->vm) return;
    WGBlinkVM_SetReg(inst->vm, reg_index, val);
}

void wg_blink_set_xmm_low(WGBlinkInstance *inst, int idx, uint64_t lo) {
    if (!inst || !inst->vm) return;
    WGBlinkVM_SetXmmLow(inst->vm, idx, lo);
}

uint64_t wg_blink_get_rip(WGBlinkInstance *inst) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_GetRIP(inst->vm);
}

// Last stop reason (0 clean, -1 halt, -4 segfault, -8 #GP, ...) and the
// faulting address — used to turn a guest memory fault into a Windows
// STATUS_ACCESS_VIOLATION dispatched through the guest's SEH chain.
int wg_blink_get_stop_reason(WGBlinkInstance *inst) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_GetStopReason(inst->vm);
}

uint64_t wg_blink_get_fault_addr(WGBlinkInstance *inst) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_GetFaultAddr(inst->vm);
}

void wg_blink_set_rip(WGBlinkInstance *inst, uint64_t rip) {
    if (!inst || !inst->vm) return;
    WGBlinkVM_SetRIP(inst->vm, rip);
}

void wg_blink_set_fs_base(WGBlinkInstance *inst, uint64_t base) {
    if (inst && inst->vm) WGBlinkVM_SetFsBase(inst->vm, base);
}

void wg_blink_set_gs_base(WGBlinkInstance *inst, uint64_t base) {
    if (inst && inst->vm) WGBlinkVM_SetGsBase(inst->vm, base);
}

uint64_t wg_blink_get_flags(WGBlinkInstance *inst) {
    return (inst && inst->vm) ? WGBlinkVM_GetFlags(inst->vm) : 0;
}
void wg_blink_set_flags(WGBlinkInstance *inst, uint64_t f) {
    if (inst && inst->vm) WGBlinkVM_SetFlags(inst->vm, f);
}
uint64_t wg_blink_get_fs_base(WGBlinkInstance *inst) {
    return (inst && inst->vm) ? WGBlinkVM_GetFsBase(inst->vm) : 0;
}
uint64_t wg_blink_get_gs_base(WGBlinkInstance *inst) {
    return (inst && inst->vm) ? WGBlinkVM_GetGsBase(inst->vm) : 0;
}

bool wg_blink_write_mem(WGBlinkInstance *inst, uint64_t addr,
                         const void *buf, uint32_t len) {
    if (!inst || !inst->vm) return false;
    return WGBlinkVM_WriteMem(inst->vm, addr, buf, len) != 0;
}

bool wg_blink_read_mem(WGBlinkInstance *inst, uint64_t addr,
                        void *buf, uint32_t len) {
    if (!inst || !inst->vm) return false;
    return WGBlinkVM_ReadMem(inst->vm, addr, buf, len) != 0;
}

// Fast guest->guest copy / fill (direct host page walk; no malloc/bounce). Return
// 0 on an unmapped page so the caller can fall back to its slow path.
uint64_t wg_blink_mem_copy(WGBlinkInstance *inst, uint64_t dst, uint64_t src, uint64_t n) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_MemCopy(inst->vm, dst, src, n);
}
uint64_t wg_blink_mem_set(WGBlinkInstance *inst, uint64_t dst, int c, uint64_t n) {
    if (!inst || !inst->vm) return 0;
    return WGBlinkVM_MemSet(inst->vm, dst, c, n);
}

bool wg_blink_has_jit(void) {
#if defined(__aarch64__) && defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

const char *wg_blink_version(void) {
    return "blink x86-64 emulator (ARM64 JIT via MAP_JIT)";
}

// ── Real-threads support ─────────────────────────────────────────────────────
// Create a Machine sharing this instance's System (guest memory). The engine
// spawns a pthread that adopts it and drives its own blink loop.
void *wg_blink_new_thread_machine(WGBlinkInstance *inst) {
    return (inst && inst->vm) ? WGBlinkVM_NewThreadMachine(inst->vm) : NULL;
}
// Make Machine `m` the calling pthread's current Machine (call on the worker).
void wg_blink_adopt_machine(void *m) { WGBlinkVM_AdoptMachine(m); }
void wg_blink_free_thread_machine(void *m) { WGBlinkVM_FreeThreadMachine(m); }
