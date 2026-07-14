// Full blink integration — compiled with blink's header paths.
// Must be compiled separately from the WineGlass Xcode project
// because blink's headers shadow system headers.

// System headers MUST come before blink includes because blink
// shadows signal.h, string.h, etc.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Force system signal.h via the SDK sysroot path
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
typedef int sig_atomic_t_placeholder_;
#include <sys/signal.h>
#pragma clang diagnostic pop

#include "blink/machine.h"
#include "blink/pml4t.h"
#include "blink/biosrom.h"   // IsRomAddress (used by the fast in-guest memcpy)
#include "blink/bus.h"
#include "blink/endian.h"
#include "blink/x86.h"
#include "blink/map.h"
#include "blink/tunables.h"
#include "blink/jit.h"

struct WGBlinkVM {
    struct Machine *m;
    struct System  *s;
    int last_stop;   // last siglongjmp code from onhalt (kMachine* / -1 halt)
};

// Real-threads rearchitecture: each guest thread runs its OWN blink Machine on
// its own pthread, all sharing one System (guest memory). We keep blink built
// with DISABLE_THREADS (its SMP pagelocks deadlock our manual per-instruction
// stepping; and blink's g_machine is a plain global there). blink's interpreter
// hot path uses the `m` we PASS (not g_machine — verified: memory.c has zero
// g_machine reads), so we track the current thread's Machine in OUR OWN real
// TLS (`__thread`, which blink's `#define _Thread_local` macro does NOT touch)
// and route every per-thread accessor through cur_m(). The main engine thread
// gets wg_tls_m == vm->m (identical to the old behaviour); a worker pthread gets
// its own. We also mirror into blink's global g_machine for the rare paths that
// read it (diagnostics/FreeMachine/asserts).
static __thread struct Machine *wg_tls_m = (struct Machine *)0;
static inline struct Machine *cur_m(struct WGBlinkVM *vm) {
    if (wg_tls_m) return wg_tls_m;
    return vm ? vm->m : (struct Machine *)0;
}

static int s_blink_initialized = 0;
static bool s_linear = false;
static bool s_jit = false;
// -1 = decide from WG_JIT env (macOS test harness); 0/1 = forced by the app.
// The iOS app runs the MAP_JIT smoke test at launch and forces this on when the
// device permits JIT (see wg_jit_probe.c / WGSceneDelegate). Must be set BEFORE
// the first VM is created (ensure_initialized reads it once).
static signed char s_jit_force = -1;
void WGBlinkVM_ForceJit(int on) { s_jit_force = on ? 1 : 0; }
static struct sigaction s_old_segv, s_old_bus, s_old_ill;

// In linear mode a guest memory fault is a HOST SIGSEGV/SIGBUS at kSkew+addr.
// Replicate blink's OnFatalSystemSignal: fix the XNU signal, let JIT handle
// self-modifying-code faults, and on a real fault while the guest is running
// (m->canhalt) longjmp to our run loop's recovery (WGBlinkVM_Run's sigsetjmp).
// For anything else (a genuine crash in our own C), chain to the prior handler.
static volatile long s_fault_count = 0;
static void wg_on_fatal_signal(int sig, siginfo_t *si, void *ptr) {
    if (sig == SIGILL) {
        // A JIT codegen bug emitted an illegal ARM instruction. Capture the guest rip
        // (path start) + host PC to a DEDICATED file (stderr didn't survive the crash).
        FILE *f = fopen("/tmp/wg_sigill.txt", "a");
        if (f) {
            fprintf(f, "guest_rip=0x%llx host_pc=%p\n",
                    wg_tls_m ? (unsigned long long)wg_tls_m->ip : 0ULL, si ? si->si_addr : (void *)0);
            fclose(f);
        }
    }
    struct Machine *m = wg_tls_m;
    if (m) {
        sig = FixXnuSignal(m, sig, si);            // Apple: resolve real fault addr/sig
        // JIT self-modifying-code fixup — LINEAR mode only (host mprotect'd code
        // pages). In nolinear (our default) SMC is handled in the memory WRITE
        // path, never here; skipping keeps this signal handler lock-free, which
        // matters now that JIT compilation takes a real mutex (WG_REAL_THREADS).
        if (!FLAG_nolinear && IsSelfModifyingCodeSegfault(m, si)) return;
        if (getenv("WG_FAULTLOG") && (++s_fault_count % 5000) == 1) {
            fprintf(stderr, "[fault #%ld] sig=%d addr=%p rip=%llx\n",
                    s_fault_count, sig, si->si_addr, (unsigned long long)m->ip);
        }
        if (m->canhalt) {
            if (getenv("WG_FAULTLOG")) {
                fprintf(stderr, "[CRASH] rip=%llx faultaddr=%p\n  regs:",
                        (unsigned long long)m->ip, si->si_addr);
                for (int _r = 0; _r < 16; _r++)
                    fprintf(stderr, " r%d=%llx", _r,
                            (unsigned long long)Read64(m->weg[_r]));
                fprintf(stderr, "\n");
            }
            g_siginfo = *si;
            siglongjmp(m->onhalt, kMachineFatalSystemSignal);
        }
    }
    struct sigaction *old = (sig == SIGBUS) ? &s_old_bus : (sig == SIGILL) ? &s_old_ill : &s_old_segv;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) { old->sa_sigaction(sig, si, ptr); return; }
    if (old->sa_handler && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig); return;
    }
    signal(sig, SIG_DFL); raise(sig);
}

static void wg_install_fault_handler(void) {
    if (!s_jit) return;   // only JIT needs it (self-modifying-code write faults)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = wg_on_fatal_signal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_old_segv);
    sigaction(SIGBUS, &sa, &s_old_bus);
    sigaction(SIGILL, &sa, &s_old_ill);   // catch JIT bad-codegen (illegal ARM instruction)
    if (getenv("WG_SIGLOG")) { fprintf(stderr, "[wg] host fault handler installed (SEGV/BUS/ILL)\n"); fflush(stderr); }
}

static void ensure_initialized(void) {
    if (!s_blink_initialized) {
        extern bool FLAG_nolinear;
        InitMap();  // probes FLAG_vabits
        // Apple Silicon uses 16KB host pages, but the Windows guest is 4KB-
        // aligned, so blink's LINEAR memory (1:1 MAP_FIXED at kSkew+va) is
        // impossible here — MAP_FIXED only succeeds on 16KB-aligned addresses.
        // Hence the software MMU (nolinear) is mandatory. blink's JIT still has
        // a nolinear path (memory ops call the software MMU; only the dispatch
        // is native), which is the real speedup available to us. WG_JIT enables
        // it (default keeps the proven interpreter). The fault handler is needed
        // for JIT self-modifying-code (write to a compiled page -> host fault).
        s_jit = (s_jit_force >= 0) ? (bool)s_jit_force : (getenv("WG_JIT") != 0);
#ifdef WG_PATHB
        // PATH B — NATIVE LINEAR MEMORY. kSkew is nonzero (config.h.ios leaves
        // WG_NOLINEAR_JIT undefined), so guest access = host(kSkew+va) in ONE
        // instruction (JIT emits it) instead of the software-MMU page walk.
        // The 16KB/4KB page-size mismatch is sidestepped by mapping the ENTIRE
        // <4GB guest space as ONE big region here (no per-4KB-page MAP_FIXED),
        // so ReserveVirtual just sub-allocates within it (see memorymalloc.c).
        // Every guest code/data/stack/thunk address is < 4GB (see below), so a
        // single 4GB reservation at kSkew backs all of it.
        s_linear = true;
        FLAG_nolinear = false;
        {
            extern void *wg_linear_base; extern unsigned long long wg_linear_size;
            // 8GB: the low 4GB is the 32-bit guest VA; the 4..8GB half backs the large
            // VirtualAlloc pools (region 3) so a full UE4 asset load doesn't OOM the
            // 32-bit heap. WG_LINEAR_GB overrides (in GB) if a host can't reserve 8GB.
            unsigned long long gsize = 0x200000000ULL;
            if (getenv("WG_LINEAR_GB")) gsize = (unsigned long long)atoi(getenv("WG_LINEAR_GB")) << 30;
            void *want = (void *)(uintptr_t)kSkew;
            void *got = Mmap(want, gsize, PROT_READ | PROT_WRITE,
                             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS_, -1, 0,
                             "wg_linear");
            if (got != want) {
                fprintf(stderr, "[wg] PATH B: linear region mmap FAILED "
                        "(want %p got %p) — cannot run linear\n", want, got);
                abort();
            }
            wg_linear_base = want;
            wg_linear_size = gsize;
            if (getenv("WG_SIGLOG"))
                fprintf(stderr, "[wg] PATH B: linear region %p..%p (kSkew=%#llx), "
                        "guest access is native host=kSkew+va\n",
                        want, (char *)want + gsize, (unsigned long long)kSkew);
        }
#else
        s_linear = false;               // linear unusable on 16KB-page hosts
        FLAG_nolinear = true;
#endif
        wg_install_fault_handler();     // installs only when s_jit
        s_blink_initialized = 1;
    }
}

static struct WGBlinkVM *create_vm_with_mode(struct XedMachineMode mode) {
    ensure_initialized();

    struct WGBlinkVM *vm = calloc(1, sizeof(struct WGBlinkVM));
    if (!vm) return NULL;

    vm->s = NewSystem(mode);
    if (!vm->s) { free(vm); return NULL; }

    if (!vm->s->real) {
        long pagesize = FLAG_pagesize;
        long real_size = (kRealSize + pagesize - 1) & ~(pagesize - 1);
        vm->s->real = Mmap(NULL, real_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS_, -1, 0, "real");
        if (!vm->s->real) { FreeSystem(vm->s); free(vm); return NULL; }
    }

    vm->m = NewMachine(vm->s, NULL);
    if (!vm->m) { FreeSystem(vm->s); free(vm); return NULL; }
    // JIT compiled in; enabled only for WG_JIT (nolinear/software-MMU JIT).
    // Default keeps the proven interpreter.
    if (!s_jit) DisableJit(&vm->s->jit);
    wg_tls_m = vm->m;    // this (main engine) thread's current Machine (real TLS)
    g_machine = vm->m;   // mirror into blink's global for its diagnostic paths

    if (mode.omode == XED_MODE_LONG) {
        // 64-bit: paging required, set up page tables
        vm->s->cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_PG;
        vm->s->cr3 = AllocatePageTable(vm->s);
        if (vm->s->cr3 == (u64)-1) {
            FreeMachine(vm->m); FreeSystem(vm->s); free(vm); return NULL;
        }
    } else if (mode.omode == XED_MODE_LEGACY) {
        // 32-bit: protected mode, NO paging — use s->real as flat memory
        vm->s->cr0 = CR0_PE | CR0_MP | CR0_ET;
    }
    vm->m->flags |= (1 << 9); // IF

    return vm;
}

struct WGBlinkVM *WGBlinkVM_Create(void) {
    return create_vm_with_mode(XED_MACHINE_MODE_LONG);
}

struct WGBlinkVM *WGBlinkVM_Create32(void) {
    // Always create System in LONG mode (page tables work).
    // We switch ONLY the Machine's decoder to 32-bit later.
    return create_vm_with_mode(XED_MACHINE_MODE_LONG);
}

void WGBlinkVM_SwitchTo32(struct WGBlinkVM *vm) {
    if (!vm) return;
    // Set ONLY the Machine's instruction decoder to 32-bit.
    // Do NOT call SetMachineMode() — that changes the System mode
    // too, which breaks ReserveVirtual permanently.
    vm->m->mode = XED_MACHINE_MODE_LEGACY_32;
}

// (Old create flow moved into create_vm_with_mode above)

void WGBlinkVM_Destroy(struct WGBlinkVM *vm) {
    if (!vm) return;
    if (vm->m) FreeMachine(vm->m);
    if (vm->s) FreeSystem(vm->s);
    free(vm);
}

int WGBlinkVM_LoadCode(struct WGBlinkVM *vm, unsigned long long addr,
                        const void *code, unsigned int size,
                        unsigned long long entry_rip) {
    if (!vm) return 0;

    // Always use long mode page tables (System is always XED_MODE_LONG)
    long long page_addr = addr & -4096LL;
    unsigned long long page_size = ((addr + size + 4095) & -4096LL) - page_addr;
    if (getenv("WG_SIGLOG"))
        fprintf(stderr, "[wg] LoadCode addr=%#llx size=%u -> ReserveVirtual(%#llx,%#llx)\n",
                (unsigned long long)addr, size, (unsigned long long)page_addr,
                (unsigned long long)page_size);
    if (ReserveVirtual(vm->s, page_addr, page_size,
                       PAGE_U | PAGE_RW, -1, 0, false, false) == -1) {
        if (getenv("WG_SIGLOG")) fprintf(stderr, "[wg] LoadCode: ReserveVirtual FAILED\n");
        return 0;
    }
    if (getenv("WG_SIGLOG")) fprintf(stderr, "[wg] LoadCode: ReserveVirtual OK, CopyToUser...\n");
    CopyToUser(vm->m, addr, (void *)code, size);
    if (getenv("WG_SIGLOG")) fprintf(stderr, "[wg] LoadCode: CopyToUser OK\n");

    if (entry_rip) {
        vm->m->ip = entry_rip;
    }

    return 1;
}

int WGBlinkVM_SetupStack(struct WGBlinkVM *vm, unsigned long long entry_rip) {
    if (!vm) return 0;

    // Determine if machine is in 32-bit decode mode
    int is_32bit = (vm->m->mode.omode == XED_MODE_LEGACY ||
                    vm->m->mode.omode == XED_MODE_REAL);

    // Allocate stack via long mode page tables. 16MB (not 1MB): real apps
    // request multi-MB stacks in their PE header (UE4's Visage asks for 11MB)
    // and deep native call chains (recursive init, big stack format buffers)
    // overflow a 1MB stack. Keep in sync with the TEB StackLimit in the engine.
    long long stack_base = 0x7FFF0000LL;
    long long stack_size = 0x1000000LL;
    if (ReserveVirtual(vm->s, stack_base - stack_size, stack_size,
                       PAGE_U | PAGE_RW, -1, 0, false, false) == -1) {
        return 0;
    }

    if (is_32bit) {
        // 32-bit: 4-byte stack frames
        unsigned int sp = (unsigned int)(stack_base - 0x100);
        sp -= 4;
        unsigned int zero = 0;
        CopyToUser(vm->m, sp, &zero, 4);
        Put32(vm->m->sp, sp);
        Put32(vm->m->bp, sp + 4);
    } else {
        // 64-bit: 8-byte stack frames
        unsigned long long sp = stack_base - 0x100;
        sp -= 8;
        unsigned char zero[8] = {0};
        CopyToUser(vm->m, sp, zero, 8);
        Put64(vm->m->sp, sp);
        Put64(vm->m->bp, sp + 8);
    }

    vm->m->ip = entry_rip;
    return 1;
}

// Set the FS/GS segment base (linear address). Windows uses FS for the 32-bit
// TEB and GS for the 64-bit TEB; the MSVC CRT reads fs:[0x18]/[0x2C]/[0x30]
// during startup, so without a real base + TEB it faults immediately.
void WGBlinkVM_SetFsBase(struct WGBlinkVM *vm, unsigned long long base) {
    if (vm) cur_m(vm)->fs.base = base;
}
void WGBlinkVM_SetGsBase(struct WGBlinkVM *vm, unsigned long long base) {
    if (vm) cur_m(vm)->gs.base = base;
}

// Flags + segment-base getters — needed to save/restore full x86-64 thread
// context on a cooperative switch (m->flags carries blink's lazy EFLAGS).
unsigned long long WGBlinkVM_GetFlags(struct WGBlinkVM *vm) {
    return vm ? (unsigned long long)cur_m(vm)->flags : 0;
}
void WGBlinkVM_SetFlags(struct WGBlinkVM *vm, unsigned long long f) {
    if (vm) cur_m(vm)->flags = (unsigned)f;
}
unsigned long long WGBlinkVM_GetFsBase(struct WGBlinkVM *vm) {
    return vm ? cur_m(vm)->fs.base : 0;
}
unsigned long long WGBlinkVM_GetGsBase(struct WGBlinkVM *vm) {
    return vm ? cur_m(vm)->gs.base : 0;
}


// Defined in wg_blink_stubs.c — lets TerminateSignal longjmp back to us
extern void wg_blink_set_onhalt(sigjmp_buf *buf);

int WGBlinkVM_Step(struct WGBlinkVM *vm) {
    if (!vm) return -1;
    struct Machine *m = cur_m(vm);

    m->canhalt = true;

    int rc = sigsetjmp(m->onhalt, 0);
    wg_blink_set_onhalt(&m->onhalt);

    if (rc) {
        vm->last_stop = rc;
        m->canhalt = false;
        wg_blink_set_onhalt(NULL);
        return 1; // any signal/halt = stop
    }

    LoadInstruction(m, GetPc(m));
    ExecuteInstruction(m);

    vm->last_stop = 0;
    m->canhalt = false;
    wg_blink_set_onhalt(NULL);

    if (m->ip == 0) return 1;
    return 0;
}

int WGBlinkVM_Run(struct WGBlinkVM *vm, int max_insns) {
    if (!vm) return -1;
    struct Machine *m = cur_m(vm);

    m->canhalt = true;

    int rc = sigsetjmp(m->onhalt, 0);
    wg_blink_set_onhalt(&m->onhalt);

    if (rc) {
        vm->last_stop = rc;
        m->canhalt = false;
        wg_blink_set_onhalt(NULL);
        return 1; // halt/signal
    }

    for (int i = 0; i < max_insns; i++) {
        if (m->ip == 0) {
            vm->last_stop = 0;
            m->canhalt = false;
            wg_blink_set_onhalt(NULL);
            return 1;
        }
        // Wild-jump guard: a call through a CORRUPT vtable (UE4 thread-lifecycle
        // type-confusion: an object ptr points at a thread record {id,handle} so
        // [obj] reads e.g. 0x0000710900001029 as a "vtable", then call [vt+0x20]
        // targets a huge bogus address) sets m->ip WAY outside the 4GB guest map.
        // The interpreter faults recoverably on the MMU read; the JIT can jump to
        // it. Every real guest code/data/stack/thunk address here is < 4GB, so an
        // ip at/above 4GB is unambiguously a corrupt indirect-call target. Halt so
        // the engine's null-indirect-call recovery (return 0 to the .text caller
        // on [rsp]) runs — same graceful skip the interpreter already gets.
        if (m->ip >= 0x100000000ull) {
            vm->last_stop = 0;      // recoverable halt; engine inspects [rsp]
            m->canhalt = false;
            wg_blink_set_onhalt(NULL);
            return 1;
        }
        LoadInstruction(m, GetPc(m));
        ExecuteInstruction(m);
    }

    vm->last_stop = 0;
    m->canhalt = false;
    wg_blink_set_onhalt(NULL);
    return 0;
}

unsigned long long WGBlinkVM_GetReg(struct WGBlinkVM *vm, int idx) {
    if (!vm || idx < 0 || idx >= 16) return 0;
    return Get64(cur_m(vm)->weg[idx]);
}

void WGBlinkVM_SetReg(struct WGBlinkVM *vm, int idx, unsigned long long val) {
    if (!vm || idx < 0 || idx >= 16) return;
    Put64(cur_m(vm)->weg[idx], val);
}

unsigned long long WGBlinkVM_GetRIP(struct WGBlinkVM *vm) {
    return vm ? cur_m(vm)->ip : 0;
}

void WGBlinkVM_SetRIP(struct WGBlinkVM *vm, unsigned long long rip) {
    if (vm) cur_m(vm)->ip = rip;
}

// Last stop reason: 0 = ran/clean, -1 = halt, kMachineSegmentationFault (-4),
// kMachineProtectionFault (-8), etc. Lets the engine tell a memory fault (which
// should become a guest exception) from a normal halt.
int WGBlinkVM_GetStopReason(struct WGBlinkVM *vm) {
    return vm ? vm->last_stop : 0;
}

// Faulting guest virtual address recorded on the last memory fault.
unsigned long long WGBlinkVM_GetFaultAddr(struct WGBlinkVM *vm) {
    return vm ? (unsigned long long)cur_m(vm)->faultaddr : 0;
}

int WGBlinkVM_WriteMem(struct WGBlinkVM *vm, unsigned long long addr,
                        const void *buf, unsigned int len) {
    if (!vm) return 0;
    CopyToUser(cur_m(vm), addr, (void *)buf, len);
    return 1;
}

int WGBlinkVM_ReadMem(struct WGBlinkVM *vm, unsigned long long addr,
                       void *buf, unsigned int len) {
    if (!vm) return 0;
    CopyFromUser(cur_m(vm), buf, addr, len);
    return 1;
}

// Fast in-guest memcpy: walk src+dst page-by-page and memmove host->host
// DIRECTLY (blink's LookupAddress = the same resolver VirtualCopy uses). This
// avoids the malloc + double copy (src->tmp->dst) the engine's memcpy thunk did
// — halving memory traffic and dropping the per-call heap ops. memmove handles
// overlap (memmove semantics; also correct for memcpy since C guarantees no
// overlap there). Returns dst, or 0 on an unmapped page (caller keeps its ret).
unsigned long long WGBlinkVM_MemCopy(struct WGBlinkVM *vm, unsigned long long dst,
                                     unsigned long long src, unsigned long long n) {
    if (!vm) return 0;
    struct Machine *m = cur_m(vm);
    unsigned long long d0 = dst;
    while (n) {
        u64 kd = 4096 - (dst & 4095), ks = 4096 - (src & 4095);
        u64 k = kd < ks ? kd : ks; if (k > n) k = n;
        u8 *pd = LookupAddress(m, (i64)dst);
        u8 *ps = LookupAddress(m, (i64)src);
        if (!pd || !ps) return 0;
        if (!IsRomAddress(m, pd)) memmove(pd, ps, k);
        n -= k; dst += k; src += k;
    }
    return d0;
}

// Fast in-guest memset: memset each host page directly (no malloc, no bounce).
unsigned long long WGBlinkVM_MemSet(struct WGBlinkVM *vm, unsigned long long dst,
                                    int c, unsigned long long n) {
    if (!vm) return 0;
    struct Machine *m = cur_m(vm);
    unsigned long long d0 = dst;
    while (n) {
        u64 k = 4096 - (dst & 4095); if (k > n) k = n;
        u8 *pd = LookupAddress(m, (i64)dst);
        if (!pd) return 0;
        if (!IsRomAddress(m, pd)) memset(pd, c, k);
        n -= k; dst += k;
    }
    return d0;
}

// ── Real-threads support: a Machine per guest thread, shared System ──────────
//
// Each guest CreateThread/_beginthreadex maps to a real pthread running its own
// blink Machine over the SAME System (shared guest memory + page tables). The
// engine's worker-pthread entry calls NewThreadMachine on the parent thread,
// then (on the new pthread) AdoptMachine to make it current, seeds regs via the
// normal wg_blink_set_* accessors (which route through g_machine), and drives it.

// Create a Machine sharing vm's System. NewMachine memcpy's the parent (so it
// inherits fs/gs base, cr3 via System, etc.) but resets its mode to the System
// mode (LONG); restore the parent's actual decode mode (LEGACY_32 for a 32-bit
// guest). Returns an opaque Machine* (NULL on failure). Caller reseeds regs/rip.
void *WGBlinkVM_NewThreadMachine(struct WGBlinkVM *vm) {
    if (!vm || !vm->s || !vm->m) return (void *)0;
    struct Machine *m = NewMachine(vm->s, vm->m);
    if (!m) return (void *)0;
    m->mode = vm->m->mode;   // match the main Machine's 32/64-bit decode mode
    m->ip = 0;
    m->canhalt = false;
    return m;
}

// Make Machine `mp` the calling pthread's current Machine. Call this FIRST on
// the worker pthread, before any wg_blink_* accessor (they route via g_machine).
void WGBlinkVM_AdoptMachine(void *mp) {
    struct Machine *m = (struct Machine *)mp;
    wg_tls_m = m;        // this worker pthread's current Machine (real TLS)
    g_machine = m;       // mirror into blink's global (racy but only diagnostics use it)
    if (m) m->thread = pthread_self();
}

void WGBlinkVM_FreeThreadMachine(void *mp) {
    if (mp) FreeMachine((struct Machine *)mp);
}
