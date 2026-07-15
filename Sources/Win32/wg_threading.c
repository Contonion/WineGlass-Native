#include "wg_threading.h"
#include "wg_blink_bridge.h"
#include "wg_log.h"
#include <stdlib.h>
#include <string.h>

#define TAG "Thread"

WGThreadScheduler *wg_sched_create(void) {
    WGThreadScheduler *s = calloc(1, sizeof(WGThreadScheduler));
    if (!s) return NULL;
    s->current = -1;
    s->next_id = 0x1000;
    s->next_handle = 0x7100;
    // box64 is identity-mapped and Apple Silicon reserves the low 4GB, so thread
    // stacks must live in a high, box64-free region (demand-paged on first touch).
    const char *cpu = getenv("WG_CPU");
    if (cpu && (!strcmp(cpu, "box64") || !strcmp(cpu, "BOX64")))
        s->next_stack_addr = 0x50000000000ULL;   // 5TB region, clear of everything
    else
        s->next_stack_addr = 0x60000000u; // 32-bit path: above the guest heap
    WG_LOGW(TAG, "sched created: next_stack_addr=0x%llX (WG_CPU=%s)",
            (unsigned long long)s->next_stack_addr, cpu ? cpu : "(null)");
    return s;
}

void wg_sched_destroy(WGThreadScheduler *sched) {
    free(sched);
}

// Full x86-64 context. The old version saved only 8 GPRs as 32-bit with no flags
// — fine for 32-bit apps, but for a 64-bit guest it dropped r8-r15, the high
// dwords, and EFLAGS, so any switch mid-computation corrupted the resumed thread
// (e.g. a lost r12 / stale ZF crashed a string-scan loop). Save/restore all 16
// GPRs (64-bit), rip, flags, and both segment bases.
static void save_regs(WGThreadRegs *regs, void *blink) {
    for (int i = 0; i < 16; i++)
        regs->gpr[i] = wg_blink_get_reg(blink, i);
    regs->rip     = wg_blink_get_rip(blink);
    regs->flags   = wg_blink_get_flags(blink);
    regs->fs_base = wg_blink_get_fs_base(blink);
    regs->gs_base = wg_blink_get_gs_base(blink);
}

static void restore_regs(const WGThreadRegs *regs, void *blink) {
    for (int i = 0; i < 16; i++)
        wg_blink_set_reg(blink, i, regs->gpr[i]);
    wg_blink_set_rip(blink, regs->rip);
    wg_blink_set_flags(blink, regs->flags);
    wg_blink_set_fs_base(blink, regs->fs_base);
    wg_blink_set_gs_base(blink, regs->gs_base);
}

uint32_t wg_sched_create_thread(WGThreadScheduler *sched, void *blink,
                                 uint64_t start_addr, uint64_t param,
                                 uint32_t flags, uint32_t *out_tid) {
    // Find a free slot
    int slot = -1;
    for (int i = 0; i < WG_MAX_THREADS; i++) {
        if (sched->threads[i].state == WG_THREAD_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        WG_LOGE(TAG, "No free thread slots");
        return 0;
    }

    WGThread *t = &sched->threads[slot];
    memset(t, 0, sizeof(*t));
    t->id = sched->next_id++;
    t->handle = sched->next_handle++;
    t->start_addr = start_addr;
    t->param = param;
    t->exit_code = 259; // STILL_ACTIVE

    // Allocate guest stack (1MB, grows downward)
    t->stack_base = sched->next_stack_addr;
    t->stack_size = WG_THREAD_STACK;
    sched->next_stack_addr += WG_THREAD_STACK + 0x1000; // guard page gap

    // Map the stack in blink (zeroed)
    uint8_t *zstack = calloc(1, t->stack_size);
    if (zstack) {
        wg_blink_load_code(blink, t->stack_base, zstack, t->stack_size, 0);
        free(zstack);
    }

    // Set up initial register state:
    // RSP = top of stack minus space for return address + arg (64-bit: box64 stacks
    // live >4GB, so a uint32 sp would truncate to a low unmapped address).
    uint64_t sp = t->stack_base + t->stack_size - 0x100;
    // x64 ABI: at a function's entry RSP must be (16-aligned - 8) — i.e. RSP%16==8,
    // the state "just after a CALL pushed the 8-byte return address" — so the proc's
    // aligned SSE spills (movaps [rsp+N]) don't #GP/SIGSEGV. Cooperative 64-bit thread
    // procs (e.g. Visage's) crashed at movaps on a misaligned stack. This is also
    // harmless for 32-bit procs (MSVC re-aligns with `and esp,-16` when it needs SSE).
    sp &= ~0xFULL; sp -= 8;
    const char *cpu = getenv("WG_CPU");
    if (cpu && (!strcmp(cpu, "box64") || !strcmp(cpu, "BOX64"))) {
        // Win64: arg is in RCX (set below); the stack just needs an 8-byte return
        // sentinel so the proc's final RET pops RIP=0 -> clean thread exit.
        uint64_t ret_sentinel = 0;
        wg_blink_write_mem(blink, sp, &ret_sentinel, 8);
    } else {
        // 32-bit __stdcall: [ret_addr=0][param]
        uint32_t ret_sentinel = 0, param32 = (uint32_t)param;
        wg_blink_write_mem(blink, sp, &ret_sentinel, 4);
        wg_blink_write_mem(blink, sp + 4, &param32, 4);
    }

    t->regs.gpr[0] = 0;         // EAX
    // RCX = param: the x64 calling convention passes the thread proc's first (only)
    // argument in RCX, NOT on the stack. A 64-bit thread proc (e.g. Visage's, whose
    // entry immediately does `mov rbx,rcx; ...; WaitForSingleObject([rbx+0x18])`)
    // reads its object from RCX — leaving RCX=0 gave it a NULL param, so it waited on
    // a null event forever (the cooperative-mode 0x9FDA6F null-event deadlock). Also
    // keep the stack param below for 32-bit __stdcall procs (which ignore RCX).
    t->regs.gpr[1] = param;     // RCX (x64 arg0)
    t->regs.gpr[2] = 0;         // EDX
    t->regs.gpr[3] = 0;         // EBX
    t->regs.gpr[4] = sp;        // ESP
    t->regs.gpr[5] = sp;        // EBP
    t->regs.gpr[6] = 0;         // ESI
    t->regs.gpr[7] = 0;         // EDI
    t->regs.rip = start_addr;

    // TEB — reuse the main thread's TEB for now (same FS base)
    // TODO: allocate per-thread TEB with unique ThreadId
    WGThread *main_t = (sched->current >= 0) ? &sched->threads[sched->current] : NULL;
    t->regs.fs_base = main_t ? main_t->regs.fs_base : 0;
    t->teb = t->regs.fs_base;

    if (flags & 0x4) { // CREATE_SUSPENDED
        t->state = WG_THREAD_SUSPENDED;
    } else {
        t->state = WG_THREAD_READY;
    }

    if (out_tid) *out_tid = t->id;
    sched->count++;

    WG_LOGI(TAG, "CreateThread: slot=%d id=0x%X handle=0x%X start=0x%llX param=0x%llX stack=0x%llX-0x%llX (next=0x%llX) %s",
            slot, t->id, t->handle, (unsigned long long)start_addr, (unsigned long long)param,
            (unsigned long long)t->stack_base, (unsigned long long)(t->stack_base + t->stack_size),
            (unsigned long long)sched->next_stack_addr,
            (flags & 0x4) ? "(suspended)" : "(ready)");

    return t->handle;
}

void wg_sched_save_current(WGThreadScheduler *sched, void *blink,
                            WGThreadState new_state) {
    if (sched->current < 0) return;
    WGThread *t = &sched->threads[sched->current];
    save_regs(&t->regs, blink);
    t->state = new_state;
}

bool wg_sched_switch_next(WGThreadScheduler *sched, void *blink) {
    int start = (sched->current >= 0) ? sched->current + 1 : 0;

    for (int i = 0; i < WG_MAX_THREADS; i++) {
        int idx = (start + i) % WG_MAX_THREADS;
        WGThread *t = &sched->threads[idx];
        if (t->state == WG_THREAD_READY) {
            int prev = sched->current;
            sched->current = idx;
            t->state = WG_THREAD_RUNNING;
            restore_regs(&t->regs, blink);
            // Only log real context switches, not self-reswitches. In a deadlock a
            // handful of threads cycle millions of times, so: log a switch only when
            // the target thread differs from the last one we LOGGED (collapses a
            // steady 0<->N ping-pong to nothing), and hard-cap the rest at 1/8192 so
            // even a long spin adds only a trickle. First 64 always log.
            if (prev != idx) {
                static uint32_t sw = 0; static int lg1 = -1, lg2 = -1;
                // Log only the first 64 switches and NEW targets vs the last two —
                // a steady-state N<->M ping-pong deadlock then emits nothing.
                if (sw < 64 || (idx != lg1 && idx != lg2)) {
                    WG_LOGD(TAG, "Switched to thread %d (id=0x%X, rip=0x%X)",
                            idx, t->id, t->regs.rip);
                    lg2 = lg1; lg1 = idx;
                }
                sw++;
            }
            return true;
        }
    }
    return false;
}

// True if some thread OTHER than the current one is READY to run. Used by the
// cooperative preemptive time-slice so we don't do a wasteful self-switch when
// the current thread is the only runnable one.
bool wg_sched_other_ready(WGThreadScheduler *sched) {
    for (int i = 0; i < WG_MAX_THREADS; i++) {
        if (i != sched->current && sched->threads[i].state == WG_THREAD_READY)
            return true;
    }
    return false;
}

bool wg_sched_yield(WGThreadScheduler *sched, void *blink,
                     WGThreadState block_reason) {
    wg_sched_save_current(sched, blink, block_reason);
    if (wg_sched_switch_next(sched, blink)) {
        return true;
    }
    // No other threads — restore the current one
    if (sched->current >= 0) {
        WGThread *t = &sched->threads[sched->current];
        t->state = WG_THREAD_RUNNING;
        restore_regs(&t->regs, blink);
    }
    return false;
}

void wg_sched_wake(WGThreadScheduler *sched, uint32_t handle) {
    for (int i = 0; i < WG_MAX_THREADS; i++) {
        WGThread *t = &sched->threads[i];
        if (t->state == WG_THREAD_WAITING && t->wait_handle == handle) {
            t->state = WG_THREAD_READY;
            t->wait_handle = 0;
            WG_LOGD(TAG, "Woke thread %d (id=0x%X) waiting on handle 0x%X",
                    i, t->id, handle);
        }
    }
}

void wg_sched_exit_thread(WGThreadScheduler *sched, void *blink,
                           uint32_t exit_code) {
    if (sched->current < 0) return;
    WGThread *t = &sched->threads[sched->current];
    t->state = WG_THREAD_EXITED;
    t->exit_code = exit_code;
    WG_LOGI(TAG, "Thread %d (id=0x%X) exited with code %u",
            sched->current, t->id, exit_code);

    // Switch to another thread
    sched->current = -1;
    wg_sched_switch_next(sched, blink);
}

WGThread *wg_sched_find(WGThreadScheduler *sched, uint32_t handle) {
    for (int i = 0; i < WG_MAX_THREADS; i++) {
        if (sched->threads[i].handle == handle &&
            sched->threads[i].state != WG_THREAD_FREE)
            return &sched->threads[i];
    }
    return NULL;
}

WGThread *wg_sched_current(WGThreadScheduler *sched) {
    if (sched->current < 0) return NULL;
    return &sched->threads[sched->current];
}

uint32_t wg_sched_current_tid(WGThreadScheduler *sched) {
    if (sched->current < 0) return 0;
    return sched->threads[sched->current].id;
}
