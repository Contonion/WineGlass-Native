#ifndef WG_THREADING_H
#define WG_THREADING_H

#include <stdint.h>
#include <stdbool.h>

#define WG_MAX_THREADS    32
#define WG_THREAD_STACK   (1024 * 1024)  // 1MB per thread stack

typedef enum {
    WG_THREAD_FREE = 0,
    WG_THREAD_RUNNING,
    WG_THREAD_READY,
    WG_THREAD_WAITING,
    WG_THREAD_SUSPENDED,
    WG_THREAD_EXITED,
} WGThreadState;

typedef struct {
    uint64_t gpr[16];    // RAX..R15 (full x86-64; low 8 double as EAX..EDI)
    uint64_t rip;
    uint64_t flags;      // blink lazy EFLAGS (m->flags) — MUST be preserved or a
                         // switch mid-computation corrupts the next cmp/Jcc
    uint64_t fs_base;    // 32-bit TEB base
    uint64_t gs_base;    // 64-bit TEB base
} WGThreadRegs;

typedef struct {
    WGThreadState state;
    uint32_t      id;
    uint32_t      handle;        // Win32 thread handle
    uint64_t      stack_base;    // guest address of stack bottom (64-bit: box64 >4GB)
    uint32_t      stack_size;
    uint64_t      teb;           // guest address of TEB (64-bit)
    uint64_t      start_addr;    // thread entry point (64-bit)
    uint64_t      param;         // thread parameter (64-bit)
    uint32_t      exit_code;
    uint32_t      wait_handle;   // handle being waited on (0 = not waiting)
    uint32_t      wait_timeout;  // timeout in ms (INFINITE = 0xFFFFFFFF)
    uint64_t      wait_start_ms; // wall-clock ms when this wait began (for real timeouts)
    uint32_t      wait_cv_gen;   // condition-variable generation captured when the sleep began
    WGThreadRegs  regs;          // saved register state
} WGThread;

typedef struct {
    WGThread threads[WG_MAX_THREADS];
    int      current;            // index of currently running thread
    int      count;              // total threads created
    uint32_t next_id;
    uint32_t next_handle;
    uint64_t next_stack_addr;    // bump allocator for thread stacks (64-bit)
} WGThreadScheduler;

WGThreadScheduler *wg_sched_create(void);
void               wg_sched_destroy(WGThreadScheduler *sched);

// Create a new thread. Returns the Win32 thread handle.
uint32_t wg_sched_create_thread(WGThreadScheduler *sched, void *blink,
                                 uint64_t start_addr, uint64_t param,
                                 uint32_t flags, uint32_t *out_tid);

// Save current thread's state from blink and mark it as `new_state`.
void wg_sched_save_current(WGThreadScheduler *sched, void *blink,
                            WGThreadState new_state);

// Pick the next runnable thread and restore its state into blink.
// Returns true if a thread was switched to, false if no runnable threads.
bool wg_sched_switch_next(WGThreadScheduler *sched, void *blink);

// True if a thread other than the current one is READY (for preemptive slicing).
bool wg_sched_other_ready(WGThreadScheduler *sched);

// Called when the current thread should yield (blocking call).
// Saves current state, finds next runnable thread, switches.
// Returns true if switched, false if stuck (no other threads).
bool wg_sched_yield(WGThreadScheduler *sched, void *blink,
                     WGThreadState block_reason);

// Wake threads waiting on a specific handle (SetEvent, etc.)
void wg_sched_wake(WGThreadScheduler *sched, uint32_t handle);

// Mark current thread as exited
void wg_sched_exit_thread(WGThreadScheduler *sched, void *blink,
                           uint32_t exit_code);

// Get thread by handle
WGThread *wg_sched_find(WGThreadScheduler *sched, uint32_t handle);

// Get current thread
WGThread *wg_sched_current(WGThreadScheduler *sched);

// Get current thread ID
uint32_t wg_sched_current_tid(WGThreadScheduler *sched);

#endif
