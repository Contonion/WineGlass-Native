#include "wg_engine.h"
#include "wg_log.h"
#include "wg_pe_loader.h"
#include "wg_x86_state.h"
#include "wg_x86_interp.h"
#include "wg_memory.h"
#include "wg_dll_mapper.h"
#include "wg_blink_bridge.h"
#include "wg_win32_windows.h"
#include "wg_win32_files.h"
#include "wg_native_download.h"

// Weak fallback so every build links. iOS provides a strong wg_native_download
// (NSURLSession) in WGSceneDelegate.m which overrides this; the macOS harness
// (no UIKit) uses this stub, so native fetch is a no-op there and the caller
// degrades to the reactor.
__attribute__((weak)) int wg_native_download(const char *url, const char *dest_path) {
    (void)url; (void)dest_path;
    return 0;
}
#include "wg_win32_gdi.h"
#include "wg_win32_bitmap.h"
#include "wg_nsis_extract.h"
#include "wg_winsock.h"
#include "wg_winhttp.h"
#include "wg_schannel.h"
#include "wg_d3d11.h"
#include "wg_threading.h"
#include "wg_sync.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <time.h>
#include <ctype.h>
#include <TargetConditionals.h>

// The reactor backstop (in the Sleep handler) is a crutch for the SLOW Mac
// interpreter, whose async handshake-recv timing stalls without it. The iOS device
// runs blink with JIT and is fast enough that Steam's natural event-driven timing
// already works there — and the backstop's force-signalling of events only
// PERTURBS that flow (it regressed device downloads). So: ON for the macOS harness,
// OFF for the iOS device. Runtime flag (not #if at the use site) so it's easy to
// flip for A/B testing on either platform.
#if TARGET_OS_IPHONE
static bool s_backstop_enabled = true;  // iOS device: RE-ENABLED. It drives the
                                        // reactor forward so the MANIFEST download's
                                        // queued GET actually gets flushed (without
                                        // it, [conn+0x134]=0 -> send-pump skips ->
                                        // manifest stalls -> "fails early" loop). It
                                        // was disabled long ago for device regressions
                                        // that predate the heap/timeout/CV/native-fetch
                                        // fixes; packages are native-fetched now so the
                                        // backstop only needs to carry the manifest.
                                        // Verified on the Mac device-sim (real timeouts
                                        // + backstop): manifest 3/3 vs 0/3 without it.
static bool s_real_timeouts   = false;  // iOS device: DISABLED. Data says real
                                        // timeouts HURT reliability: they make Steam's
                                        // pool workers time out on their 250ms job
                                        // waits and EXIT prematurely -> "Illegal
                                        // termination of worker thread" -> deadlock,
                                        // instead of blocking until the backstop wakes
                                        // them. With the backstop carrying coordination
                                        // (and packages native-fetched), the legacy
                                        // block-until-signalled path is far more
                                        // reliable. Mac device-sim: manifest 4/5 with
                                        // timeouts OFF vs 2/5 with them ON.
#else
static bool s_backstop_enabled = true;  // macOS harness: needs the crutch
static bool s_real_timeouts   = false;  // macOS harness: legacy no-timeout path + the
                                        // backstop is what actually gets Steam through;
                                        // firing finite timeouts here reshuffles Steam's
                                        // timing and breaks the manifest download
#endif

// ── Real-threads mode (docs/threads_rearchitect.md) ─────────────────────────
// When TRUE, each guest CreateThread/_beginthreadex becomes a REAL pthread with
// its own blink Machine over the shared System, and the Win32 sync handlers
// (WaitForSingleObject/Sleep/events/mutexes/critical sections) use the real
// pthread-backed wg_sync objects instead of the cooperative wg_sched_yield.
// This eliminates the cooperative-scheduler deadlocks (0x207 / "Illegal
// termination"). DEFAULT ON for the iOS device (JIT is fast enough + the
// deadlocks are gone); the macOS harness stays cooperative by default (it's the
// slow interpreter used for A/B debugging). Override either way at runtime:
//   WG_REAL_THREADS=1     force ON     WG_NO_REAL_THREADS=1  force OFF
#if TARGET_OS_IPHONE
static bool s_use_real_threads = true;
#else
static bool s_use_real_threads = false;
#endif

// Global "big lock" serialising Win32 thunk dispatch across guest threads. Guest
// CODE (blink execution) runs concurrently; only handle_blink_thunk (which
// touches the many shared s_* statics + file/socket/heap tables) is serialised.
// Recursive so a handler can re-enter safely. A blocking handler (WFSO/Sleep)
// RELEASES this around the actual block (see wg_thunk_block_*) so other threads'
// SetEvent thunks can run. Only used when s_use_real_threads.
static pthread_mutex_t s_thunk_lock;
static bool            s_thunk_lock_inited = false;

static void wg_thunk_lock_init(void) {
    if (s_thunk_lock_inited) return;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_thunk_lock, &a);
    pthread_mutexattr_destroy(&a);
    s_thunk_lock_inited = true;
}
// Fair (FIFO ticket) recursive GIL — WG_FAIR_GIL=1. The default pthread mutex is
// unfair: a worker polling its work-event (WG_WAITCAP) can repeatedly re-grab the
// GIL ahead of a just-resumed config thread, STARVING it so it never signals the
// driver -> the thread-startup handshake deadlocks. A ticket lock hands the GIL out
// in request order, so every thread (incl. the resumed config thread) runs. Recursion
// is tracked because a handler can re-enter; block points hold it exactly once.
static pthread_mutex_t s_fair_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_fair_c = PTHREAD_COND_INITIALIZER;
static unsigned long   s_fair_next = 0, s_fair_serving = 0;
static pthread_t       s_fair_owner; static int s_fair_owned = 0, s_fair_rec = 0;
static signed char     s_fair_on = -1;
static inline int wg_fair(void) { if (s_fair_on < 0) s_fair_on = getenv("WG_FAIR_GIL") ? 1 : 0; return s_fair_on; }
static void fair_lock(void) {
    pthread_mutex_lock(&s_fair_m);
    if (s_fair_owned && pthread_equal(s_fair_owner, pthread_self())) { s_fair_rec++; pthread_mutex_unlock(&s_fair_m); return; }
    unsigned long my = s_fair_next++;
    while (s_fair_serving != my) pthread_cond_wait(&s_fair_c, &s_fair_m);
    s_fair_owner = pthread_self(); s_fair_owned = 1; s_fair_rec = 1;
    pthread_mutex_unlock(&s_fair_m);
}
static void fair_unlock(void) {
    pthread_mutex_lock(&s_fair_m);
    if (--s_fair_rec <= 0) { s_fair_rec = 0; s_fair_owned = 0; s_fair_serving++; pthread_cond_broadcast(&s_fair_c); }
    pthread_mutex_unlock(&s_fair_m);
}

// The calling pthread's guest thread id (real-threads mode). Main engine thread
// keeps 1; each worker sets its own in wg_worker_thread_entry. Used by
// GetCurrentThreadId, as the caller_tid for wg_sync mutex ownership, and by the
// directed GIL below.
static _Thread_local uint32_t s_cur_guest_tid = 1;

// ---- DIRECTED-HANDOFF GIL (WG_DIRECTED_GIL, default ON) ---------------------
// The real fix for UE4's producer-consumer lost-wakeup chain. A plain/fair GIL
// releases to whichever thread the OS schedules next -> nondeterministic, so a
// handshake occasionally drops a wake and the whole task-graph parks. Instead,
// when a thread BLOCKS on a wait (WFSO on event E), we hand the GIL to the thread
// that will SIGNAL E -- the "producer", learned from SetEvent history. That makes
// the handshake deterministic and Windows-ordered: the producer runs next, sets
// the event, and the waiter wakes. Recursion tracked (handlers re-enter; block
// points hold it exactly once). Fallback: if the preferred thread doesn't take the
// GIL (it's itself blocked), waiters drop the preference after a few ms so no
// deadlock. WG_NO_DIRECTED_GIL disables.
static pthread_mutex_t s_dir_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_dir_c = PTHREAD_COND_INITIALIZER;
static pthread_t s_dir_owner; static int s_dir_owned = 0, s_dir_rec = 0;
static volatile uint32_t s_dir_owner_tid = 0;   // guest tid currently holding the GIL
static uint32_t s_dir_prefer = 0;     // guest tid to hand the GIL to next (0 = free)
static int      s_dir_stall  = 0;     // consecutive acquire-timeouts blocked by prefer
static signed char s_dir_on = -1;
// Which guest tids are currently BLOCKED in a wait (between block_begin/end).
// Each thread writes only its own slot (no lock); reads are a racy hint. Used so
// we only hand the GIL to a producer that can actually run (not one parked itself).
#define WG_MAX_TID 0x1100
static volatile uint8_t s_tid_blocked[WG_MAX_TID];
// Count of threads currently CONTENDING for the GIL — i.e. blocked in dir_lock's
// acquire loop wanting to run guest code (NOT parked on a guest event, which
// releases the GIL and does not count). This is the signal that separates the two
// adaptive-slice cases the RIP heuristic alone cannot: a RIP-pinned main with a
// contender (>0) is a real busy-wait where the awaited worker is starved -> shrink
// so it can run; a RIP-pinned main with NO contender (==0) is a tight construction/
// registration loop whose workers are blocked on events waiting for it to finish
// and SetEvent -> keep BIG slices so it races to that signal. Read racy (a hint).
static volatile int s_gil_waiters = 0;
// WG_BLOCK_WORKERS: on real HW, UObject registration/loading during boot is
// single-threaded; WineGlass only corrupts it because it runs the UE4 task-graph
// pool workers (all start at 0x9FFC70) IN PARALLEL with the main, so a worker reads
// a half-built, early-published object (the construction race -> O(N^2) name scan).
// This gate PARKS those pool workers while the main is making forward progress
// (its RIP advances), so the boot runs effectively single-threaded (no race). It is
// deadlock-safe: if the main's RIP goes PINNED (it is genuinely waiting for a worker
// to produce something), s_main_rip_stall climbs past the threshold and the workers
// are released to run. Pair with -noasyncloadingthread so the main does its own
// loading and rarely needs a worker during boot. Main-only publisher (main tick).
static volatile uint64_t s_main_rip_pub = 0;    // the main guest thread's last RIP
static volatile unsigned s_main_rip_stall = 0;  // consecutive main slices at the same RIP
// Monotonic heartbeat bumped once per MAIN tick slice. The deadlock watchdog watches
// THIS (not the global thunk count) so a worker spinning (SetEvent livelock) can't
// mask a stalled main thread — the main being frozen IS the deadlock we must break.
static volatile unsigned long long s_main_tick_pub = 0;
static volatile int s_main_blocked = 0;         // main is in a genuine blocking wait (release gated workers)
// WG_CTOR_HOOK: inline entry hook on the UObject base constructor (default 0x9E36E0,
// called by ALL ~4036 register sites). On each call it GIL-pins the constructing
// thread for a short window so the publish-before-construct sequence (alloc → publish
// → construct → link) runs ATOMICALLY w.r.t. every other guest thread — the true fix
// for the multi-threaded level-load construction race (works for main AND workers,
// unlike WG_BLOCK_WORKERS). Uses the proven WG_TRACE restore→single-step→re-arm path.
static bool     s_ctor_armed = false;
static uint8_t  s_ctor_orig  = 0;
static uint64_t s_ctor_addr  = 0x9E36E0;
// WG_LOOPPROBE: inline hook on the construction-driver's hash-chain walk body
// (default 0xB72547 = `mov eax,esi`, 89 F0). Counts chain nodes visited so we can
// tell a NON-growing/degenerate table (huge nodes-per-construction => O(N) walks =>
// grow-the-table fix) from pure emulation speed (few nodes => JIT is the answer).
// Multiple guest sites walk the SAME UObject +0x28 intrusive "next" chain with the
// identical instruction `mov rbx,[rbx+0x28]` (48 8B 5B 28): the ProcessNewlyLoaded
// walk at 0xA5AC68 and the FUObjectHashTables bucket iterator inside 0x6fb950 at
// 0x6FB9A4. A single self-referential node (next==self, terminator never linked)
// hangs ALL of them. Arm each; on a genuinely stuck self-loop, PATCH the node's
// memory ([node+0x28]=0) so every other walk site is fixed too — not just this one.
// {addr, reg, off}: a `mov <reg>,[<reg>+off]` intrusive-list advance. reg is a blink
// register index (rbx=3, rax=0). A node whose [reg+off]==self hangs the walk forever.
static struct { uint64_t addr; uint8_t reg; uint8_t off; uint8_t orig; bool armed; } s_loops[] = {
    { 0xA5AC68, 3, 0x28, 0, false },   // ProcessNewlyLoadedUObjects: mov rbx,[rbx+0x28]
    { 0x6FB9A4, 3, 0x28, 0, false },   // FUObjectHashTables iterator: mov rbx,[rbx+0x28]
    { 0xB7BD93, 0, 0x20, 0, false },   // outer-chain walk (error path): mov rax,[rax+0x20]
};
#define WG_NLOOPS ((int)(sizeof(s_loops)/sizeof(s_loops[0])))
static bool     s_loop_armed = false;
static uint8_t  s_loop_orig  = 0;
static uint64_t s_loop_addr  = 0xA5AC68;
// WG_ANIMFIX: the boot fatals ("Couldn't find default curve compression settings under
// '[Animation.DefaultObjectSettings]'") because that Engine default lives in the pak's
// BaseEngine.ini, which our config layer doesn't deliver to GConfig — so the config
// value getter returns an empty path and LoadObject fails. Hook the exact getter call
// (0x18822A3: `call 0x2f8190` with rdx = output FString) and populate the FString with
// the correct object path, skipping the getter, so LoadObject finds the (present) asset.
static bool     s_anim_armed = false;
static uint8_t  s_anim_orig  = 0;
static uint64_t s_anim_addr  = 0x18822A3;   // the value getter (call 0x2f8190)
static uint8_t  s_anim_orig2 = 0;
static uint64_t s_anim_addr2 = 0x1882274;   // the section lookup (call 0x58d410) — force non-null
// Set once the boot is PAST early UObject registration (the corruption window) — the
// game shows its main window only after engine PreInit/registration. From then on the
// WG_BLOCK_WORKERS gate stops parking pool workers so the post-init task-graph
// coordination (viewport/swapchain/first frame) runs with full worker parallelism.
static volatile int s_past_init = 0;
static inline int wg_directed(void) {
    if (s_dir_on < 0) s_dir_on = getenv("WG_NO_DIRECTED_GIL") ? 0 : 1;
    return s_dir_on;
}
static void dir_lock(void) {
    pthread_mutex_lock(&s_dir_m);
    if (s_dir_owned && pthread_equal(s_dir_owner, pthread_self())) { s_dir_rec++; pthread_mutex_unlock(&s_dir_m); return; }
    uint32_t me = s_cur_guest_tid;
    int counted = 0;
    for (;;) {
        if (!s_dir_owned && (s_dir_prefer == 0 || s_dir_prefer == me)) break;
        if (!counted) { s_gil_waiters++; counted = 1; }   // contending for the GIL
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000 * 1000;   // 1ms
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = pthread_cond_timedwait(&s_dir_c, &s_dir_m, &ts);
        if (rc == ETIMEDOUT && !s_dir_owned && s_dir_prefer != 0 && s_dir_prefer != me) {
            // Preferred thread isn't taking the GIL (blocked itself) -> don't
            // deadlock: drop the preference after one timeout so we can proceed.
            s_dir_prefer = 0; s_dir_stall = 0;
        }
    }
    if (counted) s_gil_waiters--;
    s_dir_owner = pthread_self(); s_dir_owned = 1; s_dir_rec = 1;
    s_dir_owner_tid = me;   // who holds the GIL (for the deadlock dump)
    if (s_dir_prefer == me) { s_dir_prefer = 0; s_dir_stall = 0; }   // consumed
    pthread_mutex_unlock(&s_dir_m);
}
// GIL-PIN ACROSS A GUEST CRITICAL SECTION / EXCLUSIVE LOCK (WG_CS_PIN, default ON
// under real threads). The render-blocking corruption is a lock-free READER (the
// UObject list walk at guest 0xA5AC30) observing a node mid-CONSTRUCTION: the
// constructing thread holds its lock, then hits a THUNK (malloc, etc.) — a GIL
// handoff point — and another guest thread grabs the GIL and walks the half-linked
// list (a node whose next still points to itself). The guest's own critical section
// is honored (real mutex), but the WALK doesn't take it, so mutual exclusion at the
// guest-lock level can't save it. Fix: while a thread holds a guest exclusive lock
// (EnterCriticalSection / AcquireSRWLockExclusive), do NOT release the GIL between
// slices — the whole locked construction runs atomically w.r.t. every other guest
// thread, so no reader can ever see it half-built. `s_blocking` overrides the pin so
// a genuine blocking wait (WFSO/CV sleep) inside the lock still releases the GIL
// (else deadlock). Both thread-local (each thread pins only its own GIL holds).
static _Thread_local int s_cs_held = 0;    // depth of guest exclusive locks held
static _Thread_local int s_blocking = 0;   // in a blocking wait -> force GIL release
static _Thread_local int s_pin_slices = 0; // consecutive pinned GIL releases (safety cap)
static _Thread_local int s_spin_pin = 0;   // slices left to pin after a guest spinlock acquire
static signed char s_cs_pin = -1;
static signed char s_spinpin_on = -1;
static long s_pin_cap = -1;
static long s_spin_window = -1;
static inline int wg_cs_pin_on(void) {
    // OPT-IN (WG_CS_PIN=1): honoring intercepted guest locks by pinning the GIL is
    // correct hardening, but it does NOT fix the Visage UObject corruption (that
    // construction is guarded by a GUEST-SIDE spinlock we don't intercept, not a
    // CS/SRW/mutex), and it can let a worker pin the GIL during the garbage-name
    // scan. Default OFF so the codebase isn't regressed; enable for CS-protected races.
    if (s_cs_pin < 0) s_cs_pin = getenv("WG_CS_PIN") ? 1 : 0;
    return s_cs_pin;
}
static inline int wg_spinpin_on(void) {
    if (s_spinpin_on < 0) s_spinpin_on = getenv("WG_SPINPIN") ? 1 : 0;
    return s_spinpin_on;
}
// GUEST-SPINLOCK GIL-PIN (WG_SPINPIN=1). The render-blocking UObject corruption is a
// lock-free READER (the 0xA5AC30 walk) seeing a node mid-construction while the
// constructing thread holds a GUEST-SIDE SPINLOCK (a `lock cmpxchg` acquire, no
// thunk to pin on). blink's cmpxchg calls wg_on_guest_spinlock when a LOCK CMPXCHG
// swaps 0 -> non-zero (a lock ACQUIRE); we then pin the GIL for the next
// WG_SPIN_WINDOW slices so the (short) locked construction runs atomically vs other
// guest threads. The window auto-expires (no release detection needed) and is
// refreshed on each acquire, so nested/re-acquired locks stay covered.
extern void (*wg_spinlock_acquire_hook)(unsigned long long addr, unsigned long long rip);
// Histogram of guest-spinlock ACQUIRE sites (RIP) — to see if construction locks and
// coordination locks are acquired at DISTINCT instruction addresses (then a RIP-scoped
// pin can target only the construction lock). WG_SPINLOG dumps it periodically.
#define WG_SPINRIP_MAX 64
static struct { uint64_t rip; uint64_t count; } s_spinrip[WG_SPINRIP_MAX];
static int s_spinrip_n = 0;
static pthread_mutex_t s_spinrip_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_spinlo = 0, s_spinhi = 0; static signed char s_spinscope = -1;
static void wg_on_guest_spinlock(unsigned long long addr, unsigned long long rip) {
    (void)addr;
    if (!s_use_real_threads) return;
    if (getenv("WG_SPINLOG")) {
        pthread_mutex_lock(&s_spinrip_lock);
        int i; for (i = 0; i < s_spinrip_n; i++) if (s_spinrip[i].rip == rip) { s_spinrip[i].count++; break; }
        if (i == s_spinrip_n && s_spinrip_n < WG_SPINRIP_MAX) { s_spinrip[s_spinrip_n].rip = rip; s_spinrip[s_spinrip_n].count = 1; s_spinrip_n++; }
        pthread_mutex_unlock(&s_spinrip_lock);
    }
    if (!wg_spinpin_on()) return;   // logging only, no pin
    // RIP-SCOPE: if WG_SPINLO/HI are set, pin ONLY for acquires whose site is in that
    // instruction range (the construction lock) — not coordination locks elsewhere.
    if (s_spinscope < 0) { const char *lo = getenv("WG_SPINLO"), *hi = getenv("WG_SPINHI");
        if (lo && hi) { s_spinlo = strtoull(lo, 0, 16); s_spinhi = strtoull(hi, 0, 16); s_spinscope = 1; } else s_spinscope = 0; }
    if (s_spinscope && !(rip >= s_spinlo && rip <= s_spinhi)) return;   // out of construction scope
    if (s_spin_window < 0) { const char *e = getenv("WG_SPIN_WINDOW"); s_spin_window = e ? atol(e) : 24; }
    s_spin_pin = (int)s_spin_window;   // pin the GIL for the next N slices
}
// Dump the spinlock-acquire-site histogram (top sites by count). Called from the tick.
static void wg_dump_spinrips(void) {
    if (!getenv("WG_SPINLOG")) return;
    pthread_mutex_lock(&s_spinrip_lock);
    // simple selection of the top ~12 by count
    for (int k = 0; k < 12 && k < s_spinrip_n; k++) {
        int best = -1; uint64_t bc = 0;
        for (int i = 0; i < s_spinrip_n; i++) if (s_spinrip[i].count > bc) { bc = s_spinrip[i].count; best = i; }
        if (best < 0) break;
        uint64_t rt = 0x400000 + (s_spinrip[best].rip - 0x140000000);
        WG_LOGW("Engine", "SPINRIP site=0x%llX (runtime~0x%llX) acquires=%llu",
                (unsigned long long)s_spinrip[best].rip, (unsigned long long)rt, (unsigned long long)bc);
        s_spinrip[best].count = 0;   // consume so next iteration finds the next
    }
    s_spinrip_n = 0;
    pthread_mutex_unlock(&s_spinrip_lock);
}
static void dir_unlock(void) {
    pthread_mutex_lock(&s_dir_m);
    if (--s_dir_rec <= 0) {
        s_dir_rec = 0;
        // SAFETY CAP: a thread that holds a lock while BUSY-WAITING (not a blocking
        // wait, so s_blocking is clear) would pin the GIL forever and deadlock the
        // awaited thread. A real construction releases its lock within a few slices,
        // so cap the number of consecutive pinned releases; past it, release anyway
        // (accepting a rare interleave over a hard hang). WG_PIN_CAP tunes it.
        if (s_pin_cap < 0) { const char *e = getenv("WG_PIN_CAP"); s_pin_cap = e ? atol(e) : 64; }
        int want_pin = (wg_cs_pin_on() && s_cs_held > 0) || s_spin_pin > 0;
        if (want_pin && !s_blocking && s_pin_slices < s_pin_cap) {
            // Pin: keep the GIL owned across the locked region (do NOT release).
            // The next wg_thunk_lock re-enters via the owner==self fast path (++rec).
            s_pin_slices++;
            if (s_spin_pin > 0) s_spin_pin--;   // spinlock window counts down
        } else {
            s_pin_slices = 0; s_spin_pin = 0;
            s_dir_owned = 0; pthread_cond_broadcast(&s_dir_c);
        }
    }
    pthread_mutex_unlock(&s_dir_m);
}
// Hand the GIL to `tid` next (called right before a thread blocks on a wait whose
// signaler is `tid`). No-op if tid is unknown/self.
static void wg_dir_set_prefer(uint32_t tid) {
    if (!s_use_real_threads || !wg_directed() || tid == 0 || tid == s_cur_guest_tid) return;
    // Only hand off to a producer that can actually run — if it's parked in its
    // own wait, preferring it just stalls everyone until the fallback fires.
    if (tid < WG_MAX_TID && s_tid_blocked[tid]) return;
    pthread_mutex_lock(&s_dir_m);
    s_dir_prefer = tid; s_dir_stall = 0;
    pthread_cond_broadcast(&s_dir_c);
    pthread_mutex_unlock(&s_dir_m);
}
// wait-handle -> the tid that last SetEvent'd it (the producer). Accessed only
// from thunk handlers, which run under the GIL, so no extra lock is needed.
#define WG_MAX_PROD 16384
static struct { uint32_t h; uint32_t tid; } s_prod[WG_MAX_PROD];
static int s_prod_n = 0;
static void wg_producer_set(uint32_t h, uint32_t tid) {
    if (!h || !tid) return;
    for (int i = 0; i < s_prod_n; i++) if (s_prod[i].h == h) { s_prod[i].tid = tid; return; }
    if (s_prod_n < WG_MAX_PROD) { s_prod[s_prod_n].h = h; s_prod[s_prod_n].tid = tid; s_prod_n++; }
}
static uint32_t wg_producer_get(uint32_t h) {
    for (int i = 0; i < s_prod_n; i++) if (s_prod[i].h == h) return s_prod[i].tid;
    return 0;
}

static inline void wg_thunk_lock(void)   { if (!s_use_real_threads) return; if (wg_directed()) dir_lock();   else if (wg_fair()) fair_lock();   else pthread_mutex_lock(&s_thunk_lock); }
static inline void wg_thunk_unlock(void) { if (!s_use_real_threads) return; if (wg_directed()) dir_unlock(); else if (wg_fair()) fair_unlock(); else pthread_mutex_unlock(&s_thunk_lock); }
// Release the thunk lock around a blocking wait, then re-acquire (a new ticket).
// In directed mode, mark this thread BLOCKED across the wait so the handoff logic
// won't prefer it (it can't take the GIL until its wait returns).
static inline void wg_thunk_block_begin(void) {
    if (!s_use_real_threads) return;
    // A genuine blocking wait must RELEASE the GIL even if this thread holds a guest
    // critical section (a CS-pin here would deadlock — no other thread could run to
    // signal the wait). s_blocking overrides the pin in dir_unlock.
    s_blocking++;
    // WG_BLOCK_WORKERS valve: release gated pool workers ONLY while the MAIN is in a
    // genuine blocking wait (it may be waiting on a worker). This is the RIGHT signal
    // — unlike RIP-stall it does NOT misfire during the main's compute-bound
    // registration loop or the O(N^2) scan (both RIP-pinned but NOT blocked), so
    // workers stay parked through construction and can't race it. Cleared on unblock.
    if (s_cur_guest_tid == 1) s_main_blocked = 1;
    if (wg_directed()) { uint32_t t = s_cur_guest_tid; if (t < WG_MAX_TID) s_tid_blocked[t] = 1; dir_unlock(); }
    else if (wg_fair()) fair_unlock(); else pthread_mutex_unlock(&s_thunk_lock);
}
static inline void wg_thunk_block_end(void) {
    if (!s_use_real_threads) return;
    if (s_cur_guest_tid == 1) s_main_blocked = 0;   // main resumed -> re-gate pool workers
    if (wg_directed()) { dir_lock(); uint32_t t = s_cur_guest_tid; if (t < WG_MAX_TID) s_tid_blocked[t] = 0; }
    else if (wg_fair()) fair_lock(); else pthread_mutex_lock(&s_thunk_lock);
    if (s_blocking > 0) s_blocking--;
}

// CREATE_SUSPENDED gates for real threads. UE4's thread pool creates workers
// SUSPENDED, fills in their per-thread context, then ResumeThread()s them. If we
// run the pthread immediately it reads an uninitialized context and jumps to
// garbage (the ~21-thread startup crash). A suspended worker parks on its gate
// (keyed by thread handle) until ResumeThread signals it.
#define WG_MAX_RESUME_GATES 64
static struct { uint32_t handle; pthread_mutex_t m; pthread_cond_t c; bool resumed; bool used; }
    s_resume_gates[WG_MAX_RESUME_GATES];
static pthread_mutex_t s_resume_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void wg_resume_gate_create(uint32_t handle) {
    pthread_mutex_lock(&s_resume_table_lock);
    for (int i = 0; i < WG_MAX_RESUME_GATES; i++)
        if (!s_resume_gates[i].used) {
            s_resume_gates[i].used = true; s_resume_gates[i].handle = handle;
            s_resume_gates[i].resumed = false;
            pthread_mutex_init(&s_resume_gates[i].m, NULL);
            pthread_cond_init(&s_resume_gates[i].c, NULL);
            break;
        }
    pthread_mutex_unlock(&s_resume_table_lock);
}
static void wg_resume_gate_wait(uint32_t handle) {
    int idx = -1;
    pthread_mutex_lock(&s_resume_table_lock);
    for (int i = 0; i < WG_MAX_RESUME_GATES; i++)
        if (s_resume_gates[i].used && s_resume_gates[i].handle == handle) { idx = i; break; }
    pthread_mutex_unlock(&s_resume_table_lock);
    if (idx < 0) return;
    pthread_mutex_lock(&s_resume_gates[idx].m);
    while (!s_resume_gates[idx].resumed)
        pthread_cond_wait(&s_resume_gates[idx].c, &s_resume_gates[idx].m);
    pthread_mutex_unlock(&s_resume_gates[idx].m);
}
static bool wg_resume_gate_signal(uint32_t handle) {
    int idx = -1;
    pthread_mutex_lock(&s_resume_table_lock);
    for (int i = 0; i < WG_MAX_RESUME_GATES; i++)
        if (s_resume_gates[i].used && s_resume_gates[i].handle == handle) { idx = i; break; }
    pthread_mutex_unlock(&s_resume_table_lock);
    if (idx < 0) return false;
    pthread_mutex_lock(&s_resume_gates[idx].m);
    s_resume_gates[idx].resumed = true;
    pthread_cond_signal(&s_resume_gates[idx].c);
    pthread_mutex_unlock(&s_resume_gates[idx].m);
    return true;
}

// Per-real-thread slot index into the s_tls_slots/s_fls_slots shadow arrays.
// The cooperative scheduler used scheduler->current as the index; under real
// threads that's meaningless (all threads would share one slot -> corrupt
// per-thread CRT state, e.g. _tiddata / errno). So each real thread gets its own
// slot from a small pool. Main = 0; workers alloc/free in wg_spawn_real_thread /
// wg_worker_thread_entry. GENERAL fix (any multithreaded guest), not app-specific.
static _Thread_local int s_tls_slot = 0;
static pthread_mutex_t   s_tls_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static bool              s_tls_slot_used[WG_MAX_THREADS]; // [0] reserved for main
static int wg_alloc_tls_slot(void) {
    pthread_mutex_lock(&s_tls_slot_lock);
    int s = 0; // 0 fallback (main's slot) if the pool is exhausted — safe, not ideal
    for (int i = 1; i < WG_MAX_THREADS; i++)
        if (!s_tls_slot_used[i]) { s_tls_slot_used[i] = true; s = i; break; }
    pthread_mutex_unlock(&s_tls_slot_lock);
    return s;
}
static void wg_free_tls_slot(int s) {
    if (s <= 0) return;
    pthread_mutex_lock(&s_tls_slot_lock);
    s_tls_slot_used[s] = false;
    pthread_mutex_unlock(&s_tls_slot_lock);
}
// Current shadow-array index: real per-thread slot in real-threads mode, else the
// cooperative scheduler's current thread. `coop` is scheduler->current (or -1).
static inline int wg_tls_index(int coop) {
    if (s_use_real_threads) return s_tls_slot;
    return coop >= 0 ? coop : 0;
}

// Spawn a real pthread for a guest CreateThread/_beginthreadex (real-threads
// mode). Defined after wg_alloc_thread_teb; forward-declared for the CreateThread
// handler inside handle_blink_thunk.
static uint32_t wg_spawn_real_thread(WGEngine *engine, uint32_t start,
                                     uint32_t param, uint32_t flags,
                                     uint32_t *out_tid);

// Critical-section support for real threads: map each guest CRITICAL_SECTION
// pointer to a recursive wg_sync mutex (lazily created). Steam wraps its heap
// allocator + many structures in critical sections, so real mutual exclusion is
// required once guest threads run concurrently. Accessed only from thunk
// handlers (under s_thunk_lock), so the map itself needs no extra lock.
#define WG_MAX_CS 16384
#define WG_CS_HASH (WG_MAX_CS * 2)   // power of 2; open-addressing load factor 0.5
static struct { uint32_t cs_ptr; uint32_t mtx; } s_cs_map[WG_MAX_CS];
static int s_cs_count = 0;
// Hash index cs_ptr -> (s_cs_map index + 1); 0 = empty. Turns the per-lock-op
// lookup from O(N) linear scan into O(1) — critical once the game does millions
// of SRW-lock/critical-section ops during shader processing (was O(N^2) overall,
// slowing the tick rate to a crawl).
static int s_cs_hash[WG_CS_HASH];
static uint32_t wg_cs_mutex_for(uint32_t cs_ptr) {
    uint32_t h = (cs_ptr * 2654435761u) & (WG_CS_HASH - 1);
    while (s_cs_hash[h]) {
        int idx = s_cs_hash[h] - 1;
        if (s_cs_map[idx].cs_ptr == cs_ptr) return s_cs_map[idx].mtx;
        h = (h + 1) & (WG_CS_HASH - 1);
    }
    if (s_cs_count < WG_MAX_CS) {
        uint32_t m = wg_sync_create_mutex(false, 0);
        s_cs_map[s_cs_count].cs_ptr = cs_ptr;
        s_cs_map[s_cs_count].mtx = m;
        s_cs_hash[h] = s_cs_count + 1;
        s_cs_count++;
        return m;
    }
    return 0;
}
// Same idea for guest CONDITION_VARIABLE pointers -> a wg_sync CV handle.
static struct { uint32_t cv_ptr; uint32_t cvh; } s_cv_map[WG_MAX_CS];
static int s_cv_count = 0;
static uint32_t wg_cv_handle_for(uint32_t cv_ptr) {
    for (int i = 0; i < s_cv_count; i++) if (s_cv_map[i].cv_ptr == cv_ptr) return s_cv_map[i].cvh;
    if (s_cv_count < WG_MAX_CS) {
        uint32_t h = wg_sync_create_cv();
        s_cv_map[s_cv_count].cv_ptr = cv_ptr;
        s_cv_map[s_cv_count].cvh = h;
        s_cv_count++;
        return h;
    }
    return 0;
}

// -- SRW locks: PROPER reader/writer semantics ------------------------------
// SRW locks were backed by wg_cs_mutex_for(), i.e. an EXCLUSIVE mutex, so
// AcquireSRWLockShared serialized readers that Windows runs concurrently. UE4's
// FRWLock read paths take a shared lock and then block waiting on another thread
// that ALSO needs the same lock shared -> on Windows both proceed; here the 2nd
// reader blocked on the 1st's exclusive mutex -> deadlock (two threads stuck in
// the config driver 0x14AA). This is a real reader/writer lock keyed by the
// guest SRW pointer: many shared holders OR one exclusive holder.
#define WG_MAX_SRW 32768
typedef struct { uint32_t ptr; int readers; uint32_t writer_tid; int writer_rec; } WGSrw;
static WGSrw s_srw[WG_MAX_SRW];
static int s_srw_count = 0;
static int s_srw_hash[WG_MAX_SRW * 2];   // ptr -> (index+1); 0 = empty
static pthread_mutex_t s_srw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_srw_cond = PTHREAD_COND_INITIALIZER;
static int s_srw_log = -1;
// Shared fallback entry for when the SRW table is FULL. Previously wg_srw_for
// returned NULL on overflow and BOTH acquire paths treated NULL as "uncontended,
// proceed WITHOUT locking" -> a full table silently disabled mutual exclusion for
// every new lock, so a UObject construction (writer) and a list walk (reader) ran
// concurrently and corrupted the registration list (the render-blocking bug). A big
// UE4 title easily exceeds a few-thousand distinct FRWLocks. Fix: never return NULL
// — hand back one shared entry so overflow locks are OVER-serialized (all treated as
// one lock: correct, never corrupt; a little extra contention is harmless vs the bug).
static WGSrw s_srw_overflow;
// Caller MUST hold s_srw_lock.
static WGSrw *wg_srw_for(uint32_t p) {
    uint32_t h = (p * 2654435761u) & (WG_MAX_SRW * 2 - 1);
    while (s_srw_hash[h]) {
        int idx = s_srw_hash[h] - 1;
        if (s_srw[idx].ptr == p) return &s_srw[idx];
        h = (h + 1) & (WG_MAX_SRW * 2 - 1);
    }
    if (s_srw_count < WG_MAX_SRW) {
        WGSrw *s = &s_srw[s_srw_count];
        s->ptr = p; s->readers = 0; s->writer_tid = 0; s->writer_rec = 0;
        s_srw_hash[h] = s_srw_count + 1; s_srw_count++;
        return s;
    }
    static int warned = 0;
    if (!warned) { warned = 1;
        fprintf(stderr, "[SRW-OVERFLOW] table full at %d locks — falling back to shared serializing entry (was a no-op corruption bug)\n", WG_MAX_SRW); }
    return &s_srw_overflow;   // over-serialize the overflow set; NEVER no-op
}
static void wg_srw_acquire_shared(uint32_t p, uint32_t tid) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    if (!s || s->writer_tid == 0 || s->writer_tid == tid) {   // uncontended
        if (s) s->readers++;
        pthread_mutex_unlock(&s_srw_lock);
        return;
    }
    pthread_mutex_unlock(&s_srw_lock);
    if (s_srw_log > 0) WG_LOGI("SRW", "shared acquire CONTENDED ptr=%08x tid=%u", p, tid);
    wg_thunk_block_begin();                                    // release GIL while blocked
    pthread_mutex_lock(&s_srw_lock);
    s = wg_srw_for(p);
    while (s && s->writer_tid != 0 && s->writer_tid != tid)
        pthread_cond_wait(&s_srw_cond, &s_srw_lock);
    if (s) s->readers++;
    pthread_mutex_unlock(&s_srw_lock);
    wg_thunk_block_end();
}
static void wg_srw_release_shared(uint32_t p) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    if (s && s->readers > 0) s->readers--;
    pthread_cond_broadcast(&s_srw_cond);
    pthread_mutex_unlock(&s_srw_lock);
}
static void wg_srw_acquire_exclusive(uint32_t p, uint32_t tid) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    if (s && s->writer_tid == tid) { s->writer_rec++; pthread_mutex_unlock(&s_srw_lock); return; }
    if (!s || (s->readers == 0 && s->writer_tid == 0)) {       // uncontended
        if (s) { s->writer_tid = tid; s->writer_rec = 1; }
        pthread_mutex_unlock(&s_srw_lock);
        return;
    }
    pthread_mutex_unlock(&s_srw_lock);
    if (s_srw_log > 0) WG_LOGI("SRW", "excl acquire CONTENDED ptr=%08x tid=%u r=%d w=%u", p, tid, s->readers, s->writer_tid);
    wg_thunk_block_begin();
    pthread_mutex_lock(&s_srw_lock);
    s = wg_srw_for(p);
    int waited_ms = 0;
    while (s && (s->readers > 0 || (s->writer_tid != 0 && s->writer_tid != tid))) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&s_srw_cond, &s_srw_lock, &ts) == ETIMEDOUT) {
            waited_ms += 200;
            if (waited_ms == 2000)   // stuck -> almost certainly a leaked reader/writer
                fprintf(stderr, "[SRW-STUCK] tid=%u EXCL on ptr=%08x 2s: readers=%d writer=%u\n",
                        tid, p, s->readers, s->writer_tid);
        }
    }
    if (s) { s->writer_tid = tid; s->writer_rec = 1; }
    pthread_mutex_unlock(&s_srw_lock);
    wg_thunk_block_end();
}
static void wg_srw_release_exclusive(uint32_t p, uint32_t tid) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    if (s && s->writer_tid == tid && --s->writer_rec <= 0) { s->writer_tid = 0; s->writer_rec = 0; }
    pthread_cond_broadcast(&s_srw_cond);
    pthread_mutex_unlock(&s_srw_lock);
}
// Deadlock-breaker (WG_DEADLOCK_KICKSRW): at a hard stall the FName-pool SRW shards can
// be stuck with a LEAKED reader/writer (e.g. a thread longjmp'd out of a locked region
// on Abort-recovery, skipping the release), so an exclusive acquirer (the main) waits
// on readers>0 forever. When the watchdog sees a genuine all-idle deadlock, force-clear
// held SRW locks and broadcast so the blocked acquirer proceeds. Returns #locks reset.
static int wg_srw_kick(void) {
    static signed char on = -1;
    if (on < 0) on = getenv("WG_DEADLOCK_KICKSRW") ? 1 : 0;
    if (!on) return 0;
    pthread_mutex_lock(&s_srw_lock);
    int n = 0;
    for (int i = 0; i < s_srw_count; i++)
        if (s_srw[i].readers > 0 || s_srw[i].writer_tid != 0) {
            s_srw[i].readers = 0; s_srw[i].writer_tid = 0; s_srw[i].writer_rec = 0; n++;
        }
    pthread_cond_broadcast(&s_srw_cond);
    pthread_mutex_unlock(&s_srw_lock);
    return n;
}
static int wg_srw_try_shared(uint32_t p, uint32_t tid) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    int ok = (!s || s->writer_tid == 0 || s->writer_tid == tid);
    if (ok && s) s->readers++;
    pthread_mutex_unlock(&s_srw_lock);
    return ok ? 1 : 0;
}
static int wg_srw_try_exclusive(uint32_t p, uint32_t tid) {
    pthread_mutex_lock(&s_srw_lock);
    WGSrw *s = wg_srw_for(p);
    int ok = (!s || (s->readers == 0 && (s->writer_tid == 0 || s->writer_tid == tid)));
    if (ok && s) { s->writer_tid = tid; s->writer_rec++; }
    pthread_mutex_unlock(&s_srw_lock);
    return ok ? 1 : 0;
}

// Recursively delete a directory and its contents (used to give NSIS a fresh
// plugins temp dir when a stale one survives from a prior run).
static void wg_rmtree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char child[1024];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) wg_rmtree(child);
            else unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

#define TAG "Engine"

// ===== Real in-VM DLL / NSIS-plug-in loading ==============================
// NSIS calls a plug-in as GetModuleHandle(dll) -> (NULL) LoadLibrary ->
// GetProcAddress(export) -> call. A faked module handle can't satisfy that, so
// we actually map the DLL into the blink VM (sections, base relocations,
// imports wired to our thunks) and parse its export table. This is also the
// foundation for eventually running steam.exe's own DLLs.
typedef struct {
    bool       in_use;
    char       name[64];   // lowercased base filename, e.g. "nsprocess.dll"
    uint32_t   base;       // load address in guest space
    uint32_t   size;       // image size
    WGPEImage *img;        // kept for export-table lookups
} WGLoadedModule;
static WGLoadedModule s_modules[16];
static uint32_t s_dll_next_base = 0x60000000u;  // bump allocator for DLL bases

// Lowercased base filename from a Windows- or unix-style path.
static void dll_basename(const char *path, char *out, int outsz) {
    const char *p = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') p = q + 1;
    int i = 0;
    for (; p[i] && i < outsz - 1; i++) out[i] = (char)tolower((unsigned char)p[i]);
    out[i] = '\0';
}

// Resolve an RVA to a pointer into the parsed file image (raw_data + sections).
static const uint8_t *pe_rva_ptr(WGPEImage *img, uint32_t rva, uint32_t need) {
    if (rva + need <= img->size_of_headers && rva + need <= img->raw_size)
        return img->raw_data + rva;
    for (int i = 0; i < img->num_sections; i++) {
        WGPESection *s = &img->sections[i];
        uint32_t vs = s->virtual_size ? s->virtual_size : s->raw_size;
        if (rva >= s->virtual_address && rva + need <= s->virtual_address + vs) {
            uint32_t off = rva - s->virtual_address;
            if (s->data && off + need <= s->raw_size) return s->data + off;
            return NULL;  // lives in uninitialized (bss) space
        }
    }
    return NULL;
}

// Load a DLL file into the VM. Returns the guest load base, or 0 on failure.
// Takes blink + mapper explicitly (rather than WGEngine, whose struct is
// defined further down) so it can live up here next to its helpers.
static uint32_t wg_load_dll(WGBlinkInstance *blink, WGDllMapper *mapper,
                            const char *real_path, const char *guest_name) {
    char base_name[64];
    dll_basename(guest_name, base_name, sizeof base_name);
    for (int i = 0; i < 16; i++)
        if (s_modules[i].in_use && strcmp(s_modules[i].name, base_name) == 0)
            return s_modules[i].base;   // same plug-in already mapped

    WGPEImage *img = wg_pe_load_file(real_path);
    if (!img) return 0;
    if (img->is_64bit) { wg_pe_image_free(img); return 0; }

    uint32_t preferred = (uint32_t)img->image_base;
    uint32_t img_size  = img->size_of_image ? img->size_of_image : 0x100000;
    uint32_t load_base;
    int32_t  delta;
    if (img->reloc_rva && img->reloc_size) {
        load_base = s_dll_next_base;
        s_dll_next_base = (s_dll_next_base + img_size + 0xFFFF) & ~0xFFFFu;
        delta = (int32_t)(load_base - preferred);
    } else {
        // No relocations — must load at the preferred base.
        load_base = preferred;
        delta = 0;
    }

    // Map headers, then each section (zero-fill then copy raw bytes).
    if (img->size_of_headers && img->raw_size >= img->size_of_headers)
        wg_blink_write_mem(blink, load_base, img->raw_data, img->size_of_headers);
    for (int i = 0; i < img->num_sections; i++) {
        WGPESection *s = &img->sections[i];
        if (!s->virtual_size) continue;
        uint64_t va = load_base + s->virtual_address;
        uint8_t *zeros = calloc(1, s->virtual_size);
        if (zeros) { wg_blink_load_code(blink, va, zeros, s->virtual_size, 0); free(zeros); }
        if (s->data && s->raw_size) {
            uint32_t n = s->raw_size < s->virtual_size ? s->raw_size : s->virtual_size;
            wg_blink_write_mem(blink, va, s->data, n);
        }
    }

    // Apply base relocations (HIGHLOW only — 32-bit DLLs).
    if (delta && img->reloc_rva && img->reloc_size) {
        uint32_t off = 0;
        while (off + 8 <= img->reloc_size) {
            const uint8_t *blk = pe_rva_ptr(img, img->reloc_rva + off, 8);
            if (!blk) break;
            uint32_t page_rva, blk_size;
            memcpy(&page_rva, blk, 4); memcpy(&blk_size, blk + 4, 4);
            if (blk_size < 8 || off + blk_size > img->reloc_size) break;
            uint32_t nent = (blk_size - 8) / 2;
            const uint8_t *ents = pe_rva_ptr(img, img->reloc_rva + off + 8, nent * 2);
            if (!ents) break;
            for (uint32_t e = 0; e < nent; e++) {
                uint16_t v; memcpy(&v, ents + e * 2, 2);
                if ((v >> 12) == 3 /*IMAGE_REL_BASED_HIGHLOW*/) {
                    uint32_t target = load_base + page_rva + (v & 0xFFF);
                    uint32_t val = 0;
                    wg_blink_read_mem(blink, target, &val, 4);
                    val += (uint32_t)delta;
                    wg_blink_write_mem(blink, target, &val, 4);
                }
            }
            off += blk_size;
        }
    }

    // Resolve the DLL's own imports against our Win32 thunk table.
    for (int i = 0; i < img->num_imports; i++) {
        WGPEImportDll *imp = &img->imports[i];
        for (int j = 0; j < imp->num_functions; j++) {
            uint64_t stub = wg_dll_mapper_resolve(mapper,
                                                  imp->dll_name, imp->functions[j].name);
            if (stub) {
                uint32_t iat = load_base + imp->functions[j].iat_rva;
                uint32_t s32 = (uint32_t)stub;
                wg_blink_write_mem(blink, iat, &s32, 4);
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        if (!s_modules[i].in_use) {
            s_modules[i].in_use = true;
            strncpy(s_modules[i].name, base_name, sizeof(s_modules[i].name) - 1);
            s_modules[i].base = load_base;
            s_modules[i].size = img_size;
            s_modules[i].img  = img;
            WG_LOGI(TAG, "Loaded DLL %s at 0x%X (size 0x%X, reloc delta 0x%X)",
                    base_name, load_base, img_size, (uint32_t)delta);
            return load_base;
        }
    }
    wg_pe_image_free(img);   // module table full — leave it mapped, drop record
    return load_base;
}

// Look up an export by name in a loaded module; returns its guest address or 0.
static uint32_t wg_module_export(uint32_t base, const char *func) {
    for (int m = 0; m < 16; m++) {
        if (!s_modules[m].in_use || s_modules[m].base != base) continue;
        // nsProcess is loaded for real, but its process-enumeration exports are
        // meaningless on iOS (no Windows processes exist) and the real path
        // doesn't work here. Return 0 so GetProcAddress falls through to our
        // emulated FindProcess/KillProcess thunk (reports "not running").
        if (strcmp(s_modules[m].name, "nsprocess.dll") == 0) return 0;
        // nsExec runs an external process (steamservice.exe) and reads its
        // stdout in a loop — we can't run child processes, and the real loop
        // hangs. Emulate its exports (Exec/ExecToLog/ExecToStack) instead.
        if (strcmp(s_modules[m].name, "nsexec.dll") == 0) return 0;
        WGPEImage *img = s_modules[m].img;
        if (!img->export_rva || !img->export_size) return 0;
        const uint8_t *ed = pe_rva_ptr(img, img->export_rva, 40);
        if (!ed) return 0;
        uint32_t nfuncs, nnames, func_rva, name_rva, ord_rva;
        memcpy(&nfuncs,   ed + 20, 4);
        memcpy(&nnames,   ed + 24, 4);
        memcpy(&func_rva, ed + 28, 4);
        memcpy(&name_rva, ed + 32, 4);
        memcpy(&ord_rva,  ed + 36, 4);
        const uint8_t *names = pe_rva_ptr(img, name_rva, nnames * 4);
        const uint8_t *ords  = pe_rva_ptr(img, ord_rva,  nnames * 2);
        const uint8_t *funcs = pe_rva_ptr(img, func_rva, nfuncs * 4);
        if (!names || !ords || !funcs) return 0;
        for (uint32_t k = 0; k < nnames; k++) {
            uint32_t nrva; memcpy(&nrva, names + k * 4, 4);
            const char *nm = (const char *)pe_rva_ptr(img, nrva, 1);
            if (nm && strcmp(nm, func) == 0) {
                uint16_t idx; memcpy(&idx, ords + k * 2, 2);
                if (idx >= nfuncs) return 0;
                uint32_t frva; memcpy(&frva, funcs + idx * 4, 4);
                return base + frva;
            }
        }
        return 0;
    }
    return 0;
}

// Return the load base of an already-loaded module matching a path, or 0.
static uint32_t wg_module_find(const char *path) {
    char base_name[64];
    dll_basename(path, base_name, sizeof base_name);
    for (int i = 0; i < 16; i++)
        if (s_modules[i].in_use && strcmp(s_modules[i].name, base_name) == 0)
            return s_modules[i].base;
    return 0;
}

typedef enum {
    WG_BACKEND_BUILTIN,
    WG_BACKEND_BLINK,
} WGBackend;

static uint32_t s_last_error = 0;

// Deadlock probe: bumped on every thunk; a watchdog thread dumps live waits when
// this stops advancing (WG_DEADLOCK_DUMP=<seconds>, e.g. 15).
static volatile unsigned long long s_thunk_progress = 0;

// Sync-op ring buffer (WG_SYNCTRACE=1): record the last N CreateEvent/SetEvent/
// ResetEvent/Wait ops so the deadlock watchdog can print the exact handshake
// sequence that led to the collapse (which thread signalled/waited on which
// handle, and the guest caller). Lock-free-ish ring (racy index is fine for a
// diagnostic). Dumped by wg_dump_synctrace() from the watchdog.
#define WG_SYNCTRACE_N 384
static struct { const char *op; uint32_t tid, h; uint64_t caller; } s_synctrace[WG_SYNCTRACE_N];
static volatile unsigned int s_synctrace_i = 0;
static signed char s_synctrace_on = -1;
static inline void wg_synctrace(const char *op, uint32_t tid, uint32_t h, uint64_t caller) {
    if (s_synctrace_on < 0) s_synctrace_on = getenv("WG_SYNCTRACE") ? 1 : 0;
    if (!s_synctrace_on) return;
    unsigned int i = __atomic_fetch_add(&s_synctrace_i, 1, __ATOMIC_RELAXED) % WG_SYNCTRACE_N;
    s_synctrace[i].op = op; s_synctrace[i].tid = tid; s_synctrace[i].h = h; s_synctrace[i].caller = caller;
}
void wg_dump_synctrace(void) {
    if (s_synctrace_on <= 0) return;
    unsigned int end = s_synctrace_i, n = end < WG_SYNCTRACE_N ? end : WG_SYNCTRACE_N;
    fprintf(stderr, "=== last %u sync ops (chronological) ===\n", n);
    for (unsigned int k = 0; k < n; k++) {
        unsigned int i = (end - n + k) % WG_SYNCTRACE_N;
        if (!s_synctrace[i].op) continue;
        fprintf(stderr, "  tid=0x%-5X %-10s h=0x%-6X caller=0x%llX\n",
                s_synctrace[i].tid, s_synctrace[i].op, s_synctrace[i].h,
                (unsigned long long)s_synctrace[i].caller);
    }
    fprintf(stderr, "=== end sync ops ===\n");
}

// Ring buffer of recent Win32 API calls for crash diagnostics
#define WG_CALL_RING_SIZE 256
static struct { const char *fn; uint64_t ret; } s_call_ring[WG_CALL_RING_SIZE];
static int s_call_ring_idx = 0;
static inline void wg_call_ring_push(const char *name, uint64_t ret) {
    // Filter out noisy cleanup/infrastructure calls
    if (name[0] == 'H' && (name[4] == 'F' || name[4] == 'S' || name[4] == 'A'))
        return; // HeapFree, HeapSize, HeapAlloc
    if (name[0] == 'D' && name[1] == 'e') return; // DeleteCriticalSection
    if (name[0] == 'E' && name[5] == 'C') return; // EnterCriticalSection
    if (name[0] == 'L' && name[5] == 'C') return; // LeaveCriticalSection
    if (name[0] == 'T' && name[3] == 'E') return; // TryEnterCriticalSection
    if (name[0] == 'G' && name[3] == 'L') return; // GetLastError
    if (name[0] == 'S' && name[3] == 'L') return; // SetLastError
    if (name[0] == 'F' && name[3] == 'G') return; // FlsGetValue
    if (name[0] == 'F' && name[3] == 'S') return; // FlsSetValue
    int idx = s_call_ring_idx % WG_CALL_RING_SIZE;
    s_call_ring[idx].fn = name;
    s_call_ring[idx].ret = ret;
    s_call_ring_idx++;
}
static bool s_nsis_data_patched = false;
// A/B diagnostic toggle for the NSIS solid-LZMA data section.
//   1 = pre-decompress the data .tmp natively (LzmaDec) and ignore the guest's
//       own writes to it. The current workaround, on the theory that blink
//       truncates NSIS's in-guest decode.
//   0 = let NSIS decompress the stream itself, so we can test whether its
//       native decode actually works (blink CALL/RET is confirmed fine — the
//       failing self-test was a no-stack harness artifact). When 0,
//       s_nsis_data_tmp_handle stays 0, which also makes the "ignore writes"
//       branch in WriteFile inert, so NSIS's decode lands on disk normally.
// Flip this and rebuild to compare the two paths on-device. Overridable from
// the build (e.g. OTHER_CFLAGS: -DWG_NSIS_PREFILL_HACK=0) without editing here.
#ifndef WG_NSIS_PREFILL_HACK
#define WG_NSIS_PREFILL_HACK 0
#endif

// Guest heap pointer for GlobalAlloc/etc. Reset per program load so behavior
// is deterministic (not dependent on prior runs in the same app session).
#define WG_GUEST_HEAP_BASE 0x20000000u
static uint32_t s_heap_ptr = WG_GUEST_HEAP_BASE;

// File mappings (CreateFileMapping + MapViewOfFile). UE4's FPakPrecacher / memory-
// mapped file path maps the pak files and reads pak blocks straight from the mapped
// memory. These were stub_default (returned 0/NULL), so the mapping "failed" and the
// precacher spun forever on unavailable data (the post-swapchain load stall). We back
// a mapped VIEW by allocating guest memory and reading the file region into it (paks
// are read-only — no write-back needed). Mapping handle = WG_FILEMAP_BASE + slot.
#define WG_MAX_FILEMAP 128
#define WG_FILEMAP_BASE 0x00FE0000u
static struct { uint8_t used; uint32_t file_handle; } s_filemap[WG_MAX_FILEMAP];

// Dynamic TLS (TlsAlloc/TlsGetValue/TlsSetValue) and FLS. These are PER-THREAD:
// the slot *index* is process-global, but each thread has its own value array.
// A global array made the MSVC CRT's per-thread data block (kept in FLS slot 1)
// shared across threads — so a worker thread used the main thread's _tiddata,
// corrupting errno/locale/per-thread state. With per-thread arrays a new thread
// reads 0 for an unset slot and the CRT lazily allocates its own block.
// Indexed by scheduler thread slot (WG_MAX_THREADS rows). Windows guarantees
// at least 1088 TLS slots.
static uint32_t s_tls_slots[WG_MAX_THREADS][1088] = {{0}};
static uint32_t s_tls_next = 0;
static uint32_t s_fls_slots[WG_MAX_THREADS][1088] = {{0}};   // Fiber-Local Storage
static uint32_t s_fls_next = 0;

// Fake event/mutex/semaphore handles. Single-threaded, so events are just
// signalled/unsignalled flags. Handles start at 0x200 to avoid collisions.
#define WG_EVENT_BASE   0x200u
// Events occupy handles 0x200..0xFFF (3584 slots) — right up to the socket base
// 0x1000, so no collision. Was 256, which OVERFLOWED (s_event_next only ever
// incremented, never freed on CloseHandle) -> CreateEvent returned 0 -> guest
// threads waited on NULL events forever (the cooperative-mode 0x9FDA6F/0x815879
// deadlock). Now also recycled via s_event_free (below).
#define WG_MAX_EVENTS   3584
#define WG_SELECT_WAIT  0x5E1EC7u   // sentinel wait_handle: thread is pacing a select() timeout
static bool s_event_signalled[WG_MAX_EVENTS];
static bool s_event_manual[WG_MAX_EVENTS];   // true = manual-reset, false = auto-reset
static uint32_t s_event_next = 0;
static uint32_t s_event_free[WG_MAX_EVENTS];  // free-list of closed slots (recycle)
static int s_event_free_n = 0;

// A satisfied wait on an AUTO-reset event consumes its signalled state. Without
// this an auto-reset event stays signalled forever and waiters spin (the network
// worker did exactly this on its job event). Manual-reset events are untouched.
static void wg_event_consume(uint32_t h) {
    if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS) {
        uint32_t idx = h - WG_EVENT_BASE;
        if (!s_event_manual[idx]) s_event_signalled[idx] = false;
    }
}

// Condition variables (SleepConditionVariableCS / WakeConditionVariable /
// WakeAllConditionVariable). Steam's thread pool coordinates work hand-off
// through CVs: idle workers Sleep on the CV, and queuing work Wakes them. The
// old stubs made Sleep return TRUE immediately (never blocking) and Wake a
// no-op, so on the device the pool never parks/dispatches and the queued
// manifest-send worker is never run. We model each CV by its guest address and
// a monotonically increasing "generation": Wake bumps the generation (and marks
// waiters runnable), a sleeper captures the generation when it parks and knows
// it was woken once the generation moves. Keyed by guest CV pointer.
#define WG_MAX_CVS 64
static struct { uint32_t ptr; uint32_t gen; } s_cvs[WG_MAX_CVS];
static uint32_t *cv_slot(uint32_t ptr) {
    for (int i = 0; i < WG_MAX_CVS; i++)
        if (s_cvs[i].ptr == ptr) return &s_cvs[i].gen;
    for (int i = 0; i < WG_MAX_CVS; i++)
        if (s_cvs[i].ptr == 0) { s_cvs[i].ptr = ptr; s_cvs[i].gen = 0; return &s_cvs[i].gen; }
    return NULL;
}

// Fake IOCP (I/O Completion Ports).
// Steam uses async sockets: bind socket to IOCP, call WSARecv/ConnectEx with
// an OVERLAPPED, then GetQueuedCompletionStatus waits for the completion.
// We do all I/O synchronously, but still need to post completions so GQCS
// returns them.
#define WG_IOCP_HANDLE      0x8500u
#define WG_MAX_IOCP_SOCK    32
#define WG_MAX_COMPLETIONS  64
typedef struct { uint32_t sock_handle; uint32_t comp_key; } WGIocpBinding;
typedef struct { uint32_t bytes; uint32_t comp_key; uint32_t overlapped; } WGCompletion;
static WGIocpBinding  s_iocp_bindings[WG_MAX_IOCP_SOCK];
static int            s_iocp_binding_count = 0;
static WGCompletion   s_completions[WG_MAX_COMPLETIONS];
static int            s_comp_head = 0, s_comp_tail = 0;
static bool           s_iocp_created = false;
// Force apps onto the synchronous select/send/recv path (we don't deliver real
// IOCP socket completions). NOTE: had no effect on Steam — its BUseIOCP() is
// OS-gated (always true on Win10), independent of CreateIoCompletionPort. Left
// false (IOCP stubs active) as the neutral default.
static bool           s_disable_iocp = false;
// Correlate the connection lifecycle across calls: record what connect() saw so
// it can be reported at the later send(0,0,0) (which is what gets pasted).
static uint32_t       s_last_conn_obj  = 0; // candidate CTCPConnection 'this' at connect
static uint32_t       s_last_conn_sock = 0; // socket passed to connect
static int            s_connect_calls  = 0; // # of connect() calls this run

// Thread pool work object table
#define WG_TP_WORK_BASE  0x9000u
#define WG_MAX_TP_WORK   16
typedef struct { uint32_t callback; uint32_t ctx; uint32_t thread_handle; } WGTpWork;
static WGTpWork  s_tp_work[WG_MAX_TP_WORK];
static int       s_tp_work_count = 0;

static uint32_t iocp_comp_key(uint32_t sock_handle) {
    for (int i = 0; i < s_iocp_binding_count; i++)
        if (s_iocp_bindings[i].sock_handle == sock_handle)
            return s_iocp_bindings[i].comp_key;
    return 0;
}
static void iocp_post(uint32_t bytes, uint32_t comp_key, uint32_t overlapped) {
    int next = (s_comp_tail + 1) % WG_MAX_COMPLETIONS;
    if (next == s_comp_head) return; // full — drop
    s_completions[s_comp_tail] = (WGCompletion){bytes, comp_key, overlapped};
    s_comp_tail = next;
    WG_LOGI("IOCP", "Posted completion: bytes=%u key=0x%X ovl=0x%X", bytes, comp_key, overlapped);
}
static bool iocp_get(uint32_t *bytes, uint32_t *comp_key, uint32_t *overlapped) {
    if (s_comp_head == s_comp_tail) return false;
    *bytes    = s_completions[s_comp_head].bytes;
    *comp_key = s_completions[s_comp_head].comp_key;
    *overlapped = s_completions[s_comp_head].overlapped;
    s_comp_head = (s_comp_head + 1) % WG_MAX_COMPLETIONS;
    return true;
}

// Thread message queue for PostThreadMessageW / PeekMessageW.
// Steam's download thread is message-driven: thread 0 posts work via
// PostThreadMessageW; thread 1 retrieves it with PeekMessageW.
#define WG_MAX_THREAD_MSGS 64
static struct { uint32_t tid; uint32_t msg; uint32_t wparam; uint32_t lparam; }
    s_thread_msgs[WG_MAX_THREAD_MSGS];
static int s_tmsg_head = 0, s_tmsg_tail = 0;

static void tmsg_push(uint32_t tid, uint32_t msg, uint32_t wp, uint32_t lp) {
    int next = (s_tmsg_tail + 1) % WG_MAX_THREAD_MSGS;
    if (next == s_tmsg_head) return; // full — drop
    s_thread_msgs[s_tmsg_tail].tid = tid;
    s_thread_msgs[s_tmsg_tail].msg = msg;
    s_thread_msgs[s_tmsg_tail].wparam = wp;
    s_thread_msgs[s_tmsg_tail].lparam = lp;
    s_tmsg_tail = next;
}
static bool tmsg_pop(uint32_t tid, uint32_t *msg, uint32_t *wp, uint32_t *lp) {
    for (int i = s_tmsg_head; i != s_tmsg_tail; i = (i + 1) % WG_MAX_THREAD_MSGS) {
        if (s_thread_msgs[i].tid == tid) {
            *msg = s_thread_msgs[i].msg;
            *wp  = s_thread_msgs[i].wparam;
            *lp  = s_thread_msgs[i].lparam;
            // Compact: shift remaining entries down one slot
            int j = i;
            while (1) {
                int next = (j + 1) % WG_MAX_THREAD_MSGS;
                if (next == s_tmsg_tail) break;
                s_thread_msgs[j] = s_thread_msgs[next];
                j = next;
            }
            s_tmsg_tail = (s_tmsg_tail - 1 + WG_MAX_THREAD_MSGS) % WG_MAX_THREAD_MSGS;
            return true;
        }
    }
    return false;
}
static uint32_t s_main_teb = 0; // TEB address of main thread
// Allocate a per-thread TEB (own TLS array, stack bounds, ClientId) sharing the
// process PEB. Returns the TEB guest address, or 0 on failure. Defined later.
static uint32_t wg_alloc_thread_teb(WGEngine *engine, uint32_t stack_base,
                                    uint32_t stack_limit, uint32_t tid);
static bool s_cmdpage_mapped = false;
static uint32_t s_nsis_exe_data_offset = 0;
static uint32_t s_nsis_data_tmp_handle = 0;    // handle to the NSIS data .tmp file
static uint32_t s_nsis_last_data_seek = 0;     // last seek position in data .tmp
static char s_nsis_data_tmp_path[1024] = {0};  // real path of data .tmp

#ifdef WG_DECODE_DIAG
// Local-repro diagnostic: diff NSIS's own in-guest decode (written to the data
// .tmp when prefill is off) against the reference stream in /tmp/ref.bin, to
// find the first byte where blink's decode diverges. Zero-cost unless defined.
static uint32_t s_diag_data_tmp_handle = 0;
static uint8_t *s_diag_ref = NULL;
static long     s_diag_ref_len = 0;
static int      s_diag_reported = 0;
static void wg_diag_check(uint32_t pos, const uint8_t *buf, uint32_t n) {
    if (s_diag_reported) return;
    if (!s_diag_ref) {
        FILE *r = fopen("/tmp/ref.bin", "rb");
        if (!r) { s_diag_reported = 1; return; }
        fseek(r, 0, SEEK_END); s_diag_ref_len = ftell(r); fseek(r, 0, SEEK_SET);
        s_diag_ref = (uint8_t *)malloc(s_diag_ref_len);
        if (!s_diag_ref || fread(s_diag_ref, 1, s_diag_ref_len, r) != (size_t)s_diag_ref_len) {
            s_diag_reported = 1; fclose(r); return;
        }
        fclose(r);
        WG_LOGI("DIAG", "ref.bin loaded (%ld bytes)", s_diag_ref_len);
    }
    for (uint32_t i = 0; i < n; i++) {
        long off = (long)pos + i;
        if (off >= s_diag_ref_len) break;
        if (buf[i] != s_diag_ref[off]) {
            WG_LOGE("DIAG", "DECODE DIVERGES at decompressed offset %ld: "
                    "guest=0x%02X ref=0x%02X (correct for %ld bytes)",
                    off, buf[i], s_diag_ref[off], off);
            // dump a little context
            WG_LOGE("DIAG", "  ref [%ld..]: %02X %02X %02X %02X %02X %02X %02X %02X",
                    off, s_diag_ref[off], s_diag_ref[off+1], s_diag_ref[off+2],
                    s_diag_ref[off+3], s_diag_ref[off+4], s_diag_ref[off+5],
                    s_diag_ref[off+6], s_diag_ref[off+7]);
            WG_LOGE("DIAG", "  got [%ld..]: %02X %02X %02X %02X %02X %02X %02X %02X",
                    off, buf[i], (i+1<n?buf[i+1]:0), (i+2<n?buf[i+2]:0),
                    (i+3<n?buf[i+3]:0), (i+4<n?buf[i+4]:0), (i+5<n?buf[i+5]:0),
                    (i+6<n?buf[i+6]:0), (i+7<n?buf[i+7]:0));
            s_diag_reported = 1;
            return;
        }
    }
}
#endif

struct WGEngine {
    WGEngineState   state;
    WGBackend       backend;

    // Builtin interpreter
    WGMemorySpace  *memory;
    WGx86State     *cpu;

    // Blink engine
    WGBlinkInstance *blink;

    // Shared
    WGPEImage      *pe_image;
    WGDllMapper    *dll_mapper;
    WGWinsock          *winsock;
    WGWinHttp          *winhttp;
    WGSchannel         *schannel;
    WGThreadScheduler  *scheduler;
    uint64_t        tick_count;
    int             instructions_per_tick;
    bool            thunks_mapped; // whether HLT stubs are in blink memory
};

WGEngine *wg_engine_create(void) {
    WGEngine *e = calloc(1, sizeof(WGEngine));
    if (!e) return NULL;
    e->state = WG_ENGINE_IDLE;
    e->instructions_per_tick = getenv("WG_IPT") ? atoi(getenv("WG_IPT")) : 100000;
    e->backend = WG_BACKEND_BLINK;
    e->winsock = wg_winsock_create();
    e->winhttp = wg_winhttp_create();
    e->schannel = wg_schannel_create();
    e->scheduler = wg_sched_create();
    return e;
}

void wg_engine_destroy(WGEngine *engine) {
    if (!engine) return;
    wg_engine_stop(engine);
    if (engine->cpu) wg_x86_state_destroy(engine->cpu);
    if (engine->memory) wg_memory_destroy(engine->memory);
    if (engine->pe_image) wg_pe_image_free(engine->pe_image);
    if (engine->dll_mapper) wg_dll_mapper_destroy(engine->dll_mapper);
    if (engine->blink) wg_blink_destroy(engine->blink);
    if (engine->winsock) wg_winsock_destroy(engine->winsock);
    if (engine->winhttp) wg_winhttp_destroy(engine->winhttp);
    if (engine->schannel) wg_schannel_destroy(engine->schannel);
    if (engine->scheduler) wg_sched_destroy(engine->scheduler);
    free(engine);
}

// Map Win32 API thunk addresses into blink's memory.
// Each thunk is a tiny x86 stub: RET (pop return address and go back).
// When the engine detects RIP landed on a thunk after a step, it
// calls the Win32 stub handler before resuming.
//

// Layout at each thunk address (8 bytes apart):
//   [thunk+0] HLT   (0xF4) — stops execution
// The engine sees the halt, checks if RIP is in the thunk range,
// runs the stub, pops the return address, and resumes.
static void map_thunks_to_blink(WGEngine *engine) {
    if (!engine->blink || !engine->dll_mapper || engine->thunks_mapped) return;

    // For 32-bit PEs, thunks must be within 16MB (kRealSize).
    // Use 0xC00000 (12MB mark) for 32-bit, WG_THUNK_BASE for 64-bit.
    bool is_32bit = (engine->pe_image && !engine->pe_image->is_64bit);
    uint64_t thunk_base = is_32bit ? 0xC00000ULL : WG_THUNK_BASE;

    // Reassign thunk addresses to the correct range
    engine->dll_mapper->next_thunk = thunk_base;
    for (int i = 0; i < engine->dll_mapper->count; i++) {
        engine->dll_mapper->entries[i].thunk_addr = thunk_base + i * 8;
    }
    engine->dll_mapper->next_thunk = thunk_base + engine->dll_mapper->count * 8;

    uint32_t thunk_region_size = 0x20000; // 128KB
    uint8_t *thunk_page = calloc(1, thunk_region_size);
    if (!thunk_page) return;

    memset(thunk_page, 0xF4, thunk_region_size); // fill with HLT

    wg_blink_load_code(engine->blink, thunk_base, thunk_page,
                       thunk_region_size, 0);
    free(thunk_page);

    engine->thunks_mapped = true;
    WG_LOGI(TAG, "Win32 API thunks mapped at 0x%llX (%d stubs)",
            (unsigned long long)thunk_base, engine->dll_mapper->count);
}

// Map the x64 CRT-init trampoline at `addr`. The _initterm/_initterm_e thunks
// jump here with RCX=first, RDX=last and the caller's return address still on
// the stack; this guest code CALLs every non-null function pointer in
// [RCX,RDX) and RETs to the caller. (A no-op stub instead leaves every static
// C++ constructor unrun — a UE4 game then derefs null singletons everywhere.)
// Must be mapped AFTER the PE sections load, or .text overwrites it.
static void map_initterm_tramp(WGEngine *engine, uint32_t addr) {
    static const uint8_t tramp[] = {
        0x53,                               // push rbx
        0x56,                               // push rsi
        0x48, 0x89, 0xCB,                   // mov  rbx, rcx
        0x48, 0x89, 0xD6,                   // mov  rsi, rdx
        /* loop: */
        0x48, 0x39, 0xF3,                   // cmp  rbx, rsi
        0x73, 0x18,                         // jae  done
        0x48, 0x8B, 0x03,                   // mov  rax, [rbx]
        0x48, 0x85, 0xC0,                   // test rax, rax
        0x74, 0x0A,                         // jz   next
        0x48, 0x83, 0xEC, 0x28,             // sub  rsp, 0x28 (shadow+align)
        0xFF, 0xD0,                         // call rax
        0x48, 0x83, 0xC4, 0x28,             // add  rsp, 0x28
        /* next: */
        0x48, 0x83, 0xC3, 0x08,             // add  rbx, 8
        0xEB, 0xE3,                         // jmp  loop
        /* done: */
        0x31, 0xC0,                         // xor  eax, eax
        0x5E,                               // pop  rsi
        0x5B,                               // pop  rbx
        0xC3,                               // ret
    };
    uint8_t page[0x1000];
    memset(page, 0xF4, sizeof(page));       // HLT-fill the rest
    memcpy(page, tramp, sizeof(tramp));
    wg_blink_load_code(engine->blink, addr, page, sizeof(page), 0);
    WG_LOGI(TAG, "x64 _initterm trampoline mapped at 0x%X", addr);
}

// ============================================================
//  Modal dialogs + control rendering (NSIS wizard UI)
// ============================================================
#define WG_DLG_SENTINEL    0xC10000u    // dlgproc return trap (HLT-filled page)
#define WG_SEH_SENTINEL    0xC10020u    // SEH handler return trap (HLT-filled)
#define WG_CTRL_HWND_BASE  0x00C70000u  // synthetic control HWND range

static bool     s_dlg_active = false;
static uint32_t s_dlg_ret_addr = 0;     // WinMain return for DialogBoxParamW
static uint32_t s_dlg_ret_rsp  = 0;     // RSP to restore on EndDialog
static uint32_t s_dlg_hwnd     = 0;     // the modal dialog window
static uint32_t s_dlg_proc     = 0;     // the dialog procedure (for WM_COMMAND)
static uint32_t s_dlg_result   = 1;

typedef struct {
    uint32_t hwnd;            // owning dialog window
    uint32_t id;              // control id
    uint32_t style;
    int16_t  x, y, cx, cy;    // dialog units
    int16_t  dlg_cx, dlg_cy;  // this dialog's unit extent (for scaling)
    uint16_t cls;             // 0x80 button, 0x82 static, 0x81 edit, ...
    bool     is_bitmap;       // SS_BITMAP static
    uint32_t hbitmap;
    uint16_t text[80];
} WGDlgCtrl;
static WGDlgCtrl s_ctrls[160];
static int       s_ctrl_count = 0;
static int       s_dlg_cx = 331, s_dlg_cy = 222; // last-parsed dialog-unit extent

// ---- HFONT registry (so SelectObject can apply CreateFont* metrics) -----
typedef struct { uint32_t handle; int px; bool bold; char name[64]; } WGFontRec;
static WGFontRec s_fonts[64];
static int       s_font_count = 0;
static uint32_t  s_next_font_h = 0xF0000;

static uint32_t wg_font_register(int px, bool bold, const char *name) {
    uint32_t h = s_next_font_h++;
    WGFontRec *f = &s_fonts[s_font_count % 64];
    s_font_count++;
    f->handle = h; f->px = px; f->bold = bold;
    if (name) { strncpy(f->name, name, sizeof(f->name) - 1); f->name[sizeof(f->name)-1] = 0; }
    else f->name[0] = 0;
    return h;
}
static WGFontRec *wg_font_find(uint32_t h) {
    for (int i = 0; i < 64; i++) if (s_fonts[i].handle == h && h) return &s_fonts[i];
    return NULL;
}

// Synthetic control classes for runtime-styled instfiles controls so the
// renderer can draw them (the dialog template stores these as class-NAME
// strings, not atoms).
#define WG_CLS_PROGRESS 0x0090   // msctls_progress32
#define WG_CLS_LISTVIEW 0x0091   // SysListView32 (the "details" log)

// Install-progress + details-log state (driven by PBM_SETPOS / LVM_INSERTITEM).
static uint32_t s_pb_pos = 0, s_pb_max = 100;
static char     s_detail_lines[256][120];
static int      s_detail_count = 0;
static uint32_t s_page_hwnd = 0;   // current inner page (where progress/list live)

static WGDlgCtrl *wg_find_ctrl(uint32_t hwnd, uint32_t id) {
    for (int i = 0; i < s_ctrl_count; i++)
        if (s_ctrls[i].hwnd == hwnd && s_ctrls[i].id == id) return &s_ctrls[i];
    return NULL;
}
static WGDlgCtrl *wg_ctrl_from_handle(uint32_t h) {
    if (h >= WG_CTRL_HWND_BASE && h < WG_CTRL_HWND_BASE + 160) {
        int idx = (int)(h - WG_CTRL_HWND_BASE);
        if (idx < s_ctrl_count) return &s_ctrls[idx];
    }
    return NULL;
}

// ---- PE resource access (parse the dialog template from the .rsrc) ----
static const uint8_t *pe_rva(WGPEImage *pe, uint32_t rva) {
    for (int i = 0; i < pe->num_sections; i++) {
        WGPESection *s = &pe->sections[i];
        if (rva >= s->virtual_address && rva < s->virtual_address + s->raw_size)
            return pe->raw_data + s->raw_offset + (rva - s->virtual_address);
    }
    return NULL;
}
static const uint8_t *pe_find_dialog(WGPEImage *pe, uint32_t dlg_id) {
    if (!pe->raw_data) return NULL;
    uint32_t e_lfanew; memcpy(&e_lfanew, pe->raw_data + 0x3C, 4);
    uint32_t rsrc_rva; memcpy(&rsrc_rva, pe->raw_data + e_lfanew + 24 + 112, 4);
    const uint8_t *base = pe_rva(pe, rsrc_rva);
    if (!base) return NULL;
    uint16_t nN, nI;
    memcpy(&nN, base + 12, 2); memcpy(&nI, base + 14, 2);
    for (int i = 0; i < nN + nI; i++) {
        const uint8_t *e = base + 16 + i * 8;
        uint32_t nameid, off; memcpy(&nameid, e, 4); memcpy(&off, e + 4, 4);
        if (nameid != 5 /*RT_DIALOG*/ || !(off & 0x80000000u)) continue;
        const uint8_t *l1 = base + (off & 0x7fffffff);
        uint16_t a, b; memcpy(&a, l1 + 12, 2); memcpy(&b, l1 + 14, 2);
        for (int j = 0; j < a + b; j++) {
            const uint8_t *e1 = l1 + 16 + j * 8;
            uint32_t id, o1; memcpy(&id, e1, 4); memcpy(&o1, e1 + 4, 4);
            if ((id & 0x7fffffff) != dlg_id || !(o1 & 0x80000000u)) continue;
            const uint8_t *l2 = base + (o1 & 0x7fffffff);
            uint32_t o2; memcpy(&o2, l2 + 16 + 4, 4);          // first lang's data-entry off
            const uint8_t *de = base + (o2 & 0x7fffffff);
            uint32_t data_rva; memcpy(&data_rva, de, 4);
            return pe_rva(pe, data_rva);
        }
    }
    return NULL;
}
static const uint8_t *res_skip_sz(const uint8_t *p, uint16_t *out, int cap) {
    uint16_t v; memcpy(&v, p, 2);
    if (v == 0)      { if (out) out[0] = 0; return p + 2; }
    if (v == 0xFFFF) { if (out) out[0] = 0; return p + 4; }
    int k = 0;
    for (;;) { uint16_t c; memcpy(&c, p, 2); p += 2; if (!c) break;
               if (out && k < cap - 1) out[k++] = c; }
    if (out) out[k] = 0;
    return p;
}
static void wg_remove_ctrls(uint32_t hwnd) {
    int n = 0;
    for (int i = 0; i < s_ctrl_count; i++)
        if (s_ctrls[i].hwnd != hwnd) s_ctrls[n++] = s_ctrls[i];
    s_ctrl_count = n;
}

// The wizard shows one inner page at a time inside the outer frame. When a new
// page is created we retire the previous one (drop its controls + destroy its
// window) so it stops compositing — otherwise the old page (e.g. the welcome
// bitmap/text) bleeds through behind the new page, since NSIS's own
// DestroyWindow for the old page often arrives with a null handle here.
static uint32_t s_inner_page = 0;
static void wg_retire_inner_page(uint32_t keep) {
    if (s_inner_page && s_inner_page != keep) {
        wg_remove_ctrls(s_inner_page);
        wg_wm_destroy(s_inner_page);
    }
    s_inner_page = keep;
}
static void wg_parse_dialog(WGEngine *engine, uint32_t hwnd, uint32_t dlg_id) {
    wg_remove_ctrls(hwnd);          // replace this window's controls
    if (!engine->pe_image) return;
    const uint8_t *t = pe_find_dialog(engine->pe_image, dlg_id);
    if (!t) { WG_LOGI(TAG, "dialog %u: no template", dlg_id); return; }
    uint16_t dlgVer, sig; memcpy(&dlgVer, t, 2); memcpy(&sig, t + 2, 2);
    if (dlgVer != 1 || sig != 0xFFFF) return;   // only DLGTEMPLATEEX
    const uint8_t *p = t + 4 + 4 + 4;           // skip dlgVer/sig, helpID, exStyle
    uint32_t style; memcpy(&style, p, 4); p += 4;
    uint16_t cItems; memcpy(&cItems, p, 2); p += 2;
    p += 4;                                     // x, y
    int16_t dcx, dcy;
    memcpy(&dcx, p, 2); p += 2;
    memcpy(&dcy, p, 2); p += 2;
    s_dlg_cx = dcx; s_dlg_cy = dcy;
    p = res_skip_sz(p, NULL, 0);                // menu
    p = res_skip_sz(p, NULL, 0);                // class
    p = res_skip_sz(p, NULL, 0);                // title
    if (style & 0x40 /*DS_SETFONT*/) { p += 6; p = res_skip_sz(p, NULL, 0); }
    for (int i = 0; i < cItems && s_ctrl_count < 160; i++) {
        size_t aoff = ((size_t)(p - t) + 3) & ~(size_t)3; p = t + aoff;  // dword align
        p += 4 + 4;                             // helpID, exStyle
        uint32_t cstyle; memcpy(&cstyle, p, 4); p += 4;
        WGDlgCtrl *c = &s_ctrls[s_ctrl_count++];
        memset(c, 0, sizeof(*c));
        c->hwnd = hwnd; c->style = cstyle;
        c->dlg_cx = dcx; c->dlg_cy = dcy;
        memcpy(&c->x, p, 2); memcpy(&c->y, p + 2, 2);
        memcpy(&c->cx, p + 4, 2); memcpy(&c->cy, p + 6, 2); p += 8;
        memcpy(&c->id, p, 4); p += 4;
        uint16_t w0; memcpy(&w0, p, 2);
        if (w0 == 0xFFFF) { memcpy(&c->cls, p + 2, 2); p += 4; }
        else {
            // String class name — capture it so we can render the progress bar
            // and the "details" list (which have name classes, not atoms).
            uint16_t clsname[40] = {0};
            p = res_skip_sz(p, clsname, 40);
            char ca[40] = {0};
            for (int k = 0; k < 39 && clsname[k]; k++)
                ca[k] = clsname[k] < 128 ? (char)tolower((unsigned char)clsname[k]) : '?';
            if (strstr(ca, "progress"))       c->cls = WG_CLS_PROGRESS;
            else if (strstr(ca, "listview") || strstr(ca, "syslistview"))
                                              c->cls = WG_CLS_LISTVIEW;
            else                              c->cls = 0;
        }
        c->is_bitmap = (c->cls == 0x0082) && ((cstyle & 0x0F) == 0x0E /*SS_BITMAP*/);
        p = res_skip_sz(p, c->text, 80);        // title
        uint16_t extra; memcpy(&extra, p, 2); p += 2 + extra;
    }
    WG_LOGI(TAG, "dialog %u parsed: %d controls (%dx%d du)",
            dlg_id, s_ctrl_count, s_dlg_cx, s_dlg_cy);
}

// Paint the parsed controls into the dialog's client framebuffer.
static void wg_render_dialog(WGEngine *engine, uint32_t hwnd) {
    (void)engine;
    int32_t cw = 0, ch = 0;
    if (!wg_wm_get_client(hwnd, &cw, &ch)) return;
    uint32_t dc = wg_gdi_get_dc(hwnd);
    if (!dc) return;
    wg_gdi_fill_rect(dc, 0, 0, cw, ch, 0x00FFFFFF);          // white dialog bg
    for (int i = 0; i < s_ctrl_count; i++) {
        WGDlgCtrl *c = &s_ctrls[i];
        if (c->hwnd != hwnd) continue;
        // Always show the progress bar + details log even if NSIS left them
        // initially hidden (the "Show details" toggle isn't wired for synthetic
        // controls); seeing the install log/progress is more useful than hiding.
        bool force = (c->cls == WG_CLS_PROGRESS) ||
                     (c->cls == WG_CLS_LISTVIEW && s_detail_count > 0);
        if (!force && !(c->style & 0x10000000u /*WS_VISIBLE*/)) continue;
        float sx = c->dlg_cx ? (float)cw / c->dlg_cx : 1.0f;
        float sy = c->dlg_cy ? (float)ch / c->dlg_cy : 1.0f;
        int px = (int)(c->x * sx), py = (int)(c->y * sy);
        int pw = (int)(c->cx * sx), ph = (int)(c->cy * sy);
        int tlen = 0; while (tlen < 79 && c->text[tlen]) tlen++;
        int lh = wg_gdi_line_height(dc);
        if (c->cls == 0x0080) {                 // button (incl. group box)
            if ((c->style & 0x07) == 0x07 /*BS_GROUPBOX*/) {
                // frame + caption, no fill
                wg_gdi_fill_rect(dc, px, py, px + pw, py + 1, 0x00A0A0A0);
                if (tlen) wg_gdi_text_out_caption(dc, px + 6, py - lh / 2, c->text, tlen);
            } else {
                // push button: light fill + center the caption both axes
                wg_gdi_fill_rect(dc, px, py, px + pw, py + ph, 0x00E1E1E1);
                int tw = wg_gdi_text_width(dc, c->text, tlen);
                int tx = px + (pw - tw) / 2; if (tx < px + 4) tx = px + 4;
                if (tlen) wg_gdi_text_out_caption(dc, tx, py + (ph - lh) / 2, c->text, tlen);
            }
        } else if (c->cls == 0x0082) {          // static
            if (c->is_bitmap && c->hbitmap)
                wg_gdi_draw_bitmap(dc, px, py, pw, ph, c->hbitmap, 0, 0, 0, 0);
            else if (tlen)
                // Wrap within the control so long labels don't clip at the edge.
                wg_gdi_text_box(dc, px, py, pw, ph > lh ? ph : lh, c->text, tlen);
        } else if (c->cls == 0x0081) {          // edit box
            wg_gdi_fill_rect(dc, px, py, px + pw, py + ph, 0x00FFFFFF);
            wg_gdi_fill_rect(dc, px, py, px + pw, py + 1, 0x00808080);   // top border
            wg_gdi_fill_rect(dc, px, py, px + 1, py + ph, 0x00808080);   // left border
            if (tlen) wg_gdi_text_out(dc, px + 3, py + (ph - lh) / 2, c->text, tlen);
        } else if (c->cls == WG_CLS_PROGRESS) {     // msctls_progress32
            wg_gdi_fill_rect(dc, px, py, px + pw, py + ph, 0x00C8C8C8);   // trough
            uint32_t mx = s_pb_max ? s_pb_max : 100;
            int fill = (int)((float)pw * (s_pb_pos > mx ? mx : s_pb_pos) / mx);
            if (fill > 0) wg_gdi_fill_rect(dc, px, py, px + fill, py + ph, 0x00D77800);
            wg_gdi_fill_rect(dc, px, py, px + pw, py + 1, 0x00808080);    // top border
        } else if (c->cls == WG_CLS_LISTVIEW) {     // the "details" install log
            wg_gdi_fill_rect(dc, px, py, px + pw, py + ph, 0x00FFFFFF);
            wg_gdi_fill_rect(dc, px, py, px + pw, py + 1, 0x00808080);
            int llh = lh + 2, rows = ph / llh;       // show the most recent lines
            int first = s_detail_count > rows ? s_detail_count - rows : 0;
            for (int r = first; r < s_detail_count; r++) {
                uint16_t wline[120]; int n = 0;
                for (; n < 119 && s_detail_lines[r][n]; n++) wline[n] = (uint8_t)s_detail_lines[r][n];
                wline[n] = 0;
                if (n) wg_gdi_text_out(dc, px + 3, py + 2 + (r - first) * llh, wline, n);
            }
        }
    }
    wg_gdi_release_dc(dc);
    WGWin32Window *w = wg_wm_find(hwnd);
    if (w) w->client_dirty = true;
}

// Register a control created at runtime via CreateWindowExW (e.g. an nsDialogs
// label/button/edit) so it paints on its parent page. Coordinates are pixels;
// we set the control's "dialog extent" to the parent's client size so the
// (scale = client/extent = 1) render path draws it at those exact pixels.
static void wg_register_child_control(uint32_t parent, uint32_t id, uint32_t style,
                                      uint16_t cls, int x, int y, int w, int h,
                                      const uint16_t *text) {
    if (s_ctrl_count >= 160) return;
    int32_t cw = 0, chh = 0;
    wg_wm_get_client(parent, &cw, &chh);
    WGDlgCtrl *c = &s_ctrls[s_ctrl_count++];
    memset(c, 0, sizeof(*c));
    c->hwnd  = parent;
    c->id    = id;
    c->style = style | 0x10000000u;          // force WS_VISIBLE so it paints
    c->dlg_cx = (int16_t)(cw  > 0 ? cw  : 1);
    c->dlg_cy = (int16_t)(chh > 0 ? chh : 1);
    c->x = (int16_t)x; c->y = (int16_t)y;
    c->cx = (int16_t)w; c->cy = (int16_t)h;
    c->cls = cls;
    if (text) for (int i = 0; i < 79 && text[i]; i++) c->text[i] = text[i];
}

// ---- Synchronous SendMessage / wndproc dispatch ----
// SendMessage must call the target window's procedure and return its result.
// We do this by jumping into the wndproc with a dedicated sentinel return; when
// it returns, the sentinel restores the SendMessage caller with the result. A
// small stack handles nested SendMessages (NSIS nests them heavily).
#define WG_SENDMSG_SENTINEL 0xC10010u
// A real Win32 call (SendMessage, CreateDialogParamW) preserves the caller's
// nonvolatile registers. Our synchronous dispatch jumps straight into the guest
// wndproc, so we snapshot the caller's GPRs here and restore them when the
// sentinel fires — otherwise the wndproc clobbers e.g. ESI and the caller faults
// (NSIS's CreateDialogParamW caller does `push [esi+0x2c]` right after the call).
typedef struct {
    uint32_t ret_addr, ret_rsp, ovr_eax; bool ovr;
    uint64_t saved_regs[16];
} WGPendingCall;
static WGPendingCall s_callstack[64];
static int           s_callstack_depth = 0;

// WM_TIMER network pump: Steam calls SetTimer(hwnd, id, ~20ms, NULL) and drives
// its whole network frame (select/recv) from the WM_TIMER handler in its wndproc.
// We record the timer and deliver WM_TIMER from GetMessageW so the pump keeps
// running (otherwise the main loop spins on GetMessage and never re-selects).
static uint32_t s_timer_hwnd = 0;
static uint32_t s_timer_id   = 0;
static bool     s_timer_active = false;

static uint32_t wg_resolve_wndproc(uint32_t hwnd) {
    if (hwnd == s_dlg_hwnd && s_dlg_proc) return s_dlg_proc;
    WGWin32Window *w = wg_wm_find(hwnd);
    return (w && w->wndproc) ? w->wndproc : 0;
}

// Set up a nested call proc(hwnd, msg, wParam, lParam) returning to the given
// caller (ret_addr/clean_rsp) with the proc's EAX. Returns true if armed.
static bool wg_call_wndproc_ovr(WGEngine *engine, uint32_t proc, uint32_t hwnd,
                                uint32_t msg, uint32_t wp, uint32_t lp,
                                uint32_t ret_addr, uint32_t clean_rsp,
                                bool ovr, uint32_t ovr_eax) {
    if (!proc || s_callstack_depth >= 64) return false;
    WGPendingCall *fr = &s_callstack[s_callstack_depth];
    fr->ret_addr = ret_addr;
    fr->ret_rsp  = clean_rsp;
    fr->ovr      = ovr;
    fr->ovr_eax  = ovr_eax;
    // Snapshot the caller's registers so the wndproc can't leak clobbered
    // nonvolatile regs (ESI/EDI/EBX/EBP) back to the caller.
    for (int i = 0; i < 16; i++)
        fr->saved_regs[i] = wg_blink_get_reg(engine->blink, i);
    s_callstack_depth++;
    uint32_t new_rsp = clean_rsp - 20;
    uint32_t sd[5] = { WG_SENDMSG_SENTINEL, hwnd, msg, wp, lp };
    wg_blink_write_mem(engine->blink, new_rsp, sd, 20);
    wg_blink_set_reg(engine->blink, 4, new_rsp);
    wg_blink_set_rip(engine->blink, proc);
    wg_blink_set_reg(engine->blink, 0, 0);
    return true;
}
static bool wg_call_wndproc(WGEngine *engine, uint32_t proc, uint32_t hwnd,
                            uint32_t msg, uint32_t wp, uint32_t lp,
                            uint32_t ret_addr, uint32_t clean_rsp) {
    return wg_call_wndproc_ovr(engine, proc, hwnd, msg, wp, lp,
                               ret_addr, clean_rsp, false, 0);
}

// Allocation size tracking for HeapSize
#define WG_MAX_ALLOCS 8192
static struct { uint32_t addr; uint32_t size; } s_alloc_sizes[WG_MAX_ALLOCS];
static int s_alloc_count = 0;

static void track_alloc(uint32_t addr, uint32_t size) {
    if (s_alloc_count < WG_MAX_ALLOCS) {
        s_alloc_sizes[s_alloc_count].addr = addr;
        s_alloc_sizes[s_alloc_count].size = size;
        s_alloc_count++;
    }
}

// Look up a tracked allocation's size by base address (most-recent first).
static uint32_t alloc_size_for(uint32_t addr) {
    for (int i = s_alloc_count - 1; i >= 0; i--)
        if (s_alloc_sizes[i].addr == addr) return s_alloc_sizes[i].size;
    return 0;
}

// Free list so VirtualFree/HeapFree actually RECLAIM memory. WineGlass's guest heap was
// a pure bump allocator that never freed, so the game's normal alloc/free churn (e.g. the
// UE4 asset buffer-grow: alloc bigger, memcpy, free old — repeated with climbing sizes)
// leaked the whole ~2.75GB 32-bit heap and OOM-fatal'd right before the UI renders. Path
// B caps the guest VA at 4GB so we can't just hand out a huge 64-bit heap; instead, put
// freed blocks back and reuse them.
#define WG_MAX_FREE 8192
static struct { uint32_t addr; uint32_t size; } s_free_blocks[WG_MAX_FREE];
static int s_free_count = 0;

static void wg_guest_free(uint32_t addr, uint32_t size) {
    if (!addr) return;
    if (!size) size = alloc_size_for(addr);
    if (!size) return;                       // unknown block — can't safely reclaim
    size = (size + 0xFFFu) & ~0xFFFu;
    if (s_free_count < WG_MAX_FREE) {
        s_free_blocks[s_free_count].addr = addr;
        s_free_blocks[s_free_count].size = size;
        s_free_count++;
    }
}

// First-fit reuse of a freed block for a new allocation (splitting a larger one). Returns
// 0 if nothing fits. The returned region is already mapped in blink from its prior life.
static uint32_t wg_free_list_take(uint32_t alloc) {
    // Round the take up to 64KB so a split leaves the remainder 64KB-aligned too — the
    // free blocks all start 64KB-aligned (VirtualAlloc granularity) and VirtualAlloc's
    // reuse depends on that alignment for FMallocBinned2's pool math.
    uint32_t take = (alloc + 0xFFFFu) & ~0xFFFFu;
    for (int i = 0; i < s_free_count; i++) {
        if (s_free_blocks[i].size >= take) {
            uint32_t a = s_free_blocks[i].addr;
            uint32_t rem = s_free_blocks[i].size - take;
            if (rem >= 0x10000u) {           // keep remainder (>=64KB) as a free block
                s_free_blocks[i].addr = a + take;
                s_free_blocks[i].size = rem;
            } else {
                s_free_blocks[i] = s_free_blocks[--s_free_count];   // consume whole block
            }
            return a;
        }
    }
    return 0;
}

static uint32_t lookup_alloc_size(uint32_t addr) {
    for (int i = s_alloc_count - 1; i >= 0; i--) {
        if (s_alloc_sizes[i].addr == addr)
            return s_alloc_sizes[i].size;
    }
    return 0;
}

// Bump-allocate `size` bytes of zeroed guest heap (shared with GlobalAlloc).
// Returns the guest address, or 0 on failure. Used by the CRT allocators that
// real DLLs (StdUtils, and eventually steam.exe) call.
// When Steam checks for a client package that isn't on disk, fetch it natively
// (host HTTPS) instead of letting the in-guest download reactor do it — the
// reactor is unreliable on our cooperative scheduler (stalls/crashes partway),
// while a native GET is rock-solid. `hostpath` is the mapped bottle path, e.g.
// .../drive_c/package/<name>. The CDN URL is /client/<name>, same basename.
// Returns true if the file is now present. Blocks the engine thread during the
// download (safe: engine runs off the UI thread).
static bool wg_try_native_package_fetch(const char *hostpath) {
    const char *p = strstr(hostpath, "/package/");
    if (!p) return false;
    const char *name = p + 9; // strlen("/package/")
    if (!name[0] || strchr(name, '/')) return false;   // must be directly in package/
    if (!strstr(name, ".zip")) return false;           // packages are .zip / .zip.vz
    // Skip Steam's control/bookkeeping files (not CDN downloads).
    if (strstr(name, ".writable") || strstr(name, ".installed") ||
        strstr(name, ".manifest") || strstr(name, "metrics")) return false;

    // Most package names end in "_<size>" — use it to verify. If the file is
    // already present at the right size, we're done (don't re-download). This
    // also lets us replace a wrong-size leftover from a prior reactor attempt.
    long expected = -1;
    const char *us = strrchr(name, '_');
    if (us && us[1]) { char *end = NULL; long v = strtol(us + 1, &end, 10);
                       if (end && *end == '\0') expected = v; }
    struct stat pst;
    if (stat(hostpath, &pst) == 0 && (expected < 0 || pst.st_size == expected))
        return true; // already downloaded (and correct size, if known)

    char url[600];
    snprintf(url, sizeof(url), "https://cdn.steamstatic.com/client/%s", name);
    WG_LOGW(TAG, "Native package fetch START: %s (expect %ld bytes)", name, expected);
    // Release the global thunk lock during the (long, blocking) download so other
    // guest threads' Win32 calls aren't frozen for the duration (real-threads).
    wg_thunk_block_begin();
    bool ok = wg_native_download(url, hostpath) != 0;
    wg_thunk_block_end();
    if (ok && expected >= 0 && stat(hostpath, &pst) == 0 && pst.st_size != expected) {
        WG_LOGW(TAG, "Native package fetch SIZE MISMATCH (%lld != %ld): %s",
                (long long)pst.st_size, expected, name);
        ok = false;
    }
    WG_LOGW(TAG, "Native package fetch %s: %s", ok ? "OK" : "FAILED", name);
    return ok;
}

// Steam opens C:\package\steam_client_win32.manifest for READ (a cache) BEFORE
// downloading the manifest over the reactor. The reactor download hits the
// GET-queue race (full handshake never queues the GET -> "http error 0" ->
// "needs to be online"). So native-fetch the manifest to that cache file when
// Steam looks for it; Steam then reads a valid manifest (signature accepted via
// the 0x4611E0 patch) and may skip the flaky reactor download. URL is
// /client/steam_client_win32 (same as the reactor would fetch).
static bool wg_try_native_manifest_fetch(const char *hostpath) {
    const char *base = strrchr(hostpath, '/');
    base = base ? base + 1 : hostpath;
    if (strcmp(base, "steam_client_win32.manifest") != 0) return false;
    struct stat st;
    if (stat(hostpath, &st) == 0 && st.st_size > 0) return true; // already fetched
    WG_LOGW(TAG, "Native manifest fetch START -> %s", base);
    wg_thunk_block_begin();   // don't freeze other guest threads during the download
    bool ok = wg_native_download("https://cdn.steamstatic.com/client/steam_client_win32",
                                 hostpath) != 0;
    wg_thunk_block_end();
    if (ok && stat(hostpath, &st) == 0 && st.st_size == 0) ok = false;
    WG_LOGW(TAG, "Native manifest fetch %s (%lld bytes)", ok ? "OK" : "FAILED",
            (stat(hostpath, &st) == 0) ? (long long)st.st_size : -1);
    return ok;
}

static uint32_t wg_guest_alloc(WGEngine *engine, uint32_t size) {
    if (size == 0) size = 1;
    if ((size & 0x80000000u) || size > 512u * 1024 * 1024) return 0;
    uint32_t alloc = (size + 0xFFF) & ~0xFFFu;
    // Reuse a reclaimed (VirtualFree'd) block first — keeps the bump pointer from
    // marching into OOM under the game's alloc/free churn. The block is already mapped;
    // re-zero it so it honors VirtualAlloc's zero-fill contract.
    uint32_t reuse = wg_free_list_take(alloc);
    if (reuse) {
        static signed char s_fm = -1;
        if (s_fm < 0) s_fm = getenv("WG_NO_FASTMEM") ? 0 : 1;
        if (!s_fm || !wg_blink_mem_set(engine->blink, reuse, 0, alloc)) {
            uint8_t *z = calloc(1, alloc);
            if (z) { wg_blink_write_mem(engine->blink, reuse, z, alloc); free(z); }
        }
        track_alloc(reuse, size);
        return reuse;
    }
    // Region 1 is 0x20000000..0x5F000000 (below the DLL/stack region at 0x60000000).
    // When it fills, jump to REGION 2 at 0xA0000000..0xF0000000 — the 2.5-4GB slice
    // is free (stacks/DLLs stay under ~0x80000000, main stack at 0x7FFF0000, image
    // at 0x140000000). This ~1.5GB extra keeps a full UE4 asset load from OOM-ing at
    // the old 1GB cap (the 23M-thunk endpoint) WITHOUT going 64-bit (which the 32-bit
    // arg-reading handlers would truncate). All addresses stay in uint32_t.
    if (s_heap_ptr + alloc > 0x5F000000u && s_heap_ptr < 0xA0000000u)
        s_heap_ptr = 0xA0000000u;                       // hop to region 2
    // The 64-bit HLT import-thunk table lives at WG_THUNK_BASE (0xDEAD0000), which is
    // INSIDE region 2 (0xA0000000..0xF0000000). A heap allocation overlapping it
    // overwrites the thunks, so every later import call (DeleteObject, ...) jumps into
    // heap data and executes garbage → the corruption cascade that stalled the UI init.
    // Keep the thunk region a permanent hole: never allocate across [BASE, BASE+0x20000).
    if (s_heap_ptr < (uint32_t)(WG_THUNK_BASE + 0x20000u) &&
        s_heap_ptr + alloc > (uint32_t)WG_THUNK_BASE)
        s_heap_ptr = (uint32_t)(WG_THUNK_BASE + 0x20000u);   // skip past the thunk hole
    // Region 2 runs to just under the 4GB guest-VA ceiling (Path B caps at 4GB). This
    // extra ~256MB above the old 0xF0000000 cap + the free-list reclaim keeps a full
    // UE4 asset load under the OOM line.
    uint32_t hi = (s_heap_ptr >= 0xA0000000u) ? 0xFFFF0000u : 0x5F000000u;
    if (s_heap_ptr + alloc > hi || s_heap_ptr + alloc < s_heap_ptr) {
        static int s_oom = 0;
        if (s_oom++ < 30) WG_LOGW(TAG, "★ wg_guest_alloc OOM #%d: heap_ptr=0x%X + alloc=0x%X > hi=0x%X — 32-bit guest heap EXHAUSTED (VirtualAlloc/HeapAlloc returns 0)", s_oom, s_heap_ptr, alloc, hi);
        return 0;
    }
    uint32_t addr = s_heap_ptr;
    uint8_t *zeros = calloc(1, alloc);
    if (!zeros) return 0;
    wg_blink_load_code(engine->blink, addr, zeros, alloc, 0);
    free(zeros);
    s_heap_ptr += alloc;
    s_heap_ptr = (s_heap_ptr + 0xFFF) & ~0xFFFu;
    track_alloc(addr, size);
    return addr;
}

// Like wg_guest_alloc but the returned base is aligned to `align` (power of 2).
// VirtualAlloc must return memory aligned to the OS allocation granularity
// (64KB on Windows): UE4's FMallocBinned2 masks pointers by that granularity to
// find pool headers, so a merely page-aligned base makes it read canaries from
// the wrong offset and (falsely) detect heap corruption.
static uint32_t wg_guest_alloc_aligned(WGEngine *engine, uint32_t size, uint32_t align) {
    if (align > 0x1000u) {
        uint32_t aligned = (s_heap_ptr + (align - 1)) & ~(align - 1);
        if (aligned >= s_heap_ptr) s_heap_ptr = aligned;   // skip forward to alignment
    }
    return wg_guest_alloc(engine, size);
}

// 64-bit guest heap for VirtualAlloc (the game's FMallocBinned2 pools). The 32-bit
// bump heap caps at ~1GB (0x5F000000), well under what a full UE4 asset load needs
// -> VirtualAlloc returns 0 -> FMalloc OOM -> the game fatals/exits (the 23M-thunk
// endpoint). blink is a 64-bit VM (wg_blink_load_code takes a u64 addr), and the
// runtime uses nothing above 0x140000000, so hand VirtualAlloc pools out of a huge
// 64-bit region far above everything. The game is 64-bit, so it uses these fine.
// Region 3: guest VA 0x100000000..0x200000000 (the 4..8GB upper half of Path B's 8GB
// linear region). Large VirtualAlloc pools live here so they don't exhaust the sub-4GB
// 32-bit heap. Capped at 8GB (the linear region end) — a reserve past it fails cleanly.
#define WG_HEAP64_BASE 0x100000000ULL
// Region-3 spans 4GB..~36GB of the linear space. It's ADDRESS SPACE only (lazy
// mmap) — physical cost is just the committed+touched pages — so a large window
// is cheap and gives the UE4 level load room despite exact-size reuse's address
// fragmentation. Run with WG_LINEAR_GB >= 40 so gsize covers this. Physical RAM
// (host ~24GB) is the real limit, not this window.
#define WG_HEAP64_END  0x1800000000ULL
static uint64_t s_heap64_ptr = WG_HEAP64_BASE;
// Region-3 free list + size tracking so VirtualFree of a large pool RECLAIMS it. Without
// this, the game's buffer-grow churn (alloc bigger, memcpy, free old) leaks region 3 too
// and 8GB still OOMs.
#define WG_MAX_ALLOC64 8192
static struct { uint64_t addr; uint64_t size; } s_alloc64[WG_MAX_ALLOC64]; static int s_alloc64_n = 0;
static struct { uint64_t addr; uint64_t size; } s_free64[WG_MAX_ALLOC64];  static int s_free64_n = 0;
static void wg_uncommit64(uint64_t addr, uint64_t size);  // fwd: clears commit bits
static void wg_track_alloc64(uint64_t addr, uint64_t size) {
    if (s_alloc64_n < WG_MAX_ALLOC64) { s_alloc64[s_alloc64_n].addr = addr; s_alloc64[s_alloc64_n].size = size; s_alloc64_n++; }
}
// True if [addr, addr+size) overlaps any currently-LIVE region-3 allocation.
// The reuse path must never hand back a block that overlaps a live alloc — doing
// so double-allocates the range and map64's zero-fill wipes the live block's
// contents (seen as FMallocBinned2 "realloc an unrecognized block ... canary==0").
// A free entry that overlaps a live alloc is corrupt (a stale/double free), so we
// drop it rather than trust it.
static uint64_t s_free64_dropped = 0;
static bool wg_overlaps_live64(uint64_t addr, uint64_t size) {
    uint64_t end = addr + size;
    for (int i = 0; i < s_alloc64_n; i++) {
        uint64_t a = s_alloc64[i].addr;
        uint64_t e = a + ((s_alloc64[i].size + 0xFFFULL) & ~0xFFFULL);
        if (addr < e && a < end) return true;
    }
    return false;
}
// Reuse a freed region-3 block. Returns its base and, via *out_real, the block's
// REAL size (the caller must track it so the whole block returns on the next free).
// NEVER SPLITS: a larger free block is handed out WHOLE for a smaller request (the
// extra tail stays part of this same allocation, unused). Splitting corrupted
// FMallocBinned2 — carving a freed 103MB block into a 46MB alloc + a 57MB remainder
// gave the SAME region-3 range to two allocations at inconsistent offsets. Exact-
// only matching (no split) avoided that but fragmented the address space so badly
// the window exhausted with only ~767 live allocs; whole-block reuse fixes both:
// one free block can satisfy any smaller size, keeping the bump pointer bounded.
static uint64_t wg_free_take64_r(uint64_t size, uint64_t *out_real) {
    uint64_t want = (size + 0xFFFULL) & ~0xFFFULL;
    for (int i = 0; i < s_free64_n; i++) {          // exact first (no wasted tail)
        if (s_free64[i].size == want) {
            uint64_t a = s_free64[i].addr;
            if (wg_overlaps_live64(a, want)) { s_free64[i] = s_free64[--s_free64_n]; s_free64_dropped++; i--; continue; }
            *out_real = s_free64[i].size;
            s_free64[i] = s_free64[--s_free64_n];
            return a;
        }
    }
    for (;;) {                                       // best-fit WHOLE block
        int best = -1;
        for (int i = 0; i < s_free64_n; i++)
            if (s_free64[i].size >= want && (best < 0 || s_free64[i].size < s_free64[best].size)) best = i;
        if (best < 0) return 0;
        uint64_t a = s_free64[best].addr, sz = s_free64[best].size;
        if (wg_overlaps_live64(a, sz)) { s_free64[best] = s_free64[--s_free64_n]; s_free64_dropped++; continue; }
        *out_real = sz;
        s_free64[best] = s_free64[--s_free64_n];
        return a;
    }
}
static void wg_guest_free64(uint64_t addr) {
    if (addr < WG_HEAP64_BASE) return;
    uint64_t size = 0;
    // Find the live alloc AND REMOVE it (swap-with-last). Previously this only
    // read the size and left the entry in place, so s_alloc64_n grew by one on
    // every alloc/free cycle (the reuse path re-appends too). A level load
    // churns the same ~100 88MB buffers thousands of times, so s_alloc64
    // overflowed WG_MAX_ALLOC64; past that, wg_track_alloc64 silently dropped
    // new allocs, their frees found no size, nothing was reclaimed, and the
    // region-3 bump pointer climbed to the 20GB cap and fell back to the (tiny)
    // 32-bit heap -> OOM. Removing on free keeps s_alloc64_n = live count.
    for (int i = s_alloc64_n - 1; i >= 0; i--) if (s_alloc64[i].addr == addr) {
        size = s_alloc64[i].size;
        s_alloc64[i] = s_alloc64[--s_alloc64_n];
        break;
    }
    if (!size) return;
    size = (size + 0xFFFULL) & ~0xFFFULL;
    // Released pages return to the OS: mark them uncommitted so that when this
    // block is handed out again, its pages are re-zeroed on commit (Windows gives
    // zeroed memory for a fresh commit of a re-reserved range).
    wg_uncommit64(addr, size);
    // Duplicate-free guard: never let the same base sit in the free list twice
    // (a double VirtualFree would otherwise hand the range out to two allocs).
    for (int i = 0; i < s_free64_n; i++) if (s_free64[i].addr == addr) return;
    if (s_free64_n < WG_MAX_ALLOC64) { s_free64[s_free64_n].addr = addr; s_free64[s_free64_n].size = size; s_free64_n++; }
}
// Reserve address space only (NO backing map) — a MEM_RESERVE, so a multi-GB
// reservation costs nothing until pages are committed. Reuses a reclaimed block first.
static uint64_t wg_guest_reserve64(uint64_t size, uint64_t align) {
    if (size == 0) size = 1;
    if (align < 0x1000ULL) align = 0x1000ULL;
    uint64_t real = 0;
    uint64_t reuse = wg_free_take64_r((size + 0xFFFULL) & ~0xFFFULL, &real);
    // Track the REAL block size (>= requested) so the whole block returns on free.
    if (reuse) { wg_track_alloc64(reuse, real); return reuse; }
    s_heap64_ptr = (s_heap64_ptr + (align - 1)) & ~(align - 1);
    uint64_t addr = s_heap64_ptr;
    uint64_t alloc = (size + 0xFFFULL) & ~0xFFFULL;
    if (addr + alloc > WG_HEAP64_END) return 0;   // region 3 full (won't fit the 8GB map)
    s_heap64_ptr += alloc;
    s_heap64_ptr = (s_heap64_ptr + 0xFFFULL) & ~0xFFFULL;
    wg_track_alloc64(addr, size);
    return addr;
}
// Region-3 commit bitmap: 1 bit per 4KB page over [WG_HEAP64_BASE, WG_HEAP64_END).
// Windows VirtualAlloc(MEM_COMMIT) zeroes a page only on its FIRST commit; a
// redundant commit of already-committed pages is a no-op that must NOT re-zero
// them. FMallocBinned2's OS page cache re-commits live pool ranges, so our old
// unconditional zero wiped canaries across the whole region-3 pool space
// ("FMallocBinned2 realloc an unrecognized block ... canary==0"). Track commit
// state so we zero each page exactly once per commit-cycle.
#define WG_PAGE64 0x1000ULL
static uint8_t *s_commit_bm = NULL;
static size_t wg_bm_bytes(void) { return (size_t)(((WG_HEAP64_END - WG_HEAP64_BASE) / WG_PAGE64 + 7) / 8); }
static bool wg_page_committed(uint64_t pg) { return s_commit_bm && (s_commit_bm[pg >> 3] & (1u << (pg & 7))); }
static void wg_page_mark(uint64_t pg, bool v) {
    if (!s_commit_bm) return;
    if (v) s_commit_bm[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    else   s_commit_bm[pg >> 3] &= (uint8_t)~(1u << (pg & 7));
}
static void wg_zero_range64(WGEngine *engine, uint64_t addr, uint64_t len) {
    uint64_t off = 0;
    while (off < len) {
        uint64_t chunk = len - off; if (chunk > 0x400000ULL) chunk = 0x400000ULL;
        uint8_t *zeros = calloc(1, (size_t)chunk);
        if (!zeros) return;
        wg_blink_load_code(engine->blink, addr + off, zeros, (uint32_t)chunk, 0);
        free(zeros);
        off += chunk;
    }
}
// Commit: back [addr,addr+size) with zeroed pages, but zero ONLY the pages not
// already committed (see above). Returns true on success.
static bool wg_guest_map64(WGEngine *engine, uint64_t addr, uint64_t size) {
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end = (addr + size + 0xFFFULL) & ~0xFFFULL;
    if (!s_commit_bm) s_commit_bm = calloc(1, wg_bm_bytes());
    // Outside the tracked region (or bitmap alloc failed): old unconditional zero.
    if (!s_commit_bm || start < WG_HEAP64_BASE || end > WG_HEAP64_END) {
        wg_zero_range64(engine, start, end - start);
        return true;
    }
    uint64_t p = start;
    while (p < end) {
        uint64_t pg = (p - WG_HEAP64_BASE) / WG_PAGE64;
        if (wg_page_committed(pg)) { p += WG_PAGE64; continue; }  // live page: preserve it
        uint64_t run = p;
        while (p < end) {
            uint64_t pg2 = (p - WG_HEAP64_BASE) / WG_PAGE64;
            if (wg_page_committed(pg2)) break;
            wg_page_mark(pg2, true);
            p += WG_PAGE64;
        }
        wg_zero_range64(engine, run, p - run);   // zero the freshly-committed run
    }
    return true;
}
// Decommit / release: mark pages uncommitted so a later commit re-zeroes them
// (matches Windows: memory read after decommit+recommit comes back zeroed).
static void wg_uncommit64(uint64_t addr, uint64_t size) {
    if (!s_commit_bm) return;
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end = (addr + size + 0xFFFULL) & ~0xFFFULL;
    if (start < WG_HEAP64_BASE) start = WG_HEAP64_BASE;
    if (end > WG_HEAP64_END) end = WG_HEAP64_END;
    for (uint64_t p = start; p < end; p += WG_PAGE64)
        wg_page_mark((p - WG_HEAP64_BASE) / WG_PAGE64, false);
}

// ===== nsDialogs plugin emulation =======================================
// NSIS plugins are invoked LoadLibrary + GetProcAddress + call with the ABI:
//   void Export(HWND parent, int string_size, TCHAR *vars, stack_t **top, void*)
// (stdcall, 5 args). Strings pass through the NSIS stack: a singly linked list
// of inline TCHAR buffers. Unicode build: stack_t = { u32 next; u16 text[ss+1] }.
// We pop the export's args and push results, then build the page's controls
// through the same child-control path CreateWindowExW uses, so the Welcome/
// Finish/custom pages render like the built-in template pages.
static uint32_t s_nsd_page = 0;        // current nsDialogs inner page hwnd
static uint32_t s_nsd_next_id = 2200;  // synthetic control ids
// True while paused inside nsDialogs::Show. The guest is suspended cleanly right
// after Show returned, so the Next tap must RESUME (let NSIS continue its natural
// page advance) rather than inject a WM_COMMAND onto the half-run WM_INITDIALOG.
static bool s_nsd_show_pause = false;

static bool nsis_pop(WGEngine *e, uint32_t topp, int ssize, uint16_t *out, int outmax) {
    uint32_t head = 0;
    wg_blink_read_mem(e->blink, topp, &head, 4);
    if (!head) { if (out && outmax) out[0] = 0; return false; }
    uint32_t next = 0;
    wg_blink_read_mem(e->blink, head, &next, 4);
    int n = ssize + 1; if (n > outmax) n = outmax; if (n < 1) n = 1;
    wg_blink_read_mem(e->blink, head + 4, out, n * 2);
    if (outmax) out[outmax - 1] = 0;
    wg_blink_write_mem(e->blink, topp, &next, 4);
    return true;
}
static int nsis_pop_int(WGEngine *e, uint32_t topp, int ssize) {
    uint16_t b[64] = {0}; nsis_pop(e, topp, ssize, b, 64);
    char a[64]; int i; for (i = 0; i < 63 && b[i]; i++) a[i] = (char)b[i]; a[i] = 0;
    return (int)strtol(a, NULL, 0);
}
static void nsis_push_int(WGEngine *e, uint32_t topp, int ssize, int v) {
    if (ssize < 1) ssize = 1024;
    uint32_t node = wg_guest_alloc(e, 4 + (uint32_t)(ssize + 1) * 2);
    if (!node) return;
    uint32_t head = 0; wg_blink_read_mem(e->blink, topp, &head, 4);
    wg_blink_write_mem(e->blink, node, &head, 4);
    char a[32]; snprintf(a, sizeof a, "%d", v);
    uint16_t w[32]; int i; for (i = 0; a[i]; i++) w[i] = (uint8_t)a[i]; w[i] = 0;
    wg_blink_write_mem(e->blink, node + 4, w, (i + 1) * 2);
    wg_blink_write_mem(e->blink, topp, &node, 4);
}

// Parse an nsDialogs coordinate string: plain px, "<n>u" dialog units, or
// "<n>%" percent of the page extent; negative anchors from the far edge.
static int nsd_coord(const uint16_t *s, int extent_px, int unit_num, int unit_den) {
    char a[32]; int i; for (i = 0; i < 31 && s[i]; i++) a[i] = (char)s[i]; a[i] = 0;
    int len = (int)strlen(a); bool pct = false, du = false;
    if (len && a[len-1] == '%') { pct = true; a[--len] = 0; }
    else if (len && (a[len-1] == 'u' || a[len-1] == 'U')) { du = true; a[--len] = 0; }
    long v = strtol(a, NULL, 0);
    int px = pct ? (int)(v * extent_px / 100)
           : du  ? (int)(v * unit_num / unit_den) : (int)v;
    if (px < 0) px += extent_px;   // negative = relative to right/bottom edge
    return px;
}

static uint16_t nsd_class(const uint16_t *cls16, uint32_t style, bool *is_bitmap) {
    char c[40]; int i;
    for (i = 0; i < 39 && cls16[i]; i++) c[i] = (char)tolower((unsigned char)cls16[i]);
    c[i] = 0;
    *is_bitmap = false;
    if (strstr(c, "button")) return 0x0080;
    if (strstr(c, "edit"))   return 0x0081;
    if ((style & 0x0F) == 0x0E /*SS_BITMAP*/) *is_bitmap = true;
    return 0x0082; // STATIC default (labels, bitmaps, links)
}

// Dispatch one nsDialogs export. Returns the value to leave in EAX.
static uint32_t handle_nsdialogs(WGEngine *engine, const char *exp, uint32_t *args) {
    uint32_t parent = args[0];
    int ssize = (int)args[1]; if (ssize <= 0 || ssize > 8192) ssize = 1024;
    uint32_t topp = args[3];   // stack_t **stacktop

    if (strcasecmp(exp, "Create") == 0) {
        int phid = nsis_pop_int(engine, topp, ssize);   // placeholder control id
        int px = 0, py = 0, pw = 480, ph = 320;
        int32_t cw = 0, chh = 0; wg_wm_get_client(parent, &cw, &chh);
        WGDlgCtrl *ph_ctrl = wg_find_ctrl(parent, (uint32_t)phid);
        if (!ph_ctrl) ph_ctrl = wg_find_ctrl(parent, 1018);
        if (ph_ctrl) {
            float sx = ph_ctrl->dlg_cx ? (float)cw / ph_ctrl->dlg_cx : 1.0f;
            float sy = ph_ctrl->dlg_cy ? (float)chh / ph_ctrl->dlg_cy : 1.0f;
            px = (int)(ph_ctrl->x * sx); py = (int)(ph_ctrl->y * sy);
            pw = (int)(ph_ctrl->cx * sx); ph = (int)(ph_ctrl->cy * sy);
        } else if (cw > 0 && chh > 0) { pw = cw; ph = chh; }
        uint16_t title[1] = {0};
        uint32_t hwnd = wg_wm_create_window(0, 0, title, 0x50000000, px, py, pw, ph, parent);
        wg_retire_inner_page(hwnd);   // drop the previous page so it stops painting
        s_nsd_page = hwnd; s_page_hwnd = hwnd;
        WG_LOGI(TAG, "nsDialogs::Create(ph=%d) -> page 0x%X @ (%d,%d %dx%d)",
                phid, hwnd, px, py, pw, ph);
        nsis_push_int(engine, topp, ssize, (int)hwnd);
        return hwnd;
    }
    if (strcasecmp(exp, "CreateControl") == 0 || strcasecmp(exp, "CreateItem") == 0) {
        uint16_t cls[40]={0}, ss[24]={0}, sex[24]={0};
        uint16_t sx_[24]={0}, sy_[24]={0}, sw_[24]={0}, sh_[24]={0}, text[256]={0};
        nsis_pop(engine, topp, ssize, cls,  40);
        nsis_pop(engine, topp, ssize, ss,   24);
        nsis_pop(engine, topp, ssize, sex,  24);
        nsis_pop(engine, topp, ssize, sx_,  24);
        nsis_pop(engine, topp, ssize, sy_,  24);
        nsis_pop(engine, topp, ssize, sw_,  24);
        nsis_pop(engine, topp, ssize, sh_,  24);
        nsis_pop(engine, topp, ssize, text, 256);
        char sb[24]; int i; for (i=0;i<23&&ss[i];i++) sb[i]=(char)ss[i]; sb[i]=0;
        uint32_t style = (uint32_t)strtoul(sb, NULL, 0);
        int32_t cw=0, chh=0; wg_wm_get_client(s_nsd_page, &cw, &chh);
        int x = nsd_coord(sx_, cw,  3, 2);    // ~1.5 px per dialog unit (x)
        int y = nsd_coord(sy_, chh, 13, 8);   // ~1.6 px per dialog unit (y)
        int w = nsd_coord(sw_, cw,  3, 2);
        int h = nsd_coord(sh_, chh, 13, 8);
        bool is_bmp = false;
        uint16_t cc = nsd_class(cls, style, &is_bmp);
        uint32_t id = s_nsd_next_id++;
        int before = s_ctrl_count;
        wg_register_child_control(s_nsd_page, id, style, cc, x, y, w, h, text);
        // Return a real control handle (WG_CTRL_HWND_BASE + index), not the raw
        // id — that's what GetDlgItem/SendMessage(STM_SETIMAGE)/SetWindowText
        // resolve through, so the script can set the control's bitmap/text later.
        uint32_t ctrl_hwnd = 0;
        if (s_ctrl_count > before) {
            int idx = s_ctrl_count - 1;
            if (is_bmp) s_ctrls[idx].is_bitmap = true;
            ctrl_hwnd = WG_CTRL_HWND_BASE + (uint32_t)idx;
        }
        char ca[40]; for (i=0;i<39&&cls[i];i++) ca[i]=(char)cls[i]; ca[i]=0;
        char ta[64]; for (i=0;i<63&&text[i];i++) ta[i]=text[i]<128?(char)text[i]:'?'; ta[i]=0;
        WG_LOGI(TAG, "nsDialogs::CreateControl(%s s=0x%X @%d,%d %dx%d) hwnd=0x%X '%s'",
                ca, style, x, y, w, h, ctrl_hwnd, ta);
        nsis_push_int(engine, topp, ssize, (int)ctrl_hwnd);
        return ctrl_hwnd;
    }
    if (strcasecmp(exp, "Show") == 0) {
        // Native welcome bitmap: MUI loads it via System::Call -> LoadImage, but
        // by load time NSIS has deleted the bmp's temp dir, so the script's image
        // fails (control ends up SS_BITMAP with no image). Here we attach the
        // cached wizard .bmp ourselves: first to an existing empty bitmap control,
        // else (if the page reserved a left strip) we add one.
        const char *wb = wg_files_wizard_bmp();
        if (s_nsd_page && wb) {
            int32_t pw = 0, ph = 0; wg_wm_get_client(s_nsd_page, &pw, &ph);
            bool attached = false, has_bmp = false; int min_x = pw;
            for (int i = 0; i < s_ctrl_count; i++) {
                if (s_ctrls[i].hwnd != s_nsd_page) continue;
                if (s_ctrls[i].is_bitmap) {
                    has_bmp = true;
                    if (!s_ctrls[i].hbitmap) {       // empty placeholder -> fill it
                        uint32_t hb = wg_bitmap_load_file(wb);
                        if (hb) { s_ctrls[i].hbitmap = hb; attached = true;
                            WG_LOGI(TAG, "welcome bitmap '%s' -> existing ctrl", wb); }
                    } else attached = true;
                } else if (s_ctrls[i].x < min_x) min_x = s_ctrls[i].x;
            }
            if (!attached && !has_bmp && min_x >= 40 && min_x < pw && ph > 0) {
                uint32_t hb = wg_bitmap_load_file(wb);
                if (hb) {
                    uint32_t id = s_nsd_next_id++;
                    int before = s_ctrl_count;
                    wg_register_child_control(s_nsd_page, id, 0x0E /*SS_BITMAP*/,
                                              0x0082, 0, 0, min_x, ph, NULL);
                    if (s_ctrl_count > before) {
                        s_ctrls[s_ctrl_count - 1].is_bitmap = true;
                        s_ctrls[s_ctrl_count - 1].hbitmap = hb;
                    }
                    WG_LOGI(TAG, "welcome bitmap '%s' -> new left strip %dx%d", wb, min_x, ph);
                }
            }
        }
        if (s_nsd_page) wg_render_dialog(engine, s_nsd_page);
        // Go modal here so the user actually sees this page. NSIS auto-advances
        // through custom pages, so without a pause it races to the next page. The
        // guest is suspended cleanly after Show returns; the Next tap RESUMES
        // (see wg_engine_dialog_command) so NSIS continues to the next page.
        engine->state = WG_ENGINE_PAUSED;
        s_nsd_show_pause = true;
        WG_LOGI(TAG, "nsDialogs::Show page=0x%X — modal, waiting", s_nsd_page);
        return 0;
    }
    // Remaining exports (SetImage/SetUserData/On*/SelectFileDialog/timers) are
    // not needed to render the page text; log so we can see what a real page
    // actually calls, then flesh out in the next iteration.
    WG_LOGI(TAG, "nsDialogs::%s (stub)", exp);
    return 0;
}

// True for the nsDialogs plugin exports we emulate (routed in GetProcAddress).
static bool is_nsdialogs_export(const char *n) {
    static const char *exps[] = {
        "Create", "CreateControl", "CreateItem", "Show", "SetImage", "SetIcon",
        "SetUserData", "GetUserData", "OnClick", "OnChange", "OnNotify", "OnBack",
        "SetRTL", "CreateTimer", "KillTimer", "SelectFileDialog",
        "SelectFolderDialog", "SetButtonLong", NULL };
    for (int i = 0; exps[i]; i++) if (strcmp(n, exps[i]) == 0) return true;
    return false;
}

// ---- Fake COM IShellLink / IPersistFile -----------------------------------
// NSIS CreateShortcut does CoCreateInstance(IShellLink) -> Set*/QueryInterface
// (IPersistFile) -> Save. Failing CoCreateInstance makes it log "Error creating
// shortcut". Shortcuts are meaningless on iOS, but we hand back a minimal COM
// object whose methods all return S_OK (QueryInterface yields the IPersistFile)
// so the wizard finishes cleanly. Method thunks are registered with the correct
// stdcall arg counts (so the stack stays balanced); all but QueryInterface just
// return S_OK via default_ret=0.
static uint32_t s_com_qi, s_com_a1, s_com_a2, s_com_a3, s_com_a4, s_com_a5;
static uint32_t s_com_shelllink = 0, s_com_persistfile = 0;

// Real path of a program the guest asked to launch (the Steam bootstrapper);
// the app chain-loads it after the current program exits.
static char s_pending_exec[1024] = {0};

// Count of recovered null indirect calls on the main thread (see the halt
// handler). Reset per PE load. Lets a run that keeps hitting uninitialized
// function pointers keep going instead of dying on the first one.
static uint64_t s_null_call_recover = 0;
// Spin-guard: a tight loop that keeps calling the same null pointer would
// recover forever (and hang the app on iOS). Track the last recovered return
// address; if the same site recovers too many times in a row, stop recovering
// and let the fault surface so the run terminates instead of spinning.
static uint64_t s_recover_last_addr = 0;
static uint32_t s_recover_streak    = 0;
static uint64_t s_recover_total     = 0;
#define WG_RECOVER_SPIN_LIMIT  20000
#define WG_RECOVER_TOTAL_LIMIT 2000000   // global cap so a MULTI-address fatal-
                                         // handler loop can't spin forever (on
                                         // device it would hang the app).
// Returns false if this recovery would exceed the per-site streak OR the global
// total (caller should NOT recover, letting the fault surface). Updates state.
static bool wg_recover_ok(uint64_t ret_addr) {
    if (++s_recover_total > WG_RECOVER_TOTAL_LIMIT) return false;
    if (ret_addr == s_recover_last_addr) {
        if (++s_recover_streak > WG_RECOVER_SPIN_LIMIT) return false;
    } else {
        s_recover_last_addr = ret_addr;
        s_recover_streak = 1;
    }
    return true;
}

// Fixed guest scratch pages. Legacy low addresses work for 32-bit / small PEs,
// but a rebased 64-bit image (e.g. Visage: 0x400000..~0x3EB5000, 62MB) spans
// right over them, so the scratch maps would clobber the game's own .text.
// For such images these are relocated to a region ABOVE the image at load time
// (see wg_place_scratch). 0 base = legacy layout.
static uint32_t s_scratch_base = 0;
static uint32_t s_cmdline_page = 0x00A00000u;   // GetCommandLineW/A page
static char     s_cmdline_extra[128] = {0};     // switches appended to the guest cmdline
static uint32_t s_tramp_addr   = 0x00C30000u;   // x64 _initterm trampoline
static uint32_t s_gai_base     = 0x00B00000u;   // getaddrinfo result scratch (1MB)

// Collapse "." and ".." segments in a Windows path, in place (ASCII —
// PathCanonicalize semantics, enough for the launcher-built exe paths).
// The result never grows, so the caller's buffer always fits.
static void wg_path_canon_a(char *p) {
    char tmp[1040];
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    bool lead = (tmp[0] == '\\' || tmp[0] == '/');
    char *segs[64];
    int ns = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "\\/", &save); t; t = strtok_r(NULL, "\\/", &save)) {
        if (strcmp(t, ".") == 0) continue;
        if (strcmp(t, "..") == 0) {
            // pop the previous segment, but never the drive ("C:")
            if (ns > 0 && !(ns == 1 && segs[0][strlen(segs[0]) - 1] == ':')) ns--;
            continue;
        }
        if (ns < 64) segs[ns++] = t;
    }
    char out[1040];
    int oi = 0;
    if (lead) out[oi++] = '\\';
    for (int s = 0; s < ns; s++) {
        int len = (int)strlen(segs[s]);
        if (oi + len + 2 >= (int)sizeof(out)) break;
        if (s) out[oi++] = '\\';
        memcpy(out + oi, segs[s], len);
        oi += len;
    }
    out[oi] = 0;
    if (oi == 2 && out[1] == ':') { out[2] = '\\'; out[3] = 0; }  // bare drive
    strcpy(p, out);
}

// FindResource: walk the main image's mapped .rsrc directory in guest memory —
// type dir, then name dir, then first language entry. Handles integer IDs
// (MAKEINTRESOURCE) and string names. Returns the guest VA of the
// IMAGE_RESOURCE_DATA_ENTRY (serves as the HRSRC), or 0 if not found.
// UE4's bootstrap launcher reads its game-exe path from RCDATA #201 this way.
static uint32_t wg_find_resource(WGEngine *engine, uint32_t name_arg,
                                 uint32_t type_arg, bool wide) {
    if (!engine->pe_image) return 0;
    uint32_t rsrc_rva = 0;
    for (int i = 0; i < engine->pe_image->num_sections; i++) {
        if (strcmp(engine->pe_image->sections[i].name, ".rsrc") == 0) {
            rsrc_rva = engine->pe_image->sections[i].virtual_address;
            break;
        }
    }
    if (!rsrc_rva) return 0;
    uint32_t base = (uint32_t)engine->pe_image->image_base + rsrc_rva;

    uint32_t dir = base;                       // current directory table VA
    uint32_t want[2] = { type_arg, name_arg }; // level 0: type, 1: name
    for (int level = 0; level < 3; level++) {
        uint16_t counts[2] = {0, 0};           // named, id
        wg_blink_read_mem(engine->blink, dir + 12, counts, 4);
        int total = counts[0] + counts[1];
        if (total <= 0 || total > 4096) return 0;
        uint32_t found = 0;
        for (int i = 0; i < total; i++) {
            uint32_t ent[2] = {0, 0};          // Name, OffsetToData
            wg_blink_read_mem(engine->blink, dir + 16 + (uint32_t)i * 8, ent, 8);
            bool match = false;
            if (level == 2) {
                match = true;                  // take the first language
            } else if (want[level] <= 0xFFFF) {
                match = !(ent[0] & 0x80000000u) && (ent[0] & 0xFFFF) == want[level];
            } else if (ent[0] & 0x80000000u) {
                // string-named entry vs guest string (case-insensitive)
                uint32_t nvo = base + (ent[0] & 0x7FFFFFFFu);
                uint16_t ln = 0;
                wg_blink_read_mem(engine->blink, nvo, &ln, 2);
                if (ln > 63) ln = 63;
                uint16_t rname[64] = {0}, gname[64] = {0};
                wg_blink_read_mem(engine->blink, nvo + 2, rname, (uint32_t)ln * 2);
                if (wide) {
                    wg_blink_read_mem(engine->blink, want[level], gname, 126);
                } else {
                    char an[64] = {0};
                    wg_blink_read_mem(engine->blink, want[level], an, 63);
                    for (int k = 0; k < 63 && an[k]; k++) gname[k] = (uint8_t)an[k];
                }
                match = true;
                for (int k = 0; k <= ln; k++) {
                    uint16_t a = (k < ln) ? rname[k] : 0, b = gname[k];
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (a != b) { match = false; break; }
                }
            }
            if (match) { found = ent[1]; break; }
        }
        if (!found) return 0;
        if (found & 0x80000000u) {
            if (level == 2) return 0;          // deeper than type/name/lang
            dir = base + (found & 0x7FFFFFFFu);
        } else {
            return base + found;               // IMAGE_RESOURCE_DATA_ENTRY VA
        }
    }
    return 0;
}

static void wg_build_fake_com(WGEngine *engine) {
    WGDllMapper *m = engine->dll_mapper;
    if (!s_com_qi) {   // register method thunks once (mapper persists across loads)
        s_com_qi = (uint32_t)wg_dll_mapper_register(m, "COM", "__comQI", NULL, 3);
        s_com_a1 = (uint32_t)wg_dll_mapper_register(m, "COM", "__comA1", NULL, 1);
        s_com_a2 = (uint32_t)wg_dll_mapper_register(m, "COM", "__comA2", NULL, 2);
        s_com_a3 = (uint32_t)wg_dll_mapper_register(m, "COM", "__comA3", NULL, 3);
        s_com_a4 = (uint32_t)wg_dll_mapper_register(m, "COM", "__comA4", NULL, 4);
        s_com_a5 = (uint32_t)wg_dll_mapper_register(m, "COM", "__comA5", NULL, 5);
    }
    if (s_com_shelllink) return;   // guest objects already built this run
    // IShellLinkW vtable (21 slots): QI, AddRef, Release, GetPath, GetIDList,
    // SetIDList, Get/SetDescription, Get/SetWorkingDirectory, Get/SetArguments,
    // Get/SetHotkey, Get/SetShowCmd, GetIconLocation, SetIconLocation,
    // SetRelativePath, Resolve, SetPath.
    uint32_t SL[21] = {
        s_com_qi, s_com_a1, s_com_a1,
        s_com_a5, s_com_a2, s_com_a2,
        s_com_a3, s_com_a2, s_com_a3, s_com_a2,
        s_com_a3, s_com_a2, s_com_a2, s_com_a2,
        s_com_a2, s_com_a2, s_com_a4, s_com_a3,
        s_com_a3, s_com_a3, s_com_a2,
    };
    // IPersistFile vtable (9 slots): QI, AddRef, Release, GetClassID, IsDirty,
    // Load, Save, SaveCompleted, GetCurFile.
    uint32_t PF[9] = {
        s_com_qi, s_com_a1, s_com_a1, s_com_a2,
        s_com_a1, s_com_a3, s_com_a3, s_com_a2, s_com_a2,
    };
    uint32_t slv = wg_guest_alloc(engine, sizeof SL);
    uint32_t pfv = wg_guest_alloc(engine, sizeof PF);
    s_com_shelllink   = wg_guest_alloc(engine, 8);
    s_com_persistfile = wg_guest_alloc(engine, 8);
    wg_blink_write_mem(engine->blink, slv, SL, sizeof SL);
    wg_blink_write_mem(engine->blink, pfv, PF, sizeof PF);
    wg_blink_write_mem(engine->blink, s_com_shelllink,   &slv, 4);
    wg_blink_write_mem(engine->blink, s_com_persistfile, &pfv, 4);
}

// ===== Guest exception (SEH) dispatch ===================================
// On a guest memory fault we synthesize a Windows STATUS_ACCESS_VIOLATION and
// walk the fs:[0] EXCEPTION_REGISTRATION chain, calling each handler until one
// handles it. MSVC's _except_handler3 calls RtlUnwind (our thunk) then jumps
// into __except, so a handled exception never returns to WG_SEH_SENTINEL.
// Enables Steam's bootstrapper to recover from the NULL-connection deref the
// way it does on Windows (instead of our zero-page map masking it).
#define WG_CTX_SIZE  0x2CC
#define WG_CTX_FLAGS 0x00
#define WG_CTX_EDI   0x9C
#define WG_CTX_ESI   0xA0
#define WG_CTX_EBX   0xA4
#define WG_CTX_EDX   0xA8
#define WG_CTX_ECX   0xAC
#define WG_CTX_EAX   0xB0
#define WG_CTX_EBP   0xB4
#define WG_CTX_EIP   0xB8
#define WG_CTX_CS    0xBC
#define WG_CTX_EFL   0xC0
#define WG_CTX_ESP   0xC4
#define WG_CTX_SS    0xC8

static bool     s_enable_guest_seh = false; // fault-hook auto-dispatch (off:
                                            // page 0 stays mapped; we trigger
                                            // SEH surgically at the send site)
static bool     s_seh_trigger_send = false; // raise AV at Steam's send-on-NULL
                                            // (off: Steam's handlers don't catch
                                            // it — the NULL conn is upstream)
static int      s_seh_send_triggers = 0;    // limit re-triggers (avoid loops)
static bool     s_seh_active = false;
static uint32_t s_seh_frame  = 0;   // current EXCEPTION_REGISTRATION_RECORD
static uint32_t s_seh_excrec = 0;   // guest EXCEPTION_RECORD
static uint32_t s_seh_ctx    = 0;   // guest CONTEXT
static uint32_t s_seh_dispctx= 0;   // DispatcherContext scratch
static int      s_seh_depth  = 0;   // nested-fault guard

static void wg_seh_call_handler(WGEngine *engine, uint32_t handler, uint32_t frame) {
    // handler(ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext)
    // -> returns disposition in EAX to WG_SEH_SENTINEL.
    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
    uint32_t new_rsp = esp - 20;
    uint32_t sd[5] = { WG_SEH_SENTINEL, s_seh_excrec, frame, s_seh_ctx, s_seh_dispctx };
    wg_blink_write_mem(engine->blink, new_rsp, sd, 20);
    wg_blink_set_reg(engine->blink, 4, new_rsp);
    wg_blink_set_rip(engine->blink, handler);
    wg_blink_set_reg(engine->blink, 0, 0);
}

// Raise `code` (STATUS_ACCESS_VIOLATION) at fault_rip and dispatch it to the
// guest SEH chain. Returns true if a handler chain exists (engine keeps RUNNING).
static bool wg_raise_guest_exception(WGEngine *engine, uint32_t code,
                                     uint32_t fault_addr, uint32_t fault_rip,
                                     bool is_write) {
    if (s_seh_depth > 8) return false; // runaway fault loop
    uint32_t seh = 0;
    wg_blink_read_mem(engine->blink, s_main_teb + 0, &seh, 4);
    if (seh <= 0x1000u || seh >= 0xFFFFFFFEu) return false; // no SEH chain

    uint32_t ctx = wg_guest_alloc(engine, WG_CTX_SIZE);
    uint32_t rec = wg_guest_alloc(engine, 0x50);
    uint32_t dispctx = wg_guest_alloc(engine, 16);
    if (!ctx || !rec || !dispctx) return false;

    uint32_t flags = 0x10007 /*CONTEXT_FULL*/, t;
    wg_blink_write_mem(engine->blink, ctx + WG_CTX_FLAGS, &flags, 4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,7); wg_blink_write_mem(engine->blink,ctx+WG_CTX_EDI,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,6); wg_blink_write_mem(engine->blink,ctx+WG_CTX_ESI,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,3); wg_blink_write_mem(engine->blink,ctx+WG_CTX_EBX,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,2); wg_blink_write_mem(engine->blink,ctx+WG_CTX_EDX,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,1); wg_blink_write_mem(engine->blink,ctx+WG_CTX_ECX,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,0); wg_blink_write_mem(engine->blink,ctx+WG_CTX_EAX,&t,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,5); wg_blink_write_mem(engine->blink,ctx+WG_CTX_EBP,&t,4);
    wg_blink_write_mem(engine->blink,ctx+WG_CTX_EIP,&fault_rip,4);
    t=(uint32_t)wg_blink_get_reg(engine->blink,4); wg_blink_write_mem(engine->blink,ctx+WG_CTX_ESP,&t,4);
    t=0x202; wg_blink_write_mem(engine->blink,ctx+WG_CTX_EFL,&t,4);
    t=0x1b;  wg_blink_write_mem(engine->blink,ctx+WG_CTX_CS,&t,4);
    t=0x23;  wg_blink_write_mem(engine->blink,ctx+WG_CTX_SS,&t,4);

    uint32_t er[7] = { code, 0, 0, fault_rip, 2, (uint32_t)(is_write?1:0), fault_addr };
    wg_blink_write_mem(engine->blink, rec, er, 28);

    s_seh_excrec = rec; s_seh_ctx = ctx; s_seh_dispctx = dispctx;
    s_seh_frame = seh; s_seh_active = true; s_seh_depth++;

    uint32_t handler = 0;
    wg_blink_read_mem(engine->blink, seh + 4, &handler, 4);
    WG_LOGW(TAG, "SEH: raise 0x%X addr=0x%X rip=0x%X -> handler 0x%X frame 0x%X",
            code, fault_addr, fault_rip, handler, seh);
    if (handler <= 0x1000u) { s_seh_active = false; return false; }
    wg_seh_call_handler(engine, handler, seh);
    return true;
}

// DIAG: trap steam.exe's OpenSSL ERR_put_error(lib,func,reason,file,line) to
// surface the exact error that aborts the TLS handshake, as it is generated
// (the spew-gated reporter never runs). No-op stub: the fatal-alert flag is set
// separately (by the SSL wrapper), so the handshake still fails identically.
static uint32_t s_errstr_bp = 0x5FE710;
static uint8_t  s_errstr_orig = 0;
static bool     s_errstr_armed = false;
static int      s_errput_count = 0;
// DIAG: one-shot read-watch at the cipher-filter decision point in
// ssl_cipher_list_to_bytes (0x6BB98B) — dumps the version range + found flag to
// learn why every cipher is filtered out (SSL_R_NO_CIPHERS_AVAILABLE).
static uint32_t s_watch_addr = 0x6BB882; // ssl_cipher_list_to_bytes, just before the cipher loop (esi=s)
static uint8_t  s_watch_orig = 0;
static bool     s_watch_armed = false;
// One-shot guard for the TLS config-patch block, RESET per PE load (see the reset
// in wg_engine_run) so SteamSetup.exe running first doesn't consume it before the
// real Steam.exe. File-scope so the per-PE reset can clear it.
static bool     s_tls_setup_done = false;
static int      s_watch_count = 0;
// Per-cipher loop trap: 0x6BB8B7 is `test eax,eax` right after ssl_cipher_disabled
// returns (eax=disabled?1:0, ebx=cipher c). Logs which ciphers are dropped.
static uint32_t s_cloop_addr = 0x69B720; // SSL_CTX_set_cipher_list entry: arg2=cipher string
static uint8_t  s_cloop_orig = 0;
static bool     s_cloop_armed = false;
static int      s_cloop_count = 0;
// Manifest-node factory entry (0x5436E0, thiscall this=ecx). Crashes at 0x5437DD
// with this(edi)=0xFFFF. Trap entry to learn if this is bad already + who called.
static uint32_t s_fac_addr = 0x5436E0;
static uint8_t  s_fac_orig = 0;
static bool     s_fac_armed = false;
static int      s_fac_count = 0;

// DIAG (package-save root cause): the CUtlBuffer error byte lives at [this+0xf].
// CheckError (0x454180) asserts '!m_data.HasError()' when (byte & 0xc0)==0xc0
// (bit 0x80 = external/non-growable buffer, bit 0x40 = error). The buffer enters
// 0xc0 in CUtlBuffer::Grow (around 0x49EF1B) at exactly two error commits:
//   0x49EFCB  external/can't-grow buffer (entry flag already had 0x80) -> 0xc0
//   0x49EFAF  realloc/GrowMemory returned NULL -> al=0xc0 (committed @0x49EFB8)
// Both are COLD (error paths only) so trapping them adds no per-grow overhead.
// 0x45419D is the assert reporter call inside CheckError — fires only when a
// buffer actually has the 0xc0 error, telling us which buffer fails to save.
// Correlate the three by the 'this' pointer (esi). All guarded image_base 0x400000.
static uint32_t s_pe_ext_addr   = 0x49EFCB; // external can't-grow -> 0xc0
static uint32_t s_pe_alloc_addr = 0x49EFAF; // realloc fail -> al=0xc0
static uint32_t s_pe_chk_addr   = 0x45419D; // CheckError assert reporter call
static uint8_t  s_pe_ext_orig = 0, s_pe_alloc_orig = 0, s_pe_chk_orig = 0;
static bool     s_pe_armed = false;
static int      s_pe_count = 0;

// DIAG (post-handshake stall): SSL_do_handshake (0x69BF10) has ONE caller,
// ThreadedPerformInitialHandshake (0x4F35A0). At 0x4F36B9 eax = the handshake
// return (1=complete, <=0=want/err). At 0x4F36F2 eax = result of the gate call
// 0x69ebe0(ssl) (nonzero=still in init; 0 -> branch to "done" 0x4f3c1c). Logging
// these tells us, after the final recv, whether TLS reports complete (=> the
// stall is the post-handshake send dispatch) or want_read (=> TLS completion
// bug under blink). conn=esi, ssl=[esi+0x298], step=[esi+0x1a0]. image_base guard.
static uint32_t s_hs_ret_addr  = 0x4F36B9; // eax = SSL_do_handshake ret
static uint32_t s_hs_gate_addr = 0x4F36F2; // eax = gate(ssl) ret
static uint8_t  s_hs_ret_orig = 0, s_hs_gate_orig = 0;
static bool     s_hs_armed = false;
static int      s_hs_count = 0;

// DIAG (post-handshake dispatch): in the connection service loop 0x4F42DF,
// [esi+0x22] is the "handshake complete" gate. After call ThreadedPerformInitial-
// Handshake, 0x4F438B = `cmp byte[esi+0x22],0`: if 0 -> skip data pump (no GET);
// if nonzero -> 0x4F4391 runs the pump (0x4f3ca0 send / 0x4f3fc0 recv). Trapping
// 0x4F438B (flag right after handshake) and 0x4F4391 (pump path taken) tells us
// whether the flag flips when SSL completes. esi=conn. image_base 0x400000.
static uint32_t s_disp_chk_addr  = 0x4F438B; // post-handshake [esi+0x22] check
static uint32_t s_disp_pump_addr = 0x4F4391; // data-pump branch entry
static uint8_t  s_disp_chk_orig = 0, s_disp_pump_orig = 0;
static bool     s_disp_armed = false;
static int      s_disp_count = 0;

// DIAG (the real gate): in the send-side data pump 0x4F3CA0, at 0x4F3D16
// (`sub eax,[edi+0x244]`) eax = total bytes queued in the connection's send buffer
// ([edi+0x230], via 0x46fb10) and [edi+0x244] = bytes already sent. If queued is
// ~0 post-handshake, the HTTP layer never put the GET into the send buffer
// (upstream gate). If queued >> sent (the GET is there) but no SSL_write happens,
// it's the send guard. edi=conn. image_base 0x400000.
static uint32_t s_snd_addr = 0x4F3D16;
static uint8_t  s_snd_orig = 0;
static bool     s_snd_armed = false;
static int      s_snd_count = 0;

// DIAG: the SEND pump is 0x4F3FC0 (NOT 0x4F3CA0, which is the recv pump calling
// SSL_read 0x69cfb0). 0x4F3FC0 skips everything if [edi+0x134]==0 (no pending
// send), else iterates a pending-send list and at 0x4F40DC does
// `call 0x69d580(ssl=[edi+0x298], buf=[ebx+4], len=[ebx+0x14])` = SSL_write.
// Trap 0x4F40DC: does the GET actually get SSL_written post-handshake? At the
// call esp=[ssl][buf][len]; edi=conn; ebx=send descriptor. first4 "GET "=0x20544547.
// Trap 0x4F3FE9 ([edi+0x134] check) to catch the "no pending send -> skip" case.
static uint32_t s_sslw_addr   = 0x4F40DC; // the SSL_write send call site
static uint32_t s_sndchk_addr = 0x4F3FE9; // [edi+0x134] pending-send gate
static uint8_t  s_sslw_orig = 0, s_sndchk_orig = 0;
static bool     s_sslw_armed = false;
static int      s_sslw_count = 0, s_sndchk_count = 0;

// WineGlass: WG_DETERM makes all entropy/time sources return fixed values, so
// the interpreter-vs-JIT differential trace isn't confounded by non-determinism
// (the MSVC security cookie mixes GetSystemTimeAsFileTime/PID/random — those
// legitimately differ every run and would mask a real JIT miscompile).
static int wg_determ(void) { static signed char d = -1; if (d < 0) d = getenv("WG_DETERM") ? 1 : 0; return d; }

// Reported logical-processor count. Default 1 (steers Steam away from IOCP), but
// UE4/games key their task-graph threading on this — with 1 core they build a
// degenerate task graph and the main thread deadlocks waiting for work it can't
// schedule. WG_NCPU=<n> reports n cores so the worker pool gets created.
static int wg_ncpu(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("WG_NCPU"); n = e ? atoi(e) : 1;
                 if (n < 1) n = 1; if (n > 64) n = 64; }
    return n;
}
static uint64_t wg_cpumask(void) { int n = wg_ncpu(); return (n >= 64) ? ~0ull : ((1ull << n) - 1); }

// Fill a guest buffer with cryptographic random bytes (any size). Used by all
// the Windows entropy APIs (RtlGenRandom/BCryptGenRandom/ProcessPrng) so TLS
// (BoringSSL) can build its ClientHello random.
static void wg_fill_random(void *blink, uint32_t guest_addr, uint32_t len) {
    if (!guest_addr || len == 0) return;
    uint8_t chunk[512];
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = (len - done) < sizeof(chunk) ? (len - done) : (uint32_t)sizeof(chunk);
        if (wg_determ()) memset(chunk, 0x41, n); else arc4random_buf(chunk, n);
        wg_blink_write_mem(blink, guest_addr + done, chunk, n);
        done += n;
    }
}

// Dump every scheduler thread (id, state, entry, current rip, what it waits on)
// — used to see why Steam's async download reactor stalls (which thread should
// drive the socket I/O and what it's blocked on).
static void wg_dump_threads(WGEngine *engine, const char *why) {
    WGThreadScheduler *s = engine->scheduler;
    if (!s) return;
    WG_LOGW(TAG, "THREADS (%s): current=%d count=%d", why, s->current, s->count);
    for (int i = 0; i < s->count; i++) {
        WGThread *t = &s->threads[i];
        const char *st = t->state == WG_THREAD_FREE ? "FREE" :
                         t->state == WG_THREAD_RUNNING ? "RUN" :
                         t->state == WG_THREAD_READY ? "READY" :
                         t->state == WG_THREAD_WAITING ? "WAIT" :
                         t->state == WG_THREAD_SUSPENDED ? "SUSP" : "EXIT";
        WG_LOGW(TAG, "  [%d] id=0x%X %-5s start=0x%X rip=0x%X wait_h=0x%X to=0x%X",
                i, t->id, st, t->start_addr, t->regs.rip, t->wait_handle, t->wait_timeout);
    }
}

// ===== C runtime translation ==========================================
// The MSVC CRT the game imports (vcruntime140 / MSVCP140 / api-ms-win-crt-*)
// is otherwise auto-stubbed to return 0, which silently breaks string/memory/
// math the game relies on during static init. Implement the common functions
// natively against guest memory. Dispatched by name (independent of which
// forwarder DLL the game imported them from). Cdecl / caller-clean, so the
// generic thunk epilogue's RSP handling is correct for x64.

// Read a NUL-terminated narrow string from guest memory into buf (bounded).
static void wg_read_cstr(WGEngine *e, uint32_t addr, char *buf, int cap) {
    if (!addr || cap <= 0) { if (cap > 0) buf[0] = 0; return; }
    int i = 0;
    while (i < cap - 1) {
        int n = (cap - 1 - i) < 256 ? (cap - 1 - i) : 256;
        char chunk[256];
        wg_blink_read_mem(e->blink, addr + (uint32_t)i, chunk, n);
        for (int j = 0; j < n; j++) {
            buf[i + j] = chunk[j];
            if (!chunk[j]) return;
        }
        i += n;
    }
    buf[cap - 1] = 0;
}
// Read a NUL-terminated wide (UTF-16) string into a uint16_t buf (bounded, chars).
static void wg_read_wstr(WGEngine *e, uint32_t addr, uint16_t *buf, int cap) {
    if (!addr || cap <= 0) { if (cap > 0) buf[0] = 0; return; }
    int i = 0;
    while (i < cap - 1) {
        int n = (cap - 1 - i) < 128 ? (cap - 1 - i) : 128;
        uint16_t chunk[128];
        wg_blink_read_mem(e->blink, addr + (uint32_t)i * 2, chunk, n * 2);
        for (int j = 0; j < n; j++) {
            buf[i + j] = chunk[j];
            if (!chunk[j]) return;
        }
        i += n;
    }
    buf[cap - 1] = 0;
}

// A small persistent guest scratch int (for _errno / __p__commode / __p__fmode
// style functions that must return a writable pointer). Allocated once.
static uint32_t s_crt_errno = 0, s_crt_commode = 0, s_crt_fmode = 0;
static uint32_t wg_crt_global(WGEngine *e, uint32_t *slot) {
    if (!*slot) *slot = wg_guest_alloc(e, 4);
    return *slot;
}

// Read the next 8-byte vararg from a guest va_list, advancing the pointer.
static uint64_t wg_va_next(WGEngine *e, uint32_t *va) {
    uint64_t v = 0; wg_blink_read_mem(e->blink, *va, &v, 8); *va += 8; return v;
}

// Implement the MSVC v*printf core against guest memory. `wide` selects the
// wchar_t vs char output buffer. Reads the format + varargs from guest memory,
// formats via the host, writes the result (bounded by `count`), and returns the
// number of characters written. Returning 0 forever (the old auto-stub) put
// UE4's logging into an infinite format-retry loop that overflowed the stack —
// this makes formatting actually work.
static int wg_guest_vsprintf(WGEngine *e, bool wide, uint32_t buf, uint32_t count,
                             uint32_t fmt_addr, uint32_t va) {
    uint32_t va0 = va;
    char fmt[2048];
    if (wide) { uint16_t wf[2048]; wg_read_wstr(e, fmt_addr, wf, 2048);
                int i=0; for (; wf[i] && i<2047; i++) fmt[i] = wf[i]<128 ? (char)wf[i] : '?'; fmt[i]=0; }
    else      { wg_read_cstr(e, fmt_addr, fmt, sizeof fmt); }

    char out[8192]; int oi = 0;
    for (int i = 0; fmt[i] && oi < (int)sizeof(out)-1; i++) {
        if (fmt[i] != '%') { out[oi++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == '%') { out[oi++]='%'; continue; }
        char spec[40]; int si = 0; spec[si++]='%';
        while (fmt[i] && strchr("-+ 0#", fmt[i]) && si<32) spec[si++]=fmt[i++];       // flags
        while (isdigit((unsigned char)fmt[i]) && si<32) spec[si++]=fmt[i++];          // width
        if (fmt[i]=='*') { i++; int w=(int)(uint32_t)wg_va_next(e,&va); si+=snprintf(spec+si,32-si,"%d",w); }
        if (fmt[i]=='.') { spec[si++]=fmt[i++]; while (isdigit((unsigned char)fmt[i])&&si<32) spec[si++]=fmt[i++];
            if (fmt[i]=='*'){ i++; int pr=(int)(uint32_t)wg_va_next(e,&va); si+=snprintf(spec+si,32-si,"%d",pr); } }
        int len64=0, narrow_mod=0, wide_mod=0;                                        // length modifiers
        while (fmt[i] && strchr("hljztLwI", fmt[i])) {
            if (fmt[i]=='l' && fmt[i+1]=='l') { len64=1; i++; }
            else if (fmt[i]=='l' || fmt[i]=='w') wide_mod=1;
            else if (fmt[i]=='j' || fmt[i]=='z' || fmt[i]=='t') len64=1;
            else if (fmt[i]=='h') narrow_mod=1;
            else if (fmt[i]=='I' && fmt[i+1]=='6' && fmt[i+2]=='4') { len64=1; i+=2; }
            i++;
        }
        char c = fmt[i]; char tmp[600];
        switch (c) {
            case 'd': case 'i': {
                long long v = len64 ? (long long)wg_va_next(e,&va) : (int)(int32_t)(uint32_t)wg_va_next(e,&va);
                spec[si++]='l'; spec[si++]='l'; spec[si++]=c; spec[si]=0;
                snprintf(tmp,sizeof tmp,spec,v); oi += snprintf(out+oi,sizeof(out)-oi,"%s",tmp); break; }
            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long v = len64 ? wg_va_next(e,&va) : (uint32_t)wg_va_next(e,&va);
                spec[si++]='l'; spec[si++]='l'; spec[si++]=c; spec[si]=0;
                snprintf(tmp,sizeof tmp,spec,v); oi += snprintf(out+oi,sizeof(out)-oi,"%s",tmp); break; }
            case 'p': { unsigned long long v = wg_va_next(e,&va);
                oi += snprintf(out+oi,sizeof(out)-oi,"0x%llX",v); break; }
            case 'c': case 'C': { unsigned v=(unsigned)wg_va_next(e,&va); if(oi<(int)sizeof(out)-1) out[oi++]=(char)(v&0xFF); break; }
            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A': {
                uint64_t bits = wg_va_next(e,&va); double dv; memcpy(&dv,&bits,8);
                spec[si++]=c; spec[si]=0; snprintf(tmp,sizeof tmp,spec,dv);
                oi += snprintf(out+oi,sizeof(out)-oi,"%s",tmp); break; }
            case 's': case 'S': {
                uint32_t p = (uint32_t)wg_va_next(e,&va);
                // Which width is the ARG? In wide printf %s is wide (unless h);
                // in narrow printf %s is narrow (unless l). %S is the opposite.
                int argwide = wide ? !narrow_mod : wide_mod;
                if (c=='S') argwide = !argwide;
                if (argwide) { uint16_t ws[2048]; wg_read_wstr(e,p,ws,2048);
                    for (int k=0; ws[k] && oi<(int)sizeof(out)-1; k++) out[oi++] = ws[k]<128?(char)ws[k]:'?'; }
                else { char s[2048]; wg_read_cstr(e,p,s,sizeof s); oi += snprintf(out+oi,sizeof(out)-oi,"%s",s); }
                break; }
            default: if(oi<(int)sizeof(out)-1) out[oi++]='%'; if(c&&oi<(int)sizeof(out)-1) out[oi++]=c; break;
        }
        if (oi > (int)sizeof(out)-1) oi = (int)sizeof(out)-1;
    }
    out[oi] = 0;
    { static int dbg = 0; if (getenv("WG_FMT") && dbg < 400) { WG_LOGI(TAG, "fmt: %s", out); dbg++; } }
    // When the guest formats the FMallocBinned2 "unrecognized block" fatal, dump
    // the guest call stack (walk RSP for .text return addrs) so we can find the
    // FMemory::Realloc caller and what pointer it rejected.
    if (getenv("WG_FMT") && strstr(out, "unrecognized block")) {
        static int shown = 0;
        if (shown++ < 2) {
            uint32_t sp = (uint32_t)wg_blink_get_reg(e->blink, 4);
            char chain[512]; int ci = 0, found = 0;
            uint32_t lo = e->pe_image ? (uint32_t)e->pe_image->image_base + 0x1000 : 0x401000;
            uint32_t hi = e->pe_image ? (uint32_t)e->pe_image->image_base + 0x2358000 : 0x2758000;
            uint32_t prev = 0;
            for (int w = 0; w < 3000 && found < 30; w++) {
                uint32_t v = 0; wg_blink_read_mem(e->blink, sp + (uint32_t)w * 8, &v, 4);
                // .text return addresses; skip dup runs (string data)
                if (v >= lo && v < hi && v != prev) {
                    ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v); found++; prev = v;
                }
            }
            uint64_t s[6] = {0};
            for (int k = 0; k < 6; k++) wg_blink_read_mem(e->blink, va0 + k*8, &s[k], 8);
            WG_LOGW(TAG, "REALLOC-FATAL args@va0: %llX %llX %llX %llX %llX %llX | callers: %s",
                    (unsigned long long)s[0],(unsigned long long)s[1],(unsigned long long)s[2],
                    (unsigned long long)s[3],(unsigned long long)s[4],(unsigned long long)s[5], chain);
        }
    }
    if (buf && count) {
        int n = oi; if ((uint32_t)n >= count) n = (int)count - 1; if (n < 0) n = 0;
        if (wide) { uint16_t *wb = malloc((size_t)(n+1)*2);
            if (wb) { for (int k=0;k<n;k++) wb[k]=(uint8_t)out[k]; wb[n]=0; wg_blink_write_mem(e->blink,buf,wb,(uint32_t)(n+1)*2); free(wb); } }
        else { wg_blink_write_mem(e->blink, buf, out, (uint32_t)n+1); }
    }
    return oi;
}

// Try to handle `fn` as a CRT function. Returns true (and sets *ret) if handled.
// Does NOT claim memcpy/memset/memmove/malloc/calloc/free/realloc/_initterm —
// those have dedicated handlers elsewhere in the dispatch.
static bool wg_try_crt(WGEngine *engine, const char *fn, uint32_t *args, uint64_t *args64, uint64_t *ret) {
    #define A0 args[0]
    #define A1 args[1]
    #define A2 args[2]
    // wcsstr — ESSENTIAL for UE4 config init (path `/..` collapse + config token
    // substitution {ENGINE}/{PROJECT}/{PLATFORM}/{USER}...). Auto-stubbed it never
    // found anything, so config init looped forever and the boot never reached the
    // renderer. Uses the FULL 64-bit args (haystacks are short path/config strings).
    if (!strcmp(fn,"wcsstr")) {
        uint64_t hay = args64[0], nd = args64[1];
        uint16_t needle[1024]; wg_read_wstr(engine, nd, needle, 1024);
        int nlen = 0; while (nlen < 1023 && needle[nlen]) nlen++;
        if (nlen == 0) { *ret = hay; return true; }
        const int WIN = 8192; uint16_t win[WIN];
        uint32_t gpos = 0; const uint32_t MAXSCAN = 8u * 1024 * 1024;
        while (gpos < MAXSCAN) {
            wg_blink_read_mem(engine->blink, hay + (uint64_t)gpos * 2, win, WIN * 2);
            int valid = 0; while (valid < WIN && win[valid]) valid++;
            int limit = valid - nlen;
            for (int i = 0; i <= limit; i++) {
                int j = 0; while (j < nlen && win[i + j] == needle[j]) j++;
                if (j == nlen) { *ret = hay + (uint64_t)(gpos + i) * 2; return true; }
            }
            if (valid < WIN) { *ret = 0; return true; }
            gpos += WIN - (nlen - 1);
        }
        *ret = 0; return true;
    }
    #define A3 args[3]

    // ---- ctype (int in / int out; no guest memory) ----
    if (!strcmp(fn,"isalpha")) { *ret = isalpha((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isdigit")) { *ret = isdigit((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isalnum")) { *ret = isalnum((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isspace")) { *ret = isspace((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isupper")) { *ret = isupper((int)A0)?1:0; return true; }
    if (!strcmp(fn,"islower")) { *ret = islower((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isxdigit")){ *ret = isxdigit((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isprint")) { *ret = isprint((int)A0)?1:0; return true; }
    if (!strcmp(fn,"isgraph")) { *ret = isgraph((int)A0)?1:0; return true; }
    if (!strcmp(fn,"ispunct")) { *ret = ispunct((int)A0)?1:0; return true; }
    if (!strcmp(fn,"iscntrl")) { *ret = iscntrl((int)A0)?1:0; return true; }
    if (!strcmp(fn,"tolower")) { *ret = (uint32_t)tolower((int)A0); return true; }
    if (!strcmp(fn,"toupper")) { *ret = (uint32_t)toupper((int)A0); return true; }
    if (!strcmp(fn,"iswalpha")){ *ret = isalpha((int)(A0&0x7f))?1:0; return true; }
    if (!strcmp(fn,"iswdigit")){ *ret = ((A0>='0')&&(A0<='9'))?1:0; return true; }
    if (!strcmp(fn,"iswalnum")){ *ret = isalnum((int)(A0&0x7f))?1:0; return true; }
    if (!strcmp(fn,"iswspace")){ *ret = (A0==' '||A0=='\t'||A0=='\n'||A0=='\r'||A0==0xA0)?1:0; return true; }
    if (!strcmp(fn,"iswupper")){ *ret = isupper((int)(A0&0x7f))?1:0; return true; }
    if (!strcmp(fn,"iswlower")){ *ret = islower((int)(A0&0x7f))?1:0; return true; }
    if (!strcmp(fn,"iswxdigit")){*ret = isxdigit((int)(A0&0x7f))?1:0; return true; }
    if (!strcmp(fn,"towlower")){ *ret = (A0<128)?(uint32_t)tolower((int)A0):A0; return true; }
    if (!strcmp(fn,"towupper")){ *ret = (A0<128)?(uint32_t)toupper((int)A0):A0; return true; }

    // ---- memory ----
    if (!strcmp(fn,"memcmp")) {
        uint32_t n = A2; if (n > 64u*1024*1024) n = 64u*1024*1024;
        uint8_t *a = malloc(n?n:1), *b = malloc(n?n:1); int r = 0;
        if (a && b) { wg_blink_read_mem(engine->blink,A0,a,n); wg_blink_read_mem(engine->blink,A1,b,n); r = memcmp(a,b,n); }
        free(a); free(b); *ret = (uint32_t)(int32_t)r; return true;
    }
    if (!strcmp(fn,"memchr")) {
        uint32_t n = A2; if (n > 64u*1024*1024) n = 64u*1024*1024;
        uint8_t *a = malloc(n?n:1); uint32_t found = 0;
        if (a) { wg_blink_read_mem(engine->blink,A0,a,n);
                 uint8_t *p = memchr(a,(int)A1,n); if (p) found = A0 + (uint32_t)(p-a); }
        free(a); *ret = found; return true;
    }

    // ---- narrow string ----
    if (!strcmp(fn,"strlen")) { char s[4096]; wg_read_cstr(engine,A0,s,sizeof s); *ret = (uint32_t)strlen(s); return true; }
    if (!strcmp(fn,"strnlen")){ char s[4096]; wg_read_cstr(engine,A0,s,sizeof s); size_t l=strlen(s); if(l>A1)l=A1; *ret=(uint32_t)l; return true; }
    if (!strcmp(fn,"strcmp")) { char a[4096],b[4096]; wg_read_cstr(engine,A0,a,sizeof a); wg_read_cstr(engine,A1,b,sizeof b); *ret=(uint32_t)(int32_t)strcmp(a,b); return true; }
    if (!strcmp(fn,"strncmp")){ char a[4096],b[4096]; wg_read_cstr(engine,A0,a,sizeof a); wg_read_cstr(engine,A1,b,sizeof b); *ret=(uint32_t)(int32_t)strncmp(a,b,A2); return true; }
    if (!strcmp(fn,"_stricmp")||!strcmp(fn,"stricmp")){ char a[4096],b[4096]; wg_read_cstr(engine,A0,a,sizeof a); wg_read_cstr(engine,A1,b,sizeof b); *ret=(uint32_t)(int32_t)strcasecmp(a,b); return true; }
    if (!strcmp(fn,"_strnicmp")||!strcmp(fn,"strnicmp")){ char a[4096],b[4096]; wg_read_cstr(engine,A0,a,sizeof a); wg_read_cstr(engine,A1,b,sizeof b); *ret=(uint32_t)(int32_t)strncasecmp(a,b,A2); return true; }
    if (!strcmp(fn,"strcpy")) { char s[4096]; wg_read_cstr(engine,A1,s,sizeof s); wg_blink_write_mem(engine->blink,A0,s,(uint32_t)strlen(s)+1); *ret=A0; return true; }
    if (!strcmp(fn,"strncpy")){ char s[4096]; wg_read_cstr(engine,A1,s,sizeof s); uint32_t n=A2; char *o=calloc(1,n?n:1); if(o){ size_t l=strlen(s); memcpy(o,s,l<n?l:n); wg_blink_write_mem(engine->blink,A0,o,n); free(o);} *ret=A0; return true; }
    if (!strcmp(fn,"strcat")) { char d[8192],s[4096]; wg_read_cstr(engine,A0,d,sizeof d); wg_read_cstr(engine,A1,s,sizeof s); size_t dl=strlen(d),sl=strlen(s); if(dl+sl<sizeof d){ memcpy(d+dl,s,sl+1); wg_blink_write_mem(engine->blink,A0,d,(uint32_t)(dl+sl+1)); } *ret=A0; return true; }
    if (!strcmp(fn,"strchr")) { char s[4096]; wg_read_cstr(engine,A0,s,sizeof s); char *p=strchr(s,(int)A1); *ret = p? A0+(uint32_t)(p-s):0; return true; }
    if (!strcmp(fn,"strrchr")){ char s[4096]; wg_read_cstr(engine,A0,s,sizeof s); char *p=strrchr(s,(int)A1); *ret = p? A0+(uint32_t)(p-s):0; return true; }
    if (!strcmp(fn,"strstr")) { char h[8192],n[1024]; wg_read_cstr(engine,A0,h,sizeof h); wg_read_cstr(engine,A1,n,sizeof n); char *p=strstr(h,n); *ret = p? A0+(uint32_t)(p-h):0; return true; }
    if (!strcmp(fn,"strspn")) { char s[4096],a[256]; wg_read_cstr(engine,A0,s,sizeof s); wg_read_cstr(engine,A1,a,sizeof a); *ret=(uint32_t)strspn(s,a); return true; }
    if (!strcmp(fn,"strcspn")){ char s[4096],a[256]; wg_read_cstr(engine,A0,s,sizeof s); wg_read_cstr(engine,A1,a,sizeof a); *ret=(uint32_t)strcspn(s,a); return true; }
    if (!strcmp(fn,"strpbrk")){ char s[4096],a[256]; wg_read_cstr(engine,A0,s,sizeof s); wg_read_cstr(engine,A1,a,sizeof a); char *p=strpbrk(s,a); *ret=p?A0+(uint32_t)(p-s):0; return true; }
    if (!strcmp(fn,"_strdup")){ char s[4096]; wg_read_cstr(engine,A0,s,sizeof s); uint32_t l=(uint32_t)strlen(s)+1; uint32_t g=wg_guest_alloc(engine,l); if(g) wg_blink_write_mem(engine->blink,g,s,l); *ret=g; return true; }

    // ---- wide string ----
    if (!strcmp(fn,"wcslen")) { uint16_t s[4096]; wg_read_wstr(engine,A0,s,4096); int l=0; while(s[l])l++; *ret=(uint32_t)l; return true; }
    if (!strcmp(fn,"wcscmp")) { uint16_t a[4096],b[4096]; wg_read_wstr(engine,A0,a,4096); wg_read_wstr(engine,A1,b,4096); int i=0; while(a[i]&&a[i]==b[i])i++; *ret=(uint32_t)(int32_t)((int)a[i]-(int)b[i]); return true; }
    if (!strcmp(fn,"wcsncmp")){ uint16_t a[4096],b[4096]; wg_read_wstr(engine,A0,a,4096); wg_read_wstr(engine,A1,b,4096); int i=0,r=0; while((uint32_t)i<A2){ if(a[i]!=b[i]){r=(int)a[i]-(int)b[i];break;} if(!a[i])break; i++; } *ret=(uint32_t)(int32_t)r; return true; }
    if (!strcmp(fn,"_wcsicmp")||!strcmp(fn,"_wcsnicmp")) {
        uint16_t a[4096],b[4096]; wg_read_wstr(engine,A0,a,4096); wg_read_wstr(engine,A1,b,4096);
        uint32_t lim = (!strcmp(fn,"_wcsnicmp"))?A2:0xFFFFFFFF; int i=0,r=0;
        while((uint32_t)i<lim){ int ca=a[i],cb=b[i]; if(ca<128)ca=tolower(ca); if(cb<128)cb=tolower(cb); if(ca!=cb){r=ca-cb;break;} if(!a[i])break; i++; }
        *ret=(uint32_t)(int32_t)r; return true;
    }
    if (!strcmp(fn,"wcschr")) { uint16_t s[4096]; wg_read_wstr(engine,A0,s,4096); int i=0; for(;;i++){ if(s[i]==(uint16_t)A1){ *ret=A0+(uint32_t)i*2; return true; } if(!s[i])break; } *ret=0; return true; }
    if (!strcmp(fn,"wcsrchr")){ uint16_t s[4096]; wg_read_wstr(engine,A0,s,4096); int last=-1,i=0; for(;;i++){ if(s[i]==(uint16_t)A1)last=i; if(!s[i])break; } *ret=last>=0?A0+(uint32_t)last*2:0; return true; }
    if (!strcmp(fn,"wcscpy")) { uint16_t s[4096]; wg_read_wstr(engine,A1,s,4096); int l=0; while(s[l])l++; wg_blink_write_mem(engine->blink,A0,s,(uint32_t)(l+1)*2); *ret=A0; return true; }
    if (!strcmp(fn,"wcsncpy")){ uint16_t s[4096]; wg_read_wstr(engine,A1,s,4096); uint32_t n=A2; uint16_t *o=calloc(n?n:1,2); if(o){ int l=0; while(s[l])l++; for(uint32_t i=0;i<n;i++)o[i]=((uint32_t)i<(uint32_t)l)?s[i]:0; wg_blink_write_mem(engine->blink,A0,o,n*2); free(o);} *ret=A0; return true; }

    // ---- MSVC C++ RTTI (x64) ----
    // Auto-stubbed, these returned garbage, so the game's typeid/dynamic_cast
    // type checks were wrong and it called virtual methods on mis-typed objects
    // (null-vtable crash). Implement against the guest's compiler RTTI data.
    // x64 layout: [obj]=vtable; [vtable-8]=CompleteObjectLocator (COL, absolute).
    //   COL: +0x0C pTypeDescriptor(RVA), +0x10 pClassDescriptor(RVA), +0x14 pSelf(RVA).
    //   TypeDescriptor(=type_info): +0x10 decorated name (".?AV...").
    if (!strcmp(fn,"__RTtypeid")) {  // type_info* __RTtypeid(void* obj)
        uint32_t obj = A0, vtable = 0, col = 0, td_rva = 0;
        uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
        wg_blink_read_mem(engine->blink, obj, &vtable, 4);
        if (vtable) wg_blink_read_mem(engine->blink, vtable - 8, &col, 4);
        if (col)    wg_blink_read_mem(engine->blink, col + 0x0C, &td_rva, 4);
        *ret = td_rva ? (base + td_rva) : 0;
        return true;
    }
    if (!strcmp(fn,"__std_type_info_compare")) {  // int(type_info* a, type_info* b): 0 if same
        uint32_t a = A0, b = A1;
        if (a == b || !a || !b) { *ret = (a == b) ? 0 : 1; return true; }
        char na[600] = {0}, nb[600] = {0};
        wg_read_cstr(engine, a + 0x10, na, sizeof na);
        wg_read_cstr(engine, b + 0x10, nb, sizeof nb);
        *ret = (uint32_t)(int32_t)strcmp(na, nb);
        return true;
    }
    if (!strcmp(fn,"__std_type_info_name")) {  // const char* name(type_info*, __type_info_node*)
        // Return the decorated name (at +0x10, skip the leading '.'). UE4 uses it
        // for hashing/compare; a stable per-type string is what matters.
        *ret = A0 ? A0 + 0x11 : 0;
        return true;
    }
    if (!strcmp(fn,"__RTDynamicCast")) {
        // void* __RTDynamicCast(void* obj, int vfDelta, TypeDescriptor* srcType,
        //                       TypeDescriptor* dstType, int isReference)
        // Walk obj's class-hierarchy base-class array for dstType; return the
        // this-adjusted pointer, or null if not in the hierarchy.
        uint32_t obj = A0, dstType = A3;
        uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
        if (!obj || !dstType) { *ret = 0; return true; }
        uint32_t vtable = 0, col = 0, off = 0, chd_rva = 0;
        wg_blink_read_mem(engine->blink, obj, &vtable, 4);
        if (!vtable) { *ret = 0; return true; }
        wg_blink_read_mem(engine->blink, vtable - 8, &col, 4);
        if (!col) { *ret = 0; return true; }
        wg_blink_read_mem(engine->blink, col + 0x04, &off, 4);      // vtable offset in object
        wg_blink_read_mem(engine->blink, col + 0x10, &chd_rva, 4);  // ClassHierarchyDescriptor RVA
        uint32_t complete = obj - off;                              // complete-object base
        uint32_t chd = base + chd_rva, numbc = 0, bca_rva = 0;
        wg_blink_read_mem(engine->blink, chd + 0x08, &numbc, 4);
        wg_blink_read_mem(engine->blink, chd + 0x0C, &bca_rva, 4);
        uint32_t bca = base + bca_rva;
        if (numbc > 4096) numbc = 4096;
        *ret = 0;
        for (uint32_t k = 0; k < numbc; k++) {
            uint32_t bcd_rva = 0; wg_blink_read_mem(engine->blink, bca + k * 4, &bcd_rva, 4);
            uint32_t bcd = base + bcd_rva, td_rva = 0;
            wg_blink_read_mem(engine->blink, bcd + 0x00, &td_rva, 4);
            if (base + td_rva != dstType) continue;
            int32_t mdisp = 0, pdisp = 0, vdisp = 0;   // PMD at bcd+0x08
            wg_blink_read_mem(engine->blink, bcd + 0x08, &mdisp, 4);
            wg_blink_read_mem(engine->blink, bcd + 0x0C, &pdisp, 4);
            wg_blink_read_mem(engine->blink, bcd + 0x10, &vdisp, 4);
            uint32_t p = complete;
            if (pdisp >= 0) {   // virtual base: vbtable indirection
                uint32_t vbtable = 0, voff = 0;
                wg_blink_read_mem(engine->blink, complete + (uint32_t)pdisp, &vbtable, 4);
                wg_blink_read_mem(engine->blink, vbtable + (uint32_t)vdisp, &voff, 4);
                p = complete + (uint32_t)pdisp + voff;
            }
            *ret = p + (uint32_t)mdisp;
            break;
        }
        return true;
    }

    // ---- conversion ----
    // Helper: pull a wide arg into a narrow ASCII buffer (number parsing only
    // ever sees ASCII: digits, sign, '.', 'e'). Reading a wide string with
    // wg_read_cstr is WRONG — UTF-16 "12" is 31 00 32 00, so the narrow read
    // stops after the first digit. Read it as wide and down-convert.
    #define WG_WIDE_TO_ASCII(dstbuf) do { \
        uint16_t _w[256]; wg_read_wstr(engine, A0, _w, 256); \
        int _i = 0; for (; _i < (int)sizeof(dstbuf) - 1 && _w[_i]; _i++) \
            (dstbuf)[_i] = (_w[_i] < 128) ? (char)_w[_i] : '?'; \
        (dstbuf)[_i] = 0; } while (0)
    bool is64 = engine->pe_image && engine->pe_image->is_64bit;
    if (!strcmp(fn,"atoi")) { char s[64]; wg_read_cstr(engine,A0,s,sizeof s); *ret=(uint32_t)(int32_t)atoi(s); return true; }
    if (!strcmp(fn,"_wtoi")) { char s[64]; WG_WIDE_TO_ASCII(s); *ret=(uint32_t)(int32_t)atoi(s); return true; }
    if (!strcmp(fn,"atol")) { char s[64]; wg_read_cstr(engine,A0,s,sizeof s); *ret=(uint32_t)(int32_t)atol(s); return true; }
    if (!strcmp(fn,"_wtol")) { char s[64]; WG_WIDE_TO_ASCII(s); *ret=(uint32_t)(int32_t)atol(s); return true; }
    if (!strcmp(fn,"_atoi64")||!strcmp(fn,"_strtoi64")) { char s[64]; wg_read_cstr(engine,A0,s,sizeof s); *ret=(uint64_t)strtoll(s,NULL,10); return true; }
    if (!strcmp(fn,"_wtoi64")||!strcmp(fn,"_wcstoi64")) { char s[64]; WG_WIDE_TO_ASCII(s); *ret=(uint64_t)strtoll(s,NULL,10); return true; }
    if (!strcmp(fn,"strtol")) { char s[128]; wg_read_cstr(engine,A0,s,sizeof s); char *end; long v=strtol(s,&end,(int)A2); if(A1){uint64_t ep=(uint64_t)A0+(uint32_t)(end-s); wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4);} *ret=(uint32_t)v; return true; }
    if (!strcmp(fn,"strtoul")) { char s[128]; wg_read_cstr(engine,A0,s,sizeof s); char *end; unsigned long v=strtoul(s,&end,(int)A2); if(A1){uint64_t ep=(uint64_t)A0+(uint32_t)(end-s); wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4);} *ret=(uint32_t)v; return true; }
    if (!strcmp(fn,"wcstol")||!strcmp(fn,"wcstoul")||!strcmp(fn,"wcstoll")||!strcmp(fn,"wcstoull")) {
        char s[128]; WG_WIDE_TO_ASCII(s); char *end;
        long long v = (fn[5]=='u') ? (long long)strtoull(s,&end,(int)A2) : strtoll(s,&end,(int)A2);
        if(A1){ uint64_t ep=(uint64_t)A0+(uint64_t)(end-s)*2; wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4); }
        *ret=(uint64_t)v; return true;
    }
    // Floating-point conversions — result returns in XMM0 per the ABI, so use
    // wg_blink_set_xmm_low. UE4's JSON reader parses every numeric field (incl.
    // the .uproject "FileVersion") via FCString::Atod -> wcstod; auto-stubbing
    // it to 0 made every JSON number read as 0 and the game exit(1) with
    // "File appears to be in a newer version (0) ... (max version: 3)".
    if (!strcmp(fn,"wcstod")||!strcmp(fn,"_wtof")||!strcmp(fn,"_wcstod_l")) {
        char s[256]; WG_WIDE_TO_ASCII(s); char *end=s; double d=strtod(s,&end);
        if(A1 && strcmp(fn,"_wtof")){ uint64_t ep=(uint64_t)A0+(uint64_t)(end-s)*2; wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4); }
        uint64_t bits; memcpy(&bits,&d,8); wg_blink_set_xmm_low(engine->blink,0,bits);
        *ret=(uint64_t)(int64_t)d; return true;
    }
    if (!strcmp(fn,"atof")||!strcmp(fn,"strtod")||!strcmp(fn,"_atof_l")) {
        char s[256]; wg_read_cstr(engine,A0,s,sizeof s); char *end=s; double d=strtod(s,&end);
        if(!strcmp(fn,"strtod") && A1){ uint64_t ep=(uint64_t)A0+(uint64_t)(end-s); wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4); }
        uint64_t bits; memcpy(&bits,&d,8); wg_blink_set_xmm_low(engine->blink,0,bits);
        *ret=(uint64_t)(int64_t)d; return true;
    }
    if (!strcmp(fn,"wcstof")) {  // returns float (low 32 bits of XMM0)
        char s[256]; WG_WIDE_TO_ASCII(s); char *end=s; float f=strtof(s,&end);
        if(A1){ uint64_t ep=(uint64_t)A0+(uint64_t)(end-s)*2; wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4); }
        uint32_t bits; memcpy(&bits,&f,4); wg_blink_set_xmm_low(engine->blink,0,(uint64_t)bits);
        *ret=(uint64_t)(int64_t)f; return true;
    }
    if (!strcmp(fn,"strtof")) {  // narrow float
        char s[256]; wg_read_cstr(engine,A0,s,sizeof s); char *end=s; float f=strtof(s,&end);
        if(A1){ uint64_t ep=(uint64_t)A0+(uint64_t)(end-s); wg_blink_write_mem(engine->blink,A1,&ep,is64?8:4); }
        uint32_t bits; memcpy(&bits,&f,4); wg_blink_set_xmm_low(engine->blink,0,(uint64_t)bits);
        *ret=(uint64_t)(int64_t)f; return true;
    }
    #undef WG_WIDE_TO_ASCII

    // ---- heap extras (base malloc/calloc/free/realloc handled elsewhere) ----
    if (!strcmp(fn,"_aligned_malloc")) { uint32_t g=wg_guest_alloc(engine,A0); *ret=g; return true; } // page-aligned already
    if (!strcmp(fn,"_aligned_free"))   { *ret=0; return true; }                    // bump heap: no-op
    if (!strcmp(fn,"_msize"))          { *ret=A0?0x1000:0; return true; }           // rounded page size (best effort)
    if (!strcmp(fn,"_set_new_mode"))   { *ret=0; return true; }
    if (!strcmp(fn,"_callnewh"))       { *ret=0; return true; }
    if (!strcmp(fn,"_get_heap_handle")){ *ret=0x00D00000; return true; }

    // ---- CRT startup / onexit (return "success"; we don't run atexit at teardown) ----
    if (!strcmp(fn,"_configure_narrow_argv")) { *ret=0; return true; }
    if (!strcmp(fn,"_configure_wide_argv"))   { *ret=0; return true; }
    if (!strcmp(fn,"_initialize_narrow_environment")) { *ret=0; return true; }
    if (!strcmp(fn,"_initialize_wide_environment"))   { *ret=0; return true; }
    if (!strcmp(fn,"_initialize_onexit_table")) { *ret=0; return true; }
    if (!strcmp(fn,"_register_onexit_function")) { *ret=0; return true; }
    if (!strcmp(fn,"_crt_atexit")||!strcmp(fn,"atexit")||!strcmp(fn,"_onexit")) { *ret=0; return true; }
    if (!strcmp(fn,"_register_thread_local_exe_atexit_callback")) { *ret=0; return true; }
    if (!strcmp(fn,"_set_app_type")||!strcmp(fn,"__setusermatherr")) { *ret=0; return true; }
    if (!strcmp(fn,"_set_fmode")||!strcmp(fn,"_configthreadlocale")) { *ret=0; return true; }
    if (!strcmp(fn,"_seh_filter_exe")||!strcmp(fn,"_seh_filter_dll")) { *ret=0; return true; } // EXCEPTION_CONTINUE_SEARCH
    if (!strcmp(fn,"_set_invalid_parameter_handler")||
        !strcmp(fn,"_set_thread_local_invalid_parameter_handler")) { *ret=0; return true; }
    if (!strcmp(fn,"_invalid_parameter_noinfo")) { *ret=0; return true; }
    if (!strcmp(fn,"_get_narrow_winmain_command_line")) { *ret = s_cmdline_page + 0x800; return true; }
    if (!strcmp(fn,"_get_wide_winmain_command_line"))   { *ret = s_cmdline_page; return true; }

    // ---- formatted output (v*printf family) ----
    // 6-arg: (options, buffer, count, format, locale, va_list)
    if (!strcmp(fn,"__stdio_common_vswprintf")||!strcmp(fn,"__stdio_common_vswprintf_s")) {
        *ret = (uint32_t)(int32_t)wg_guest_vsprintf(engine,true,A1,A2,A3,args[5]); return true; }
    if (!strcmp(fn,"__stdio_common_vsprintf")||!strcmp(fn,"__stdio_common_vsprintf_s")) {
        *ret = (uint32_t)(int32_t)wg_guest_vsprintf(engine,false,A1,A2,A3,args[5]); return true; }
    // 7-arg _s with an extra max_count: (options, buffer, count, max, format, locale, va_list)
    if (!strcmp(fn,"__stdio_common_vsnwprintf_s")) {
        *ret = (uint32_t)(int32_t)wg_guest_vsprintf(engine,true,A1,A2,args[4],args[6]); return true; }
    if (!strcmp(fn,"__stdio_common_vsnprintf_s")) {
        *ret = (uint32_t)(int32_t)wg_guest_vsprintf(engine,false,A1,A2,args[4],args[6]); return true; }

    // ---- pointer-returning CRT globals (must be a writable guest address) ----
    if (!strcmp(fn,"_errno"))      { *ret = wg_crt_global(engine,&s_crt_errno);   return true; }
    if (!strcmp(fn,"__p__commode")){ *ret = wg_crt_global(engine,&s_crt_commode); return true; }
    if (!strcmp(fn,"__p__fmode"))  { *ret = wg_crt_global(engine,&s_crt_fmode);   return true; }

    #undef A0
    #undef A1
    #undef A2
    #undef A3
    return false;
}

// ===== Directory enumeration (FindFirstFile/FindNextFile/FindClose) ==========
// The old stub returned INVALID_HANDLE for FindFirstFile, so the guest could
// never discover files — UE4 found none of its pakchunk*.pak content and quit.
// Snapshot the matching leaf names at FindFirst time and iterate.
#define WG_FIND_BASE   0xF1000000u
#define WG_MAX_FINDS   64
typedef struct {
    bool  in_use;
    char  dir[1024];   // real host directory
    char **names;      // matched leaf names
    int   count, pos;
} WGFindState;
static WGFindState s_finds[WG_MAX_FINDS];

// Fill a WIN32_FIND_DATAW (0x250 bytes) for `name` in real dir `dir`.
static void wg_write_find_data(WGEngine *engine, uint32_t data_addr,
                               const char *dir, const char *name) {
    if (!data_addr) return;
    uint8_t fd[0x250]; memset(fd, 0, sizeof(fd));
    char full[2048]; snprintf(full, sizeof(full), "%s/%s", dir, name);
    uint32_t attrs = 0x80;           // FILE_ATTRIBUTE_NORMAL
    uint64_t size = 0;
    struct stat st;
    if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) attrs = 0x10;   // FILE_ATTRIBUTE_DIRECTORY
        else size = (uint64_t)st.st_size;
    }
    memcpy(fd + 0x00, &attrs, 4);
    uint32_t hi = (uint32_t)(size >> 32), lo = (uint32_t)size;
    memcpy(fd + 0x1C, &hi, 4);
    memcpy(fd + 0x20, &lo, 4);
    for (int i = 0; name[i] && i < 259; i++) {   // cFileName[260] wide at +0x2C
        uint16_t w = (uint8_t)name[i];
        memcpy(fd + 0x2C + i * 2, &w, 2);
    }
    wg_blink_write_mem(engine->blink, data_addr, fd, sizeof(fd));
}

// Native startup-movie playback (WGMetalBackend.m; weak no-op stub on headless).
extern void wg_gpu_play_movie(const char *dir);

// FindFirstFile: map the guest pattern, glob the directory, snapshot matches.
// Returns the guest search handle (WG_FIND_BASE+idx) or INVALID_HANDLE_VALUE.
static uint64_t wg_findfile_first(WGEngine *engine, uint32_t pattern_addr,
                                  uint32_t data_addr) {
    uint16_t wpat[600] = {0}; char apat[600] = {0};
    if (pattern_addr) {
        wg_blink_read_mem(engine->blink, pattern_addr, wpat, sizeof(wpat) - 2);
        for (int i = 0; i < 599 && wpat[i]; i++) apat[i] = wpat[i] < 128 ? (char)wpat[i] : '_';
    }
    char mapbuf[1024]; strncpy(mapbuf, apat, sizeof(mapbuf) - 1); mapbuf[sizeof(mapbuf)-1] = 0;
    const char *real = wg_files_map_path(pattern_addr, engine->blink, mapbuf, sizeof(mapbuf));
    if (!real) return (engine->pe_image && engine->pe_image->is_64bit) ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu;

    // WG_SKIP_MOVIES: report ZERO startup movies so UE4's FDefaultGameMoviePlayer
    // has nothing to play and skips straight to the game (the first D3D-rendered
    // frame / menu). Visage's logo movies need Media Foundation video decode which
    // we don't implement, so the movie player otherwise busy-waits forever on a
    // movie frame that never arrives, gating the first Present. Case-insensitive
    // match on ".../Content/Movies" in the mapped path.
    // WG_NATIVE_MOVIE: additionally decode+present the game's logo movies natively
    // (VideoToolbox) — so you SEE the game's intro while the guest grinds through
    // the (hours-long) UObject drain that gates its own first Present.
    { static signed char skipmov = -1, natmov = -1;
      if (skipmov < 0) skipmov = getenv("WG_SKIP_MOVIES") ? 1 : 0;
      if (natmov  < 0) natmov  = getenv("WG_NATIVE_MOVIE") ? 1 : 0;
      if (skipmov || natmov) {
        const char *p = real; int hit = 0;
        for (const char *q = p; *q; q++) {
            if ((q[0]=='M'||q[0]=='m') && strncasecmp(q, "Movies", 6) == 0) { hit = 1; break; }
        }
        if (hit) {
            if (natmov) {
                // Extract the movies directory (real minus the trailing glob).
                char mdir[1024]; strncpy(mdir, real, sizeof(mdir)-1); mdir[sizeof(mdir)-1]=0;
                char *sl = strrchr(mdir, '/'); if (sl) *sl = 0;
                WG_LOGW(TAG, "WG_NATIVE_MOVIE: playing game logos natively from '%s'", mdir);
                wg_gpu_play_movie(mdir);
            }
            WG_LOGW(TAG, "movie enum: reporting no files in '%s' (guest skips its MF path)", real);
            s_last_error = 2; // ERROR_FILE_NOT_FOUND
            return (engine->pe_image && engine->pe_image->is_64bit) ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu;
        }
      }
    }

    // Split into directory + glob (last separator).
    char rdir[1024]; const char *glob = "*";
    strncpy(rdir, real, sizeof(rdir) - 1); rdir[sizeof(rdir)-1] = 0;
    char *slash = strrchr(rdir, '/');
    if (slash) { *slash = 0; glob = slash + 1; }
    if (!glob[0]) glob = "*";

    DIR *d = opendir(rdir);
    if (!d) { s_last_error = 3; // ERROR_PATH_NOT_FOUND
              return (engine->pe_image && engine->pe_image->is_64bit) ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu; }

    int slot = -1;
    for (int i = 0; i < WG_MAX_FINDS; i++) if (!s_finds[i].in_use) { slot = i; break; }
    if (slot < 0) { closedir(d); return (engine->pe_image && engine->pe_image->is_64bit) ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu; }

    WGFindState *fs = &s_finds[slot];
    memset(fs, 0, sizeof(*fs));
    strncpy(fs->dir, rdir, sizeof(fs->dir) - 1);
    int cap = 16; fs->names = malloc(sizeof(char*) * cap);
    struct dirent *e;
    while ((e = readdir(d)) && fs->names) {
        if (fnmatch(glob, e->d_name, FNM_CASEFOLD) != 0) continue;
        if (fs->count >= cap) { cap *= 2; char **n = realloc(fs->names, sizeof(char*) * cap);
                                if (!n) break; fs->names = n; }
        fs->names[fs->count++] = strdup(e->d_name);
    }
    closedir(d);
    if (fs->count == 0) { free(fs->names); fs->names = NULL;
        s_last_error = 2; // ERROR_FILE_NOT_FOUND
        return (engine->pe_image && engine->pe_image->is_64bit) ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu; }
    fs->in_use = true;
    fs->pos = 1;
    wg_write_find_data(engine, data_addr, fs->dir, fs->names[0]);
    WG_LOGI(TAG, "FindFirstFile('%s') in %s -> %d matches (first '%s')",
            glob, fs->dir, fs->count, fs->names[0]);
    return WG_FIND_BASE + (uint32_t)slot;
}

static uint32_t wg_findfile_next(WGEngine *engine, uint32_t handle, uint32_t data_addr) {
    if (handle < WG_FIND_BASE || handle >= WG_FIND_BASE + WG_MAX_FINDS) return 0;
    WGFindState *fs = &s_finds[handle - WG_FIND_BASE];
    if (!fs->in_use || fs->pos >= fs->count) { s_last_error = 18; return 0; } // ERROR_NO_MORE_FILES
    wg_write_find_data(engine, data_addr, fs->dir, fs->names[fs->pos]);
    fs->pos++;
    return 1;
}

static void wg_findfile_close(uint32_t handle) {
    if (handle < WG_FIND_BASE || handle >= WG_FIND_BASE + WG_MAX_FINDS) return;
    WGFindState *fs = &s_finds[handle - WG_FIND_BASE];
    if (!fs->in_use) return;
    for (int i = 0; i < fs->count; i++) free(fs->names[i]);
    free(fs->names);
    memset(fs, 0, sizeof(*fs));
}

// General address-trace: HLT breakpoints that log register state when hit, then
// restore/step/re-arm. Gated by WG_TRACE. Used to trace where a value (e.g.
// GuardedMain's return code) originates.
#define WG_MAX_TRACE 24
static struct { uint32_t addr; uint8_t orig; bool armed; const char *label; } s_trace[WG_MAX_TRACE];
static int s_trace_count = 0;
static void wg_trace_add(uint32_t addr, const char *label) {
    if (s_trace_count < WG_MAX_TRACE) { s_trace[s_trace_count].addr = addr;
        s_trace[s_trace_count].label = label; s_trace[s_trace_count].armed = false; s_trace_count++; }
}

// ── WaitOnAddress / WakeByAddress (Win8+ futex) ──────────────────────────────
// UE4's task-graph "parking lot" (FEventCount / low-level thread coordination)
// is built on these. They were R1S/RS stubs: WaitOnAddress returned immediately
// (never blocking) and WakeByAddress did nothing, so worker dispatch never
// coordinated -> the boot deadlock. Real impl: block while the AddressSize bytes
// at Address still equal those at CompareAddress, until a WakeByAddress broadcast
// (or timeout). One global lock+cond with broadcast-and-recheck is correct — a
// spurious wake just re-reads guest memory. Guest memory is read WITHOUT the GIL
// (released via wg_thunk_block_begin by the caller), same as wg_sync waits.
static pthread_mutex_t s_woa_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_woa_cond = PTHREAD_COND_INITIALIZER;
static int wg_wait_on_address(WGEngine *e, uint32_t addr, uint32_t cmp,
                              uint32_t size, uint32_t ms) {
    if (size == 0 || size > 8) size = (size > 8) ? 8 : 1;
    uint8_t want[8] = {0}, cur[8] = {0};
    wg_blink_read_mem(e->blink, cmp, want, size);
    bool timed = (ms != 0xFFFFFFFFu);
    uint64_t deadline = 0;
    if (timed) { struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
        deadline = (uint64_t)n.tv_sec * 1000 + n.tv_nsec / 1000000 + ms; }
    pthread_mutex_lock(&s_woa_lock);
    int rv;
    for (;;) {
        wg_blink_read_mem(e->blink, addr, cur, size);
        if (memcmp(cur, want, size) != 0) { rv = 1; break; }   // value changed
        // Bounded wait: woken early by WakeByAddress, else re-poll every 1ms so a
        // value change WITHOUT a wake (UE4 does this) is still caught — avoids the
        // indefinite hang while staying off the 100%-spin the old stub caused.
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&s_woa_cond, &s_woa_lock, &ts);
        if (timed) { struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
            uint64_t now = (uint64_t)n.tv_sec * 1000 + n.tv_nsec / 1000000;
            if (now >= deadline) {
                wg_blink_read_mem(e->blink, addr, cur, size);
                rv = (memcmp(cur, want, size) != 0) ? 1 : 0;   // FALSE(0) => ERROR_TIMEOUT
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_woa_lock);
    return rv;
}
static void wg_wake_by_address(void) {
    pthread_mutex_lock(&s_woa_lock);
    pthread_cond_broadcast(&s_woa_cond);   // all waiters re-check their own address
    pthread_mutex_unlock(&s_woa_lock);
}

// WG_WAITCAP=<ms>: cap FINITE wait/sleep timeouts to <ms>. UE4 workers park on
// WaitForSingleObject(work_event, 500ms) and coordination advances one poll at a
// time, so 500ms granularity makes boot crawl (~2 min/phase). Capping to e.g.
// 20ms makes the polls ~25x faster -> coordination proceeds far quicker (and can
// break a poll-based stall). INFINITE (0xFFFFFFFF) and 0 are left untouched.
static uint32_t wg_cap_timeout(uint32_t ms) {
    static int32_t s_cap = -2, s_cap_inf = -2;
    if (s_cap == -2) { const char *e = getenv("WG_WAITCAP"); s_cap = e ? atoi(e) : -1; }
    if (s_cap_inf == -2) { const char *e = getenv("WG_WAITCAP_INF"); s_cap_inf = e ? atoi(e) : 0; }
    if (s_cap <= 0) return ms;                       // disabled
    if (ms == 0) return ms;                          // poll-now untouched
    // Cap only WORKER threads (tid != 1). The MAIN thread (tid 1) runs config-parse
    // whose finite waits are timing-sensitive — capping them broke boot. Workers
    // just poll their task queues, so a shorter poll only speeds task pickup.
    if (s_cur_guest_tid == 1) return ms;
    if (ms == 0xFFFFFFFFu) {
        // Worker INFINITE wait -> turn into a poll ONLY if WG_WAITCAP_INF is set.
        // Breaks a "clean" deadlock where a worker parks forever on an event that
        // is never SetEvent'd but whose task IS enqueued (worker re-checks its queue
        // each poll). Off by default (changing INF semantics is risky).
        return s_cap_inf ? (uint32_t)s_cap_inf : ms;
    }
    return (ms > (uint32_t)s_cap) ? (uint32_t)s_cap : ms;
}

// Check if RIP is in the thunk range and handle the Win32 API call.
// Returns true if a thunk was handled.
static bool handle_blink_thunk(WGEngine *engine) {
    uint64_t rip = wg_blink_get_rip(engine->blink);
    s_thunk_progress++;   // deadlock-watchdog progress heartbeat

    for (int i = 0; i < s_trace_count; i++) {
        if (!s_trace[i].armed || rip != s_trace[i].addr) continue;
        uint64_t trcx = wg_blink_get_reg(engine->blink, 1);
        uint32_t tvt = 0; wg_blink_read_mem(engine->blink, (uint32_t)trcx, &tvt, 4);  // [RCX] vtable
        WG_LOGW(TAG, "TRACE %s @0x%llX: RAX=0x%llX RCX=0x%llX [RCX]=0x%X RDX=0x%llX R8=0x%llX RSP=0x%llX",
                s_trace[i].label, (unsigned long long)rip,
                (unsigned long long)wg_blink_get_reg(engine->blink, 0),
                (unsigned long long)trcx, tvt,
                (unsigned long long)wg_blink_get_reg(engine->blink, 2),
                (unsigned long long)wg_blink_get_reg(engine->blink, 8),
                (unsigned long long)wg_blink_get_reg(engine->blink, 4));
        s_trace[i].armed = false;  // one-shot: disarm after first hit (avoid spin spam)
        // Walk the stack for .text return addresses (rough caller chain).
        if (getenv("WG_TRACE_STACK")) {
            uint32_t sp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
            char chain[400]; int ci = 0, found = 0;
            uint32_t lo = engine->pe_image ? (uint32_t)engine->pe_image->image_base + 0x1000 : 0x401000;
            uint32_t hi = engine->pe_image ? (uint32_t)engine->pe_image->image_base + 0x2358000 : 0x2758000;
            for (int w = 0; w < 200 && found < 12; w++) {
                uint32_t v = 0; wg_blink_read_mem(engine->blink, sp + (uint32_t)w * 8, &v, 4);
                if (v >= lo && v < hi) { ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v); found++; }
            }
            WG_LOGW(TAG, "  callers: %s", chain);
        }
        wg_blink_write_mem(engine->blink, rip, &s_trace[i].orig, 1);
        wg_blink_set_rip(engine->blink, (uint32_t)rip);
        wg_blink_step(engine->blink);
        if (s_trace[i].armed) { uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, rip, &hlt, 1); }
        return true;
    }

    // WG_CTOR_HOOK: UObject base-constructor entry — serialize the construction.
    if (s_ctor_armed && rip == s_ctor_addr) {
        // Pin the GIL on THIS thread for the construction window so no other guest
        // thread reads the published-but-unlinked object (the self-loop corruption).
        // The publish (mov [slot],obj) happens in the caller a few insns BEFORE the
        // `call <ctor>`, with no thunk in between, so it is already slice-atomic; the
        // pin then covers the ctor body + the caller's field-linking that follows.
        if (s_use_real_threads) {
            static int s_ctor_win = -1;
            if (s_ctor_win < 0) { const char *e = getenv("WG_CTOR_PIN_SLICES"); s_ctor_win = e ? atoi(e) : 8; }
            if (s_spin_pin < s_ctor_win) s_spin_pin = s_ctor_win;
        }
        // EMULATE the displaced first instruction `mov [rsp+8], rbx` (48 89 5C 24 08)
        // in C and jump PAST it (rip+5), leaving the HLT byte IN PLACE. Single-stepping
        // the restored instruction would let the JIT re-cache the block at s_ctor_addr
        // as the mov, so the re-armed HLT would never trap again — the hook would fire
        // once and silently die (the bug that let the corruption persist). Keeping the
        // HLT means the block stays cached as a halt and traps on every construction.
        static unsigned long long s_ctor_hits = 0;
        if ((++s_ctor_hits % 5000ULL) == 1) {
            struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts);
            WG_LOGW(TAG, "WG_CTOR_HOOK: %llu constructions @ %ld.%03lds (rate probe)",
                    s_ctor_hits, (long)_ts.tv_sec, _ts.tv_nsec/1000000);
        }
        uint64_t crsp = wg_blink_get_reg(engine->blink, 4);
        uint64_t crbx = wg_blink_get_reg(engine->blink, 3);
        wg_blink_write_mem(engine->blink, (uint32_t)(crsp + 8), &crbx, 8);
        wg_blink_set_rip(engine->blink, (uint32_t)(rip + 5));
        (void)s_ctor_orig;
        return true;
    }

    // WG_LOOPPROBE: UObject intrusive list-walk ADVANCE (0xA5AC68 = `mov rbx,[rbx+0x28]`,
    // rbx = rbx->next). A node whose next points to ITSELF is a single-element circular
    // list whose terminator was never linked (the render-blocking self-loop the code
    // comment at dir_lock() describes) — the guest walk `test rbx; jne` never exits and
    // the boot hangs forever at 0xA5AC30. Fix generically + O(1): if next == current
    // node, hand back 0 so the guest's own null-check terminates the walk. Emulate the
    // 4-byte mov and jump +4, keeping the HLT so it re-traps under the JIT.
    if (s_loop_armed) {
        for (int li = 0; li < WG_NLOOPS; li++) {
            if (!s_loops[li].armed || rip != s_loops[li].addr) continue;
            // CAUTION: a node whose next==self is only a BUG if the walk is genuinely
            // stuck there forever. During construction a node is *transiently* self-
            // referential (terminator not yet linked) — nulling that would corrupt the
            // half-built list and crash the hash insert (0xb72582). So only break after
            // the SAME self-node is re-walked many CONSECUTIVE times (a real infinite
            // loop); a transient self-ref advances (next!=cur, or cur changes) & resets.
            static uint64_t s_loop_last = 0; static unsigned long long s_loop_same = 0;
            static unsigned long long s_loop_breaks = 0;
            const unsigned long long BREAK_AFTER = 200000ULL;
            uint8_t rg = s_loops[li].reg, of = s_loops[li].off;
            uint64_t cur = wg_blink_get_reg(engine->blink, rg);   // current node
            uint64_t nxt = 0; wg_blink_read_mem(engine->blink, (uint32_t)(cur + of), &nxt, 8);
            if (nxt == cur && cur != 0 && cur == s_loop_last) s_loop_same++;
            else { s_loop_same = 0; s_loop_last = cur; }
            if (s_loop_same > BREAK_AFTER) {                       // genuinely stuck self-loop
                // Diagnose the walk's EXIT test (0xA5AC30: cmp [rbx+0x20], rsi; jne exit).
                // On real HW this loop terminates, so either [node+0x20] or rsi is wrong
                // under emulation. Dump both + the node header so we can find the real bug
                // instead of patching (which can drop objects -> missing CDOs).
                uint64_t f20 = 0, f10 = 0, f00 = 0;
                wg_blink_read_mem(engine->blink, (uint32_t)(cur + 0x20), &f20, 8);
                wg_blink_read_mem(engine->blink, (uint32_t)(cur + 0x10), &f10, 8);
                wg_blink_read_mem(engine->blink, (uint32_t)(cur + 0x00), &f00, 8);
                uint64_t rsi = wg_blink_get_reg(engine->blink, 6);
                uint64_t zero = 0;
                wg_blink_write_mem(engine->blink, (uint32_t)(cur + of), &zero, 8);
                nxt = 0;
                s_loop_same = 0;
                if ((++s_loop_breaks % 1000ULL) == 1)
                    WG_LOGW(TAG, "WG_LOOPBREAK: self-loop node=0x%llX @0x%llX | [+0x20]=0x%llX rsi=0x%llX (eq=%d) [+0x10 class]=0x%llX [+0 vtbl]=0x%llX",
                            (unsigned long long)cur, (unsigned long long)rip,
                            (unsigned long long)f20, (unsigned long long)rsi, (f20==rsi),
                            (unsigned long long)f10, (unsigned long long)f00);
            }
            wg_blink_set_reg(engine->blink, rg, nxt);            // reg = next (or 0 to break)
            wg_blink_set_rip(engine->blink, (uint32_t)(rip + 4)); // past the 4-byte mov
            (void)s_loop_orig;
            return true;
        }
    }

    // WG_ANIMFIX: supply the default anim curve compression settings path the config
    // getter (0x18822A3: call 0x2f8190) would otherwise return empty. rdx = out FString
    // {Data(8), ArrayNum(4), ArrayMax(4)}. Write the wide path into a fresh guest buffer,
    // point the FString at it, return "found" (rax=1), and skip the getter call.
    // Build a minimal real config entry (once) whose value string is the correct path,
    // so the natural flow: section -> entry[0] -> value.Data (rbx) -> LoadObject(r8=path)
    // loads the (pak-present) asset instead of fatal'ing. Layout the guest code expects:
    //   section S: [S+0] = entries array base
    //   entry  E (48B): value struct at E+8, i.e. value.Data at E+0x18, value.Len at E+0x20
    static uint32_t s_anim_section = 0;
    if (s_anim_armed && (rip == s_anim_addr || rip == s_anim_addr2) && !s_anim_section) {
        static const char kPath[] = "/Engine/Animation/DefaultAnimCurveCompressionSettings";
        int len = (int)strlen(kPath);
        uint32_t P = wg_guest_alloc(engine, (uint32_t)((len + 1) * 2));
        uint32_t E = wg_guest_alloc(engine, 64);
        uint32_t S = wg_guest_alloc(engine, 16);
        if (P && E && S) {
            uint16_t w[80]; for (int i = 0; i <= len; i++) w[i] = (uint16_t)(uint8_t)kPath[i];
            wg_blink_write_mem(engine->blink, P, w, (uint32_t)((len + 1) * 2));
            uint8_t z[64] = {0}; wg_blink_write_mem(engine->blink, E, z, 64);
            uint64_t pd = P; int32_t plen = len + 1;
            wg_blink_write_mem(engine->blink, E + 0x18, &pd, 8);    // value.Data = path
            wg_blink_write_mem(engine->blink, E + 0x20, &plen, 4);  // value.Len
            uint64_t eb = E; wg_blink_write_mem(engine->blink, S, &eb, 8);  // [S+0] = entries
            s_anim_section = S;
            WG_LOGW(TAG, "WG_ANIMFIX: built config entry S=0x%X E=0x%X path=0x%X", S, E, P);
        }
    }
    // Section lookup: return our fake section so the value path is taken.
    if (s_anim_armed && rip == s_anim_addr2) {
        wg_blink_set_reg(engine->blink, 0, s_anim_section);     // rax = section
        wg_blink_set_rip(engine->blink, (uint32_t)(rip + 5));
        (void)s_anim_orig2;
        return true;
    }
    // Value getter: return index 0 (our single entry). rdx = &out index ([rsp+0x40]).
    if (s_anim_armed && rip == s_anim_addr) {
        uint64_t rdx = wg_blink_get_reg(engine->blink, 2);
        int32_t idx = 0;
        if (rdx) wg_blink_write_mem(engine->blink, (uint32_t)rdx, &idx, 4);
        wg_blink_set_reg(engine->blink, 0, 0);
        wg_blink_set_rip(engine->blink, (uint32_t)(rip + 5));
        (void)s_anim_orig;
        return true;
    }

    // Real-threads: handle the FUNCTIONAL cipher-list max_ver trap here (not just
    // in the main tick) because the ClientHello is built on whichever thread runs
    // the SSL — often a worker. Without it the cipher list is empty -> NO_CIPHERS
    // -> BoringSSL sends a fatal internal_error alert instead of a ClientHello.
    // (Cooperative mode keeps handling it in the tick.)
    if (s_use_real_threads && s_watch_armed && rip == s_watch_addr) {
        uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
        uint32_t sub = 0; wg_blink_read_mem(engine->blink, esi + 0x7c, &sub, 4);
        uint32_t ver = 0;
        if (sub) {
            wg_blink_read_mem(engine->blink, sub + 0x2ac, &ver, 4);
            if (ver > 0x303) { uint32_t v12 = 0x303; wg_blink_write_mem(engine->blink, sub + 0x2ac, &v12, 4); }
        }
        if (s_watch_count < 6)
            WG_LOGW(TAG, "*** [realthr] s_watch FIRED tid=0x%X esi=0x%X sub=0x%X max_ver 0x%X->0x303",
                    s_cur_guest_tid, esi, sub, ver);
        s_watch_count++;
        wg_blink_write_mem(engine->blink, s_watch_addr, &s_watch_orig, 1);
        wg_blink_set_rip(engine->blink, s_watch_addr);
        wg_blink_step(engine->blink);
        uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_watch_addr, &hlt, 1);
        return true;
    }

    // [facwatch] WG_FACWATCH=1: trap the Visage boot-blocking +0x38 factory call
    // (guest 0xe18b46 = `movq (%rcx),%rax`, just before `call [rax+0x38]` which
    // fills [rsp+0x48] and returns NULL). Logs prevobj + its vtable + the +0x38
    // method address so the null-returning factory can be identified/disassembled.
    {
        static uint32_t s_fw_addr = 0xe18b46;
        static uint8_t  s_fw_orig = 0;
        static bool     s_fw_armed = false;
        static signed char s_fw_on = -1;
        if (s_fw_on < 0) s_fw_on = getenv("WG_FACWATCH") ? 1 : 0;
        if (s_fw_on && !s_fw_armed) {
            // Wait until the shipping exe's real code (`48 8b 01` = movq (%rcx),%rax)
            // is loaded at 0xe18b46 — arming during the launcher phase reads 0x00 and
            // the HLT gets overwritten when the image loads.
            uint8_t op[3] = {0};
            if (wg_blink_read_mem(engine->blink, s_fw_addr, op, 3) &&
                op[0] == 0x48 && op[1] == 0x8b && op[2] == 0x01) {
                s_fw_orig = op[0];
                uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_fw_addr, &hlt, 1);
                s_fw_armed = true;
                WG_LOGW(TAG, "[facwatch] armed @0x%X", s_fw_addr);
            }
        }
        if (s_fw_armed && rip == s_fw_addr) {
            uint32_t rcxo = (uint32_t)wg_blink_get_reg(engine->blink, 1); // prevobj
            uint64_t vt = 0, m38 = 0;
            wg_blink_read_mem(engine->blink, rcxo, &vt, 8);
            if (vt) wg_blink_read_mem(engine->blink, (uint32_t)vt + 0x38, &m38, 8);
            static int _fw = 0;
            if (_fw++ < 12)
                WG_LOGW(TAG, "[facwatch] tid=0x%X prevobj=0x%X vtable=0x%llx [+0x38]=0x%llx",
                        s_cur_guest_tid, rcxo, (unsigned long long)vt, (unsigned long long)m38);
            wg_blink_write_mem(engine->blink, s_fw_addr, &s_fw_orig, 1);
            wg_blink_set_rip(engine->blink, s_fw_addr);
            wg_blink_step(engine->blink);
            uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_fw_addr, &hlt, 1);
            return true;
        }
    }

    // Real-threads: cap the SSL_CTX max_proto_version at SSL_CTX_set_cipher_list
    // (0x69B720, arg1=ctx). WITHOUT this the ClientHello's supported_versions ext
    // (and key_share) still advertise TLS1.3 (0x0304) even though s_watch clamps
    // the CIPHER list to TLS1.2 — an inconsistent hello (offers only a TLS1.2
    // suite yet invites TLS1.3) that the CDN rejects with a fatal handshake_failure
    // (proven by replaying the exact bytes to cdn.steamstatic.com). Capping
    // ctx+0xb8 -> 0x303 makes the whole hello consistently TLS1.2. Cooperative
    // mode already does this in the tick; real-threads (device default) skipped it,
    // which is why the manifest handshake never completed. (esp+8 = the ctx arg.)
    if (s_use_real_threads && s_cloop_armed && rip == s_cloop_addr) {
        uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
        uint32_t a1 = 0; wg_blink_read_mem(engine->blink, esp + 4, &a1, 4);
        if (a1) {
            uint32_t maxv = 0;
            wg_blink_read_mem(engine->blink, a1 + 0xb8, &maxv, 4);
            if (maxv == 0 || maxv > 0x303) {
                uint32_t v12 = 0x303;
                wg_blink_write_mem(engine->blink, a1 + 0xb8, &v12, 4);
                if (s_cloop_count < 6)
                    WG_LOGW(TAG, "*** [realthr] capped ctx=0x%X max_proto_version 0x%X -> 0x303", a1, maxv);
            }
        }
        s_cloop_count++;
        wg_blink_write_mem(engine->blink, s_cloop_addr, &s_cloop_orig, 1);
        wg_blink_set_rip(engine->blink, s_cloop_addr);
        wg_blink_step(engine->blink);
        uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_cloop_addr, &hlt, 1);
        return true;
    }

    // D3D11/DXGI COM vtable method call (a separate thunk region). x64
    // caller-clean: the callee pops only the return address. this=RCX.
    if (wg_d3d11_is_thunk(rip)) {
        uint64_t rsp = wg_blink_get_reg(engine->blink, 4);
        uint64_t ret_addr = 0;
        wg_blink_read_mem(engine->blink, rsp, &ret_addr, 8);
        uint32_t cargs[16] = {0};
        cargs[0] = (uint32_t)wg_blink_get_reg(engine->blink, 1); // RCX (this)
        cargs[1] = (uint32_t)wg_blink_get_reg(engine->blink, 2); // RDX
        cargs[2] = (uint32_t)wg_blink_get_reg(engine->blink, 8); // R8
        cargs[3] = (uint32_t)wg_blink_get_reg(engine->blink, 9); // R9
        uint64_t stackargs[12] = {0};
        wg_blink_read_mem(engine->blink, rsp + 8 + 32, stackargs, sizeof(stackargs));
        for (int i = 0; i < 12; i++) cargs[4 + i] = (uint32_t)stackargs[i];
        uint64_t cret = 0;
        wg_d3d11_dispatch(engine, rip, cargs, &cret);
        wg_blink_set_reg(engine->blink, 4, rsp + 8);     // pop return addr
        wg_blink_set_rip(engine->blink, (uint32_t)ret_addr);
        wg_blink_set_reg(engine->blink, 0, cret);         // RAX
        return true;
    }

    // Check both 32-bit (0xC00000) and 64-bit (0xDEAD0000) thunk ranges
    bool in_thunk_range = false;
    if (rip >= 0xC00000ULL && rip < 0xC00000ULL + 0x20000) in_thunk_range = true;
    if (rip >= WG_THUNK_BASE && rip < WG_THUNK_BASE + 0x20000) in_thunk_range = true;
    if (!in_thunk_range) return false;
 
    // SEH handler returned a disposition: advance the chain or resume.
    if (rip == WG_SEH_SENTINEL) {
        uint32_t disp = (uint32_t)wg_blink_get_reg(engine->blink, 0); // EAX
        WG_LOGW(TAG, "SEH: handler disposition=%d", (int)disp);
        if (disp == 0 /*ExceptionContinueExecution*/) {
            // Resume from the (handler-modified) CONTEXT.
            uint32_t eip=0, esp=0, r;
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EIP,&eip,4);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_ESP,&esp,4);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EAX,&r,4); wg_blink_set_reg(engine->blink,0,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_ECX,&r,4); wg_blink_set_reg(engine->blink,1,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EDX,&r,4); wg_blink_set_reg(engine->blink,2,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EBX,&r,4); wg_blink_set_reg(engine->blink,3,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EBP,&r,4); wg_blink_set_reg(engine->blink,5,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_ESI,&r,4); wg_blink_set_reg(engine->blink,6,r);
            wg_blink_read_mem(engine->blink, s_seh_ctx+WG_CTX_EDI,&r,4); wg_blink_set_reg(engine->blink,7,r);
            wg_blink_set_reg(engine->blink, 4, esp);
            wg_blink_set_rip(engine->blink, eip);
            s_seh_active = false; s_seh_depth = 0;
            return true;
        }
        // ExceptionContinueSearch (1) / other: go to the next registration.
        uint32_t next = 0;
        wg_blink_read_mem(engine->blink, s_seh_frame + 0, &next, 4);
        if (next > 0x1000u && next < 0xFFFFFFFEu && next > s_seh_frame) {
            uint32_t handler = 0;
            wg_blink_read_mem(engine->blink, next + 4, &handler, 4);
            if (handler > 0x1000u) {
                s_seh_frame = next;
                wg_seh_call_handler(engine, handler, next);
                return true;
            }
        }
        WG_LOGE(TAG, "SEH: unhandled exception — terminating");
        s_seh_active = false; s_seh_depth = 0;
        wg_blink_set_rip(engine->blink, 0); // halt
        return true;
    }

    // Modal-dialog return trap: the dlgproc has returned from WM_INITDIALOG, so
    // the dialog is now up. Paint its controls and pause (modal) — we stay here
    // until the guest calls EndDialog (which returns control to WinMain).
    if (rip == WG_DLG_SENTINEL) {
        if (s_dlg_active && s_dlg_hwnd) wg_render_dialog(engine, s_dlg_hwnd);
        engine->state = WG_ENGINE_PAUSED;
        WG_LOGI(TAG, "Dialog up (HWND=0x%X) — modal, waiting", s_dlg_hwnd);
        return true;
    }
    // A SendMessage'd wndproc just returned — hand its result back to the
    // SendMessage caller (and re-render in case the page content changed).
    if (rip == WG_SENDMSG_SENTINEL) {
        uint32_t result = (uint32_t)wg_blink_get_reg(engine->blink, 0);
        if (s_callstack_depth > 0) {
            s_callstack_depth--;
            WGPendingCall *pc = &s_callstack[s_callstack_depth];
            // CreateDialog dispatches WM_INITDIALOG but must still return the
            // HWND, not the dlgproc's result — that's what ovr_eax carries.
            if (pc->ovr) result = pc->ovr_eax;
            // Restore the caller's registers (the wndproc may have clobbered
            // nonvolatile ones). RSP and RAX are set explicitly below.
            for (int i = 1; i < 16; i++) {
                if (i == 4) continue;            // RSP set from ret_rsp
                wg_blink_set_reg(engine->blink, i, pc->saved_regs[i]);
            }
            wg_blink_set_reg(engine->blink, 4, pc->ret_rsp);
            wg_blink_set_rip(engine->blink, pc->ret_addr);
            wg_blink_set_reg(engine->blink, 0, result);
            if (s_dlg_active && s_dlg_hwnd) wg_render_dialog(engine, s_dlg_hwnd);
        } else {
            wg_blink_set_rip(engine->blink, 0);
        }
        return true;
    }

    WGWin32StubFunc handler = wg_dll_mapper_get_handler(
        engine->dll_mapper, rip);

    // Find the entry for this thunk
    WGDllEntry *entry = NULL;
    for (int i = 0; i < engine->dll_mapper->count; i++) {
        if (engine->dll_mapper->entries[i].thunk_addr == rip) {
            entry = &engine->dll_mapper->entries[i];
            break;
        }
    }

    if (entry && getenv("WG_WORKERLOG")) {
        // Diagnostic: log EVERY thunk from a worker thread (tid != 1), bypassing
        // the quiet-list, so we can see what a parked worker is actually calling.
        uint32_t _wtid = engine->scheduler ? wg_sched_current_tid(engine->scheduler) : 0;
        if (_wtid != 0 && _wtid != 1) {
            static int _wn = 0;
            if (_wn++ < 200)
                WG_LOGW(TAG, "[worker tid=0x%X] %s (rip=0x%llX)",
                        _wtid, entry->func_name, (unsigned long long)rip);
        }
    }

    if (entry) {
        // Suppress noisy repetitive calls
        static const char *quiet_funcs[] = {
            "GetTickCount", "CharNextW", "CharNextA",
            "CharPrevW", "lstrcpynW", "lstrlenW", "lstrcatW",
            "lstrcmpiW", "GlobalAlloc", "GlobalFree",
            "ReadFile", "WriteFile",
            "PeekMessageW", "lstrlenA",
            "Sleep", "SleepEx",  // spin-loop noise; watchdog covers stuck loops
            "WaitForSingleObject", "WaitForSingleObjectEx", // poll-loop noise
            "QueryPerformanceCounter", "GetSystemTimePreciseAsFileTime",
            // High-frequency CRT/heap/TLS noise — drowns out the network trace and
            // produces multi-GB logs over a 200s run. Safe to silence.
            "HeapAlloc", "HeapFree", "HeapSize", "HeapReAlloc",
            "GetLastError", "SetLastError", "TlsGetValue", "TlsSetValue",
            "FlsGetValue", "FlsSetValue", // download-completion poll spins on these
            "EnterCriticalSection", "LeaveCriticalSection",
            "TryEnterCriticalSection",
            "TranslateMessage", "DispatchMessageW", "GetMessageW",
            "GetCurrentThreadId",
            // Winsock poll-loop ordinals: select(18), __WSAFDIsSet(151),
            // recv(16), setsockopt(21), htonl(8)/htons(9)/ntohl(14). These fire
            // per-iteration during downloads and bury the actual recv/save trace.
            // recv still logs its own "[WSock] recv(...) -> N bytes" line.
            "Ordinal_18", "Ordinal_151", "Ordinal_16", "Ordinal_21",
            "Ordinal_8", "Ordinal_9", "Ordinal_14",
            NULL
        };
        bool quiet = false;
        for (int i = 0; quiet_funcs[i]; i++) {
            if (strcmp(entry->func_name, quiet_funcs[i]) == 0) { quiet = true; break; }
        }
        // Also quiet the high-frequency CRT mem/str helpers — they dominate the
        // trace and drown out real progress (only matters for the log, not dispatch).
        if (!quiet && getenv("WG_QUIET_CRT")) {
            const char *f2 = entry->func_name;
            if (strncmp(f2,"mem",3)==0 || strncmp(f2,"wcs",3)==0 ||
                strncmp(f2,"str",3)==0 || strncmp(f2,"_wcs",4)==0) quiet = true;
        }
        if (!quiet) {
            uint32_t cur_tid = s_use_real_threads ? s_cur_guest_tid
                                                  : wg_sched_current_tid(engine->scheduler);
            // Read first 3 args speculatively (safe — within the guest stack page)
            uint64_t peek_rsp = wg_blink_get_reg(engine->blink, 4);
            bool peek_32 = (engine->pe_image && !engine->pe_image->is_64bit);
            uint32_t a0=0, a1=0, a2=0;
            if (peek_32 && peek_rsp) {
                wg_blink_read_mem(engine->blink, peek_rsp + 4,  &a0, 4);
                wg_blink_read_mem(engine->blink, peek_rsp + 8,  &a1, 4);
                wg_blink_read_mem(engine->blink, peek_rsp + 12, &a2, 4);
            } else if (!peek_32) {
                a0 = (uint32_t)wg_blink_get_reg(engine->blink, 1);  // RCX
                a1 = (uint32_t)wg_blink_get_reg(engine->blink, 2);  // RDX
                a2 = (uint32_t)wg_blink_get_reg(engine->blink, 8);  // R8
            }
            WG_LOGI(TAG, "[tid=%X] Win32: %s!%s(0x%X,0x%X,0x%X)",
                    cur_tid, entry->dll_name, entry->func_name, a0, a1, a2);
        }
    }

    // Determine pointer size (32-bit PE uses 4-byte pointers)
    bool is_32bit = (engine->pe_image && !engine->pe_image->is_64bit);
    int ptr_size = is_32bit ? 4 : 8;

    // Read return address from stack
    uint64_t rsp = wg_blink_get_reg(engine->blink, 4); // RSP
    uint64_t ret_addr = 0;
    if (is_32bit) {
        uint32_t r32;
        wg_blink_read_mem(engine->blink, rsp, &r32, 4);
        ret_addr = r32;
    } else {
        wg_blink_read_mem(engine->blink, rsp, &ret_addr, 8);
    }

    // For 32-bit cdecl/stdcall, arguments are on the stack after the return address.
    // Read up to 16 args (CreateFontW has 14 params; reading a few extra words
    // past a shorter call's args is harmless — we only use the indices we need).
    // For 64-bit (Microsoft x64): RCX, RDX, R8, R9, then stack args after the
    // return address + 32-byte shadow space. Truncating to 32 bits is safe:
    // 64-bit images are rebased below 4GB and stack/heap/thunks all sit there.
    uint32_t args[16] = {0};
    // Full 64-bit args (parallel to the 32-bit `args`). Handlers that take guest
    // POINTERS must use args64 so they work with the 64-bit VirtualAlloc heap
    // (>4GB) — reading the pointer as 32-bit `args[i]` truncates it to garbage.
    uint64_t args64[16] = {0};
    if (is_32bit) {
        wg_blink_read_mem(engine->blink, rsp + 4, args, sizeof(args));
        for (int i = 0; i < 16; i++) args64[i] = args[i];
    } else {
        args64[0] = wg_blink_get_reg(engine->blink, 1);  // RCX
        args64[1] = wg_blink_get_reg(engine->blink, 2);  // RDX
        args64[2] = wg_blink_get_reg(engine->blink, 8);  // R8
        args64[3] = wg_blink_get_reg(engine->blink, 9);  // R9
        uint64_t stack_args[12] = {0};
        wg_blink_read_mem(engine->blink, rsp + 8 + 32, stack_args, sizeof(stack_args));
        for (int i = 0; i < 12; i++) args64[4 + i] = stack_args[i];
        for (int i = 0; i < 16; i++) args[i] = (uint32_t)args64[i];
    }

    // Default return value: the registered stub's intent (R1S->1, etc.). The
    // explicit handlers below override this; functions NOT handled explicitly
    // now honor their registration instead of always returning 0 (the dead-stub
    // trap that silently broke IsWindowEnabled/CreateThread/GetExitCodeProcess…).
    uint64_t ret_val = entry ? (uint64_t)(int64_t)(int32_t)entry->default_ret : 0;

    // Handle specific Win32 functions that affect visual output
    if (entry) {
        const char *fn = entry->func_name;

        // Capture MessageBox text — UE4's fatal-error path pops a MessageBox
        // ("Assertion failed", "Fatal error", missing-content, etc.) right
        // before exit(1). We need the message to know WHY the game bails.
        if (!strcmp(fn, "MessageBoxW") || !strcmp(fn, "MessageBoxA")) {
            char text[2048] = {0}, cap[512] = {0};
            if (fn[10] == 'W') {  // MessageBoxW
                uint16_t wt[2048], wc[512];
                wg_read_wstr(engine, (uint32_t)args64[1], wt, 2048);
                wg_read_wstr(engine, (uint32_t)args64[2], wc, 512);
                for (int i = 0; i < 2047 && wt[i]; i++) text[i] = (char)(wt[i] < 128 ? wt[i] : '?');
                for (int i = 0; i < 511  && wc[i]; i++) cap[i]  = (char)(wc[i] < 128 ? wc[i] : '?');
            } else {
                wg_read_cstr(engine, (uint32_t)args64[1], text, sizeof text);
                wg_read_cstr(engine, (uint32_t)args64[2], cap, sizeof cap);
            }
            WG_LOGE(TAG, "*** MessageBox [%s]: %s", cap, text);
        }

        // DIAG (passive — does NOT handle wcsstr, so it stays auto-stubbed): log the
        // needle the game keeps searching for in the asset-scan loop, once per unique
        // needle string, to identify what it's looking for and never finding.
        if (!strcmp(fn, "wcsstr")) {
            static uint64_t s_seen_needle[64]; static int s_seen_n = 0;
            uint64_t ndl = args64[1]; int seen = 0;
            for (int i = 0; i < s_seen_n; i++) if (s_seen_needle[i] == ndl) { seen = 1; break; }
            if (!seen && s_seen_n < 64) {
                s_seen_needle[s_seen_n++] = ndl;
                uint16_t w[256]; wg_read_wstr(engine, ndl, w, 256);
                char nb[256]; int i = 0; for (; i < 255 && w[i]; i++) nb[i] = (char)(w[i] < 128 ? w[i] : '?'); nb[i] = 0;
                uint16_t hw[64]; wg_read_wstr(engine, args64[0], hw, 64);
                char hb[64]; int k = 0; for (; k < 63 && hw[k]; k++) hb[k] = (char)(hw[k] < 128 ? hw[k] : '?'); hb[k] = 0;
                WG_LOGW(TAG, "wcsstr needle[%d]=\"%s\"  haystack@0x%llX starts \"%s\"",
                        s_seen_n, nb, (unsigned long long)args64[0], hb);
            }
        }

        // DIAG: identify a thunk the MAIN gets stuck spinning on (same fn 5000x in a row).
        if (s_cur_guest_tid == 1) {
            static const char *s_lastfn = 0; static unsigned s_samefn = 0;
            if (fn == s_lastfn) { if (++s_samefn == 5000) WG_LOGW(TAG, "*** MAIN spinning on thunk %s @0x%llX (rip)", fn, (unsigned long long)rip); }
            else { s_samefn = 0; s_lastfn = fn; }
        }

        // WG_POLLYIELD: general busy-wait breaker for the post-swapchain async-load
        // coordination. The main thread polls a worker-produced buffer/flag through
        // MANY sites (PeekMessageW empty, memcmp, wcsstr, _wtoi64, ... — each seen as
        // the SAME thunk called with the SAME args over and over). Holding the GIL
        // across such a poll starves the very worker that must change the value (the
        // workers block on their FEvent so they don't count as GIL contenders, so
        // adaptive slicing keeps the main on huge slices). When the main repeats one
        // (fn,args) past a threshold, RELEASE the GIL each iteration so the worker
        // gets a real run window. A genuine (non-poll) call varies its args and
        // resets the counter, so real work isn't penalized. WG_POLLTHR / WG_POLLYIELD_US tune.
        // WG_SETEVLOG: one-shot per tid — log the caller of a hot SetEvent so a
        // livelock (a worker spinning SetEvent on an unwaited event) can be located.
        if (getenv("WG_SETEVLOG") && !strcmp(fn, "SetEvent")) {
            static uint32_t s_sel_h[16]; static uint32_t s_sel_t[16]; static int s_sel_n = 0;
            uint32_t h = args[0]; int seen = 0;
            for (int i = 0; i < s_sel_n; i++) if (s_sel_h[i]==h && s_sel_t[i]==s_cur_guest_tid) { seen=1; break; }
            if (!seen && s_sel_n < 16) { s_sel_h[s_sel_n]=h; s_sel_t[s_sel_n]=s_cur_guest_tid; s_sel_n++;
                WG_LOGW(TAG, "SETEVLOG tid=0x%X SetEvent(0x%X) caller=0x%llX", s_cur_guest_tid, h, (unsigned long long)ret_addr); }
        }
        // WG_TASKPROBE: at the task-graph completion livelock, capture the task object
        // (rdi/rbx, preserved through FEvent::Trigger) on each worker SetEvent from the
        // 0x9FFF59 completion site, so we can see if it is the SAME task re-processed
        // (queue-pop that never advances) or different tasks. Also dump the predicate
        // object [rdi+0x18] and its first qword so a self-loop/closed-list is visible.
        {
            static signed char s_tpen = -1;
            if (s_tpen < 0) s_tpen = getenv("WG_TASKPROBE") ? 1 : 0;
            if (s_tpen && s_past_init && s_cur_guest_tid != 1 && fn[0]=='S' && !strcmp(fn, "SetEvent")) {
                // Detect the SPIN post-init: count SetEvents per (tid,handle); once one
                // crosses a threshold it is the livelock — then log its task pointer
                // (rdi) across a window. Constant rdi => the SAME task re-processed
                // (queue/subsequents that never advances). Counting is cheap; guest mem
                // is read only in the log window.
                static uint32_t s_h[64], s_t[64]; static unsigned s_c[64]; static int s_n=0;
                uint32_t h=args[0]; int idx=-1;
                for (int i=0;i<s_n;i++) if(s_h[i]==h && s_t[i]==s_cur_guest_tid){idx=i;break;}
                if (idx<0 && s_n<64){ idx=s_n++; s_h[idx]=h; s_t[idx]=s_cur_guest_tid; s_c[idx]=0; }
                if (idx>=0){ s_c[idx]++;
                    if (s_c[idx] >= 3000 && s_c[idx] <= 3040) {
                        uint64_t rdi = wg_blink_get_reg(engine->blink, 7);
                        uint32_t f18=0, f20=0, sub0=0, f28=0, pfn=0;
                        wg_blink_read_mem(engine->blink,(uint32_t)rdi+0x18,&f18,4);
                        wg_blink_read_mem(engine->blink,(uint32_t)rdi+0x20,&f20,4);
                        wg_blink_read_mem(engine->blink,(uint32_t)rdi+0x28,&f28,4);
                        if(f18){ wg_blink_read_mem(engine->blink,f18,&sub0,4);      // predicate vtable
                                 if(sub0) wg_blink_read_mem(engine->blink,sub0,&pfn,4); } // vtable[0] fn
                        WG_LOGW(TAG,"TASKSPIN tid=0x%X h=0x%X cnt=%u rdi=0x%llX [+0x18]=0x%X vt=0x%X vt[0]=0x%X [+0x20]=0x%X [+0x28]=0x%X",
                            s_cur_guest_tid,h,s_c[idx],(unsigned long long)rdi,f18,sub0,pfn,f20,f28);
                    }
                }
            }
        }
        if (s_use_real_threads && s_cur_guest_tid == 1 && getenv("WG_POLLYIELD")) {
            static const char *s_lfn = 0; static uint32_t s_la0=0,s_la1=0,s_la2=0; static int s_prep=0;
            if (fn == s_lfn && args[0]==s_la0 && args[1]==s_la1 && args[2]==s_la2) {
                if (s_prep < 1000000) s_prep++;
            } else { s_lfn = fn; s_la0=args[0]; s_la1=args[1]; s_la2=args[2]; s_prep = 0; }
            long thr = getenv("WG_POLLTHR") ? atol(getenv("WG_POLLTHR")) : 8;
            if (s_prep >= thr) {
                long us = getenv("WG_POLLYIELD_US") ? atol(getenv("WG_POLLYIELD_US")) : 150;
                wg_thunk_block_begin(); usleep((useconds_t)us); wg_thunk_block_end();
            }
        }

        // [VIEWER heartbeat] every N registered-thunk calls, print a liveness/phase
        // line to stderr (visible at WG_LOG_LEVEL=E). The guest caller addr reveals
        // the phase (0x9xxxxx config-init, 0xA/0xBxxxxx shader/UObject grind). Lets a
        // long full-speed grind be observed without the disk-I/O throttle of INFO logs.
        {
            static unsigned long long s_hb_calls = 0; static unsigned long s_hb_every = 0;
            if (!s_hb_every) { const char *e = getenv("WG_HB");
                s_hb_every = (e && strtoul(e,0,0)) ? strtoul(e,0,0) : 2000000UL; }
            if ((++s_hb_calls % s_hb_every) == 0)
                fprintf(stderr, "[HB] %llu thunk-calls  caller=0x%llx  last=%s\n",
                        s_hb_calls, (unsigned long long)ret_addr, fn ? fn : "?");
        }

        // [objfill DIAG] WG_OBJDIAG=1: name imported calls whose return address is in
        // a watched code range — used to identify the imports feeding a corrupt/NULL
        // out-param object (e.g. CoCreateInstance for WMI, EnterCriticalSection for the
        // Visage config-parse factory). Off by default.
        static signed char s_objdiag = -1;
        if (s_objdiag < 0) s_objdiag = getenv("WG_OBJDIAG") ? 1 : 0;
        if (s_objdiag && !is_32bit &&
            ((ret_addr >= 0x9f3c00 && ret_addr <= 0x9f4a00) ||
             (ret_addr >= 0x90fb00 && ret_addr <= 0x90fc00) ||
             (ret_addr >= 0x519900 && ret_addr <= 0x519f00))) {
            static int s_objfill_diag = 0;
            if (s_objfill_diag++ < 120)
                WG_LOGW(TAG, "[objfill] ret=0x%llx %s!%s rcx=0x%x rdx=0x%x r8=0x%x r9=0x%x arg5=0x%x",
                        (unsigned long long)ret_addr, entry->dll_name ? entry->dll_name : "?",
                        fn, args[0], args[1], args[2], args[3], args[4]);
        }

        if (entry->dll_name && strcasecmp(entry->dll_name, "nsDialogs.dll") == 0) {
            ret_val = handle_nsdialogs(engine, fn, args);
        } else if (wg_try_crt(engine, fn, args, args64, &ret_val)) {
            // Handled as a C runtime function (string/memory/ctype/heap/startup).
        } else if (strcmp(fn, "CreateDXGIFactory") == 0 ||
                   strcmp(fn, "CreateDXGIFactory1") == 0 ||
                   strcmp(fn, "CreateDXGIFactory2") == 0) {
            ret_val = wg_d3d11_CreateDXGIFactory(engine, args, fn[16] != 0);
        } else if (strcmp(fn, "D3D11CreateDevice") == 0) {
            ret_val = wg_d3d11_D3D11CreateDevice(engine, args, false);
        } else if (strcmp(fn, "D3D11CreateDeviceAndSwapChain") == 0) {
            ret_val = wg_d3d11_D3D11CreateDevice(engine, args, true);
        } else if (strcmp(fn, "CreateWindowExW") == 0) {
            // stdcall CreateWindowExW(exStyle, className, windowName, style,
            //                         x, y, w, h, parent, menu, instance, param)
            // args[0]=exStyle, args[1]=className, args[2]=windowName, args[3]=style
            // args[4]=x, args[5]=y, args[6]=w, args[7]=h, args[8]=parent
            uint16_t title_buf[256] = {0};
            if (args[2]) {
                wg_blink_read_mem(engine->blink, args[2], title_buf, 510);
            }
            ret_val = wg_wm_create_window(args[0], args[1], title_buf,
                                          args[3], (int32_t)args[4], (int32_t)args[5],
                                          (int32_t)args[6], (int32_t)args[7], args[8]);
            // If this is a child control with a known window class (nsDialogs
            // builds its page body this way), register it so its text/box paints
            // on the parent page. args[8]=parent, args[9]=hMenu(=child id),
            // args[1]=class (string ptr for STATIC/BUTTON/EDIT, or an atom).
            if (args[8] && args[1] > 0xFFFF) {
                uint16_t clsw[40] = {0}; char clsa[40] = {0};
                wg_blink_read_mem(engine->blink, args[1], clsw, 78);
                for (int i = 0; i < 39 && clsw[i]; i++)
                    clsa[i] = clsw[i] < 128 ? (char)tolower((unsigned char)clsw[i]) : '?';
                uint16_t cls = 0;
                if (strstr(clsa, "static"))      cls = 0x0082;
                else if (strstr(clsa, "button")) cls = 0x0080;
                else if (strstr(clsa, "edit"))   cls = 0x0081;
                if (cls) {
                    wg_register_child_control(args[8], args[9], args[3], cls,
                                              (int32_t)args[4], (int32_t)args[5],
                                              (int32_t)args[6], (int32_t)args[7], title_buf);
                    if (s_dlg_active) wg_render_dialog(engine, args[8]);
                }
            }
        } else if (strcmp(fn, "ShowWindow") == 0) {
            s_past_init = 1;   // window shown => past registration; ungate pool workers
            wg_wm_show(args[0], args[1]);
            ret_val = 1;
        } else if (strcmp(fn, "DestroyWindow") == 0) {
            wg_wm_destroy(args[0]);
            wg_remove_ctrls(args[0]);   // drop the page's controls
            ret_val = 1;
        } else if (strcmp(fn, "SetWindowTextW") == 0) {
            uint16_t text_buf[256] = {0};
            if (args[1]) {
                wg_blink_read_mem(engine->blink, args[1], text_buf, 510);
            }
            // Control handle? store its text and repaint the page. NSIS sets the
            // directory edit's default path this way (SetWindowText on the edit's
            // HWND, not SetDlgItemText); without this the field shows blank AND
            // GetWindowText/GetDlgItemText read back empty -> $INSTDIR empty ->
            // everything installs to the drive_c root.
            WGDlgCtrl *cc = wg_ctrl_from_handle(args[0]);
            if (cc) {
                int n = 0; while (n < 79 && text_buf[n]) { cc->text[n] = text_buf[n]; n++; }
                cc->text[n] = 0;
                if (s_dlg_active) wg_render_dialog(engine, cc->hwnd);
                ret_val = 1;
                goto setwindowtext_done;
            }
            wg_wm_set_text(args[0], text_buf);
            // Render text into the window's client area
            WGWin32Window *tw = wg_wm_find(args[0]);
            if (tw && tw->w > 0 && tw->h > 0 && text_buf[0]) {
                int32_t cw, ch;
                uint32_t *client = wg_wm_get_client(args[0], &cw, &ch);
                if (client && cw > 0 && ch > 0) {
                    // Fill with light gray background
                    for (int p = 0; p < cw * ch; p++)
                        client[p] = 0xFFF0F0F0;
                    // Count chars for text
                    int tlen = 0;
                    while (tlen < 255 && text_buf[tlen]) tlen++;
                    // Render text (use GDI text_out on this window's DC)
                    uint32_t dc = wg_gdi_get_dc(args[0]);
                    wg_gdi_set_text_color(dc, 0x00000000);
                    int lh = wg_gdi_line_height(dc);
                    wg_gdi_text_out(dc, 4, (ch - lh) / 2, text_buf, tlen);
                    wg_gdi_release_dc(dc);
                    tw->client_dirty = true;
                }
            }
            ret_val = 1;
        setwindowtext_done: ;
        } else if (strcmp(fn, "GetClientRect") == 0 || strcmp(fn, "GetWindowRect") == 0) {
            WGWin32Window *w = wg_wm_find(args[0]);
            if (w && args[1]) {
                int32_t rect[4] = {0, 0, w->w, w->h};
                if (strcmp(fn, "GetWindowRect") == 0) {
                    rect[0] = w->x; rect[1] = w->y;
                    rect[2] = w->x + w->w; rect[3] = w->y + w->h;
                }
                wg_blink_write_mem(engine->blink, args[1], rect, 16);
            }
            ret_val = 1;
        } else if (strcmp(fn, "GetDC") == 0 || strcmp(fn, "BeginPaint") == 0) {
            uint32_t dc = wg_gdi_get_dc(args[0]);
            // BeginPaint: fill PAINTSTRUCT.hdc (offset 0) if provided
            if (strcmp(fn, "BeginPaint") == 0 && args[1]) {
                wg_blink_write_mem(engine->blink, args[1], &dc, 4);
            }
            ret_val = dc;
        } else if (strcmp(fn, "ReleaseDC") == 0) {
            wg_gdi_release_dc(args[1]);
            ret_val = 1;
        } else if (strcmp(fn, "CreateSolidBrush") == 0) {
            ret_val = wg_gdi_create_solid_brush(args[0]);
        } else if (strcmp(fn, "SetTextColor") == 0) {
            wg_gdi_set_text_color(args[0], args[1]);
            ret_val = 0;
        } else if (strcmp(fn, "SetBkColor") == 0) {
            wg_gdi_set_bk_color(args[0], args[1]);
            ret_val = 0;
        } else if (strcmp(fn, "SetBkMode") == 0) {
            wg_gdi_set_bk_mode(args[0], (int)args[1]);
            ret_val = 1;
        } else if (strcmp(fn, "FillRect") == 0) {
            int32_t rc[4] = {0,0,0,0};
            if (args[1]) wg_blink_read_mem(engine->blink, args[1], rc, 16);
            bool found = false;
            uint32_t color = wg_gdi_brush_color(args[2], &found);
            // System color brushes (COLOR_WINDOW+1 etc.) come in as small ints
            if (!found) color = 0x00FFFFFF; // default white
            wg_gdi_fill_rect(args[0], rc[0], rc[1], rc[2], rc[3], color);
            ret_val = 1;
        } else if (strcmp(fn, "TextOutW") == 0) {
            int count = (int)args[4];
            if (count < 0) count = 0;
            if (count > 1024) count = 1024;
            uint16_t buf[1025];
            if (args[3] && count > 0)
                wg_blink_read_mem(engine->blink, args[3], buf, count * 2);
            wg_gdi_text_out(args[0], (int)args[1], (int)args[2], buf, count);
            ret_val = 1;
        } else if (strcmp(fn, "GetTextExtentPoint32W") == 0 ||
                   strcmp(fn, "GetTextExtentPoint32A") == 0) {
            // (hdc, lpString, c, lpSize) -> write measured {cx, cy}
            bool wide = (fn[strlen(fn) - 1] == 'W');
            int count = (int)args[2];
            if (count < 0) count = 0; if (count > 1024) count = 1024;
            uint16_t buf[1025] = {0};
            if (args[1] && count > 0) {
                if (wide) wg_blink_read_mem(engine->blink, args[1], buf, count * 2);
                else { char a[1025]; wg_blink_read_mem(engine->blink, args[1], a, count);
                       for (int i = 0; i < count; i++) buf[i] = (uint8_t)a[i]; }
            }
            int cx = wg_gdi_text_width(args[0], buf, count);
            int cy = wg_gdi_line_height(args[0]);
            if (args[3]) { int32_t sz[2] = { cx, cy };
                           wg_blink_write_mem(engine->blink, args[3], sz, 8); }
            ret_val = 1;
        } else if (strcmp(fn, "MoveToEx") == 0) {
            wg_gdi_move_to(args[0], (int)args[1], (int)args[2]);
            ret_val = 1;
        } else if (strcmp(fn, "LineTo") == 0) {
            wg_gdi_line_to(args[0], (int)args[1], (int)args[2]);
            ret_val = 1;
        } else if (strcmp(fn, "LoadImageW") == 0) {
            // LoadImageW(hInst, name, type, cx, cy, fuLoad)
            // We support IMAGE_BITMAP (type 0) loaded from a file
            // (LR_LOADFROMFILE = 0x10) — that's how NSIS pulls in the wizard's
            // header/branding BMPs it just extracted to $PLUGINSDIR.
            uint32_t type = args[2];
            uint32_t fuLoad = args[5];
            ret_val = 0;
            if (type == 0 /*IMAGE_BITMAP*/ && (fuLoad & 0x10) && args[1]) {
                uint16_t wpath[260] = {0};
                char apath[260] = {0};
                wg_blink_read_mem(engine->blink, args[1], wpath, 518);
                for (int i = 0; i < 259 && wpath[i]; i++)
                    apath[i] = wpath[i] < 128 ? (char)wpath[i] : '_';
                const char *real = wg_files_map_path(args[1], engine->blink,
                                                     apath, sizeof(apath));
                if (real) ret_val = wg_bitmap_load_file(real);
                // The wizard bmp is usually loaded after NSIS deletes its temp
                // dir, so the direct path fails — fall back to our stable cache.
                if (!ret_val && strcasestr(apath, "wizard")) {
                    const char *cache = wg_files_wizard_bmp();
                    if (cache) ret_val = wg_bitmap_load_file(cache);
                }
                WG_LOGI(TAG, "LoadImageW('%s') -> HBITMAP 0x%X", apath,
                        (uint32_t)ret_val);
            }
        } else if (strcmp(fn, "CreateCompatibleDC") == 0) {
            ret_val = wg_gdi_create_memory_dc();
        } else if (strcmp(fn, "DeleteDC") == 0) {
            wg_gdi_delete_dc(args[0]);
            ret_val = 1;
        } else if (strcmp(fn, "CreateCompatibleBitmap") == 0) {
            // (hdc, w, h)
            ret_val = wg_bitmap_create((int)args[1], (int)args[2]);
        } else if (strcmp(fn, "SelectObject") == 0) {
            // (hdc, hgdiobj) — we only track bitmaps; for anything else return
            // a benign non-zero "previous object" the caller can restore.
            if (wg_bitmap_is(args[1])) {
                uint32_t prev = wg_gdi_select_bitmap(args[0], args[1]);
                ret_val = prev ? prev : 1;
            } else {
                WGFontRec *f = wg_font_find(args[1]);
                if (f) wg_gdi_select_font(args[0], f->px, f->bold, f->name);
                ret_val = 1;
            }
        } else if (strcmp(fn, "DeleteObject") == 0) {
            if (wg_bitmap_is(args[0])) wg_bitmap_delete(args[0]);
            ret_val = 1;
        } else if (strcmp(fn, "GetObjectW") == 0) {
            // (hgdiobj, cb, lpvObject) — fill a BITMAP struct for bitmaps.
            int bw, bh;
            if (wg_bitmap_pixels(args[0], &bw, &bh) && args[2] && args[1] >= 24) {
                int32_t bm[6] = {
                    0,                  // bmType
                    bw,                 // bmWidth
                    bh,                 // bmHeight
                    bw * 4,             // bmWidthBytes
                    (int32_t)((1u << 16) | 32u), // bmPlanes=1, bmBitsPixel=32
                    0                   // bmBits (NULL)
                };
                wg_blink_write_mem(engine->blink, args[2], bm, 24);
                ret_val = 24;
            } else {
                ret_val = 0;
            }
        } else if (strcmp(fn, "BitBlt") == 0) {
            // (hdcDst, x, y, w, h, hdcSrc, sx, sy, rop)
            wg_gdi_blit(args[0], (int)args[1], (int)args[2], (int)args[3],
                        (int)args[4], args[5], (int)args[6], (int)args[7],
                        (int)args[3], (int)args[4]);
            ret_val = 1;
        } else if (strcmp(fn, "StretchBlt") == 0) {
            // (hdcDst, x, y, w, h, hdcSrc, sx, sy, sw, sh, rop)
            wg_gdi_blit(args[0], (int)args[1], (int)args[2], (int)args[3],
                        (int)args[4], args[5], (int)args[6], (int)args[7],
                        (int)args[8], (int)args[9]);
            ret_val = 1;
        } else if (strcmp(fn, "StretchDIBits") == 0 ||
                   strcmp(fn, "SetDIBitsToDevice") == 0) {
            // StretchDIBits(hdc, xD,yD,wD,hD, xS,yS,wS,hS, bits, bmi, usage, rop)
            // SetDIBitsToDevice(hdc, xD,yD, w,h, xS,yS, startScan,scanLines,
            //                   bits, bmi, usage)
            // Both hand us a packed DIB in guest memory; decode and blit it.
            // (StretchDIBits' stretch to a different src size is approximated by
            // drawing the DIB at its natural size into the dest rect.)
            // lpBits and lpbmi sit at the same arg slots (9, 10) in both APIs,
            // as do the destination width/height (3, 4).
            uint32_t bits_addr = args[9];
            uint32_t bmi_addr  = args[10];
            if (bmi_addr && bits_addr) {
                uint8_t bih[40] = {0};
                wg_blink_read_mem(engine->blink, bmi_addr, bih, 40);
                int32_t biW = (int32_t)(bih[4] | (bih[5]<<8) | (bih[6]<<16) | (bih[7]<<24));
                int32_t biH = (int32_t)(bih[8] | (bih[9]<<8) | (bih[10]<<16) | (bih[11]<<24));
                uint16_t biBpp = (uint16_t)(bih[14] | (bih[15]<<8));
                int aH = biH < 0 ? -biH : biH;
                if (biW > 0 && aH > 0 && biW <= 16384 && aH <= 16384 &&
                    (biBpp == 8 || biBpp == 24 || biBpp == 32)) {
                    size_t stride = ((size_t)biW * (biBpp/8) + 3) & ~(size_t)3;
                    size_t total = stride * aH;
                    if (total <= 64u*1024*1024) {
                        uint8_t *px = (uint8_t *)malloc(total);
                        if (px) {
                            wg_blink_read_mem(engine->blink, bits_addr, px, (uint32_t)total);
                            uint32_t hb = wg_bitmap_from_dib(bih, px);
                            free(px);
                            if (hb) {
                                wg_gdi_draw_bitmap(args[0], (int)args[1], (int)args[2],
                                                   (int)args[3], (int)args[4],
                                                   hb, 0, 0, 0, 0);
                                wg_bitmap_delete(hb);
                            }
                        }
                    }
                }
                ret_val = aH;
            }
        } else if (strcmp(fn, "SetStretchBltMode") == 0) {
            ret_val = 1;
        } else if (strcmp(fn, "DefWindowProcW") == 0 ||
                   strcmp(fn, "TranslateMessage") == 0) {
            ret_val = 0;
        } else if (strcmp(fn, "SetTimer") == 0) {
            // SetTimer(hWnd=args[0], nIDEvent=args[1], uElapse=args[2], lpTimerFunc=args[3])
            s_timer_hwnd = args[0];
            s_timer_id   = args[1];
            s_timer_active = true;
            WG_LOGI(TAG, "SetTimer(hwnd=0x%X, id=0x%X, %ums) -> WM_TIMER pump armed",
                    args[0], args[1], args[2]);
            ret_val = args[1] ? args[1] : 1;
        } else if (strcmp(fn, "KillTimer") == 0) {
            if (args[1] == s_timer_id) s_timer_active = false;
            ret_val = 1;
        } else if (strcmp(fn, "DispatchMessageW") == 0 ||
                   strcmp(fn, "DispatchMessageA") == 0) {
            // Route the message to its window procedure. Critically, this lets
            // WM_TIMER reach Steam's wndproc, which runs the network frame
            // (select/recv) — the pump that completes the TLS handshake.
            uint32_t dmsg[4] = {0};
            if (args[0]) wg_blink_read_mem(engine->blink, args[0], dmsg, 16);
            uint32_t dhwnd = dmsg[0], dwmsg = dmsg[1], dwp = dmsg[2], dlp = dmsg[3];
            if (dwmsg != 0 && is_32bit) { // skip WM_NULL
                uint32_t proc = wg_resolve_wndproc(dhwnd);
                uint64_t caller_clean = rsp + ptr_size + (1 * ptr_size);
                if (proc && wg_call_wndproc(engine, proc, dhwnd, dwmsg, dwp, dlp,
                                            (uint32_t)ret_addr, (uint32_t)caller_clean)) {
                    return true;
                }
            }
            ret_val = 0;
        } else if (strcmp(fn, "PostThreadMessageW") == 0 ||
                   strcmp(fn, "PostThreadMessageA") == 0) {
            // PostThreadMessageW(dwThreadId, Msg, wParam, lParam)
            tmsg_push(args[0], args[1], args[2], args[3]);
            WG_LOGI(TAG, "PostThreadMessageW(tid=0x%X, msg=0x%X, wp=0x%X, lp=0x%X)",
                    args[0], args[1], args[2], args[3]);
            ret_val = 1;
        } else if ((strcmp(fn, "GetMessageW") == 0 || strcmp(fn, "GetMessageA") == 0)
                   && s_use_real_threads) {
            // Real-threads: the main thread is a real pthread (driven by
            // wg_engine_tick) that runs continuously — NO cooperative yield / PAUSE
            // (those operate on the cooperative scheduler, which doesn't manage the
            // real worker pthreads, and would freeze the main thread). Deliver a
            // queued thread message, else WM_TIMER so the wndproc pumps its network
            // frame, return 1, and briefly sleep (releasing the thunk lock) so the
            // pump doesn't spin at 100% while workers run concurrently.
            uint32_t gm_msg = 0, gm_wp = 0, gm_lp = 0, gm_hwnd = 0;
            bool had_msg = tmsg_pop(s_cur_guest_tid, &gm_msg, &gm_wp, &gm_lp);
            if (!had_msg && s_timer_active) {
                gm_hwnd = s_timer_hwnd; gm_msg = 0x0113; gm_wp = s_timer_id; gm_lp = 0;
            }
            if (args[0]) {
                uint32_t msgbuf[7] = { gm_hwnd, gm_msg, gm_wp, gm_lp, 0, 0, 0 };
                wg_blink_write_mem(engine->blink, args[0], msgbuf, 28);
            }
            uint64_t gm_rsp = rsp + ptr_size + (4 * ptr_size);
            wg_blink_set_reg(engine->blink, 4, gm_rsp);
            wg_blink_set_rip(engine->blink, ret_addr);
            wg_blink_set_reg(engine->blink, 0, 1); // EAX = 1
            if (!had_msg) { wg_thunk_block_begin(); usleep(1000); wg_thunk_block_end(); }
            return true;
        } else if (strcmp(fn, "GetMessageW") == 0 || strcmp(fn, "GetMessageA") == 0) {
            // Real cooperative message pump. The old behavior froze the engine
            // (PAUSED) on the first call, so the main thread's message loop body —
            // which in apps like Steam pumps the network manager via RunFrame() —
            // never executed, and worker threads polled forever for work that the
            // main loop was supposed to drive. Instead: deliver a queued thread
            // message (or WM_NULL), return 1 so the loop body runs, then YIELD so
            // other threads (the download/poll thread) also make progress.
            uint32_t gm_msg = 0, gm_wp = 0, gm_lp = 0, gm_hwnd = 0;
            uint32_t cur_tid = wg_sched_current_tid(engine->scheduler);
            bool had_msg = tmsg_pop(cur_tid, &gm_msg, &gm_wp, &gm_lp); // WM_NULL(0) if none
            if (!had_msg && s_timer_active) {
                // No queued message — deliver WM_TIMER so the wndproc runs its
                // network frame (select/recv) and the TLS handshake can advance.
                gm_hwnd = s_timer_hwnd; gm_msg = 0x0113 /*WM_TIMER*/; gm_wp = s_timer_id; gm_lp = 0;
            }
            if (args[0]) {
                // MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y (28 bytes)
                uint32_t msgbuf[7] = { gm_hwnd, gm_msg, gm_wp, gm_lp, 0, 0, 0 };
                wg_blink_write_mem(engine->blink, args[0], msgbuf, 28);
            }
            static int s_getmsg_log = 0;
            if (s_getmsg_log++ < 3)
                WG_LOGI(TAG, "GetMessageW: pump cycling (msg=0x%X), not freezing", gm_msg);
            // Clean stack (4 stdcall args), return 1 (a message; 0 = WM_QUIT only),
            // and yield to other threads — mirrors the Sleep handler epilogue.
            uint64_t gm_rsp = rsp + ptr_size + (4 * ptr_size);
            wg_blink_set_reg(engine->blink, 4, gm_rsp);
            wg_blink_set_rip(engine->blink, ret_addr);
            wg_blink_set_reg(engine->blink, 0, 1); // EAX = 1
            // Yield so worker/poll threads run. If another thread is runnable,
            // stay RUNNING (full speed, so the network pumps fast); if nothing
            // else is runnable, idle (PAUSED) so a single-threaded app's loop
            // doesn't spin at 100% CPU — it still cycles slowly via PAUSED ticks.
            bool gm_switched = wg_sched_yield(engine->scheduler, engine->blink,
                                              WG_THREAD_READY);
            engine->state = gm_switched ? WG_ENGINE_RUNNING : WG_ENGINE_PAUSED;
            return true;
        } else if (strcmp(fn, "exit") == 0 || strcmp(fn, "_exit") == 0 ||
                   strcmp(fn, "_cexit") == 0 || strcmp(fn, "_c_exit") == 0 ||
                   strcmp(fn, "quick_exit") == 0 || strcmp(fn, "_o_exit") == 0) {
            // CRT exit functions. Auto-stubs merely RETURN, so the CRT teardown
            // runs off the end into garbage (RIP=0xffff crash). Halt the VM like
            // ExitProcess so a normal console app terminates cleanly.
            WG_LOGW(TAG, "%s(%u) -> halt (called from 0x%llX)", fn, args[0],
                    (unsigned long long)ret_addr);
            wg_blink_set_rip(engine->blink, 0);
            return true;
        } else if (strcmp(fn, "ExitProcess") == 0) {
            WG_LOGI(TAG, "ExitProcess(%u) called from 0x%llX",
                    args[0], (unsigned long long)ret_addr);
            WG_LOGI(TAG, "  last API calls before exit:");
            for (int ri = 0; ri < WG_CALL_RING_SIZE; ri++) {
                int idx = (s_call_ring_idx - WG_CALL_RING_SIZE + ri) % WG_CALL_RING_SIZE;
                if (idx < 0) idx += WG_CALL_RING_SIZE;
                if (s_call_ring[idx].fn)
                    WG_LOGI(TAG, "    %s -> 0x%llX",
                        s_call_ring[idx].fn,
                        (unsigned long long)s_call_ring[idx].ret);
            }
            wg_blink_set_reg(engine->blink, 4, rsp + ptr_size);
            wg_blink_set_rip(engine->blink, 0);
            return true;
        } else if (strcmp(fn, "TerminateProcess") == 0) {
            // Real apps call TerminateProcess(GetCurrentProcess(), code) to die
            // (e.g. the CRT's __report_gsfailure / unhandled-exception path).
            // Halt the VM instead of returning (a no-op return runs into garbage
            // and SIGSEGVs). args[0]=hProcess, args[1]=exitCode.
            WG_LOGI(TAG, "TerminateProcess(0x%X, %u) -> halt", args[0], args[1]);
            wg_blink_set_rip(engine->blink, 0);
            return true;
        } else if (strcmp(fn, "GetStartupInfoW") == 0 ||
                   strcmp(fn, "GetStartupInfoA") == 0) {
            // Zero the STARTUPINFO(W) and set cb so the CRT reads sane values
            // (no STARTF_USESTDHANDLES, default show). Struct is 68 bytes (32-bit).
            if (args[0]) {
                uint8_t zero[68] = {0};
                uint32_t cb = 68; memcpy(zero, &cb, 4);
                wg_blink_write_mem(engine->blink, args[0], zero, 68);
            }
            ret_val = 0;
        } else if (strcmp(fn, "GetProcAddress") == 0) {
            // GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
            uint32_t hmod = args[0];
            uint32_t name_ptr = args[1];

            // Figure out which DLL this module handle might be
            const char *dll = "KERNEL32.dll";
            // TODO: track module handles to DLL names properly

            if (name_ptr > 0xFFFF) {
                char func_name[256] = {0};
                wg_blink_read_mem(engine->blink, name_ptr, func_name, 255);

                // nsDialogs plugin exports route to our native emulation BEFORE
                // any real module export, so the Welcome/Finish custom pages get
                // built by us (the real plugin's message loop fights our modal).
                if (is_nsdialogs_export(func_name)) {
                    uint32_t t = wg_dll_mapper_resolve(engine->dll_mapper,
                                                       "nsDialogs.dll", func_name);
                    if (t >= 0xC00000ULL && t < 0xC00000ULL + 0x20000) {
                        uint8_t hlt = 0xF4;
                        wg_blink_write_mem(engine->blink, t, &hlt, 1);
                    }
                    ret_val = t;
                    WG_LOGI(TAG, "GetProcAddress(nsDialogs!%s) -> 0x%llx",
                            func_name, (unsigned long long)t);
                    // fall through to stack cleanup below
                    goto gpa_done;
                }

                // If hModule is a DLL we actually mapped, resolve from its real
                // export table so the plug-in's own code runs (e.g. nsProcess).
                uint32_t exp = wg_module_export(hmod, func_name);
                if (exp) {
                    ret_val = exp;
                    WG_LOGI(TAG, "GetProcAddress(%s) -> 0x%X (module export)",
                            func_name, exp);
                } else {
                    // Try the specific DLL first, then search all registered
                    // DLLs (GetProcAddress is often called with ws2_32/user32
                    // handles but we only have one mapper namespace).
                    ret_val = wg_dll_mapper_find_any(engine->dll_mapper, func_name);
                    if (!ret_val) {
                        ret_val = wg_dll_mapper_resolve(engine->dll_mapper, dll, func_name);
                    }
                    if (ret_val >= 0xC00000ULL && ret_val < 0xC00000ULL + 0x20000) {
                        uint8_t hlt = 0xF4;
                        wg_blink_write_mem(engine->blink, ret_val, &hlt, 1);
                    }
                    WG_LOGI(TAG, "GetProcAddress(%s) -> 0x%llx",
                            func_name, (unsigned long long)ret_val);
                }
            } else {
                // Ordinal import — map known ordinals
                char ordinal_name[64];
                uint16_t ord = (uint16_t)name_ptr;

                // Known SHELL32 ordinals
                if (ord == 680) snprintf(ordinal_name, sizeof(ordinal_name), "IsUserAnAdmin");
                else snprintf(ordinal_name, sizeof(ordinal_name), "Ordinal_%u", ord);

                ret_val = wg_dll_mapper_resolve(engine->dll_mapper, "SHELL32.dll", ordinal_name);
                if (ret_val >= 0xC00000ULL && ret_val < 0xC00000ULL + 0x20000) {
                    uint8_t hlt = 0xF4;
                    wg_blink_write_mem(engine->blink, ret_val, &hlt, 1);
                }

                WG_LOGI(TAG, "GetProcAddress(ordinal %u -> %s) -> 0x%llx",
                        ord, ordinal_name, (unsigned long long)ret_val);
            }
        gpa_done: ;
        } else if (strcmp(fn, "LoadLibraryExW") == 0 ||
                   strcmp(fn, "LoadLibraryW") == 0) {
            // LoadLibrary[Ex]W(lpLibFileName, [hFile, dwFlags])
            uint16_t libname[512] = {0};
            char ascii[512] = {0};
            if (args[0]) {
                wg_blink_read_mem(engine->blink, args[0], libname, 1022);
                for (int i = 0; i < 511 && libname[i]; i++)
                    ascii[i] = libname[i] < 128 ? (char)libname[i] : '?';
            }
            WG_LOGI(TAG, "%s('%s')", fn, ascii);
            if (!ascii[0] || !args[0]) {
                // Empty or NULL library name — return main module handle
                ret_val = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            } else {
                // Try to actually map the DLL from disk (NSIS plug-ins live in the
                // bottle). On success we return its real load base so GetProcAddress
                // + the call execute the plug-in's own code; otherwise (system DLLs
                // we don't have) fall back to a fake handle.
                uint32_t base = 0;
                char mapbuf[512];
                strncpy(mapbuf, ascii, sizeof(mapbuf) - 1);
                mapbuf[sizeof(mapbuf) - 1] = '\0';
                const char *real = wg_files_map_path(args[0], engine->blink,
                                                     mapbuf, sizeof(mapbuf));
                if (real) base = wg_load_dll(engine->blink, engine->dll_mapper, real, ascii);
                ret_val = base ? base
                               : 0x10000000 + (uint32_t)(engine->dll_mapper->count * 0x1000);
            }
        } else if (strcmp(fn, "GetModuleHandleA") == 0) {
            uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            if (args[0] == 0) {
                ret_val = base;
            } else {
                char modname[256] = {0};
                wg_blink_read_mem(engine->blink, args[0], modname, 255);
                WG_LOGD(TAG, "GetModuleHandleA('%s')", modname);
                if (strcasestr(modname, "kernel32") ||
                    strcasestr(modname, "kernelbase") ||
                    strcasestr(modname, "ntdll") ||
                    strcasestr(modname, "ws2_32") ||
                    strcasestr(modname, "wsock32") ||
                    strcasestr(modname, "advapi32") ||
                    strcasestr(modname, "user32") ||
                    strcasestr(modname, "gdi32") ||
                    strcasestr(modname, "shell32") ||
                    strcasestr(modname, "ole32") ||
                    strcasestr(modname, "crypt32") ||
                    strcasestr(modname, "bcrypt") ||
                    strcasestr(modname, "msvcrt") ||
                    strcasestr(modname, "ucrtbase") ||
                    strcasestr(modname, "vcruntime") ||
                    strcasestr(modname, "msvcp") ||
                    strcasestr(modname, "iphlpapi") ||
                    strcasestr(modname, "winhttp") ||
                    strcasestr(modname, "wininet") ||
                    strcasestr(modname, "secur32") ||
                    strcasestr(modname, "schannel") ||
                    strcasestr(modname, "sspicli") ||
                    strcasestr(modname, "api-ms-win")) {
                    ret_val = 0xBFFF0000u;
                } else {
                    ret_val = 0;
                }
            }
        } else if (strcmp(fn, "GetModuleHandleW") == 0) {
            uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            if (args[0] == 0) {
                ret_val = base;
            } else {
                uint16_t modname[256] = {0};
                wg_blink_read_mem(engine->blink, args[0], modname, 510);
                char ascii[256] = {0};
                for (int i = 0; i < 255 && modname[i]; i++)
                    ascii[i] = modname[i] < 128 ? (char)modname[i] : '?';
                WG_LOGD(TAG, "GetModuleHandleW('%s')", ascii);
                uint32_t loaded = wg_module_find(ascii);
                if (strcasestr(ascii, "nsdialogs") || strstr(ascii, "nsDialogs")) {
                    // nsDialogs must NOT run for real — its message loop
                    // fights our DialogBoxParamW modal path. Return a fake
                    // handle so NSIS skips LoadLibrary; GetProcAddress on
                    // this handle auto-stubs harmlessly.
                    ret_val = 0xBFFF0000u;
                } else if (loaded) {
                    ret_val = loaded;
                } else if (strcasestr(ascii, "kernel32") ||
                           strcasestr(ascii, "kernelbase") ||
                           strcasestr(ascii, "ntdll") ||
                           strcasestr(ascii, "ws2_32") ||
                           strcasestr(ascii, "wsock32") ||
                           strcasestr(ascii, "advapi32") ||
                           strcasestr(ascii, "user32") ||
                           strcasestr(ascii, "gdi32") ||
                           strcasestr(ascii, "shell32") ||
                           strcasestr(ascii, "ole32") ||
                           strcasestr(ascii, "crypt32") ||
                           strcasestr(ascii, "bcrypt") ||
                           strcasestr(ascii, "msvcrt") ||
                           strcasestr(ascii, "ucrtbase") ||
                           strcasestr(ascii, "vcruntime") ||
                           strcasestr(ascii, "msvcp") ||
                           strcasestr(ascii, "iphlpapi") ||
                           strcasestr(ascii, "winhttp") ||
                           strcasestr(ascii, "wininet") ||
                           strcasestr(ascii, "secur32") ||
                           strcasestr(ascii, "schannel") ||
                           strcasestr(ascii, "api-ms-win")) {
                    ret_val = 0xBFFF0000u;
                } else {
                    ret_val = 0;
                }
            }
        } else if (strstr(fn, "FindProcess") || strstr(fn, "KillProcess")) {
            // nsProcess plug-in (exports are _FindProcess/_KillProcess/
            // _FindProcessId/... in v1.6). Process enumeration is meaningless on
            // iOS — there is never a real Steam process — and the real DLL's
            // enumeration path doesn't work here (it bails after GetVersionEx and
            // reports "running"), so we emulate the result directly. Plug-in ABI:
            //   void f(HWND parent, int string_size, TCHAR *vars,
            //          stack_t **stacktop, extra_parameters *extra)
            // It pops the process name and pushes a result code. We reuse the top
            // node (struct stack_t { stack_t *next; TCHAR text[]; } — text at +4
            // in 32-bit) and overwrite its text:
            //   FindProcess: "603" = not found (it returns "0" when RUNNING)
            //   KillProcess: "0"   = success
            bool is_find = strstr(fn, "FindProcess") != NULL;
            uint32_t head = 0;
            if (args[3]) wg_blink_read_mem(engine->blink, args[3], &head, 4);
            if (head) {
                static const uint16_t notfound[] = {'6','0','3',0};
                static const uint16_t okstr[]    = {'0',0};
                wg_blink_write_mem(engine->blink, head + 4,
                                   is_find ? notfound : okstr,
                                   is_find ? sizeof(notfound) : sizeof(okstr));
            }
            WG_LOGI(TAG, "nsProcess::%s -> %s (emulated)", fn,
                    is_find ? "not running" : "ok");
            ret_val = 0;
        } else if (strcmp(fn, "Exec") == 0 || strcmp(fn, "ExecToLog") == 0 ||
                   strcmp(fn, "ExecToStack") == 0) {
            // nsExec plug-in. The real DLL spawns a child process (e.g.
            // steamservice.exe) and reads its stdout in a loop — we can't run
            // children, and that loop hangs the install. Emulate: pop the command
            // and push "0" (exit code success) onto the NSIS stack so the
            // installer proceeds. (Same stack-node trick as nsProcess.)
            uint32_t head = 0;
            if (args[3]) wg_blink_read_mem(engine->blink, args[3], &head, 4);
            if (head) {
                static const uint16_t okstr[] = {'0',0};
                wg_blink_write_mem(engine->blink, head + 4, okstr, sizeof(okstr));
            }
            WG_LOGI(TAG, "nsExec::%s -> 0 (emulated, child not run)", fn);
            ret_val = 0;
        } else if (strcmp(fn, "GetVersionExW") == 0 ||
                   strcmp(fn, "GetVersionExA") == 0) {
            // Fill a Windows 10 OSVERSIONINFO(EX) and return TRUE. Was unhandled
            // (returned garbage), which broke OS-version checks (nsProcess bailed,
            // and the bootstrapper can reject "unsupported OS"). args[0]=lpVI;
            // [+4]=major,[+8]=minor,[+12]=build,[+16]=platformId(2=NT),[+20]=CSD.
            uint32_t vi = args[0];
            if (vi) {
                uint32_t major = 10, minor = 0, build = 19045, plat = 2, zero = 0;
                wg_blink_write_mem(engine->blink, vi + 4,  &major, 4);
                wg_blink_write_mem(engine->blink, vi + 8,  &minor, 4);
                wg_blink_write_mem(engine->blink, vi + 12, &build, 4);
                wg_blink_write_mem(engine->blink, vi + 16, &plat,  4);
                wg_blink_write_mem(engine->blink, vi + 20, &zero,  4); // szCSDVersion[0]=0
            }
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "GetVersion") == 0) {
            ret_val = 0x00000A00;
        } else if (strcmp(fn, "GetSystemInfo") == 0 ||
                   strcmp(fn, "GetNativeSystemInfo") == 0) {
            // SYSTEM_INFO (36 bytes). Report 1 processor so apps that gate IOCP
            // on CPU count (Steam's BUseIOCP) use the synchronous socket path we
            // support. Must be a real handler — auto-stub (num_args=0) on this
            // 1-arg stdcall would corrupt the guest stack for the next call.
            bool si64 = (engine->pe_image && engine->pe_image->is_64bit);
            if (args[0] && si64) {
                // x64 SYSTEM_INFO (48 bytes, 8-byte pointer fields). The 32-bit
                // layout put lpMaximumApplicationAddress in the wrong place, so
                // UE4's FMallocBinned2 read a garbage 64-bit max address and
                // sized a multi-GB pool reservation off it. A modest max keeps
                // the reservation inside our sub-4GB guest.
                uint8_t si[48] = {0};
                uint32_t v32; uint64_t v64;
                uint16_t arch = 9; memcpy(si + 0, &arch, 2);   // PROCESSOR_ARCHITECTURE_AMD64
                v32 = 4096;        memcpy(si + 4,  &v32, 4);   // dwPageSize
                v64 = 0x00010000;  memcpy(si + 8,  &v64, 8);   // lpMinimumApplicationAddress
                v64 = 0x3FF000000ULL; memcpy(si + 16, &v64, 8);  // lpMaximumApplicationAddress (~16GB — must cover region 3 (guest 4..16GB) where large VirtualAlloc pools live, so FMallocBinned2 accepts those pool pointers; Path B's linear region is 16GB)
                v64 = wg_cpumask(); memcpy(si + 24, &v64, 8);  // dwActiveProcessorMask
                v32 = wg_ncpu();   memcpy(si + 32, &v32, 4);   // dwNumberOfProcessors
                v32 = 8664;        memcpy(si + 36, &v32, 4);   // dwProcessorType
                v32 = 0x00010000;  memcpy(si + 40, &v32, 4);   // dwAllocationGranularity
                uint16_t w16 = 6;  memcpy(si + 44, &w16, 2);   // wProcessorLevel
                wg_blink_write_mem(engine->blink, args[0], si, 48);
            } else if (args[0]) {
                uint8_t si[36] = {0};
                uint32_t v32;
                v32 = 4096;       memcpy(si + 4,  &v32, 4); // dwPageSize
                v32 = 0x00010000; memcpy(si + 8,  &v32, 4); // lpMinimumApplicationAddress
                v32 = 0x7FFE0000; memcpy(si + 12, &v32, 4); // lpMaximumApplicationAddress
                v32 = (uint32_t)wg_cpumask(); memcpy(si + 16, &v32, 4); // dwActiveProcessorMask
                v32 = wg_ncpu();  memcpy(si + 20, &v32, 4); // dwNumberOfProcessors
                v32 = 586;        memcpy(si + 24, &v32, 4); // dwProcessorType (PROCESSOR_INTEL_PENTIUM)
                v32 = 0x00010000; memcpy(si + 28, &v32, 4); // dwAllocationGranularity
                uint16_t w16 = 6; memcpy(si + 32, &w16, 2); // wProcessorLevel
                wg_blink_write_mem(engine->blink, args[0], si, 36);
            }
            WG_LOGI(TAG, "%s -> %d processor(s)", fn, wg_ncpu());
            ret_val = 0;
        } else if (strcmp(fn, "GetLogicalProcessorInformation") == 0) {
            // GetLogicalProcessorInformation(Buffer, ReturnedLength). UE4 counts
            // RelationProcessorCore records here to size its task-graph worker pool
            // — with 0/1 it builds a degenerate graph and the main thread deadlocks.
            // Report wg_ncpu() cores (each a 32-byte SYSTEM_LOGICAL_PROCESSOR_INFORMATION
            // on x64: ProcessorMask u64, Relationship u32=0 (RelationProcessorCore),
            // pad u32, union u64x2). Two-call size probe: FALSE + ERROR_INSUFFICIENT_BUFFER
            // when the buffer is too small.
            int n = wg_ncpu();
            uint32_t need = (uint32_t)n * 32;
            uint32_t plen = args[1], have = 0;
            if (plen) wg_blink_read_mem(engine->blink, plen, &have, 4);
            if (!args[0] || have < need) {
                if (plen) wg_blink_write_mem(engine->blink, plen, &need, 4);
                s_last_error = 122; // ERROR_INSUFFICIENT_BUFFER
                ret_val = 0;        // FALSE
            } else {
                for (int i = 0; i < n; i++) {
                    uint8_t rec[32] = {0};
                    uint64_t mask = 1ull << i;
                    memcpy(rec + 0, &mask, 8);   // ProcessorMask = this core
                    // rec[8..11] Relationship = 0 (RelationProcessorCore); rest 0
                    wg_blink_write_mem(engine->blink, args[0] + (uint32_t)(i * 32), rec, 32);
                }
                if (plen) wg_blink_write_mem(engine->blink, plen, &need, 4);
                ret_val = 1;        // TRUE
            }
            WG_LOGI(TAG, "GetLogicalProcessorInformation -> %d cores (need=%u have=%u)", n, need, have);
        } else if (strcmp(fn, "GlobalMemoryStatusEx") == 0) {
            // MEMORYSTATUSEX (DWORDLONG fields, same layout on 32/64-bit). Report
            // a modest, sane machine — otherwise the buffer keeps its garbage
            // stack contents and UE4 sizes its allocator pools off nonsense.
            if (args[0]) {
                uint32_t v32; uint64_t v64;
                v32 = 30;          wg_blink_write_mem(engine->blink, args[0] + 0x04, &v32, 4); // dwMemoryLoad
                v64 = 0x80000000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x08, &v64, 8); // ullTotalPhys (2GB)
                v64 = 0x40000000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x10, &v64, 8); // ullAvailPhys (1GB)
                v64 = 0x80000000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x18, &v64, 8); // ullTotalPageFile
                v64 = 0x40000000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x20, &v64, 8); // ullAvailPageFile
                v64 = 0x7FFF0000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x28, &v64, 8); // ullTotalVirtual
                v64 = 0x40000000ULL; wg_blink_write_mem(engine->blink, args[0] + 0x30, &v64, 8); // ullAvailVirtual
                v64 = 0;             wg_blink_write_mem(engine->blink, args[0] + 0x38, &v64, 8); // ullAvailExtendedVirtual
            }
            ret_val = 1;
        } else if (strcmp(fn, "GetCommandLineW") == 0 ||
                   strcmp(fn, "GetCommandLineA") == 0) {
            // Map the cmdline page on first call (W at +0, A at +0x800). The
            // page address (s_cmdline_page) is relocated above the image for
            // large 64-bit PEs so it doesn't clobber the game's .text.
            // The A slot must sit past the longest W line: a quoted install
            // path (…\Visage\Binaries\Win64\Visage-Win64-Shipping.exe) is
            // 200+ bytes as UTF-16, which overran the old +0x100 A slot and
            // left the W string unterminated.
            if (!s_cmdpage_mapped) {
                const char *winpath = wg_files_exe_win_path();
                // Extra switches appended to the guest command line. -ansimalloc
                // forces UE4 to use FMallocAnsi (plain malloc/free, which we back)
                // instead of FMallocBinned2, whose pool-canary bookkeeping assumes
                // exact Windows VirtualAlloc reserve/commit semantics we can't
                // fully reproduce (it otherwise fails "Corruption Canary" on boot).
                const char *extra = s_cmdline_extra[0] ? s_cmdline_extra : "";
                uint8_t page[0x1000];
                memset(page, 0, sizeof(page));
                // Wide command line at offset 0
                uint16_t *wcmd = (uint16_t *)page;
                wcmd[0] = '"';
                int i = 0;
                for (; winpath[i] && i < 500; i++)
                    wcmd[i + 1] = (uint8_t)winpath[i];
                wcmd[i + 1] = '"';
                int w = i + 2;
                for (int k = 0; extra[k] && w < 1000; k++) wcmd[w++] = (uint8_t)extra[k];
                wcmd[w] = 0;
                // ANSI command line at offset 0x800
                char *acmd = (char *)(page + 0x800);
                snprintf(acmd, 0x7FF, "\"%s\"%s", winpath, extra);
                wg_blink_load_code(engine->blink, s_cmdline_page, page, 0x1000, 0);
                s_cmdpage_mapped = true;
            }
            ret_val = (fn[14] == 'W') ? s_cmdline_page : s_cmdline_page + 0x800;
        } else if (strcmp(fn, "CommandLineToArgvW") == 0) {
            // CommandLineToArgvW(lpCmdLine=args[0], pNumArgs=args[1])
            // Read the wide command line from guest memory
            uint16_t cmdw[512] = {0};
            if (args[0])
                wg_blink_read_mem(engine->blink, args[0], cmdw, sizeof(cmdw) - 2);
            // Count wchars
            int len = 0;
            while (len < 511 && cmdw[len]) len++;
            // Allocate guest memory: argv[0] pointer + string data. The
            // pointer slot must be guest-pointer-sized — a 64-bit guest reads
            // argv[0] as 8 bytes, so a 4-byte slot leaks string chars into the
            // pointer's high half.
            uint32_t psz = (engine->pe_image && engine->pe_image->is_64bit) ? 8 : 4;
            uint32_t base = s_heap_ptr;
            uint32_t str_off = base + psz; // argv[0] string right after pointer
            uint32_t str_bytes = (len + 1) * 2;
            uint32_t total = psz + str_bytes;
            total = (total + 0xFFF) & ~0xFFFu;
            uint8_t *buf = calloc(1, total);
            if (buf) {
                // argv[0] = pointer to the string (high half stays zero)
                uint64_t str_addr = str_off;
                memcpy(buf, &str_addr, psz);
                memcpy(buf + psz, cmdw, str_bytes);
                wg_blink_load_code(engine->blink, base, buf, total, 0);
                free(buf);
                s_heap_ptr += total;
                // Write argc = 1
                if (args[1]) {
                    uint32_t one = 1;
                    wg_blink_write_mem(engine->blink, args[1], &one, 4);
                }
                ret_val = base;
            }
        } else if (strcmp(fn, "GetModuleFileNameA") == 0) {
            // GetModuleFileNameA(hModule, lpFilename, nSize)
            uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            const char *path;
            if (args[0] == 0 || args[0] == base)
                path = wg_files_exe_win_path();
            else
                path = "C:\\Windows\\System32\\kernel32.dll";
            int len = (int)strlen(path);
            if (args[1] && args[2] > 0) {
                int max = (int)args[2] - 1;
                if (len > max) len = max;
                wg_blink_write_mem(engine->blink, args[1], path, len + 1);
            }
            ret_val = len;
        } else if (strcmp(fn, "GetModuleFileNameW") == 0) {
            // GetModuleFileNameW(hModule, lpFilename, nSize)
            uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            const char *winpath;
            if (args[0] == 0 || args[0] == base)
                winpath = wg_files_exe_win_path();
            else
                winpath = "C:\\Windows\\System32\\kernel32.dll";
            int len = (int)strlen(winpath);
            if (args[1] && args[2] > 0) {
                int max = (int)args[2] - 1;
                if (len > max) len = max;
                uint16_t wbuf[520] = {0};
                for (int i = 0; i < len; i++)
                    wbuf[i] = (uint8_t)winpath[i];
                wbuf[len] = 0;
                wg_blink_write_mem(engine->blink, args[1], wbuf, (len + 1) * 2);
            }
            ret_val = len;
        } else if (strcmp(fn, "GetTempPathW") == 0) {
            // GetTempPathW(nBufferLength=args[0], lpBuffer=args[1]). Only write
            // if the caller's buffer is big enough; never overflow it.
            uint16_t tmp[] = {'C',':','\\','T','e','m','p','\\',0};
            int n = 9; // chars incl NUL
            if (args[1] && args[0] >= (uint32_t)n)
                wg_blink_write_mem(engine->blink, args[1], tmp, n * 2);
            ret_val = (args[0] >= (uint32_t)n) ? (uint32_t)(n - 1) : (uint32_t)n;
        } else if (strcmp(fn, "PathRemoveFileSpecW") == 0) {
            // SHLWAPI PathRemoveFileSpecW(pszPath) — strip the trailing file
            // component in place. The UE4 launcher applies this to its
            // GetModuleFileNameW result to find the game directory.
            uint16_t w[520] = {0};
            if (args[0]) wg_blink_read_mem(engine->blink, args[0], w, sizeof(w) - 2);
            int len = 0; while (len < 519 && w[len]) len++;
            int cut = -1;
            for (int i = len - 1; i >= 0; i--)
                if (w[i] == '\\' || w[i] == '/') { cut = i; break; }
            ret_val = 0;
            if (cut >= 0) {
                if (cut == 2 && w[1] == ':') cut = 3;   // keep the "C:\" root
                uint16_t nul = 0;
                wg_blink_write_mem(engine->blink, args[0] + (uint32_t)cut * 2, &nul, 2);
                ret_val = (cut < len) ? 1 : 0;
            }
        } else if (strcmp(fn, "PathCombineW") == 0 ||
                   strcmp(fn, "PathCanonicalizeW") == 0) {
            // SHLWAPI PathCombineW(dest, dir, file) -> dest, and
            // PathCanonicalizeW(dest, src) -> BOOL. Both must actually write the
            // combined/canonicalized path — the launcher builds the shipping-exe
            // path with them.
            bool is_combine = (strcmp(fn, "PathCombineW") == 0);
            char dir[520] = {0}, file[520] = {0}, out[1040];
            uint32_t src1 = args[1], src2 = is_combine ? args[2] : 0;
            if (src1) {
                uint16_t w[520] = {0};
                wg_blink_read_mem(engine->blink, src1, w, sizeof(w) - 2);
                for (int i = 0; i < 519 && w[i]; i++)
                    dir[i] = w[i] < 128 ? (char)w[i] : '?';
            }
            if (src2) {
                uint16_t w[520] = {0};
                wg_blink_read_mem(engine->blink, src2, w, sizeof(w) - 2);
                for (int i = 0; i < 519 && w[i]; i++)
                    file[i] = w[i] < 128 ? (char)w[i] : '?';
            }
            if (!is_combine) {
                snprintf(out, sizeof(out), "%s", dir);          // canonicalize src
            } else if (file[0] && (file[1] == ':' || file[0] == '\\')) {
                snprintf(out, sizeof(out), "%s", file);         // file is absolute
            } else if (dir[0]) {
                size_t dl = strlen(dir);
                snprintf(out, sizeof(out), "%s%s%s", dir,
                         (dir[dl - 1] == '\\' || dir[dl - 1] == '/') ? "" : "\\",
                         file);
            } else {
                snprintf(out, sizeof(out), "%s", file);
            }
            wg_path_canon_a(out);
            if (args[0]) {
                uint16_t wout[520] = {0};
                int n = 0;
                for (; n < 519 && out[n]; n++) wout[n] = (uint8_t)out[n];
                wout[n] = 0;
                wg_blink_write_mem(engine->blink, args[0], wout, (n + 1) * 2);
                WG_LOGI(TAG, "%s -> '%s'", fn, out);
            }
            ret_val = is_combine ? args[0] : 1;
        } else if (!is_32bit && (strcmp(fn, "_initterm") == 0 ||
                                 strcmp(fn, "_initterm_e") == 0)) {
            // Run the CRT initializer array for real: jump to the guest
            // trampoline with RCX/RDX and the return address untouched — its
            // RET lands back in the caller. (See map_thunks_to_blink.)
            // Set WG_SKIP_INITTERM=1 to skip constructors (return 0) — useful
            // when a constructor mis-executes under blink and aborts startup.
            uint64_t it_first = wg_blink_get_reg(engine->blink, 1);
            uint64_t it_last  = wg_blink_get_reg(engine->blink, 2);
            static int skip_initterm = -1;
            if (skip_initterm < 0) {
                const char *e = getenv("WG_SKIP_INITTERM");
                skip_initterm = (e && e[0] == '1') ? 1 : 0;
            }
            WG_LOGI(TAG, "%s: %s %llu initializers (0x%llX..0x%llX)",
                    fn, skip_initterm ? "SKIPPING" : "running",
                    (unsigned long long)((it_last - it_first) / 8),
                    (unsigned long long)it_first, (unsigned long long)it_last);
            if (!skip_initterm) {
                wg_blink_set_rip(engine->blink, s_tramp_addr);
                wg_call_ring_push(fn, 0);
                return true;
            }
            ret_val = 0; // _initterm is void / _initterm_e returns 0 on success
        } else if (strcmp(fn, "FindResourceW") == 0 ||
                   strcmp(fn, "FindResourceA") == 0) {
            // FindResource(hModule, lpName, lpType) -> HRSRC (the guest VA of
            // the IMAGE_RESOURCE_DATA_ENTRY in the mapped .rsrc).
            ret_val = wg_find_resource(engine, args[1], args[2],
                                       fn[12] == 'W');
            WG_LOGI(TAG, "%s(name=0x%X, type=0x%X) -> 0x%llX",
                    fn, args[1], args[2], (unsigned long long)ret_val);
        } else if (strcmp(fn, "LoadResource") == 0) {
            // The data entry's first field is the RVA of the resource bytes.
            uint32_t drva = 0;
            if (args[1]) wg_blink_read_mem(engine->blink, args[1], &drva, 4);
            ret_val = drva ? (uint32_t)engine->pe_image->image_base + drva : 0;
        } else if (strcmp(fn, "LockResource") == 0) {
            ret_val = args[0];   // HGLOBAL from LoadResource IS the data VA
        } else if (strcmp(fn, "SizeofResource") == 0) {
            uint32_t rsz = 0;
            if (args[1]) wg_blink_read_mem(engine->blink, args[1] + 4, &rsz, 4);
            ret_val = rsz;
        } else if (strcmp(fn, "SetCurrentDirectoryW") == 0 ||
                   strcmp(fn, "SetCurrentDirectoryA") == 0) {
            // Track the current dir so relative file writes (NSIS SetOutPath +
            // File) resolve under $INSTDIR instead of the drive_c root.
            char path[520] = {0};
            if (args[0]) {
                bool wide = (fn[strlen(fn) - 1] == 'W');
                if (wide) {
                    uint16_t w[520] = {0};
                    wg_blink_read_mem(engine->blink, args[0], w, 1038);
                    for (int i = 0; i < 519 && w[i]; i++)
                        path[i] = w[i] < 128 ? (char)w[i] : '?';
                } else {
                    wg_blink_read_mem(engine->blink, args[0], path, 519);
                }
            }
            wg_files_set_cwd(path[0] ? path : NULL);
            WG_LOGI(TAG, "SetCurrentDirectory('%s')", path);
            ret_val = 1;
        } else if (strcmp(fn, "GetCurrentDirectoryW") == 0 ||
                   strcmp(fn, "GetCurrentDirectoryA") == 0) {
            const char *cwd = wg_files_get_cwd();
            const char *winpath = cwd ? cwd : wg_files_exe_win_path();
            char dir[520] = {0};
            // A tracked cwd is already a directory; only strip a filename when
            // falling back to the exe path.
            const char *last = cwd ? NULL : strrchr(winpath, '\\');
            int dirlen = last ? (int)(last - winpath) : (int)strlen(winpath);
            memcpy(dir, winpath, dirlen);
            // Ensure trailing backslash for root dirs (C: -> C:\)
            if (dirlen >= 2 && dir[dirlen-1] == ':') {
                dir[dirlen++] = '\\';
            }
            dir[dirlen] = 0;
            bool wide = (fn[19] == 'W');
            if (wide) {
                if (args[1] && args[0] > (uint32_t)dirlen) {
                    uint16_t wbuf[520] = {0};
                    for (int i = 0; i < dirlen; i++)
                        wbuf[i] = (uint8_t)dir[i];
                    wbuf[dirlen] = 0;
                    wg_blink_write_mem(engine->blink, args[1], wbuf, (dirlen + 1) * 2);
                }
            } else {
                if (args[1] && args[0] > (uint32_t)dirlen) {
                    wg_blink_write_mem(engine->blink, args[1], dir, dirlen + 1);
                }
            }
            ret_val = dirlen;
        } else if (strcmp(fn, "GetFullPathNameW") == 0) {
            // GetFullPathNameW(lpFileName=args[0], nBufferLength=args[1],
            //                   lpBuffer=args[2], lpFilePart=args[3])
            uint16_t fname[520] = {0};
            if (args[0]) wg_blink_read_mem(engine->blink, args[0], fname, 1038);
            // Convert to ASCII for processing
            char aname[520] = {0};
            for (int i = 0; i < 519 && fname[i]; i++)
                aname[i] = fname[i] < 128 ? (char)fname[i] : '?';
            char full[520] = {0};
            if (aname[0] && aname[1] == ':') {
                // Already absolute (C:\...)
                snprintf(full, sizeof(full), "%s", aname);
            } else {
                // Relative — prepend current directory
                const char *winpath = wg_files_exe_win_path();
                const char *last = strrchr(winpath, '\\');
                int dirlen = last ? (int)(last - winpath) : (int)strlen(winpath);
                char dir[520] = {0};
                memcpy(dir, winpath, dirlen);
                if (dirlen >= 2 && dir[dirlen-1] == ':')
                    dir[dirlen++] = '\\';
                dir[dirlen] = 0;
                snprintf(full, sizeof(full), "%s%s%s",
                    dir, (dir[dirlen-1] == '\\') ? "" : "\\", aname);
            }
            // Fix separators
            for (char *p = full; *p; p++) { if (*p == '/') *p = '\\'; }
            int len = (int)strlen(full);
            if (args[2] && args[1] > (uint32_t)len) {
                uint16_t wfull[520] = {0};
                for (int i = 0; i <= len; i++)
                    wfull[i] = (uint8_t)full[i];
                wg_blink_write_mem(engine->blink, args[2], wfull, (len + 1) * 2);
                // Set lpFilePart to point to the filename portion
                if (args[3]) {
                    const char *fp = strrchr(full, '\\');
                    uint32_t fp_off = fp ? (uint32_t)(fp - full + 1) : 0;
                    uint32_t fp_addr = args[2] + fp_off * 2;
                    wg_blink_write_mem(engine->blink, args[3], &fp_addr, 4);
                }
            }
            ret_val = len;
        } else if (strcmp(fn, "GetFullPathNameA") == 0) {
            char aname[520] = {0};
            if (args[0]) wg_blink_read_mem(engine->blink, args[0], aname, 519);
            char full[520] = {0};
            if (aname[0] && aname[1] == ':') {
                snprintf(full, sizeof(full), "%s", aname);
            } else {
                const char *winpath = wg_files_exe_win_path();
                const char *last = strrchr(winpath, '\\');
                int dirlen = last ? (int)(last - winpath) : (int)strlen(winpath);
                char dir[520] = {0};
                memcpy(dir, winpath, dirlen);
                if (dirlen >= 2 && dir[dirlen-1] == ':')
                    dir[dirlen++] = '\\';
                dir[dirlen] = 0;
                snprintf(full, sizeof(full), "%s%s%s",
                    dir, (dir[dirlen-1] == '\\') ? "" : "\\", aname);
            }
            for (char *p = full; *p; p++) { if (*p == '/') *p = '\\'; }
            int len = (int)strlen(full);
            if (args[2] && args[1] > (uint32_t)len) {
                wg_blink_write_mem(engine->blink, args[2], full, len + 1);
                if (args[3]) {
                    const char *fp = strrchr(full, '\\');
                    uint32_t fp_off = fp ? (uint32_t)(fp - full + 1) : 0;
                    uint32_t fp_addr = args[2] + fp_off;
                    wg_blink_write_mem(engine->blink, args[3], &fp_addr, 4);
                }
            }
            ret_val = len;
        } else if (strcmp(fn, "GetDiskFreeSpaceExW") == 0 ||
                   strcmp(fn, "GetDiskFreeSpaceExA") == 0) {
            // (lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes,
            //  lpTotalNumberOfFreeBytes) — all ULARGE_INTEGER* (8-byte writes)
            uint64_t total = (uint64_t)200 * 1024 * 1024 * 1024;  // 200 GB
            uint64_t avail = (uint64_t)100 * 1024 * 1024 * 1024;  // 100 GB free
            if (args[1]) wg_blink_write_mem(engine->blink, args[1], &avail, 8);
            if (args[2]) wg_blink_write_mem(engine->blink, args[2], &total, 8);
            if (args[3]) wg_blink_write_mem(engine->blink, args[3], &avail, 8);
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "GetWindowsDirectoryW") == 0) {
            // GetWindowsDirectoryW(lpBuffer=args[0], uSize=args[1] in chars).
            uint16_t windir[] = {'C',':','\\','W','i','n','d','o','w','s',0};
            int n = 11;
            if (args[0] && args[1] >= (uint32_t)n)
                wg_blink_write_mem(engine->blink, args[0], windir, n * 2);
            ret_val = (args[1] >= (uint32_t)n) ? (uint32_t)(n - 1) : (uint32_t)n;
        } else if (strcmp(fn, "GetSystemDirectoryW") == 0) {
            // GetSystemDirectoryW(lpBuffer=args[0], uSize=args[1] in chars).
            uint16_t sysdir[] = {'C',':','\\','W','i','n','d','o','w','s','\\','S','y','s','t','e','m','3','2',0};
            int n = 20;
            if (args[0] && args[1] >= (uint32_t)n)
                wg_blink_write_mem(engine->blink, args[0], sysdir, n * 2);
            ret_val = (args[1] >= (uint32_t)n) ? (uint32_t)(n - 1) : (uint32_t)n;
        } else if (strcmp(fn, "RegisterClassW") == 0) {
            ret_val = 0xC001;
        } else if (strcmp(fn, "GetSystemMetrics") == 0) {
            // SM_CXSCREEN=0 -> 800, SM_CYSCREEN=1 -> 600
            if (args[0] == 0) ret_val = 800;
            else if (args[0] == 1) ret_val = 600;
            else ret_val = 0;
        } else if (strcmp(fn, "GetSysColor") == 0) {
            ret_val = 0x00F0F0F0; // light gray
        } else if (strcmp(fn, "GetDeviceCaps") == 0) {
            ret_val = 96; // LOGPIXELSX/Y
        } else if (strcmp(fn, "PeekMessageW") == 0) {
            // args[0]=lpMsg, args[1]=hWnd, args[2]=filterMin, args[3]=filterMax, args[4]=removeMsg
            uint32_t cur_tid = wg_sched_current_tid(engine->scheduler);
            if (!cur_tid) cur_tid = 1;
            uint32_t tmsg = 0, twp = 0, tlp = 0;
            if (args[0] && tmsg_pop(cur_tid, &tmsg, &twp, &tlp)) {
                // Fill MSG struct: hwnd(4), message(4), wParam(4), lParam(4), time(4), pt.x(4), pt.y(4)
                uint32_t msgbuf[7] = {0, tmsg, twp, tlp, 0, 0, 0};
                wg_blink_write_mem(engine->blink, args[0], msgbuf, 28);
                WG_LOGI(TAG, "PeekMessageW: delivered msg=0x%X to tid=0x%X", tmsg, cur_tid);
                ret_val = 1;
            } else {
                ret_val = 0;
                // WG_PEEKYIELD: an EMPTY PeekMessageW is the main's idle game-loop
                // tick — it is spin-waiting for background async-load/task-graph
                // workers to finish loading the next package. Those workers BLOCK on
                // their FEvent (not counted as GIL-contenders), so adaptive slicing
                // keeps the main on huge 48M-instruction slices and STARVES them, so
                // the drain never resumes (the post-swapchain task-graph stall).
                // Release the GIL here so a load worker gets a guaranteed run window,
                // then re-acquire. WG_PEEKYIELD_US tunes the window (default 200us).
                if (s_use_real_threads && cur_tid == 1 && getenv("WG_PEEKYIELD")) {
                    long us = getenv("WG_PEEKYIELD_US") ? atol(getenv("WG_PEEKYIELD_US")) : 200;
                    wg_thunk_block_begin();
                    usleep((useconds_t)us);
                    wg_thunk_block_end();
                }
            }
        } else if (strcmp(fn, "CharNextW") == 0) {
            // CharNextW(LPCWSTR p) — advance to next char, don't go past null
            if (args[0]) {
                uint16_t ch = 0;
                wg_blink_read_mem(engine->blink, args[0], &ch, 2);
                ret_val = (ch != 0) ? args[0] + 2 : args[0];
            }
        } else if (strcmp(fn, "CharNextA") == 0) {
            if (args[0]) {
                uint8_t ch = 0;
                wg_blink_read_mem(engine->blink, args[0], &ch, 1);
                ret_val = (ch != 0) ? args[0] + 1 : args[0];
            }
        } else if (strcmp(fn, "CharPrevW") == 0) {
            // CharPrevW(LPCWSTR start, LPCWSTR current)
            if (args[1] > args[0]) ret_val = args[1] - 2;
            else ret_val = args[0];
        } else if (strcmp(fn, "lstrlenW") == 0) {
            if (args[0]) {
                uint16_t buf[1024];
                wg_blink_read_mem(engine->blink, args[0], buf, sizeof(buf));
                int len = 0;
                while (len < 1023 && buf[len]) len++;
                ret_val = len;
            }
        } else if (strcmp(fn, "lstrlenA") == 0) {
            if (args[0]) {
                char buf[1024];
                wg_blink_read_mem(engine->blink, args[0], buf, sizeof(buf));
                buf[1023] = 0;
                int len = 0;
                while (len < 1023 && buf[len]) len++;
                ret_val = len;
            }
        } else if (strcmp(fn, "lstrcpyW") == 0) {
            if (args[0] && args[1]) {
                uint16_t buf[1024];
                wg_blink_read_mem(engine->blink, args[1], buf, sizeof(buf));
                buf[1023] = 0;
                int len = 0;
                while (len < 1023 && buf[len]) len++;
                wg_blink_write_mem(engine->blink, args[0], buf, (len + 1) * 2);
                ret_val = args[0];
            }
        } else if (strcmp(fn, "lstrcpyA") == 0) {
            if (args[0] && args[1]) {
                char buf[1024];
                wg_blink_read_mem(engine->blink, args[1], buf, sizeof(buf));
                buf[1023] = 0;
                int len = 0;
                while (len < 1023 && buf[len]) len++;
                wg_blink_write_mem(engine->blink, args[0], buf, len + 1);
                ret_val = args[0];
            }
        } else if (strcmp(fn, "lstrcpynW") == 0) {
            // lstrcpynW(dst, src, maxlen): copy AT MOST maxlen-1 chars, stopping
            // at the NUL, then NUL-terminate. Writing the full maxlen (as we used
            // to) overflows the destination with garbage and corrupts adjacent
            // guest memory — e.g. it was smashing NSIS's LZMA decoder context.
            if (args[0] && args[1] && (int32_t)args[2] > 0) {
                int maxlen = args[2];
                if (maxlen > 1024) maxlen = 1024;
                uint16_t buf[1024];
                wg_blink_read_mem(engine->blink, args[1], buf, maxlen * 2);
                int len = 0;
                while (len < maxlen - 1 && buf[len]) len++;
                buf[len] = 0;
                wg_blink_write_mem(engine->blink, args[0], buf, (len + 1) * 2);
                ret_val = args[0];
            }
        } else if (strcmp(fn, "lstrcatW") == 0) {
            if (args[0] && args[1]) {
                // Find end of dst
                uint16_t dst[1024], src[1024];
                wg_blink_read_mem(engine->blink, args[0], dst, sizeof(dst));
                wg_blink_read_mem(engine->blink, args[1], src, sizeof(src));
                dst[1023] = src[1023] = 0;
                int dlen = 0; while (dlen < 1023 && dst[dlen]) dlen++;
                int slen = 0; while (slen < 1023 && src[slen]) slen++;
                int copylen = slen;
                if (dlen + copylen > 1022) copylen = 1022 - dlen;
                for (int i = 0; i <= copylen; i++) dst[dlen + i] = src[i];
                dst[dlen + copylen] = 0;
                wg_blink_write_mem(engine->blink, args[0], dst, (dlen + copylen + 1) * 2);
                ret_val = args[0];
            }
        } else if (strcmp(fn, "lstrcmpW") == 0 || strcmp(fn, "lstrcmpiW") == 0) {
            ret_val = 0; // equal
        } else if (strcmp(fn, "lstrcmpiA") == 0) {
            ret_val = 0;
        } else if (strcmp(fn, "GetTempFileNameW") == 0) {
            // GetTempFileNameW(lpPathName, lpPrefixString, uUnique, lpTempFileName)
            // Windows creates the file on disk! NSIS depends on this.
            static uint32_t s_temp_counter = 1;
            uint32_t unique = args[2] ? args[2] : s_temp_counter++;
            uint16_t tmpname[260];
            int pos = 0;
            if (args[0]) {
                uint16_t path[260] = {0};
                wg_blink_read_mem(engine->blink, args[0], path, 518);
                for (int i = 0; path[i] && pos < 240; i++) tmpname[pos++] = path[i];
            }
            if (pos > 0 && tmpname[pos-1] != '\\' && tmpname[pos-1] != '/') {
                tmpname[pos++] = '\\';
            }
            uint16_t pfx[4] = {'t','m','p'};
            if (args[1]) {
                wg_blink_read_mem(engine->blink, args[1], pfx, 6);
            }
            for (int i = 0; i < 3 && pfx[i]; i++) tmpname[pos++] = pfx[i];
            char numstr[16];
            snprintf(numstr, sizeof(numstr), "%05X", unique & 0xFFFFF);
            for (int i = 0; numstr[i] && pos < 250; i++) tmpname[pos++] = numstr[i];
            tmpname[pos++] = '.'; tmpname[pos++] = 't'; tmpname[pos++] = 'm'; tmpname[pos++] = 'p';
            tmpname[pos] = 0;
            if (args[3]) {
                wg_blink_write_mem(engine->blink, args[3], tmpname, (pos + 1) * 2);
            }
            // Convert to ASCII and create the file on disk (like Windows does)
            char aname[520] = {0};
            for (int i = 0; i < pos && i < 519; i++)
                aname[i] = tmpname[i] < 128 ? (char)tmpname[i] : '_';
            const char *real = wg_files_map_path(0, engine->blink, aname, sizeof(aname));
            if (real) {
                FILE *f = fopen(real, "wb");
                if (f) fclose(f);
            }
            ret_val = unique;
        } else if (strcmp(fn, "MessageBoxIndirectW") == 0) {
            // MSGBOXPARAMSW struct: cbSize(4), hwndOwner(4), hInstance(4),
            // lpszText(4), lpszCaption(4), ...
            // lpszText is at offset 12 in the struct
#ifdef WG_DECODE_DIAG
            WG_LOGE("DIAG", "MessageBoxIndirectW called from RIP=0x%llx",
                    (unsigned long long)ret_addr);
#endif
            if (args[0]) {
                uint32_t text_ptr = 0, caption_ptr = 0;
                wg_blink_read_mem(engine->blink, args[0] + 12, &text_ptr, 4);
                wg_blink_read_mem(engine->blink, args[0] + 16, &caption_ptr, 4);
                if (text_ptr) {
                    uint16_t text[512] = {0};
                    wg_blink_read_mem(engine->blink, text_ptr, text, 1022);
                    char atext[512] = {0};
                    for (int i = 0; i < 511 && text[i]; i++)
                        atext[i] = text[i] < 128 ? (char)text[i] : '?';
                    WG_LOGI(TAG, "MessageBox: \"%s\"", atext);
                }
                if (caption_ptr) {
                    uint16_t cap[256] = {0};
                    wg_blink_read_mem(engine->blink, caption_ptr, cap, 510);
                    char acap[256] = {0};
                    for (int i = 0; i < 255 && cap[i]; i++)
                        acap[i] = cap[i] < 128 ? (char)cap[i] : '?';
                    WG_LOGI(TAG, "MessageBox caption: \"%s\"", acap);
                }
            }
            ret_val = 1; // IDOK
        } else if (strcmp(fn, "DialogBoxParamW") == 0) {
            // DialogBoxParamW(hInstance, lpTemplateName=args[1], hWndParent,
            //                 lpDialogFunc=args[3], dwInitParam=args[4])
            uint32_t dlg_id  = args[1];   // MAKEINTRESOURCE id
            uint32_t dlgproc = args[3];
            uint32_t initParam = args[4];
            WG_LOGI(TAG, "DialogBoxParamW(id=%u, dlgproc=0x%X)", dlg_id, dlgproc);

            uint16_t title[] = {'S','t','e','a','m',' ','S','e','t','u','p',0};
            uint32_t hwnd = wg_wm_create_window(0, 0, title, 0x10CF0000,
                                                50, 50, 500, 360, 0);
            // Parse the dialog template so we can render its controls.
            wg_parse_dialog(engine, hwnd, dlg_id);

            // Clean up DialogBoxParamW's stack frame (5 args stdcall)
            uint64_t clean_rsp = rsp + ptr_size + (5 * ptr_size);

            // Call dlgproc(hwnd, WM_INITDIALOG, 0, initParam). The return address
            // is a SENTINEL in the HLT thunk page — when the dlgproc returns the
            // engine catches it and goes modal (instead of falling back into
            // WinMain, which would make it exit). EndDialog later returns to the
            // real WinMain call site (s_dlg_ret_*).
            if (dlgproc && is_32bit) {
                s_dlg_active = true;
                s_dlg_hwnd = hwnd;
                s_dlg_proc = dlgproc;
                s_dlg_ret_addr = (uint32_t)ret_addr;
                s_dlg_ret_rsp  = (uint32_t)clean_rsp;
                s_dlg_result = 1;
                uint32_t new_rsp = (uint32_t)clean_rsp - 20;
                uint32_t stack_data[5] = {
                    WG_DLG_SENTINEL, hwnd, 0x0110 /*WM_INITDIALOG*/, 0, initParam
                };
                wg_blink_write_mem(engine->blink, new_rsp, stack_data, 20);
                wg_blink_set_reg(engine->blink, 4, new_rsp);
                wg_blink_set_rip(engine->blink, dlgproc);
                wg_blink_set_reg(engine->blink, 0, 0);
                return true;
            }

            engine->state = WG_ENGINE_PAUSED;
            wg_blink_set_reg(engine->blink, 4, clean_rsp);
            wg_blink_set_rip(engine->blink, ret_addr);
            wg_blink_set_reg(engine->blink, 0, 0);
            return true;
        } else if (strcmp(fn, "EndDialog") == 0) {
            // EndDialog(hDlg, nResult=args[1]) — return modally to WinMain.
            if (s_dlg_active) {
                s_dlg_result = args[1];
                s_dlg_active = false;
                s_callstack_depth = 0;   // abandon any nested SendMessage frames
                wg_blink_set_reg(engine->blink, 4, s_dlg_ret_rsp);
                wg_blink_set_rip(engine->blink, s_dlg_ret_addr);
                wg_blink_set_reg(engine->blink, 0, s_dlg_result);
                WG_LOGI(TAG, "EndDialog(%u) -> return to WinMain", s_dlg_result);
                return true;
            }
            ret_val = 1;
        } else if (strcmp(fn, "GetDlgItem") == 0) {
            // GetDlgItem(hDlg=args[0], id=args[1]) -> synthetic control HWND.
            WGDlgCtrl *c = wg_find_ctrl(args[0], args[1]);
            ret_val = c ? (WG_CTRL_HWND_BASE + (uint32_t)(c - s_ctrls)) : 0;
        } else if (strcmp(fn, "GetDlgItemTextW") == 0) {
            // GetDlgItemTextW(hDlg=args[0], id=args[1], lpString=args[2], cch=args[3])
            // Return the control's stored text. Without this it returned 0/empty,
            // so NSIS read an empty install path from the directory page's edit
            // field -> $INSTDIR/$OUTDIR empty -> files extracted to the drive_c
            // root instead of C:\Program Files\Steam.
            WGDlgCtrl *c = wg_find_ctrl(args[0], args[1]);
            int n = 0;
            if (args[2] && args[3] > 0) {
                uint16_t tmp[80] = {0};
                if (c) while (n < 79 && c->text[n] && (uint32_t)(n + 1) < args[3])
                           { tmp[n] = c->text[n]; n++; }
                tmp[n] = 0;
                wg_blink_write_mem(engine->blink, args[2], tmp, (n + 1) * 2);
            }
            ret_val = n;
        } else if (strcmp(fn, "RtlUnwind") == 0) {
            // RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue).
            // _except_handler3 calls this (via _global_unwind2) to unwind to the
            // __try frame before running __except. We unlink the SEH chain down
            // to TargetFrame and commit the in-flight exception; the stdcall
            // return lands at TargetIp (== the call's return address in
            // _global_unwind2). Intermediate __finally handlers are skipped for
            // now (iteration 1).
            uint32_t target_frame = args[0];
            if (target_frame > 0x1000u && target_frame < 0xFFFFFFFEu)
                wg_blink_write_mem(engine->blink, s_main_teb + 0, &target_frame, 4);
            s_seh_active = false; s_seh_depth = 0;
            WG_LOGW(TAG, "RtlUnwind(target=0x%X) -> fs:[0] set; SEH committed", target_frame);
            ret_val = args[3];
        } else if (strcmp(fn, "GetWindowTextW") == 0) {
            // GetWindowTextW(hWnd=args[0], lpString=args[1], nMaxCount=args[2]).
            // For a control handle return its stored text (the directory edit's
            // path); for a window return its title. Needed so NSIS reads the
            // chosen install path back (-> $INSTDIR) instead of empty.
            int n = 0;
            if (args[1] && args[2] > 0) {
                uint16_t tmp[80] = {0};
                WGDlgCtrl *c = wg_ctrl_from_handle(args[0]);
                if (c) {
                    while (n < 79 && c->text[n] && (uint32_t)(n + 1) < args[2])
                        { tmp[n] = c->text[n]; n++; }
                }
                tmp[n] = 0;
                wg_blink_write_mem(engine->blink, args[1], tmp, (n + 1) * 2);
            }
            ret_val = n;
        } else if (strcmp(fn, "SetDlgItemTextW") == 0) {
            // SetDlgItemTextW(hDlg=args[0], id=args[1], lpString=args[2])
            WGDlgCtrl *c = wg_find_ctrl(args[0], args[1]);
            if (c && args[2]) {
                wg_blink_read_mem(engine->blink, args[2], c->text, sizeof(c->text));
                c->text[79] = 0;
                if (s_dlg_active) wg_render_dialog(engine, c->hwnd);
            }
            // Surface the install status line (NSIS sets it to "Installing…",
            // file names, or error text) so we can see what the installer is
            // doing in the instfiles page.
            if (args[2]) {
                uint16_t w[128] = {0}; char a[128] = {0};
                wg_blink_read_mem(engine->blink, args[2], w, 254);
                for (int i = 0; i < 127 && w[i]; i++) a[i] = w[i] < 128 ? (char)w[i] : '?';
                if (a[0]) WG_LOGI(TAG, "  status[id=%u]: \"%s\"", args[1], a);
            }
            ret_val = 1;
        } else if (strcmp(fn, "SendMessageW") == 0 ||
                   strcmp(fn, "SendDlgItemMessageW") == 0) {
            // SendDlgItemMessageW(hDlg, id, msg, wParam, lParam): redirect to the
            // child control's handle, then fall through to SendMessage logic.
            uint32_t hwnd, msg, wParam, lParam;
            if (strcmp(fn, "SendDlgItemMessageW") == 0) {
                WGDlgCtrl *c = wg_find_ctrl(args[0], args[1]);
                hwnd = c ? (WG_CTRL_HWND_BASE + (uint32_t)(c - s_ctrls)) : 0;
                msg = args[2]; wParam = args[3]; lParam = args[4];
            } else {
                hwnd = args[0]; msg = args[1]; wParam = args[2]; lParam = args[3];
            }
            // NSIS instfiles "details" lines go to a SysListView32 via
            // LVM_INSERTITEMW(0x104D)/A(0x1007). Log the inserted text — this is
            // the install log ("Output folder:", "Extract: steam.exe", errors…)
            // shown behind the "show details" button.
            if (msg == 0x104D || msg == 0x1007) {
                uint32_t lvitem = lParam;            // LVITEM*: pszText at +20
                uint32_t psz = 0;
                wg_blink_read_mem(engine->blink, lvitem + 20, &psz, 4);
                if (psz) {
                    char line[256] = {0};
                    if (msg == 0x104D) {             // wide
                        uint16_t w[256] = {0};
                        wg_blink_read_mem(engine->blink, psz, w, 510);
                        for (int i = 0; i < 255 && w[i]; i++)
                            line[i] = w[i] < 128 ? (char)w[i] : '?';
                    } else {
                        wg_blink_read_mem(engine->blink, psz, line, 255);
                    }
                    if (line[0]) {
                        WG_LOGI(TAG, "  detail: \"%s\"", line);
                        if (s_detail_count < 256) {
                            strncpy(s_detail_lines[s_detail_count], line,
                                    sizeof(s_detail_lines[0]) - 1);
                            s_detail_count++;
                            if (s_dlg_active && s_page_hwnd)
                                wg_render_dialog(engine, s_page_hwnd);
                        }
                    }
                }
            }
            // Progress bar position: PBM_SETPOS(0x402, wParam=pos),
            // PBM_SETRANGE32(0x406, lParam=max), PBM_DELTAPOS(0x403, wParam=delta).
            if (msg == 0x0402 || msg == 0x0403 || msg == 0x0406) {
                if (msg == 0x0402) s_pb_pos = wParam;
                else if (msg == 0x0403) s_pb_pos += wParam;
                else if (lParam) s_pb_max = lParam;
                if (s_dlg_active && s_page_hwnd) wg_render_dialog(engine, s_page_hwnd);
            }
            // STM_SETIMAGE on one of our synthetic static controls: attach the
            // bitmap (these controls have no guest wndproc to dispatch to).
            if (msg == 0x0172 /*STM_SETIMAGE*/ && wg_ctrl_from_handle(hwnd)) {
                WGDlgCtrl *c = wg_ctrl_from_handle(hwnd);
                c->hbitmap = lParam; c->is_bitmap = true;
                if (s_dlg_active) wg_render_dialog(engine, c->hwnd);
                ret_val = 0;
            } else {
                // Real dispatch: call the target window's procedure and return
                // its result. This is what drives NSIS's page navigation
                // (e.g. SendMessage(hDlg, 0x408, ...) = "show next page").
                uint32_t proc = wg_resolve_wndproc(hwnd);
                int nargs = (strcmp(fn, "SendDlgItemMessageW") == 0) ? 5 : 4;
                uint64_t caller_clean = rsp + ptr_size + (nargs * ptr_size);
                if (proc && is_32bit &&
                    wg_call_wndproc(engine, proc, hwnd, msg, wParam, lParam,
                                    (uint32_t)ret_addr, (uint32_t)caller_clean)) {
                    return true;
                }
                ret_val = 0;
            }
        } else if (strcmp(fn, "CreateDialogParamW") == 0) {
            // CreateDialogParamW(hInstance, lpTemplateName=args[1], hWndParent=args[2],
            //                    lpDialogFunc=args[3], dwInitParam=args[4])
            uint32_t dlg_id = args[1], parent = args[2], dlgproc = args[3];
            uint32_t initParam = args[4];
            // Position the inner page in the IDD_INST inner-dialog placeholder
            // (id 1018) so the parent's header/buttons stay visible around it.
            int px = 0, py = 0, pw = 480, ph = 320;
            WGDlgCtrl *placeholder = wg_find_ctrl(parent, 1018);
            if (placeholder) {
                int32_t cw = 0, chh = 0;
                wg_wm_get_client(parent, &cw, &chh);
                float sx = placeholder->dlg_cx ? (float)cw / placeholder->dlg_cx : 1.0f;
                float sy = placeholder->dlg_cy ? (float)chh / placeholder->dlg_cy : 1.0f;
                px = (int)(placeholder->x * sx); py = (int)(placeholder->y * sy);
                pw = (int)(placeholder->cx * sx); ph = (int)(placeholder->cy * sy);
            }
            uint16_t title[] = {0};
            uint32_t hwnd = wg_wm_create_window(0, 0, title, 0x50000000,
                                                px, py, pw, ph, parent);
            WGWin32Window *pw_win = wg_wm_find(hwnd);
            if (pw_win) pw_win->wndproc = dlgproc;
            wg_retire_inner_page(hwnd);   // drop the previous page so it stops painting
            // Parse the page's own dialog template (e.g. the directory page) so
            // we can render its controls — these are built-in NSIS dialogs, not
            // nsDialogs plugin pages.
            wg_parse_dialog(engine, hwnd, dlg_id);
            s_page_hwnd = hwnd;       // progress/details updates re-render this
            wg_render_dialog(engine, hwnd);
            WG_LOGI(TAG, "CreateDialogParamW(template=%u) -> page HWND=0x%X", dlg_id, hwnd);

            // Dispatch the page's WM_INITDIALOG so NSIS fills the dynamic text
            // (install path in the edit field, disk-space numbers). This must
            // still return the HWND to the caller, so use the override form
            // (ovr_eax=hwnd). The register snapshot in wg_call_wndproc_ovr keeps
            // the caller's ESI intact across the dispatch (its very next
            // instruction is `push [esi+0x2c]`).
            if (dlgproc && is_32bit) {
                uint64_t clean_rsp = rsp + ptr_size + (5 * ptr_size);
                if (wg_call_wndproc_ovr(engine, dlgproc, hwnd,
                                        0x0110 /*WM_INITDIALOG*/, 0, initParam,
                                        (uint32_t)ret_addr, (uint32_t)clean_rsp,
                                        true, hwnd)) {
                    return true;
                }
            }
            ret_val = hwnd;
        } else if (strcmp(fn, "GetLastError") == 0) {
            ret_val = s_last_error;
        } else if (strcmp(fn, "SetLastError") == 0) {
            s_last_error = args[0];
        } else if (strcmp(fn, "GetEnvironmentVariableW") == 0 ||
                   strcmp(fn, "GetEnvironmentVariableA") == 0) {
            // GetEnvironmentVariable(lpName, lpBuffer, nSize)
            // No environment set — always "not found"
            s_last_error = 203; // ERROR_ENVVAR_NOT_FOUND
            ret_val = 0;
        } else if (strcmp(fn, "GetModuleHandleExW") == 0 ||
                   strcmp(fn, "GetModuleHandleExA") == 0) {
            // GetModuleHandleEx(dwFlags, lpModuleName, phModule).
            // dwFlags bit 0x4 = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: then
            // lpModuleName is a CODE ADDRESS, not a name. UE4 uses this on one of
            // its own functions to find its module, then GetModuleFileNameW on the
            // result to derive the game directory — so an address inside the main
            // image MUST map to the image base (returning the fake 0xBFFF0000
            // handle made GetModuleFileNameW yield C:\Windows\System32 and the
            // game hunted for its .uproject/Content there).
            uint32_t base = engine->pe_image ? (uint32_t)engine->pe_image->image_base : 0x400000;
            uint32_t img_end = base + (engine->pe_image ? engine->pe_image->size_of_image : 0x100000);
            uint32_t flags = args[0];
            uint32_t handle;
            if (args[1] == 0)                handle = base;                 // NULL name = main module
            else if ((flags & 0x4) && args[1] >= base && args[1] < img_end) handle = base; // addr in main image
            else                             handle = 0xBFFF0000u;          // other module
            if (args[2]) wg_blink_write_mem(engine->blink, args[2], &handle, 4);
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "SystemFunction036") == 0) {
            // RtlGenRandom(pvBuffer=args[0], cbBuffer=args[1]) -> BOOLEAN.
            // Fill ANY size (BoringSSL/Steam may request large seeds; the old
            // <=256 cap left big buffers uninitialized while still returning
            // TRUE — TLS then aborts with internal_error building ClientHello).
            wg_fill_random(engine->blink, args[0], args[1]);
            WG_LOGI(TAG, "RtlGenRandom(buf=0x%X, %u) -> TRUE", args[0], args[1]);
            ret_val = 1; // TRUE = success
        } else if (strcmp(fn, "BCryptGenRandom") == 0) {
            // BCryptGenRandom(hAlgorithm=args[0], pbBuffer=args[1], cbBuffer=args[2], dwFlags=args[3])
            wg_fill_random(engine->blink, args[1], args[2]);
            WG_LOGI(TAG, "BCryptGenRandom(buf=0x%X, %u) -> 0", args[1], args[2]);
            ret_val = 0; // STATUS_SUCCESS
        } else if (strcmp(fn, "ProcessPrng") == 0) {
            // BOOL ProcessPrng(PBYTE pbData=args[0], SIZE_T cbData=args[1]).
            // BoringSSL on modern Windows reads entropy through this
            // (bcryptprimitives.dll) before falling back to RtlGenRandom.
            wg_fill_random(engine->blink, args[0], args[1]);
            WG_LOGI(TAG, "ProcessPrng(buf=0x%X, %u) -> TRUE", args[0], args[1]);
            ret_val = 1; // TRUE = success
        } else if (strcmp(fn, "RtlGenRandom") == 0) {
            wg_fill_random(engine->blink, args[0], args[1]);
            WG_LOGI(TAG, "RtlGenRandom(buf=0x%X, %u) -> TRUE", args[0], args[1]);
            ret_val = 1;
        } else if (strcmp(fn, "CryptAcquireContextW") == 0 ||
                   strcmp(fn, "CryptAcquireContextA") == 0) {
            // CryptAcquireContext(phProv, container, provider, type, flags).
            // OpenSSL/Steam TLS seeds RAND via the legacy CryptoAPI; the default
            // auto-stub returned FALSE, so RAND_poll failed and the handshake
            // aborted with internal_error before sending ClientHello.
            if (args[0]) { uint32_t h = 0x43500001; wg_blink_write_mem(engine->blink, args[0], &h, 4); }
            WG_LOGI(TAG, "%s -> TRUE (fake HCRYPTPROV)", fn);
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "CryptGenRandom") == 0) {
            // CryptGenRandom(hProv, dwLen=args[1], pbBuffer=args[2]) -> BOOL
            wg_fill_random(engine->blink, args[2], args[1]);
            WG_LOGI(TAG, "CryptGenRandom(buf=0x%X, %u) -> TRUE", args[2], args[1]);
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "CryptReleaseContext") == 0) {
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "CreateFontW") == 0 ||
                   strcmp(fn, "CreateFontA") == 0 ||
                   strcmp(fn, "CreateFontIndirectW") == 0 ||
                   strcmp(fn, "CreateFontIndirectA") == 0) {
            int height = 0, weight = 400; char face[64] = {0};
            bool wide = (fn[strlen(fn) - 1] == 'W');
            if (strstr(fn, "Indirect")) {
                // arg0 -> LOGFONT{W,A}: lfHeight@0, lfWeight@16, lfFaceName@28
                if (args[0]) {
                    int32_t lf[5];
                    wg_blink_read_mem(engine->blink, args[0], lf, 20);
                    height = (int32_t)lf[0]; weight = (int32_t)lf[4];
                    if (wide) {
                        uint16_t wf[32] = {0};
                        wg_blink_read_mem(engine->blink, args[0] + 28, wf, 64);
                        for (int i = 0; i < 31 && wf[i]; i++) face[i] = wf[i] < 128 ? (char)wf[i] : '?';
                    } else {
                        wg_blink_read_mem(engine->blink, args[0] + 28, face, 32);
                    }
                }
            } else {
                // CreateFont(cHeight, cWidth, ..., cWeight@4, ..., pszFaceName@13)
                height = (int32_t)args[0]; weight = (int32_t)args[4];
                if (args[13]) {
                    if (wide) {
                        uint16_t wf[32] = {0};
                        wg_blink_read_mem(engine->blink, args[13], wf, 64);
                        for (int i = 0; i < 31 && wf[i]; i++) face[i] = wf[i] < 128 ? (char)wf[i] : '?';
                    } else {
                        wg_blink_read_mem(engine->blink, args[13], face, 32);
                    }
                }
            }
            int px = height < 0 ? -height : height;       // negative = em height
            if (px <= 0 || px > 200) px = 13;             // clamp / default
            ret_val = wg_font_register(px, weight >= 600, face);
            WG_LOGI(TAG, "%s('%s' h=%d w=%d) -> px=%d HFONT 0x%X",
                    fn, face, height, weight, px, (uint32_t)ret_val);
        } else if (strcmp(fn, "MulDiv") == 0) {
            // MulDiv(nNumber=args[0], nNumerator=args[1], nDenominator=args[2])
            int32_t a = (int32_t)args[0];
            int32_t b = (int32_t)args[1];
            int32_t d = (int32_t)args[2];
            if (d == 0) ret_val = (uint64_t)(int64_t)-1;
            else ret_val = (uint64_t)(int64_t)((int64_t)a * b / d);
        } else if (strcmp(fn, "RegisterClassExW") == 0 ||
                   strcmp(fn, "RegisterClassExA") == 0 ||
                   strcmp(fn, "RegisterClassW") == 0 ||
                   strcmp(fn, "RegisterClassA") == 0) {
            static uint32_t s_next_atom = 0xC000;
            ret_val = s_next_atom++;
        } else if (strcmp(fn, "OutputDebugStringA") == 0) {
            if (args[0]) {
                char dbg[512] = {0};
                wg_blink_read_mem(engine->blink, args[0], dbg, 511);
                WG_LOGI(TAG, "DbgPrint: %s", dbg);
                // DIAG: Steam's "invalid input pointer" (strtools_unicode.cpp) on the
                // download orchestrator precedes its early exit (big packages never
                // download). Dump the .text caller chain so we can see WHICH op passed
                // a bad string pointer (likely a path/name we returned wrong).
                if (engine->pe_image && engine->pe_image->image_base == 0x400000 &&
                    (strstr(dbg, "invalid input") || strstr(dbg, "strtools"))) {
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    char chain[300] = {0}; int ci = 0, found = 0;
                    for (int k = 0; k < 160 && found < 12; k++) {
                        uint32_t v = 0;
                        wg_blink_read_mem(engine->blink, esp + k * 4, &v, 4);
                        if (v >= 0x401000 && v < 0x700000) {
                            ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v);
                            found++;
                        }
                    }
                    WG_LOGW(TAG, "*** DBGERR-CHAIN esp=0x%X: %s", esp, chain);
                }
            }
        } else if (strcmp(fn, "OutputDebugStringW") == 0) {
            if (args[0]) {
                uint16_t wdbg[256] = {0};
                wg_blink_read_mem(engine->blink, args[0], wdbg, 510);
                char dbg[256] = {0};
                for (int i = 0; i < 255 && wdbg[i]; i++)
                    dbg[i] = wdbg[i] < 128 ? (char)wdbg[i] : '?';
                WG_LOGI(TAG, "DbgPrint: %s", dbg);
            }
        } else if (strcmp(fn, "AppPolicyGetProcessTerminationMethod") == 0) {
            // AppPolicyGetProcessTerminationMethod(token, *policy)
            // Write 0 (ExitProcess) to *policy, return ERROR_SUCCESS
            if (args[1]) {
                uint32_t zero = 0;
                wg_blink_write_mem(engine->blink, args[1], &zero, 4);
            }
            ret_val = 0;
        } else if (strcmp(fn, "GetCurrentPackageId") == 0) {
            // GetCurrentPackageId(*bufferLength, buffer)
            // Not packaged — return APPMODEL_ERROR_NO_PACKAGE (15700)
            ret_val = 15700;
        } else if (strcmp(fn, "IsUserAnAdmin") == 0) {
            ret_val = 1; // yes, admin
        } else if (strcmp(fn, "IsWindowEnabled") == 0 ||
                   strcmp(fn, "IsWindowVisible") == 0) {
            // The registered stub_return_1 is dead under blink (it writes the
            // builtin-interpreter CPU state, which we don't use), so without an
            // explicit case these default to 0. NSIS's WM_COMMAND handler bails
            // out when IsWindowEnabled(nextButton) is 0 — i.e. every wizard
            // button tap was silently ignored. Report our controls as enabled
            // and visible so navigation proceeds.
            ret_val = 1;
        } else if (strcmp(fn, "IsWindow") == 0) {
            ret_val = (args[0] != 0) ? 1 : 0;
        } else if (strcmp(fn, "WideCharToMultiByte") == 0) {
            // WideCharToMultiByte(CodePage, dwFlags, lpWideCharStr=args[2],
            //   cchWideChar=args[3], lpMultiByteStr=args[4], cbMultiByte=args[5], ...)
            // MUST actually convert + return the byte count: NSIS's plug-in
            // export resolver (exe 0x4065c7) converts the wide function name to
            // ANSI here and bails to NULL (skipping GetProcAddress) if this
            // returns 0 — which is exactly why nsProcess::FindProcess was never
            // resolved/called and the installer looped.
            uint32_t wstr = args[2];
            int32_t  cch  = (int32_t)args[3];
            uint32_t mbstr = args[4];
            int32_t  cbmb = (int32_t)args[5];
            uint16_t wbuf[2048]; int wlen = 0;
            if (wstr) {
                if (cch < 0) {
                    for (; wlen < 2047; wlen++) {
                        uint16_t c = 0;
                        wg_blink_read_mem(engine->blink, wstr + wlen * 2, &c, 2);
                        wbuf[wlen] = c;
                        if (!c) { wlen++; break; }   // null-terminated: include NUL
                    }
                } else {
                    wlen = cch < 2047 ? cch : 2047;
                    wg_blink_read_mem(engine->blink, wstr, wbuf, wlen * 2);
                }
            }
            char abuf[2048]; int alen = 0;
            for (int i = 0; i < wlen; i++)
                abuf[alen++] = wbuf[i] < 128 ? (char)wbuf[i] : '?';
            if (cbmb == 0) {
                ret_val = alen;                      // query required size
            } else {
                int n = alen < cbmb ? alen : cbmb;
                if (mbstr) wg_blink_write_mem(engine->blink, mbstr, abuf, n);
                ret_val = n;
            }
        } else if (strcmp(fn, "MultiByteToWideChar") == 0) {
            // MultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr=args[2],
            //   cbMultiByte=args[3], lpWideCharStr=args[4], cchWideChar=args[5])
            uint32_t mbstr = args[2];
            int32_t  cbmb  = (int32_t)args[3];
            uint32_t wstr  = args[4];
            int32_t  cch   = (int32_t)args[5];
            char abuf[2048]; int alen = 0;
            if (mbstr) {
                if (cbmb < 0) {
                    for (; alen < 2047; alen++) {
                        uint8_t c = 0;
                        wg_blink_read_mem(engine->blink, mbstr + alen, &c, 1);
                        abuf[alen] = (char)c;
                        if (!c) { alen++; break; }   // null-terminated: include NUL
                    }
                } else {
                    alen = cbmb < 2047 ? cbmb : 2047;
                    wg_blink_read_mem(engine->blink, mbstr, abuf, alen);
                }
            }
            if (cch == 0) {
                ret_val = alen;                      // query required size (chars)
            } else {
                int n = alen < cch ? alen : cch;
                if (wstr) {
                    uint16_t wbuf[2048];
                    for (int i = 0; i < n; i++) wbuf[i] = (uint8_t)abuf[i];
                    wg_blink_write_mem(engine->blink, wstr, wbuf, n * 2);
                }
                ret_val = n;
            }
        } else if (strcmp(fn, "TlsAlloc") == 0) {
            // TLS slot indices are process-global (shared across threads).
            ret_val = (s_tls_next < 1088) ? s_tls_next++ : 0xFFFFFFFF;
        } else if (strcmp(fn, "TlsGetValue") == 0) {
            int ti = wg_tls_index(engine->scheduler ? engine->scheduler->current : -1);
            ret_val = (args[0] < 1088) ? s_tls_slots[ti][args[0]] : 0;
            s_last_error = 0;
        } else if (strcmp(fn, "TlsSetValue") == 0) {
            int ti = wg_tls_index(engine->scheduler ? engine->scheduler->current : -1);
            if (args[0] < 1088) s_tls_slots[ti][args[0]] = args[1];
            ret_val = 1;
        } else if (strcmp(fn, "TlsFree") == 0) {
            ret_val = 1;
        } else if (strcmp(fn, "FlsAlloc") == 0) {
            // FLS slot indices are process-global; values are per-thread.
            ret_val = (s_fls_next < 1088) ? s_fls_next++ : 0xFFFFFFFF;
        } else if (strcmp(fn, "FlsGetValue") == 0) {
            int fi = wg_tls_index(engine->scheduler ? engine->scheduler->current : -1);
            ret_val = (args[0] < 1088) ? s_fls_slots[fi][args[0]] : 0;
            s_last_error = 0;
        } else if (strcmp(fn, "FlsSetValue") == 0) {
            int fi = wg_tls_index(engine->scheduler ? engine->scheduler->current : -1);
            if (args[0] < 1088) s_fls_slots[fi][args[0]] = args[1];
            ret_val = 1;
        } else if (strcmp(fn, "FlsFree") == 0) {
            ret_val = 1;
        } else if (strcmp(fn, "GetStringTypeW") == 0 ||
                   strcmp(fn, "GetStringTypeExW") == 0) {
            // GetStringTypeW(dwInfoType, lpSrcStr, cchSrc, lpCharType)
            // GetStringTypeExW(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType)
            int ofs = (fn[13] == 'E') ? 1 : 0; // ExW has Locale as first arg
            uint32_t info_type = args[0 + ofs];
            uint32_t src_ptr = args[1 + ofs];
            int32_t count = (int32_t)args[2 + ofs];
            uint32_t out_ptr = args[3 + ofs];
            if (count < 0 && src_ptr) {
                count = 0;
                uint16_t ch;
                do { wg_blink_read_mem(engine->blink, src_ptr + count * 2, &ch, 2); count++; }
                while (ch && count < 256);
            }
            if (out_ptr && count > 0 && count <= 512) {
                uint16_t types[512];
                memset(types, 0, count * 2);
                if ((info_type & 1) && src_ptr) {
                    uint16_t str[512];
                    wg_blink_read_mem(engine->blink, src_ptr, str, (uint32_t)(count * 2));
                    for (int i = 0; i < count; i++) {
                        uint16_t c = str[i];
                        if (c >= 'A' && c <= 'Z')      types[i] = 0x0181;
                        else if (c >= 'a' && c <= 'z')  types[i] = 0x0182;
                        else if (c >= '0' && c <= '9')  types[i] = 0x0084;
                        else if (c == ' ')               types[i] = 0x0048;
                        else if (c < 0x20)               types[i] = 0x0020;
                        else if (c == 0)                 types[i] = 0x0020;
                        else                             types[i] = 0x0010;
                    }
                }
                wg_blink_write_mem(engine->blink, out_ptr, types, (uint32_t)(count * 2));
            }
            ret_val = 1;
        } else if (strcmp(fn, "LCMapStringW") == 0 ||
                   strcmp(fn, "LCMapStringA") == 0 ||
                   strcmp(fn, "LCMapStringEx") == 0) {
            // For LCMAP_LOWERCASE/UPPERCASE, just copy the string through
            // For length query (dest=NULL or destLen=0), return source length
            bool is_ex = (fn[11] == 'E');
            int ofs = is_ex ? 1 : 0; // LCMapStringEx has locale name as first arg
            uint32_t flags = args[1 + ofs];
            uint32_t src = args[2 + ofs];
            int32_t src_len = (int32_t)args[3 + ofs];
            uint32_t dst = args[4 + ofs];
            int32_t dst_len = (int32_t)args[5 + ofs];
            if (src_len < 0 && src) {
                src_len = 0;
                if (fn[12] == 'A') {
                    uint8_t ch; do { wg_blink_read_mem(engine->blink, src + src_len, &ch, 1); src_len++; } while (ch && src_len < 512);
                } else {
                    uint16_t ch; do { wg_blink_read_mem(engine->blink, src + src_len*2, &ch, 2); src_len++; } while (ch && src_len < 512);
                }
            }
            if (dst == 0 || dst_len == 0) {
                ret_val = (uint32_t)src_len; // return required length
            } else if (src && dst && src_len > 0) {
                int bpc = (fn[12] == 'A') ? 1 : 2;
                int copy = src_len < dst_len ? src_len : dst_len;
                uint8_t tmp[1024];
                int bytes = copy * bpc;
                if (bytes > 1024) bytes = 1024;
                wg_blink_read_mem(engine->blink, src, tmp, (uint32_t)bytes);
                wg_blink_write_mem(engine->blink, dst, tmp, (uint32_t)bytes);
                ret_val = (uint32_t)copy;
            } else {
                ret_val = 0;
            }
        } else if (strcmp(fn, "GetACP") == 0) {
            ret_val = 1252;   // Windows-1252; ACP=0 trips the CRT _invalid_parameter
        } else if (strcmp(fn, "GetOEMCP") == 0) {
            ret_val = 437;
        } else if (strcmp(fn, "GetConsoleOutputCP") == 0 ||
                   strcmp(fn, "GetConsoleCP") == 0) {
            ret_val = 437;
        } else if (strcmp(fn, "IsValidCodePage") == 0) {
            ret_val = 1;      // accept whatever codepage the CRT probes
        } else if (strcmp(fn, "GetCPInfo") == 0) {
            // GetCPInfo(CodePage, lpCPInfo=args[1]) -> CPINFO{MaxCharSize, DefaultChar[2], LeadByte[12]}
            if (args[1]) {
                uint8_t cp[18] = {0};
                uint32_t mcs = 1; memcpy(cp, &mcs, 4);  // MaxCharSize=1
                cp[4] = '?';                              // DefaultChar[0]
                wg_blink_write_mem(engine->blink, args[1], cp, 18);
            }
            ret_val = 1;
        } else if (strcmp(fn, "EncodePointer") == 0 ||
                   strcmp(fn, "DecodePointer") == 0) {
            // MUST be identity: the CRT stores EncodePointer(fnptr) and later
            // DecodePointer+calls it. Returning 0 would null function pointers.
            ret_val = args[0];
        } else if (strcmp(fn, "GetCurrentThread") == 0) {
            ret_val = 0xFFFFFFFE;   // pseudo-handle for the current thread
        } else if (strcmp(fn, "GetProcessHeap") == 0) {
            ret_val = 0x00D00000;   // matches PEB->ProcessHeap in the TEB setup
        } else if (strcmp(fn, "HeapCreate") == 0) {
            ret_val = 0x00D10000;   // a distinct non-null fake heap handle
        } else if (strcmp(fn, "HeapAlloc") == 0) {
            // HeapAlloc(hHeap, dwFlags, dwBytes=args[2]) -> real guest heap
            // (always zeroed; HEAP_ZERO_MEMORY is then satisfied). The CRT's
            // malloc/startup heap-init goes through here — returning 0 crashed
            // steam.exe right after the TEB setup.
            ret_val = wg_guest_alloc(engine, args[2]);
        } else if (strcmp(fn, "HeapSize") == 0) {
            // HeapSize(hHeap, dwFlags, lpMem=args[2])
            ret_val = lookup_alloc_size(args[2]);
        } else if (strcmp(fn, "HeapReAlloc") == 0) {
            // HeapReAlloc(hHeap, dwFlags, lpMem=args[2], dwBytes=args[3])
            uint32_t np = wg_guest_alloc(engine, args[3]);
            if (np && args[2] && args[3]) {
                uint8_t *tmp = malloc(args[3]);
                if (tmp) {
                    wg_blink_read_mem(engine->blink, args[2], tmp, args[3]);
                    wg_blink_write_mem(engine->blink, np, tmp, args[3]);
                    free(tmp);
                }
            }
            ret_val = np;
        } else if (strcmp(fn, "VirtualAlloc") == 0) {
            // VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect).
            // The old stub returned a bogus, UNMAPPED pointer. Steam's client
            // downloader buffers each package with VirtualAlloc (small packages
            // go through the CRT heap/HeapAlloc which we back, but the big 25-94MB
            // ones use VirtualAlloc), so it was writing tens of MB into unmapped
            // memory -> page-fault storm + heap corruption (the manifest/first few
            // small packages worked, then it blew up on the large ones). Back it
            // with real, mapped, zeroed guest heap. A commit at an address we
            // already reserved+mapped just returns that address.
            // 64-bit guest: args[] is truncated to 32-bit, so read the FULL 64-bit
            // RCX(addr)/RDX(size) — the game's FMallocBinned2 pools live in the
            // 64-bit heap (>4GB) once the 1GB 32-bit heap is used up.
            uint64_t va_addr = args64[0], va_size = args64[1];
            uint32_t va_type = args[2];              // MEM_COMMIT=0x1000, MEM_RESERVE=0x2000
            bool va_is64 = engine->pe_image && engine->pe_image->is_64bit;
            // Threshold for routing a fresh reservation into the 20GB region-3
            // (64-bit) heap vs the ~3GB 32-bit heap. A full UE4 level load issues
            // ~100 large single-buffer allocs just under 32MB (23MB pools, 11.5MB,
            // 5.77MB, ...) — at a 32MB cutoff they all piled into the 32-bit heap
            // and OOM'd ("Ran out of memory allocating 23072768 bytes"). Route any
            // alloc >=1MB to region 3 for 64-bit guests: those are individual large
            // buffers touched only via the args64-aware bulk handlers
            // (memcpy/memmove/ReadFile), so >4GB pointers are safe, while the many
            // sub-1MB FMallocBinned2 pool CHUNKS (whose objects are touched by the
            // 32-bit-only handlers) stay <4GB. 32-bit guests can't address >4GB, so
            // they keep everything in the 32-bit heap. map64 commits page-granular,
            // so no waste. WG_REGION3_MB overrides the cutoff.
            //
            // Cutoff is 1MB. The FMallocBinned2 "realloc an unrecognized block"
            // corruption that first appeared at a 1MB cutoff was NOT pointer
            // truncation — it was CROSS-SIZE free-list reuse (a 103MB block split
            // into a 46MB alloc) making FMallocBinned2's internal block offsets
            // inconsistent. Now that region-3 reuse is EXACT-size only, a base is
            // always reallocated at the same size, so pools stay consistent and are
            // safe in region 3. Keeping the cutoff low is REQUIRED: at 20MB the
            // 1..20MB allocs piled into the ~3GB 32-bit heap and OOM'd mid-load
            // ("Ran out of memory allocating 11538432 bytes"). Only tiny (<1MB)
            // FMallocBinned2 pool chunks stay in the 32-bit heap. WG_REGION3_MB overrides.
            uint64_t region3_min = 1ull * 1024 * 1024;
            if (getenv("WG_REGION3_MB")) region3_min = (uint64_t)atoi(getenv("WG_REGION3_MB")) * 1024 * 1024;
            if (!va_is64) region3_min = 0xFFFFFFFFFFFFFFFFull;  // 32-bit: never region 3
            if (va_size == 0) {
                ret_val = 0;                     // Windows: size 0 -> ERROR_INVALID_PARAMETER
                s_last_error = 87;
            } else if (va_addr != 0) {
                // Commit into an existing reservation. Region-3 reserves handed back
                // address space only, so back this sub-range with pages now (if committing).
                if (va_addr >= WG_HEAP64_BASE && (va_type & 0x1000))
                    wg_guest_map64(engine, va_addr, va_size);
                ret_val = va_addr;
            } else if (va_size >= region3_min) {
                // LARGE pool -> region 3 (guest 4..8GB, Path B's extended linear half).
                // Keeps the game's big FMallocBinned2 pools out of the ~3GB 32-bit heap so
                // a full UE4 asset load doesn't OOM. Reserve is address-space-only (cheap);
                // only MEM_COMMIT backs pages. Its pointers flow through the args64-aware
                // mem handlers. Falls back to the 32-bit heap if region 3 is full.
                uint64_t a = wg_guest_reserve64(va_size, 0x10000);
                int mapped = 1;
                if (a && (va_type & 0x1000)) { mapped = wg_guest_map64(engine, a, va_size); if (!mapped) a = 0; }
                if (!a) WG_LOGW(TAG, "region3 FAIL size=%llu reserve=%s heap64_ptr=0x%llX free64=%d live=%d dropped=%llu",
                                (unsigned long long)va_size, mapped ? "0(full)" : "map-failed",
                                (unsigned long long)s_heap64_ptr, s_free64_n, s_alloc64_n,
                                (unsigned long long)s_free64_dropped);
                ret_val = a ? a : wg_guest_alloc_aligned(engine, (uint32_t)va_size, 0x10000);
            } else {
                // Fresh small reservation: align to the OS allocation granularity (64KB).
                // FMallocBinned2 depends on this alignment for its pool math. Small allocs
                // stay in the 32-bit heap so the many 32-bit-only handlers are unaffected;
                // the free-list reclaims VirtualFree'd blocks so churn doesn't leak.
                ret_val = wg_guest_alloc_aligned(engine, (uint32_t)va_size, 0x10000);
            }
            WG_LOGI(TAG, "VirtualAlloc(addr=0x%llX, size=%llu, type=0x%X) -> 0x%llX",
                    (unsigned long long)va_addr, (unsigned long long)va_size, va_type,
                    (unsigned long long)ret_val);
        } else if (strcmp(fn, "VirtualFree") == 0) {
            // VirtualFree(lpAddress, dwSize, dwFreeType). RECLAIM the block so the guest
            // heap doesn't leak the game's buffer-grow churn to OOM. MEM_RELEASE(0x8000)
            // frees the whole reservation (dwSize must be 0); MEM_DECOMMIT(0x4000) frees
            // a sub-range. Only reclaim on RELEASE. Use args64 for the address so region-3
            // (>4GB) pool frees are reclaimed too.
            if (args[2] & 0x8000) {
                if (args64[0] >= WG_HEAP64_BASE) wg_guest_free64(args64[0]);
                else wg_guest_free((uint32_t)args64[0], 0);
            } else if ((args[2] & 0x4000) && args64[0] >= WG_HEAP64_BASE) {
                // MEM_DECOMMIT of a region-3 sub-range: mark it uncommitted so a
                // later re-commit re-zeroes it (Windows semantics), matching the
                // commit-once bitmap. The reservation itself stays reserved.
                wg_uncommit64(args64[0], args64[1] ? args64[1] : 0x1000);
            }
            ret_val = 1;   // TRUE
        } else if (strcmp(fn, "??2@YAPAXI@Z") == 0 ||   // operator new(uint)
                   strcmp(fn, "malloc") == 0) {
            // CRT allocators used by real DLLs (StdUtils, etc.). Returning 0
            // (the old auto-stub default) made the plug-in deref a NULL buffer
            // and SIGSEGV. Hand back real guest heap.
            ret_val = wg_guest_alloc(engine, args[0]);
        } else if (strcmp(fn, "calloc") == 0) {
            ret_val = wg_guest_alloc(engine, args[0] * args[1]);  // already zeroed
        } else if (strcmp(fn, "realloc") == 0) {
            // Bump allocator can't grow in place; allocate fresh and copy. We
            // don't know the old size, so copy a bounded amount (new size).
            uint32_t np = wg_guest_alloc(engine, args[1]);
            if (np && args[0] && args[1]) {
                uint8_t *tmp = malloc(args[1]);
                if (tmp) {
                    wg_blink_read_mem(engine->blink, args[0], tmp, args[1]);
                    wg_blink_write_mem(engine->blink, np, tmp, args[1]);
                    free(tmp);
                }
            }
            ret_val = np;
        } else if (strcmp(fn, "??3@YAXPAX@Z") == 0 ||   // operator delete(void*)
                   strcmp(fn, "free") == 0) {
            ret_val = 0;   // bump allocator: free is a no-op
        } else if (strcmp(fn, "memset") == 0) {
            // memset(dest, c, n) -> returns dest (cdecl). Use args64 for the pointer so
            // it works with 64-bit-heap buffers (>4GB); the blink helpers take u64.
            uint64_t dst = args64[0], n = args64[2];
            if (dst && n && n <= 64u * 1024 * 1024) {
                // Fast direct in-guest fill (no malloc/bounce) — ~2x the memcpy/memset
                // grind rate. Now DEFAULT ON: the old deadlock it exposed is fixed by
                // the adaptive spin-yield, so the throughput win is free. Falls back to
                // the malloc bounce on an unmapped page. WG_NO_FASTMEM disables.
                static signed char s_fastmem = -1;
                if (s_fastmem < 0) s_fastmem = getenv("WG_NO_FASTMEM") ? 0 : 1;
                if (!s_fastmem || !wg_blink_mem_set(engine->blink, dst, (int)args64[1], n)) {
                    uint8_t *tmp = malloc((size_t)n);
                    if (tmp) { memset(tmp, (int)args64[1], (size_t)n);
                        wg_blink_write_mem(engine->blink, dst, tmp, (uint32_t)n); free(tmp); }
                }
            }
            ret_val = dst;
        } else if (strcmp(fn, "memcpy") == 0 || strcmp(fn, "memmove") == 0) {
            // mem(c)py(dest, src, n) -> returns dest. Use args64 for the pointers.
            uint64_t dst = args64[0], src = args64[1], n = args64[2];
            if (dst && src && n && n <= 64u * 1024 * 1024) {
                // Fast direct guest->guest copy (no malloc) — default ON (see memset).
                static signed char s_fastmem = -1;
                if (s_fastmem < 0) s_fastmem = getenv("WG_NO_FASTMEM") ? 0 : 1;
                if (!s_fastmem || !wg_blink_mem_copy(engine->blink, dst, src, n)) {
                    uint8_t *tmp = malloc((size_t)n);
                    if (tmp) { wg_blink_read_mem(engine->blink, src, tmp, (uint32_t)n);
                        wg_blink_write_mem(engine->blink, dst, tmp, (uint32_t)n); free(tmp); }
                }
            }
            ret_val = dst;
        } else if (s_use_real_threads &&
                   (strcmp(fn, "EnterCriticalSection") == 0 ||
                    strcmp(fn, "TryEnterCriticalSection") == 0)) {
            uint32_t m = wg_cs_mutex_for(args[0]);
            if (strcmp(fn, "TryEnterCriticalSection") == 0) {
                uint32_t r = m ? wg_sync_wait_single(m, 0, s_cur_guest_tid) : WG_WAIT_TIMEOUT;
                ret_val = (r == WG_WAIT_OBJECT_0) ? 1 : 0;
                if (ret_val) s_cs_held++;   // GIL-pin: hold the lock atomically
            } else {
                // Lock atomicity fix: try to acquire WITHOUT releasing the GIL; only
                // release (letting other guest threads run) if the CS is CONTENDED.
                // Releasing the GIL for an UNCONTENDED acquire lets another thread run
                // while this thread is mid-way through a lock-protected update, so a
                // lock-free reader observes half-updated data -> the config-init race
                // that crashes full JIT under real-threads (NCPU>=2). Uncontended
                // acquire is instant, so blocking-begin/end was pure race window.
                if (m && wg_sync_wait_single(m, 0, s_cur_guest_tid) != WG_WAIT_OBJECT_0) {
                    wg_thunk_block_begin();
                    wg_sync_wait_single(m, WG_SYNC_INFINITE, s_cur_guest_tid);
                    wg_thunk_block_end();
                }
                s_cs_held++;   // GIL-pin: keep the whole locked region atomic vs other guest threads
                ret_val = 1;
            }
        } else if (s_use_real_threads && strcmp(fn, "LeaveCriticalSection") == 0) {
            uint32_t m = wg_cs_mutex_for(args[0]);
            if (m) wg_sync_release_mutex(m, s_cur_guest_tid);
            if (s_cs_held > 0) s_cs_held--;   // release the GIL-pin
            ret_val = 1;
        } else if (s_use_real_threads &&
                   (strcmp(fn, "InitializeCriticalSection") == 0 ||
                    strcmp(fn, "InitializeCriticalSectionAndSpinCount") == 0 ||
                    strcmp(fn, "InitializeCriticalSectionEx") == 0)) {
            ret_val = 1;   // wg_sync mutex is lazily created on first Enter
        } else if (s_use_real_threads && strcmp(fn, "DeleteCriticalSection") == 0) {
            ret_val = 1;
        } else if (strcmp(fn, "CreateEventA") == 0 ||
                   strcmp(fn, "CreateEventW") == 0) {
            // CreateEvent(lpSecurityAttributes, bManualReset, bInitialState, lpName)
            if (s_use_real_threads) {
                ret_val = wg_sync_create_event(args[1] != 0, args[2] != 0);
                wg_synctrace(args[1] ? "CreateEvtM" : "CreateEvtA", s_cur_guest_tid, (uint32_t)ret_val, ret_addr);
            } else {
            uint32_t handle = 0;
            uint32_t idx = 0xFFFFFFFFu;
            if (s_event_free_n > 0) idx = s_event_free[--s_event_free_n]; // recycle a closed slot
            else if (s_event_next < WG_MAX_EVENTS) idx = s_event_next++;
            if (idx != 0xFFFFFFFFu) {
                s_event_signalled[idx] = (args[2] != 0);
                s_event_manual[idx] = (args[1] != 0); // bManualReset
                handle = WG_EVENT_BASE + idx;
            } else {
                WG_LOGW(TAG, "CreateEvent: event table FULL (%d) — returning 0", WG_MAX_EVENTS);
            }
            WG_LOGI(TAG, "CreateEvent(manualReset=%u, initState=%u) -> h=0x%X",
                    args[1], args[2], handle);
            ret_val = handle;
            }
        } else if (strcmp(fn, "CreateEventExW") == 0 || strcmp(fn, "CreateEventExA") == 0) {
            // CreateEventEx(lpEventAttributes, lpName, dwFlags, dwDesiredAccess).
            // dwFlags: CREATE_EVENT_MANUAL_RESET=0x1, CREATE_EVENT_INITIAL_SET=0x2.
            // UE4's FEventWin uses CreateEventExW (NOT CreateEventW) — it was UNHANDLED,
            // so the FEvent got a null handle and threads waited on it forever (the
            // cooperative 0x9FDA6F/null-event deadlock). Map to the same event table.
            uint32_t dwFlags = args[2];
            bool manual = (dwFlags & 0x1u) != 0;
            bool initset = (dwFlags & 0x2u) != 0;
            if (s_use_real_threads) {
                ret_val = wg_sync_create_event(manual, initset);
                wg_synctrace(manual ? "CreateEvtExM" : "CreateEvtExA", s_cur_guest_tid, (uint32_t)ret_val, ret_addr);
            } else {
                uint32_t handle = 0, idx = 0xFFFFFFFFu;
                if (s_event_free_n > 0) idx = s_event_free[--s_event_free_n];
                else if (s_event_next < WG_MAX_EVENTS) idx = s_event_next++;
                if (idx != 0xFFFFFFFFu) {
                    s_event_signalled[idx] = initset;
                    s_event_manual[idx] = manual;
                    handle = WG_EVENT_BASE + idx;
                }
                WG_LOGI(TAG, "CreateEventEx(flags=0x%X manual=%u initSet=%u) -> h=0x%X",
                        dwFlags, manual, initset, handle);
                ret_val = handle;
            }
        } else if (strcmp(fn, "SetEvent") == 0) {
            uint32_t h = args[0];
            wg_synctrace("SetEvent", s_cur_guest_tid, h, ret_addr);
            wg_producer_set(h, s_cur_guest_tid);   // learn: this tid signals h (directed GIL)
            if (getenv("WG_WAITLOG")) {   // log each distinct SetEvent handle — deadlock diag
                static uint32_t s_eh[64]; static int s_en = 0;
                int seen=0; for(int i=0;i<s_en;i++) if(s_eh[i]==h){seen=1;break;}
                if(!seen && s_en<64){ s_eh[s_en++]=h;
                    WG_LOGW(TAG,"[waitlog tid=0x%X] SetEvent(h=0x%X) caller=0x%llX",
                            s_cur_guest_tid, h, (unsigned long long)ret_addr); }
            }
            if (s_use_real_threads) {
                ret_val = wg_sync_set_event(h) ? 1 : 0;
                // Mimic Windows: SetEvent can immediately switch to the woken
                // thread. Under the GIL the signaler otherwise keeps running and
                // can consume its OWN signal (UE4 does Trigger(E) then Wait(E)) or
                // race the intended waiter before it's scheduled -> lost wakeup ->
                // the config-parse thread-coordination deadlock. Release the GIL so
                // the just-woken waiter runs before this thread continues.
                static signed char s_sey = -1;
                if (s_sey < 0) s_sey = getenv("WG_SETEVENT_YIELD") ? 1 : 0;
                if (s_sey) { wg_thunk_block_begin(); sched_yield(); wg_thunk_block_end(); }
            }
            else {
            if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS)
                s_event_signalled[h - WG_EVENT_BASE] = true;
            WG_LOGI(TAG, "SetEvent(h=0x%X)", h);
            wg_sched_wake(engine->scheduler, h);
            ret_val = 1;
            }
        } else if (strcmp(fn, "ResetEvent") == 0) {
            uint32_t h = args[0];
            wg_synctrace("ResetEvent", s_cur_guest_tid, h, ret_addr);
            if (s_use_real_threads) { ret_val = wg_sync_reset_event(h) ? 1 : 0; }
            else {
            if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS)
                s_event_signalled[h - WG_EVENT_BASE] = false;
            ret_val = 1;
            }
        } else if (strcmp(fn, "CreateIoCompletionPort") == 0) {
            // CreateIoCompletionPort(FileHandle, ExistingPort, CompletionKey, NumberOfConcurrentThreads)
            // FileHandle == INVALID_HANDLE_VALUE (-1 / 0xFFFFFFFF): create new IOCP
            // else: associate FileHandle (socket) with existing IOCP + CompletionKey
            uint32_t file_h = args[0];
            uint32_t comp_key = args[2];
            // We do NOT deliver real IOCP socket completions, so steering apps to
            // IOCP-based async I/O leaves them in an inconsistent state (Steam's
            // tcpconnection asserts !BUseIOCP() then send(0,0,0) and bails).
            // Fail IOCP creation so the app uses the synchronous select/send/recv
            // path, which we fully support.
            if (s_disable_iocp) {
                s_last_error = 50; // ERROR_NOT_SUPPORTED
                WG_LOGI(TAG, "CreateIoCompletionPort -> 0 (IOCP disabled, force select path)");
                ret_val = 0;
            } else if (file_h == 0xFFFFFFFFu) {
                s_iocp_created = true;
                WG_LOGI(TAG, "CreateIoCompletionPort(new) -> 0x%X", WG_IOCP_HANDLE);
                ret_val = WG_IOCP_HANDLE;
            } else {
                // Bind socket to IOCP with its completion key
                if (s_iocp_binding_count < WG_MAX_IOCP_SOCK) {
                    s_iocp_bindings[s_iocp_binding_count++] = (WGIocpBinding){file_h, comp_key};
                }
                WG_LOGI(TAG, "CreateIoCompletionPort(sock=0x%X, key=0x%X) -> 0x%X",
                        file_h, comp_key, WG_IOCP_HANDLE);
                ret_val = WG_IOCP_HANDLE;
            }
        } else if (strcmp(fn, "GetQueuedCompletionStatus") == 0 ||
                   strcmp(fn, "GetQueuedCompletionStatusEx") == 0) {
            // GQCS(iocp, lpBytes, lpKey, lpOverlapped, timeout)
            // GQCSE(iocp, lpEntries, count, lpRemoved, timeout, alertable)
            bool is_ex = (strcmp(fn, "GetQueuedCompletionStatusEx") == 0);
            uint32_t timeout = is_ex ? args[4] : args[4];
            uint32_t bytes_out = 0, key_out = 0, ovl_out = 0;
            bool got = iocp_get(&bytes_out, &key_out, &ovl_out);
            WG_LOGI(TAG, "%s(timeout=0x%X) got=%d bytes=%u key=0x%X ovl=0x%X",
                    fn, timeout, (int)got, bytes_out, key_out, ovl_out);
            if (got) {
                if (!is_ex) {
                    // Write results to guest
                    if (args[1]) wg_blink_write_mem(engine->blink, args[1], &bytes_out, 4);
                    if (args[2]) wg_blink_write_mem(engine->blink, args[2], &key_out, 4);
                    if (args[3]) wg_blink_write_mem(engine->blink, args[3], &ovl_out, 4);
                    // Write success into OVERLAPPED.Internal = 0 (STATUS_SUCCESS)
                    if (ovl_out) {
                        uint32_t zero = 0;
                        wg_blink_write_mem(engine->blink, ovl_out, &zero, 4);
                        wg_blink_write_mem(engine->blink, ovl_out + 4, &bytes_out, 4);
                    }
                } else {
                    // OVERLAPPED_ENTRY: {lpCompletionKey, lpOverlapped, Internal, dwNumberOfBytesTransferred}
                    uint32_t entry_ptr = args[1];
                    if (entry_ptr) {
                        wg_blink_write_mem(engine->blink, entry_ptr,      &key_out,   4);
                        wg_blink_write_mem(engine->blink, entry_ptr + 4,  &ovl_out,   4);
                        wg_blink_write_mem(engine->blink, entry_ptr + 8,  &bytes_out, 4);
                        wg_blink_write_mem(engine->blink, entry_ptr + 12, &bytes_out, 4);
                    }
                    uint32_t one = 1;
                    if (args[3]) wg_blink_write_mem(engine->blink, args[3], &one, 4);
                }
                ret_val = 1; // TRUE
            } else if (timeout == 0) {
                // WAIT_TIMEOUT — no completions available
                s_last_error = 258; // WAIT_TIMEOUT
                ret_val = 0; // FALSE
            } else {
                // Block (yield) until a completion arrives or another thread posts one
                WGThread *cur = wg_sched_current(engine->scheduler);
                if (cur) { cur->wait_handle = WG_IOCP_HANDLE; cur->wait_timeout = timeout; }
                bool switched = wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_WAITING);
                WG_LOGI(TAG, "GQCS yield switched=%d", (int)switched);
                if (switched) return true;
                // No other threads — pretend timeout
                s_last_error = 258;
                ret_val = 0;
            }
        } else if (strcmp(fn, "PostQueuedCompletionStatus") == 0) {
            // PostQueuedCompletionStatus(iocp, bytes, key, overlapped)
            iocp_post(args[1], args[2], args[3]);
            wg_sched_wake(engine->scheduler, WG_IOCP_HANDLE);
            ret_val = 1;
        } else if (strcmp(fn, "CreateThreadpoolWork") == 0) {
            // CreateThreadpoolWork(pfnwk, pv, pcbe) -> PTP_WORK handle
            uint32_t cb  = args[0];
            uint32_t ctx = args[1];
            if (s_tp_work_count < WG_MAX_TP_WORK) {
                int idx = s_tp_work_count++;
                s_tp_work[idx] = (WGTpWork){cb, ctx, 0};
                ret_val = WG_TP_WORK_BASE + idx;
                WG_LOGI(TAG, "CreateThreadpoolWork(cb=0x%X ctx=0x%X) -> h=0x%X", cb, ctx, (uint32_t)ret_val);
            } else {
                ret_val = 0;
            }
        } else if (strcmp(fn, "SubmitThreadpoolWork") == 0) {
            // SubmitThreadpoolWork(pwk) — create a thread to run the callback
            uint32_t pwk = args[0];
            if (pwk >= WG_TP_WORK_BASE && pwk < WG_TP_WORK_BASE + s_tp_work_count) {
                int idx = (int)(pwk - WG_TP_WORK_BASE);
                uint32_t cb  = s_tp_work[idx].callback;
                uint32_t ctx = s_tp_work[idx].ctx;
                if (cb) {
                    uint32_t tid2 = 0;
                    // PTP_WORK_CALLBACK(PTP_CALLBACK_INSTANCE, ctx, PTP_WORK) — 3 args
                    // We pass ctx as the sole arg via a stub; the callback ignores inst/work.
                    uint32_t h2 = wg_sched_create_thread(engine->scheduler, engine->blink,
                                                          cb, ctx, 0, &tid2);
                    if (h2) {
                        s_tp_work[idx].thread_handle = h2;
                        WG_LOGI(TAG, "SubmitThreadpoolWork(h=0x%X cb=0x%X) -> thread tid=0x%X", pwk, cb, tid2);
                    } else {
                        WG_LOGI(TAG, "SubmitThreadpoolWork(h=0x%X) scheduler full — skipped", pwk);
                    }
                }
            }
            ret_val = 0; // void return
        } else if (strcmp(fn, "WaitOnAddress") == 0 && s_use_real_threads) {
            // WaitOnAddress(Address, CompareAddress, AddressSize, dwMilliseconds).
            // Block while *Address == *CompareAddress; TRUE when it changes (woken),
            // FALSE(0)+ERROR_TIMEOUT on timeout. UE4's parking lot depends on this.
            uint32_t r = wg_wait_on_address(engine, args[0], args[1], args[2], args[3]);
            if (!r) s_last_error = 1460; // ERROR_TIMEOUT
            ret_val = r;
        } else if ((strcmp(fn, "WakeByAddressSingle") == 0 ||
                    strcmp(fn, "WakeByAddressAll") == 0) && s_use_real_threads) {
            // Wake waiters on Address (broadcast-and-recheck covers single & all).
            wg_wake_by_address();
            ret_val = 0; // void
        } else if (strcmp(fn, "WaitForSingleObject") == 0 && s_use_real_threads) {
            // Real-threads: block THIS pthread on the wg_sync object. Release the
            // thunk lock around the block so other threads' SetEvent thunks run.
            uint32_t h = args[0], timeout = args[1];
            if (timeout == 0xFFFFFFFFu) wg_synctrace("WFSO-INF", s_cur_guest_tid, h, ret_addr); // skip finite polls (flood)
            if (getenv("WG_WAITLOG")) {   // log each distinct (tid,handle,INF) wait — deadlock diag
                static uint32_t s_wh[64], s_wt[64]; static int s_wn = 0;
                int seen=0; for(int i=0;i<s_wn;i++) if(s_wh[i]==h && s_wt[i]==s_cur_guest_tid){seen=1;break;}
                if(!seen && s_wn<64){ s_wh[s_wn]=h; s_wt[s_wn]=s_cur_guest_tid; s_wn++;
                    WG_LOGW(TAG,"[waitlog tid=0x%X] WFSO(h=0x%X timeout=0x%X) caller=0x%llX",
                            s_cur_guest_tid, h, timeout, (unsigned long long)ret_addr); }
            }
            if (wg_sync_is_known(h)) {
                // Directed handoff + directed kick: hand the GIL to h's signaler and,
                // if that producer is parked on its own wake-event, wake it NOW so the
                // dependency chain cascades forward instead of resolving one 2ms poll
                // at a time (the deep blocked-wait churn). Only for real INFINITE
                // waits (the ones that actually block / deadlock). WG_NO_DIRKICK off.
                uint32_t prod = wg_producer_get(h);
                wg_dir_set_prefer(prod);
                if (prod && timeout == 0xFFFFFFFFu && s_use_real_threads && !getenv("WG_NO_DIRKICK"))
                    wg_sync_kick_tid(prod);
                wg_thunk_block_begin();
                uint32_t wr = wg_sync_wait_single(h, wg_cap_timeout(timeout), s_cur_guest_tid);
                wg_thunk_block_end();
                ret_val = wr;   // WAIT_OBJECT_0(0) / WAIT_TIMEOUT(0x102) / WAIT_FAILED
            } else {
                // Unknown handle (e.g. the process pseudo-handle 0xFFFFFFFF, which
                // never signals). Don't hang: finite timeout -> sleep+TIMEOUT,
                // INFINITE -> WAIT_OBJECT_0 to avoid a permanent block.
                if (timeout == 0xFFFFFFFFu) { ret_val = 0; }
                else {
                    uint32_t ms = timeout > 50 ? 50 : timeout;
                    wg_thunk_block_begin(); usleep(wg_cap_timeout(ms) * 1000); wg_thunk_block_end();
                    ret_val = 258;
                }
            }
        } else if (strcmp(fn, "WaitForSingleObject") == 0) {
            uint32_t h = args[0];
            uint32_t timeout = args[1];
            // NULL-EVENT PROBE: the cooperative deadlock is a thread busy-polling
            // WFSO(h=0). Dump the wait object (rbx) + its fields ONCE so we can see
            // why the FEvent handle [rbx+0x18] is 0 (uncreated) vs a bad/zero param.
            if (h == 0 && getenv("WG_NULLPROBE")) {
                static int s_np = 0;
                if (s_np < 6) { s_np++;
                    uint64_t rbx = wg_blink_get_reg(engine->blink, 3);
                    uint32_t f08=0,f10=0,f18=0,f40=0;
                    wg_blink_read_mem(engine->blink, (uint32_t)rbx + 0x08, &f08, 4);
                    wg_blink_read_mem(engine->blink, (uint32_t)rbx + 0x10, &f10, 4);
                    wg_blink_read_mem(engine->blink, (uint32_t)rbx + 0x18, &f18, 4);
                    wg_blink_read_mem(engine->blink, (uint32_t)rbx + 0x40, &f40, 4);
                    WG_LOGW(TAG, "NULLPROBE tid=0x%X rbx=0x%llX [+8]=0x%X [+0x10]=0x%X [+0x18]=0x%X [+0x40]=0x%X caller=0x%llX",
                            engine->scheduler ? wg_sched_current_tid(engine->scheduler) : 0,
                            (unsigned long long)rbx, f08, f10, f18, f40, (unsigned long long)ret_addr);
                }
            }
            if (getenv("WG_WAITLOG")) {
                uint32_t _t = engine->scheduler ? wg_sched_current_tid(engine->scheduler) : 0;
                static uint32_t s_sh[8], s_st[8]; static int s_sn = 0;
                int _seen = 0; for (int _i=0;_i<s_sn;_i++) if (s_sh[_i]==h && s_st[_i]==_t) {_seen=1;break;}
                if (!_seen && s_sn < 8) {   // one line per distinct (tid,handle)
                    s_sh[s_sn]=h; s_st[s_sn]=_t; s_sn++;
                    WG_LOGW(TAG, "[waitlog tid=0x%X] WaitForSingleObject(h=0x%X, timeout=0x%X) caller=0x%llX",
                            _t, h, timeout, (unsigned long long)ret_addr);
                }
            }
            // Check if the handle is already signalled
            bool signalled = false;
            // NULL handle: the guest waits on h=0 (an FEvent whose creation didn't
            // store a handle) in a tight loop that only exits on WAIT_OBJECT_0, so it
            // spins forever (the 0x9FDA6F 62M-call stall in cooperative mode). A null
            // wait can't ever be signalled — treat it as already-signalled so the
            // thread proceeds instead of deadlocking. WG_NO_NULLWAIT disables.
            if (h == 0 && getenv("WG_NULLWAIT")) signalled = true;   // opt-in: proceed past null-handle waits
            else if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS)
                signalled = s_event_signalled[h - WG_EVENT_BASE];
            // Check if it's a thread handle that has exited
            WGThread *wt = wg_sched_find(engine->scheduler, h);
            if (wt && wt->state == WG_THREAD_EXITED) signalled = true;
            // Poll-loop dedup: workers alternate two handles (e.g. 0x20C/0x207) at
            // a fixed timeout, which defeats a plain consecutive-dedup. Log only
            // when the (handle,signalled) pair is new vs the last TWO seen, and
            // hard-cap repeats at 1/1024 so a steady poll can't flood the buffer.
            static uint32_t s_wfso_h1 = ~0u, s_wfso_h2 = ~0u; static int s_wfso_s1 = -1, s_wfso_s2 = -1;
            int si = (int)signalled;
            bool seen = (h == s_wfso_h1 && si == s_wfso_s1) || (h == s_wfso_h2 && si == s_wfso_s2);
            // Log only NEW (handle,signalled) transitions vs the last two seen — a
            // steady-state poll deadlock (e.g. 0x207/0xFFFFFFFF forever) then emits
            // nothing at all. The Sleep-spin watchdog still fires periodically to
            // show a loop is happening.
            if (!seen) {
                WG_LOGI(TAG, "WaitForSingleObject(h=0x%X, timeout=0x%X) signalled=%d thread_found=%d",
                        h, timeout, (int)signalled, (wt != NULL));
                s_wfso_h2 = s_wfso_h1; s_wfso_s2 = s_wfso_s1; s_wfso_h1 = h; s_wfso_s1 = si;
            }
            WGThread *cur = wg_sched_current(engine->scheduler);
            if (signalled) {
                wg_event_consume(h); // auto-reset events clear after a satisfied wait
                if (s_real_timeouts && cur) cur->wait_handle = 0; // reset timeout tracking
                ret_val = 0; // WAIT_OBJECT_0
            } else if (timeout == 0) {
                // Poll (WAIT_TIMEOUT). BUT a guest BUSY-POLL — WFSO(h, 0) in a tight
                // loop waiting for a WORKER to signal h — starves the worker if we
                // just return immediately (the poller never yields, so the signaller
                // never runs → infinite spin, e.g. the 62M-call stall at 0x9FDA6F).
                // If another guest thread is READY, YIELD to it first (it runs, maybe
                // signals h), THEN the poll returns WAIT_TIMEOUT when we resume — poll
                // semantics preserved, but the producer gets to run. WG_NO_POLLYIELD
                // disables.
                if (!getenv("WG_NO_POLLYIELD") && engine->scheduler &&
                    wg_sched_other_ready(engine->scheduler)) {
                    if (cur) { cur->wait_handle = h; cur->wait_timeout = 0; }
                    bool sw = wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY);
                    if (sw) return true;   // ran another thread; guest re-polls on resume
                }
                ret_val = 258; // WAIT_TIMEOUT
            } else {
                // Finite timeout → cooperative POLL with a REAL wall-clock deadline
                // (we have no timer interrupt, so we track when the wait began and
                // fire WAIT_TIMEOUT once that many ms have actually elapsed). INFINITE
                // → truly block (WAITING) until signalled. Real timeouts matter: a
                // finite wait on an event whose signaller has exited (or a wait on a
                // pseudo-handle like the process, which never signals) must eventually
                // return instead of yielding forever — that freeze is what stalled the
                // reactor test and Steam's post-resumption flow.
                bool timed_out = false;
                if (s_real_timeouts && timeout != 0xFFFFFFFFu && cur) {
                    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                    uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
                    if (cur->wait_handle != h || cur->wait_timeout != timeout)
                        cur->wait_start_ms = now;            // a new wait began
                    else if (now - cur->wait_start_ms >= (uint64_t)timeout)
                        timed_out = true;                    // deadline reached
                }
                if (timed_out) {
                    cur->wait_handle = 0;
                    ret_val = 258; // WAIT_TIMEOUT
                } else {
                    WGThreadState blk = (timeout == 0xFFFFFFFFu)
                                        ? WG_THREAD_WAITING : WG_THREAD_READY;
                    if (cur) {
                        cur->wait_handle = h;
                        cur->wait_timeout = timeout;
                    }
                    bool switched = wg_sched_yield(engine->scheduler, engine->blink, blk);
                    if (switched) {
                        return true; // switched to another thread
                    }
                    ret_val = (timeout == 0xFFFFFFFFu) ? 0 : 258; // alone: INFINITE→OBJ_0, else TIMEOUT
                }
            }
        } else if ((strcmp(fn, "WaitForMultipleObjects") == 0 ||
                    strcmp(fn, "WaitForMultipleObjectsEx") == 0) && s_use_real_threads) {
            // Real-threads WFMO over wg_sync objects.
            uint32_t ncount = args[0], hptr = args[1];
            bool wait_all = (args[2] != 0);
            uint32_t timeout = args[3];
            if (ncount > 64) ncount = 64;
            uint32_t handles[64] = {0};
            if (hptr && ncount) wg_blink_read_mem(engine->blink, hptr, handles, ncount * 4);
            // Only wait if all handles are wg_sync objects; otherwise fall back to
            // a short timeout so an unknown handle can't hang the thread.
            bool all_known = ncount > 0;
            for (uint32_t i = 0; i < ncount; i++) if (!wg_sync_is_known(handles[i])) { all_known = false; break; }
            if (all_known) {
                // Prefer the signaler of the first handle with a known producer
                // (wait_all needs all, but nudging one producer forward unblocks it).
                for (uint32_t i = 0; i < ncount; i++) { uint32_t p = wg_producer_get(handles[i]); if (p) { wg_dir_set_prefer(p); break; } }
                wg_thunk_block_begin();
                uint32_t wr = wg_sync_wait_multiple(handles, (int)ncount, wait_all, wg_cap_timeout(timeout), s_cur_guest_tid);
                wg_thunk_block_end();
                ret_val = wr;
            } else {
                uint32_t ms = timeout > 50 ? 50 : timeout;
                if (timeout == 0xFFFFFFFFu) ms = 50;
                wg_thunk_block_begin(); usleep(wg_cap_timeout(ms) * 1000); wg_thunk_block_end();
                ret_val = 258;
            }
        } else if (strcmp(fn, "WaitForMultipleObjects") == 0 ||
                   strcmp(fn, "WaitForMultipleObjectsEx") == 0 ||
                   strcmp(fn, "MsgWaitForMultipleObjects") == 0 ||
                   strcmp(fn, "MsgWaitForMultipleObjectsEx") == 0) {
            // args: nCount, lpHandles, bWaitAll, dwMilliseconds [, dwWakeMask [, dwFlags]]
            uint32_t ncount   = args[0];
            uint32_t hptr     = args[1];
            bool     wait_all = (args[2] != 0);
            uint32_t timeout  = args[3];
            if (ncount > 16) ncount = 16;
            uint32_t handles[16] = {0};
            if (hptr) wg_blink_read_mem(engine->blink, hptr, handles, ncount * 4);

            int first_signalled = -1;
            int nsig = 0;
            for (uint32_t idx = 0; idx < ncount; idx++) {
                uint32_t h = handles[idx];
                bool sig = false;
                if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS)
                    sig = s_event_signalled[h - WG_EVENT_BASE];
                WGThread *wt2 = wg_sched_find(engine->scheduler, h);
                if (wt2 && wt2->state == WG_THREAD_EXITED) sig = true;
                if (sig) { nsig++; if (first_signalled < 0) first_signalled = (int)idx; }
            }

            if (ncount == 0) {
                // MsgWait with no handles = pure message wait; pretend message arrived
                ret_val = 0;
            } else {
                bool all_done = wait_all ? (nsig == (int)ncount) : (nsig > 0);
                WG_LOGI(TAG, "%s(n=%u, waitAll=%d, timeout=0x%X) nsig=%d all_done=%d",
                        fn, ncount, (int)wait_all, timeout, nsig, (int)all_done);
                if (all_done) {
                    // Consume auto-reset events that satisfied the wait.
                    if (wait_all) {
                        for (uint32_t idx = 0; idx < ncount; idx++) wg_event_consume(handles[idx]);
                    } else if (first_signalled >= 0) {
                        wg_event_consume(handles[first_signalled]);
                    }
                    ret_val = (first_signalled >= 0) ? (uint32_t)first_signalled : 0;
                } else if (timeout == 0) {
                    ret_val = 258; // WAIT_TIMEOUT
                } else {
                    WGThreadState blk = (timeout == 0xFFFFFFFFu)
                                        ? WG_THREAD_WAITING : WG_THREAD_READY;
                    WGThread *cur = wg_sched_current(engine->scheduler);
                    if (cur) { cur->wait_handle = handles[0]; cur->wait_timeout = timeout; }
                    bool switched = wg_sched_yield(engine->scheduler, engine->blink, blk);
                    if (switched) return true;
                    ret_val = (timeout == 0xFFFFFFFFu) ? 0 : 258;
                }
            }
        } else if (strcmp(fn, "CreateMutexW") == 0 ||
                   strcmp(fn, "CreateMutexA") == 0) {
            // CreateMutex(secAttr, bInitialOwner, lpName)
            if (s_use_real_threads) {
                ret_val = wg_sync_create_mutex(args[1] != 0, s_cur_guest_tid);
            } else {
            // Return a unique fake handle. Steam uses mutexes for single-instance.
            if (s_event_next < WG_MAX_EVENTS) {
                ret_val = WG_EVENT_BASE + s_event_next++;
            }
            }
            s_last_error = 0; // not ERROR_ALREADY_EXISTS
        } else if (strcmp(fn, "OpenMutexW") == 0 ||
                   strcmp(fn, "OpenMutexA") == 0) {
            ret_val = 0; // mutex not found
            s_last_error = 2; // ERROR_FILE_NOT_FOUND
        } else if (strcmp(fn, "ReleaseMutex") == 0) {
            if (s_use_real_threads) ret_val = wg_sync_release_mutex(args[0], s_cur_guest_tid) ? 1 : 0;
            else ret_val = 1;
        } else if (strcmp(fn, "CreateThread") == 0) {
            // CreateThread(secAttr, stackSize, start=args[2], param=args[3],
            //              flags=args[4], lpThreadId=args[5]).
            uint32_t start = args[2], param = args[3], flags = args[4];
            uint32_t tid = 0;
            uint32_t hthread = 0;
            bool ct_real = false;
            if (s_use_real_threads) {
                hthread = wg_spawn_real_thread(engine, start, param, flags, &tid);
                if (hthread) ct_real = true;   // real pthread spawned; skip cooperative
            }
            if (!ct_real) {
                hthread = wg_sched_create_thread(
                    engine->scheduler, engine->blink,
                    start, param, flags, &tid);
            }
            if (args[5] && tid) {
                wg_blink_write_mem(engine->blink, args[5], &tid, 4);
            }
            if (!ct_real) {
                if (hthread) {
                    // Give the new thread its own TEB (stack bounds, ClientId, TLS).
                    WGThread *nt = wg_sched_find(engine->scheduler, hthread);
                    if (nt) {
                        uint32_t teb = wg_alloc_thread_teb(engine,
                            nt->stack_base + nt->stack_size, nt->stack_base, tid);
                        if (teb) { nt->teb = teb; nt->regs.fs_base = teb; }
                    }
                } else {
                    // Fallback: run synchronously (for NSIS compatibility)
                    hthread = 0x7100;
                    uint64_t clean_rsp = rsp + ptr_size + (6 * ptr_size);
                    if (start && is_32bit && !(flags & 0x4u)) {
                        wg_call_wndproc_ovr(engine, start, param, 0, 0, 0,
                                            (uint32_t)ret_addr, (uint32_t)clean_rsp,
                                            true, hthread);
                        WG_LOGI(TAG, "CreateThread: fallback sync start=0x%X", start);
                        return true;
                    }
                }
            }
            ret_val = hthread;
        } else if (strcmp(fn, "_beginthreadex") == 0) {
            // uintptr_t _beginthreadex(security, stack, start=args[2], arg=args[3],
            //                          initflag=args[4], thrdaddr=args[5])  __cdecl.
            // MSVC CRT thread spawn. Must create a REAL cooperative thread or the
            // app's worker (e.g. Steam's network/download thread) never runs and
            // the main thread polls forever. start is __stdcall, like CreateThread.
            uint32_t start = args[2], param = args[3], flags = args[4];
            uint32_t tid = 0;
            uint32_t hthread = wg_sched_create_thread(
                engine->scheduler, engine->blink, start, param, flags, &tid);
            if (args[5] && tid)
                wg_blink_write_mem(engine->blink, args[5], &tid, 4);
            if (hthread) {
                WGThread *nt = wg_sched_find(engine->scheduler, hthread);
                if (nt) {
                    uint32_t teb = wg_alloc_thread_teb(engine,
                        nt->stack_base + nt->stack_size, nt->stack_base, tid);
                    if (teb) { nt->teb = teb; nt->regs.fs_base = teb; }
                }
            }
            WG_LOGI(TAG, "_beginthreadex(start=0x%X param=0x%X flags=0x%X) -> h=0x%X tid=0x%X",
                    start, param, flags, hthread, tid);
            ret_val = hthread; // 0 on failure (CRT maps to errno)
        } else if (strcmp(fn, "_beginthread") == 0) {
            // uintptr_t _beginthread(start=args[0], stack=args[1], arg=args[2]) __cdecl.
            // Older CRT spawn; start is __cdecl but returns to RIP=0 sentinel either way.
            uint32_t start = args[0], param = args[2];
            uint32_t tid = 0;
            uint32_t hthread = wg_sched_create_thread(
                engine->scheduler, engine->blink, start, param, 0, &tid);
            if (hthread) {
                WGThread *nt = wg_sched_find(engine->scheduler, hthread);
                if (nt) {
                    uint32_t teb = wg_alloc_thread_teb(engine,
                        nt->stack_base + nt->stack_size, nt->stack_base, tid);
                    if (teb) { nt->teb = teb; nt->regs.fs_base = teb; }
                }
            }
            WG_LOGI(TAG, "_beginthread(start=0x%X param=0x%X) -> h=0x%X tid=0x%X",
                    start, param, hthread, tid);
            ret_val = hthread;
        } else if (strcmp(fn, "QueueUserWorkItem") == 0) {
            // QueueUserWorkItem(Function, Context, Flags)
            // Create a cooperative thread for the callback — same signature as CreateThread
            uint32_t func  = args[0];
            uint32_t ctx   = args[1];
            if (func) {
                uint32_t tid2 = 0;
                uint32_t h2 = wg_sched_create_thread(engine->scheduler, engine->blink,
                                                      func, ctx, 0, &tid2);
                if (h2) {
                    WG_LOGI(TAG, "QueueUserWorkItem(fn=0x%X, ctx=0x%X) -> thread h=0x%X tid=0x%X",
                            func, ctx, h2, tid2);
                    ret_val = 1; // TRUE
                } else {
                    // Fallback: run synchronously
                    WG_LOGI(TAG, "QueueUserWorkItem(fn=0x%X, ctx=0x%X) -> sync", func, ctx);
                    uint64_t clean_rsp = rsp + ptr_size + (3 * ptr_size);
                    if (is_32bit) {
                        wg_call_wndproc_ovr(engine, func, ctx, 0, 0, 0,
                                            (uint32_t)ret_addr, (uint32_t)clean_rsp,
                                            true, 0x7100);
                        return true;
                    }
                    ret_val = 1;
                }
            } else {
                ret_val = 0;
            }
        } else if (strcmp(fn, "WaitForSingleObjectEx") == 0 && s_use_real_threads) {
            uint32_t h = args[0], timeout = args[1];
            if (wg_sync_is_known(h)) {
                wg_thunk_block_begin();
                ret_val = wg_sync_wait_single(h, wg_cap_timeout(timeout), s_cur_guest_tid);
                wg_thunk_block_end();
            } else if (timeout == 0xFFFFFFFFu) { ret_val = 0; }
            else {
                uint32_t ms = timeout > 50 ? 50 : timeout;
                wg_thunk_block_begin(); usleep(wg_cap_timeout(ms) * 1000); wg_thunk_block_end();
                ret_val = 258;
            }
        } else if (strcmp(fn, "WaitForSingleObjectEx") == 0) {
            // WaitForSingleObjectEx(hObject, dwMilliseconds, bAlertable) — treat same as WaitForSingleObject
            uint32_t h = args[0];
            uint32_t timeout = args[1];
            bool signalled = false;
            if (h >= WG_EVENT_BASE && h < WG_EVENT_BASE + WG_MAX_EVENTS)
                signalled = s_event_signalled[h - WG_EVENT_BASE];
            WGThread *wt_ex = wg_sched_find(engine->scheduler, h);
            if (wt_ex && wt_ex->state == WG_THREAD_EXITED) signalled = true;
            WG_LOGI(TAG, "WaitForSingleObjectEx(h=0x%X, timeout=0x%X) signalled=%d",
                    h, timeout, (int)signalled);
            if (signalled) {
                wg_event_consume(h);
                ret_val = 0;
            } else if (timeout == 0) {
                ret_val = 258;
            } else {
                WGThreadState blk = (timeout == 0xFFFFFFFFu)
                                    ? WG_THREAD_WAITING : WG_THREAD_READY;
                WGThread *cur_ex = wg_sched_current(engine->scheduler);
                if (cur_ex) { cur_ex->wait_handle = h; cur_ex->wait_timeout = timeout; }
                bool sw = wg_sched_yield(engine->scheduler, engine->blink, blk);
                if (sw) return true;
                ret_val = (timeout == 0xFFFFFFFFu) ? 0 : 258;
            }
        } else if (s_use_real_threads && strcmp(fn, "AcquireSRWLockShared") == 0) {
            if (s_srw_log < 0) { const char *e = getenv("WG_SRW_LOG"); s_srw_log = e ? atoi(e) : 0; }
            wg_srw_acquire_shared(args[0], s_cur_guest_tid);
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "AcquireSRWLockExclusive") == 0) {
            if (s_srw_log < 0) { const char *e = getenv("WG_SRW_LOG"); s_srw_log = e ? atoi(e) : 0; }
            wg_srw_acquire_exclusive(args[0], s_cur_guest_tid);
            s_cs_held++;   // GIL-pin: exclusive lock holds the construction atomic
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "ReleaseSRWLockShared") == 0) {
            wg_srw_release_shared(args[0]);
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "ReleaseSRWLockExclusive") == 0) {
            wg_srw_release_exclusive(args[0], s_cur_guest_tid);
            if (s_cs_held > 0) s_cs_held--;   // release the GIL-pin
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "TryAcquireSRWLockShared") == 0) {
            ret_val = wg_srw_try_shared(args[0], s_cur_guest_tid);
        } else if (s_use_real_threads && strcmp(fn, "TryAcquireSRWLockExclusive") == 0) {
            ret_val = wg_srw_try_exclusive(args[0], s_cur_guest_tid);
            if (ret_val) s_cs_held++;   // GIL-pin on successful exclusive acquire
        } else if (s_use_real_threads && strcmp(fn, "InitializeSRWLock") == 0) {
            ret_val = 0;   // RW-lock state lazily created on first Acquire
        } else if (s_use_real_threads && (strcmp(fn, "_Mtx_lock") == 0 ||
                   strcmp(fn, "mtx_lock") == 0)) {
            // MSVC C++ std::mutex / C11 mtx_lock — real lock (was a no-op auto-stub,
            // so std::mutex-protected data raced across GIL-release blocking points ->
            // the FMallocBinned2/vtable UAF corruption under real-threads+JIT).
            uint32_t mtx = wg_cs_mutex_for(args[0]);
            if (mtx && wg_sync_wait_single(mtx, 0, s_cur_guest_tid) != WG_WAIT_OBJECT_0) { wg_thunk_block_begin(); wg_sync_wait_single(mtx, WG_SYNC_INFINITE, s_cur_guest_tid); wg_thunk_block_end(); }
            s_cs_held++;   // GIL-pin: std::mutex-protected region atomic vs other guest threads
            ret_val = 0;   // _Thrd_success
        } else if (s_use_real_threads && (strcmp(fn, "_Mtx_unlock") == 0 ||
                   strcmp(fn, "mtx_unlock") == 0)) {
            uint32_t mtx = wg_cs_mutex_for(args[0]);
            if (mtx) wg_sync_release_mutex(mtx, s_cur_guest_tid);
            if (s_cs_held > 0) s_cs_held--;   // release the GIL-pin
            ret_val = 0;
        } else if (s_use_real_threads && (strcmp(fn, "_Mtx_trylock") == 0 ||
                   strcmp(fn, "mtx_trylock") == 0)) {
            uint32_t mtx = wg_cs_mutex_for(args[0]);
            uint32_t r = mtx ? wg_sync_wait_single(mtx, 0, s_cur_guest_tid) : WG_WAIT_TIMEOUT;
            if (r == WG_WAIT_OBJECT_0) s_cs_held++;   // GIL-pin on successful acquire
            ret_val = (r == WG_WAIT_OBJECT_0) ? 0 : 3;   // _Thrd_success / _Thrd_busy
        } else if (s_use_real_threads && (strcmp(fn, "_Mtx_init") == 0 ||
                   strcmp(fn, "_Mtx_destroy") == 0 || strcmp(fn, "mtx_init") == 0 ||
                   strcmp(fn, "mtx_destroy") == 0)) {
            ret_val = 0;   // wg_sync mutex lazily created on first _Mtx_lock
        } else if (s_use_real_threads && strcmp(fn, "_Cnd_wait") == 0) {
            // std::condition_variable::wait(unique_lock&) -> _Cnd_wait(cnd, mtx).
            // Atomically release the mutex, block on the CV, re-acquire on wake.
            // Was a no-op auto-stub: the consumer NEVER waited for the producer's
            // _Cnd_signal, so it used shared state before construction -> the
            // garbage-vtable UAF the JIT's speed exposed (interp dodged by timing).
            uint32_t cvh = wg_cv_handle_for(args[0]);
            uint32_t csm = wg_cs_mutex_for(args[1]);   // same _Mtx_t the unique_lock holds
            wg_thunk_block_begin();
            wg_sync_cv_sleep(cvh, csm, WG_SYNC_INFINITE, s_cur_guest_tid);
            wg_thunk_block_end();
            ret_val = 0;   // _Thrd_success
        } else if (s_use_real_threads && strcmp(fn, "_Cnd_timedwait") == 0) {
            // _Cnd_timedwait(cnd, mtx, const xtime* xt). xt is an ABSOLUTE deadline
            // {int64 sec; long nsec}. Convert to a relative ms, clamp so a stale
            // deadline can't hang, then wait.
            uint32_t cvh = wg_cv_handle_for(args[0]);
            uint32_t csm = wg_cs_mutex_for(args[1]);
            uint32_t ms = WG_SYNC_INFINITE;
            if (args[2]) {
                struct { int64_t sec; int32_t nsec; } xt = {0, 0};
                wg_blink_read_mem(engine->blink, args[2], &xt, 12);
                struct timeval now; gettimeofday(&now, NULL);
                int64_t target_ms = xt.sec * 1000 + xt.nsec / 1000000;
                int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
                int64_t delta = target_ms - now_ms;
                if (delta < 0) delta = 0;
                if (delta > 60000) delta = 60000;
                ms = (uint32_t)delta;
            }
            wg_thunk_block_begin();
            uint32_t r = wg_sync_cv_sleep(cvh, csm, wg_cap_timeout(ms), s_cur_guest_tid);
            wg_thunk_block_end();
            ret_val = (r == WG_WAIT_OBJECT_0) ? 0 : 3;   // _Thrd_success / _Thrd_timedout
        } else if (s_use_real_threads && strcmp(fn, "_Cnd_signal") == 0) {
            wg_sync_cv_wake(wg_cv_handle_for(args[0]), false);
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "_Cnd_broadcast") == 0) {
            wg_sync_cv_wake(wg_cv_handle_for(args[0]), true);
            ret_val = 0;
        } else if (s_use_real_threads && strcmp(fn, "_Cnd_init") == 0) {
            // int _Cnd_init(_Cnd_t* cnd) — give *cnd a stable, unique key (itself) so
            // later _Cnd_wait/_Cnd_signal(*cnd) map to one consistent wg_sync CV. The
            // guest only ever passes _Cnd_t back to _Cnd_* (all our handlers), so it
            // may be any opaque stable value.
            if (args[0]) { uint64_t key = args[0]; wg_blink_write_mem(engine->blink, args[0], &key, 8); }
            ret_val = 0;
        } else if (s_use_real_threads && (strcmp(fn, "_Cnd_init_in_situ") == 0 ||
                   strcmp(fn, "_Cnd_destroy_in_situ") == 0 || strcmp(fn, "_Cnd_destroy") == 0)) {
            ret_val = 0;   // CV lazily created on first _Cnd_wait/_Cnd_signal
        } else if (strcmp(fn, "_Thrd_yield") == 0) {
            if (s_use_real_threads) sched_yield();
            ret_val = 0;
        } else if (strcmp(fn, "_Thrd_id") == 0) {
            ret_val = s_cur_guest_tid;   // std::this_thread::get_id() identity
        } else if (strcmp(fn, "InitializeConditionVariable") == 0) {
            if (s_use_real_threads) { wg_cv_handle_for(args[0]); ret_val = 0; }
            else {
            uint32_t *g = cv_slot(args[0]);
            if (g) *g = 0;
            ret_val = 0;
            }
        } else if ((strcmp(fn, "WakeConditionVariable") == 0 ||
                    strcmp(fn, "WakeAllConditionVariable") == 0) && s_use_real_threads) {
            wg_sync_cv_wake(wg_cv_handle_for(args[0]), strcmp(fn, "WakeAllConditionVariable") == 0);
            ret_val = 0;
        } else if (strcmp(fn, "WakeConditionVariable") == 0 ||
                   strcmp(fn, "WakeAllConditionVariable") == 0) {
            // Signal the CV: advance its generation (so parked sleepers see the
            // change) and mark any WAITING waiters runnable.
            uint32_t cv = args[0];
            uint32_t *g = cv_slot(cv);
            if (g) (*g)++;
            wg_sched_wake(engine->scheduler, cv);
            ret_val = 0;
        } else if ((strcmp(fn, "SleepConditionVariableCS") == 0 ||
                    strcmp(fn, "SleepConditionVariableSRW") == 0) && s_use_real_threads) {
            // Real-threads: atomically release the CS/SRW mutex, block on the CV,
            // re-acquire. Returns TRUE if woken, FALSE (0) on timeout (Steam
            // re-checks its work-queue predicate either way).
            uint32_t cvh = wg_cv_handle_for(args[0]);
            uint32_t csm = wg_cs_mutex_for(args[1]);   // CS or SRW pointer -> its mutex
            uint32_t ms  = args[2];
            wg_thunk_block_begin();
            uint32_t r = wg_sync_cv_sleep(cvh, csm, wg_cap_timeout(ms), s_cur_guest_tid);
            wg_thunk_block_end();
            ret_val = (r == WG_WAIT_OBJECT_0) ? 1 : 0;
        } else if (strcmp(fn, "SleepConditionVariableCS") == 0 ||
                   strcmp(fn, "SleepConditionVariableSRW") == 0) {
            // SleepConditionVariableCS(cv, cs, dwMs[, flags]). The old stub returned
            // TRUE immediately (never blocking), so Steam's thread-pool workers never
            // parked and queued work (the manifest send) was never dispatched. Real
            // behavior: release the lock (we have no real locks, so nothing to do),
            // block until the CV is woken or the timeout elapses, re-acquire. We
            // busy-park (yield, stay runnable) and detect a wake via the CV
            // generation. Always report TRUE — a spurious wakeup is always legal for
            // a CV, so we never surface a timeout as an error; Steam re-checks its
            // work-queue predicate after every return. Device-gated: the macOS
            // harness keeps the legacy immediate-return stub (its timing already
            // gets Steam through and real CV blocking reshuffles it).
            uint32_t cv = args[0];
            uint32_t ms = args[2];
            if (!s_real_timeouts) {
                ret_val = 1; // macOS harness: legacy stub
            } else {
                WGThread *cur = wg_sched_current(engine->scheduler);
                uint32_t *g = cv_slot(cv);
                if (cur && cur->wait_handle == cv) {
                    // Continuation of a park on this CV.
                    bool woken = (g && *g != cur->wait_cv_gen);
                    bool timed_out = false;
                    if (!woken && ms != 0xFFFFFFFFu) {
                        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                        uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
                        if (now - cur->wait_start_ms >= (uint64_t)ms) timed_out = true;
                    }
                    if (woken || timed_out) {
                        cur->wait_handle = 0;
                        ret_val = 1; // woken (or timed out — report as spurious wake)
                    } else {
                        bool sw = wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY);
                        if (sw) return true;
                        ret_val = 1; // alone: report a spurious wake so we never hard-hang
                    }
                } else {
                    // First entry: begin a park on this CV.
                    if (cur) {
                        cur->wait_handle = cv;
                        cur->wait_cv_gen = g ? *g : 0;
                        cur->wait_timeout = ms;
                        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                        cur->wait_start_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
                    }
                    bool sw = wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY);
                    if (sw) return true;
                    ret_val = 1;
                }
            }
        } else if (strcmp(fn, "ResumeThread") == 0) {
            uint32_t h = args[0];
            wg_synctrace("ResumeThr", s_cur_guest_tid, h, ret_addr);
            WGThread *wt = wg_sched_find(engine->scheduler, h);
            if (wt && wt->state == WG_THREAD_SUSPENDED) {
                wt->state = WG_THREAD_READY;
                WG_LOGI(TAG, "ResumeThread: h=0x%X id=0x%X now READY", h, wt->id);
                ret_val = 1; // previous suspend count
            } else if (s_use_real_threads && wg_resume_gate_signal(h)) {
                WG_LOGI(TAG, "ResumeThread: h=0x%X real thread released", h);
                ret_val = 1;
                // Let the just-resumed worker RUN before the resumer continues.
                // UE4's FRunnableThreadWin setup reads the worker-CONSTRUCTED
                // FRunnable vtable ([rdi+0x20], guest 0x9eb16a) IMMEDIATELY after
                // ResumeThread. Under the GIL the resumer keeps the lock and reads
                // it BEFORE the worker constructs it (guest 0x9eb4bf) -> it reads
                // the stale {thread-id, handle} pair as a vtable (0x0000710900001029)
                // -> the type-confusion bad call the JIT can't recover from. Windows'
                // preemptive scheduler lets the worker construct first; emulate that
                // by releasing the GIL here so the worker makes progress before we
                // return into the read. Default ON for real threads (correctness);
                // WG_NO_RESUME_YIELD disables.
                if (!getenv("WG_NO_RESUME_YIELD")) {
                    // Light touch: just enough GIL-release for the worker to run
                    // its FRunnable constructor; too long disrupts the pool
                    // handshake (over-delayed workers -> a producer-consumer stall
                    // at 0x815879). One yield + a short sleep, tunable via WG_RY_US.
                    static int ry_us = -1;
                    if (ry_us < 0) { const char *e = getenv("WG_RY_US"); ry_us = e ? atoi(e) : 200; }
                    wg_thunk_block_begin();
                    sched_yield();
                    if (ry_us > 0) usleep((useconds_t)ry_us);
                    wg_thunk_block_end();
                }
            } else {
                ret_val = (uint32_t)-1; // error if not found
            }
        } else if (strcmp(fn, "GetExitCodeThread") == 0) {
            uint32_t h = args[0];
            WGThread *wt = wg_sched_find(engine->scheduler, h);
            if (wt) {
                uint32_t code = (wt->state == WG_THREAD_EXITED) ? wt->exit_code : 259 /*STILL_ACTIVE*/;
                if (args[1]) wg_blink_write_mem(engine->blink, args[1], &code, 4);
            } else {
                // thread ran synchronously — report done
                if (args[1]) { uint32_t code = 0;
                    wg_blink_write_mem(engine->blink, args[1], &code, 4); }
            }
            ret_val = 1;
        } else if (strcmp(fn, "CreateProcessW") == 0 ||
                   strcmp(fn, "CreateProcessA") == 0) {
            // We can't run a child .exe (steam.exe), but the install section's
            // final step launches it. Report SUCCESS with a fake, already-exited
            // process so any Exec/ExecWait/nsExec path completes immediately and
            // the install worker RETURNS (otherwise the single guest thread stays
            // inside the worker and the UI never pumps → buttons dead).
            uint32_t pi = args[9];   // lpProcessInformation
            if (pi) {
                uint32_t info[4] = { 0x00007200 /*hProcess*/, 0x00007201 /*hThread*/,
                                     0x00001200 /*pid*/, 0x00001201 /*tid*/ };
                wg_blink_write_mem(engine->blink, pi, info, sizeof(info));
            }
            ret_val = 1; // TRUE
            // If the installer is launching the Steam bootstrapper (steam.exe,
            // NOT steamservice.exe), remember it so the app can chain-load and
            // run it once the installer exits — that's the "fancier" Steam UI.
            {
                char cl[512] = {0};
                uint32_t cl_ptr = args[1] ? args[1] : args[0]; // cmdline or appname
                if (cl_ptr) {
                    uint16_t w[512] = {0};
                    wg_blink_read_mem(engine->blink, cl_ptr, w, 1022);
                    for (int i = 0; i < 511 && w[i]; i++)
                        cl[i] = w[i] < 128 ? (char)w[i] : '?';
                }
                // lowercase copy for matching
                char low[512]; int li = 0;
                for (; cl[li] && li < 511; li++)
                    low[li] = (char)tolower((unsigned char)cl[li]);
                low[li] = 0;
                if (strstr(low, "steam.exe") && !strstr(low, "steamservice")) {
                    // Extract the .exe path (strip a leading quote; stop at the
                    // closing quote or the space before args), then map to real.
                    char win[512]; int wi = 0; const char *p = cl;
                    if (*p == '"') p++;
                    while (*p && *p != '"' && wi < 511) {
                        // stop at " .exe" boundary + following space
                        win[wi++] = *p;
                        if (wi >= 9 && strncasecmp(win + wi - 9, "steam.exe", 9) == 0) break;
                        p++;
                    }
                    win[wi] = 0;
                    char mapbuf[512];
                    strncpy(mapbuf, win, sizeof(mapbuf) - 1); mapbuf[sizeof(mapbuf)-1] = 0;
                    const char *real = wg_files_map_path(0, engine->blink, mapbuf, sizeof(mapbuf));
                    if (real) {
                        strncpy(s_pending_exec, real, sizeof(s_pending_exec) - 1);
                        s_pending_exec[sizeof(s_pending_exec)-1] = 0;
                        WG_LOGI(TAG, "Steam bootstrapper launch queued: %s", s_pending_exec);
                    }
                } else if (engine->pe_image && engine->pe_image->is_64bit &&
                           strstr(low, ".exe")) {
                    // 64-bit guests (e.g. the Visage/UE4 launcher) spawn their
                    // real game binary. Queue any child .exe that exists in the
                    // bottle for chain-loading once the launcher exits.
                    char win[512]; int wi = 0; const char *p = cl;
                    if (*p == '"') p++;
                    while (*p && *p != '"' && wi < 511) {
                        win[wi++] = *p;
                        if (wi >= 4 && strncasecmp(win + wi - 4, ".exe", 4) == 0) break;
                        p++;
                    }
                    win[wi] = 0;
                    char mapbuf[512];
                    strncpy(mapbuf, win, sizeof(mapbuf) - 1); mapbuf[sizeof(mapbuf)-1] = 0;
                    const char *real = wg_files_map_path(0, engine->blink, mapbuf, sizeof(mapbuf));
                    struct stat cst;
                    if (real && stat(real, &cst) == 0) {
                        strncpy(s_pending_exec, real, sizeof(s_pending_exec) - 1);
                        s_pending_exec[sizeof(s_pending_exec)-1] = 0;
                        WG_LOGI(TAG, "Child exe launch queued: %s", s_pending_exec);
                    } else {
                        WG_LOGW(TAG, "CreateProcessW: child exe not found: '%s'", win);
                    }
                }
            }
        } else if (strcmp(fn, "CreatePipe") == 0) {
            // CreatePipe(hReadPipe=args[0], hWritePipe=args[1], attrs, size).
            // Succeed with fake handles so nsExec's pipe loop is well-formed and
            // (with PeekNamedPipe=no-data + GetExitCodeProcess=done) exits at once.
            uint32_t hr = 0x00007300, hw = 0x00007301;
            if (args[0]) wg_blink_write_mem(engine->blink, args[0], &hr, 4);
            if (args[1]) wg_blink_write_mem(engine->blink, args[1], &hw, 4);
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "GetExitCodeProcess") == 0) {
            // The (fake) child "exited" with code 0 — NOT STILL_ACTIVE — so
            // nsExec's "while child running" pipe-read loop terminates instead
            // of spinning forever (which froze the installer after launching
            // steam.exe and made Back/Next/Cancel unresponsive).
            if (args[1]) { uint32_t code = 0;
                wg_blink_write_mem(engine->blink, args[1], &code, 4); }
            ret_val = 1;
        } else if (strcmp(fn, "PeekNamedPipe") == 0) {
            // Report no data available (and success) so nsExec sees "no output,
            // child done" and stops reading. args[3]=lpBytesRead,
            // args[4]=lpTotalBytesAvail, args[5]=lpBytesLeftThisMessage.
            uint32_t zero = 0;
            if (args[3]) wg_blink_write_mem(engine->blink, args[3], &zero, 4);
            if (args[4]) wg_blink_write_mem(engine->blink, args[4], &zero, 4);
            if (args[5]) wg_blink_write_mem(engine->blink, args[5], &zero, 4);
            ret_val = 1;
        } else if (strcmp(fn, "SHGetFolderPathW") == 0) {
            // SHGetFolderPathW(hwnd, csidl=args[1], hToken, dwFlags, pszPath=args[4]).
            // MUST write a valid path; otherwise NSIS reuses a stale buffer as
            // the Start-Menu/Desktop folder ("Create folder: Error creating
            // shortcut\Steam"). Map common CSIDLs into the bottle.
            int csidl = (int)(args[1] & 0xFF);
            const char *path;
            switch (csidl) {
                case 0x00: case 0x10: path = "C:\\users\\steamuser\\Desktop"; break;
                case 0x02:            path = "C:\\users\\steamuser\\Start Menu\\Programs"; break;
                case 0x0B:            path = "C:\\users\\steamuser\\Start Menu"; break;
                case 0x17:            path = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"; break;
                case 0x05:            path = "C:\\users\\steamuser\\Documents"; break;
                case 0x1A: case 0x23: path = "C:\\users\\steamuser\\AppData\\Roaming"; break;
                case 0x1C:            path = "C:\\users\\steamuser\\AppData\\Local"; break;
                case 0x24:            path = "C:\\Windows"; break;
                case 0x25:            path = "C:\\Windows\\System32"; break;
                case 0x26:            path = "C:\\Program Files"; break;
                case 0x2A:            path = "C:\\Program Files (x86)"; break; // CSIDL_PROGRAM_FILESX86
                default:              path = "C:\\users\\steamuser"; break;
            }
            if (args[4]) {
                uint16_t w[260]; int i = 0;
                for (; path[i] && i < 259; i++) w[i] = (uint8_t)path[i];
                w[i] = 0;
                wg_blink_write_mem(engine->blink, args[4], w, (i + 1) * 2);
            }
            ret_val = 0; // S_OK
        } else if (strcmp(fn, "CoCreateInstance") == 0) {
            // CoCreateInstance(rclsid=args[0], pUnkOuter, dwClsContext, riid, ppv=args[4]).
            // NSIS CreateShortcut wants IShellLink {00021401-0000-0000-C000-...46};
            // hand back our minimal fake for THAT CLSID only. For any OTHER CLSID
            // (e.g. Visage's WMI/WbemLocator hardware probe: CoInit -> CoCreateInstance
            // -> vtbl call -> SysAllocString), we must NOT hand back the IShellLink
            // stand-in — the caller invokes a WMI method at vtbl+0x30 that lands in
            // garbage and jumps into the heap (the real-threads+JIT crash). Fail
            // cleanly instead: REGDB_E_CLASSNOTREG + NULL *ppv, so the caller's
            // standard `if (FAILED(hr) || !obj) skip;` guard skips the vtable call.
            // Also write the FULL pointer width (was a 4-byte write into a 64-bit
            // ppv slot, leaving garbage high bits).
            static const uint8_t kCLSID_ShellLink[16] = {
                0x01,0x14,0x02,0x00, 0x00,0x00, 0x00,0x00,
                0xC0,0x00,0x00,0x00, 0x00,0x00,0x00,0x46 };
            uint8_t clsid[16] = {0};
            if (args[0]) wg_blink_read_mem(engine->blink, args[0], clsid, 16);
            if (memcmp(clsid, kCLSID_ShellLink, 16) == 0) {
                wg_build_fake_com(engine);
                uint64_t p = s_com_shelllink;
                if (args[4]) wg_blink_write_mem(engine->blink, args[4], &p, ptr_size);
                ret_val = s_com_shelllink ? 0 : 0x80040154;
            } else {
                uint64_t zero = 0;
                if (args[4]) wg_blink_write_mem(engine->blink, args[4], &zero, ptr_size);
                ret_val = 0x80040154; // REGDB_E_CLASSNOTREG (negative HRESULT)
            }
            s_last_error = 0;
        } else if (strcmp(fn, "__comQI") == 0) {
            // IShellLink/IPersistFile::QueryInterface(this, riid, ppv) — hand
            // back the IPersistFile object (NSIS QIs the link for it before Save).
            if (args[2]) wg_blink_write_mem(engine->blink, args[2], &s_com_persistfile, 4);
            ret_val = 0; // S_OK
        } else if (strcmp(fn, "CreateDirectoryW") == 0) {
            // CreateDirectoryW(lpPathName, lpSecurityAttributes)
            uint16_t wpath[260] = {0};
            char apath[260] = {0};
            if (args[0]) {
                wg_blink_read_mem(engine->blink, args[0], wpath, 518);
                for (int i = 0; i < 259 && wpath[i]; i++)
                    apath[i] = wpath[i] < 128 ? (char)wpath[i] : '_';
            }
            const char *real = wg_files_map_path(args[0], engine->blink, apath, sizeof(apath));
            if (real) {
                wg_files_ensure_parents(real);   // deep bottle paths: make parents
                int r = mkdir(real, 0755);
                if (r == 0) {
                    ret_val = 1;
                    s_last_error = 0;
                } else if (errno == EEXIST) {
                    // NSIS needs a FRESH plugins dir (it creates one with a
                    // restricted ACL and bails if it already exists). Stale
                    // ns*.tmp dirs survive across runs, so clear and recreate.
                    if (strstr(apath, ".tmp")) {
                        wg_rmtree(real);
                        if (mkdir(real, 0755) == 0) {
                            ret_val = 1;
                            s_last_error = 0;
                            WG_LOGI(TAG, "CreateDirectoryW: cleared stale %s", apath);
                        } else {
                            ret_val = 0;
                            s_last_error = 183;
                        }
                    } else {
                        ret_val = 0;
                        s_last_error = 183; // ERROR_ALREADY_EXISTS
                    }
                } else {
                    ret_val = 0;
                    s_last_error = 3; // ERROR_PATH_NOT_FOUND
                }
                WG_LOGI(TAG, "CreateDirectoryW('%s') -> %s", apath, ret_val ? "OK" : "exists/fail");
            } else {
                ret_val = 1;
                s_last_error = 0;
            }
        } else if (strcmp(fn, "PeekMessageW") == 0) {
            // PeekMessageW(lpMsg, hWnd, filterMin, filterMax, removeMsg)
            // First: check the thread message queue (PostThreadMessageW items).
            uint32_t cur_tid = wg_sched_current_tid(engine->scheduler);
            if (!cur_tid) cur_tid = 1;
            uint32_t tmsg = 0, twp = 0, tlp = 0;
            if (args[0] && tmsg_pop(cur_tid, &tmsg, &twp, &tlp)) {
                uint32_t msgbuf[7] = {0, tmsg, twp, tlp, 0, 0, 0};
                wg_blink_write_mem(engine->blink, args[0], msgbuf, 28);
                WG_LOGI(TAG, "PeekMessageW: delivered msg=0x%X to tid=0x%X", tmsg, cur_tid);
                ret_val = 1;
            } else {
                // Idle — yield to ready worker threads (e.g. NSIS install thread).
                bool has_ready = false;
                for (int ti = 0; ti < WG_MAX_THREADS; ti++) {
                    if (engine->scheduler->threads[ti].state == WG_THREAD_READY) {
                        has_ready = true; break;
                    }
                }
                if (has_ready) {
                    if (wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY)) {
                        return true; // switched to worker
                    }
                }
                static int peek_count = 0;
                peek_count++;
                if (peek_count > 5) {
                    peek_count = 0;
                    engine->state = WG_ENGINE_PAUSED;
                    WG_LOGI(TAG, "Message loop — pausing for UI");
                }
                ret_val = 0; // no messages
            }
        } else if (strcmp(fn, "QueryPerformanceCounter") == 0) {
            // Was an R1S stub: returned TRUE but left *lpPerformanceCount STALE, so the
            // game read garbage timing. Write a REAL monotonic counter. This also lets
            // guest timing be well-defined for the real-threads coordination that was
            // reading garbage QPC deltas.
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t counter = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            // WG_QPC_SCALE=<n>: run the guest's performance clock n× real time. Any
            // time-based loading gate (frame pacing / timed wait) then completes n×
            // faster, cutting the boot-to-Present grind. Anchored to a fixed base so
            // the scaled counter is monotonic and doesn't overflow.
            static int s_qpc_scale = -1; static uint64_t s_qpc_base = 0;
            if (s_qpc_scale < 0) { const char *e = getenv("WG_QPC_SCALE"); s_qpc_scale = e ? atoi(e) : 1; s_qpc_base = counter; }
            if (s_qpc_scale > 1) counter = s_qpc_base + (counter - s_qpc_base) * (uint64_t)s_qpc_scale;
            if (args[0]) wg_blink_write_mem(engine->blink, args[0], &counter, 8);
            ret_val = 1;
        } else if (strcmp(fn, "QueryPerformanceFrequency") == 0) {
            uint64_t freq = 1000000000ULL;  // 1 GHz (ns units) to match the counter above
            if (args[0]) wg_blink_write_mem(engine->blink, args[0], &freq, 8);
            ret_val = 1;
        } else if (strcmp(fn, "GetTickCount") == 0) {
            // Seed from a real clock so it varies across launches — NSIS
            // derives its temp-dir names from this, and a fixed seed makes
            // every run collide on the same stale directory.
            static uint32_t s_tick = 0;
            if (s_tick == 0) s_tick = wg_determ() ? 0x100001u : ((uint32_t)(time(NULL) * 1000u) | 1u);
            ret_val = s_tick;
            s_tick += 16;
        } else if (strcmp(fn, "GetSystemTimeAsFileTime") == 0 ||
                   strcmp(fn, "GetSystemTimePreciseAsFileTime") == 0) {
            if (args[0]) {
                uint64_t ft;
                if (wg_determ()) {
                    ft = 0x01D8000000000000ULL;   // fixed FILETIME for deterministic runs
                } else {
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    // Convert Unix time to FILETIME (100ns ticks since 1601-01-01).
                    ft = (uint64_t)ts.tv_sec * 10000000ULL
                       + (uint64_t)ts.tv_nsec / 100ULL
                       + 116444736000000000ULL;
                }
                wg_blink_write_mem(engine->blink, args[0], &ft, 8);
            }
            ret_val = 0;
        } else if (strcmp(fn, "GetSystemTime") == 0) {
            if (args[0]) {
                time_t t = time(NULL);
                struct tm *tm = gmtime(&t);
                uint16_t st[8] = {
                    (uint16_t)(1900 + tm->tm_year), (uint16_t)(1 + tm->tm_mon),
                    (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                    (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min,
                    (uint16_t)tm->tm_sec, 0
                };
                wg_blink_write_mem(engine->blink, args[0], st, 16);
            }
            ret_val = 0;
        } else if (strcmp(fn, "GetLocalTime") == 0) {
            if (args[0]) {
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);
                uint16_t st[8] = {
                    (uint16_t)(1900 + tm->tm_year), (uint16_t)(1 + tm->tm_mon),
                    (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                    (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min,
                    (uint16_t)tm->tm_sec, 0
                };
                wg_blink_write_mem(engine->blink, args[0], st, 16);
            }
            ret_val = 0;
        } else if (strcmp(fn, "GetCurrentThreadId") == 0) {
            if (s_use_real_threads) { ret_val = s_cur_guest_tid; }
            else {
            ret_val = wg_sched_current_tid(engine->scheduler);
            if (!ret_val) ret_val = 1;
            }
        } else if ((strcmp(fn, "Sleep") == 0 || strcmp(fn, "SleepEx") == 0) && s_use_real_threads) {
            // Real-threads: a real sleep on this pthread (release the thunk lock).
            uint32_t ms = args[0];
            if (ms == 0xFFFFFFFFu) ms = 100;   // INFINITE sleep -> cap so we stay responsive
            if (ms > 0) { wg_thunk_block_begin(); usleep(wg_cap_timeout(ms) * 1000); wg_thunk_block_end(); }
            ret_val = 0;
        } else if (strcmp(fn, "Sleep") == 0 || strcmp(fn, "SleepEx") == 0) {
            if (args[0] > 0) {
                // On first few calls, dump guest call stack so we know what's looping.
                static uint32_t s_sleep_loop_rip = 0;
                static int      s_sleep_loop_cnt = 0;
                static int      s_backstop_log = 0;
                uint32_t cur_rip = (uint32_t)ret_addr; // instruction after the Sleep call
                if (cur_rip == s_sleep_loop_rip) {
                    s_sleep_loop_cnt++;
                } else {
                    s_sleep_loop_rip = cur_rip;
                    s_sleep_loop_cnt = 1;
                }
                // Reactor backstop: the orchestrator's Sleep loop is the heartbeat
                // while it waits for async I/O to finish. If a connected socket has
                // data ready but the app's I/O worker threads are parked on their
                // job events (Windows auto-signals those via IOCP/WSAEventSelect;
                // we have no OS notifier), wake every thread waiting on an event so
                // one of them does the recv and drives the TLS handshake / download
                // forward. Self-limiting: once the socket is drained it stops being
                // readable, so we stop signalling. Guarded to Steam (image_base
                // 0x400000) so it can't perturb other apps' I/O.
                if (s_backstop_enabled &&
                    s_sleep_loop_cnt > 3 && (s_sleep_loop_cnt & 7) == 0 &&
                    engine->pe_image && engine->pe_image->image_base == 0x400000 &&
                    wg_winsock_any_readable(engine->winsock)) {
                    WGThreadScheduler *sc = engine->scheduler;
                    int woke = 0;
                    for (int ti = 0; ti < sc->count; ti++) {
                        WGThread *t = &sc->threads[ti];
                        if (t->state == WG_THREAD_FREE || t->state == WG_THREAD_EXITED)
                            continue;
                        uint32_t wh = t->wait_handle;
                        if (wh >= WG_EVENT_BASE && wh < WG_EVENT_BASE + WG_MAX_EVENTS) {
                            s_event_signalled[wh - WG_EVENT_BASE] = true;
                            wg_sched_wake(sc, wh);
                            woke++;
                        }
                    }
                    if (woke && s_backstop_log < 40) {
                        s_backstop_log++;
                        WG_LOGW(TAG, "*** REACTOR-BACKSTOP: socket readable, woke %d event-waiter(s) (spin=%d)",
                                woke, s_sleep_loop_cnt);
                    }
                }
                if (s_sleep_loop_cnt <= 2) {
                    uint32_t ebp0 = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                    uint32_t cur_tid2 = wg_sched_current_tid(engine->scheduler);
                    WG_LOGI(TAG, "Sleep(%u) tid=0x%X ret=0x%X EBP=0x%X",
                            args[0], cur_tid2, cur_rip, ebp0);
                    // Dump 128 bytes BEFORE cur_rip to see the condition check and Sleep CALL
                    // (cur_rip is the ret addr after Sleep — POP EBP; RET is at cur_rip itself)
                    uint32_t dump_start = (cur_rip >= 128) ? cur_rip - 128 : 0;
                    uint8_t loop_code[128] = {0};
                    wg_blink_read_mem(engine->blink, dump_start, loop_code, 128);
                    WG_LOGI(TAG, "  x86@0x%X [before ret=0x%X]:", dump_start, cur_rip);
                    for (int di = 0; di < 8; di++) {
                        int b = di * 16;
                        WG_LOGI(TAG, "  0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                dump_start + (uint32_t)b,
                                loop_code[b+0],loop_code[b+1],loop_code[b+2],loop_code[b+3],
                                loop_code[b+4],loop_code[b+5],loop_code[b+6],loop_code[b+7],
                                loop_code[b+8],loop_code[b+9],loop_code[b+10],loop_code[b+11],
                                loop_code[b+12],loop_code[b+13],loop_code[b+14],loop_code[b+15]);
                    }
                    if (ebp0 > 0x10000 && ebp0 < 0xF0000000u) {
                        uint32_t ovl_ptr = 0;
                        wg_blink_read_mem(engine->blink, ebp0 + 16, &ovl_ptr, 4);
                        if (ovl_ptr > 0x100000u && ovl_ptr < 0x80000000u) {
                            uint32_t ovl[4] = {0};
                            wg_blink_read_mem(engine->blink, ovl_ptr, ovl, 16);
                            WG_LOGI(TAG, "  OVERLAPPED[0x%X]: Internal=0x%X InternalHigh=0x%X",
                                    ovl_ptr, ovl[0], ovl[1]);
                        }
                    }
                    // Walk guest EBP chain; on frame[0] dump the outer loop code
                    uint32_t ebp = ebp0;
                    uint32_t outer_loop_ra = 0; // ret addr of frame[0] = outer loop site
                    for (int fi = 0; fi < 8 && ebp > 0x10000 && ebp < 0xF0000000u; fi++) {
                        uint32_t frame_ra = 0, prev_ebp = 0;
                        wg_blink_read_mem(engine->blink, ebp + 4, &frame_ra, 4);
                        wg_blink_read_mem(engine->blink, ebp,     &prev_ebp, 4);
                        WG_LOGI(TAG, "  [%d] EBP=0x%X ret=0x%X", fi, ebp, frame_ra);
                        if (fi == 0 && frame_ra > 0x400000u && frame_ra < 0x80000000u) {
                            outer_loop_ra = frame_ra;
                            // Dump 96 bytes starting 32 before the outer loop ret address
                            // to see the condition check and loop branch
                            uint32_t ols = (frame_ra >= 32) ? frame_ra - 32 : 0;
                            uint8_t olb[96] = {0};
                            wg_blink_read_mem(engine->blink, ols, olb, 96);
                            WG_LOGI(TAG, "  outer_loop@0x%X (dump from 0x%X):", frame_ra, ols);
                            for (int oi = 0; oi < 6; oi++) {
                                int ob = oi * 16;
                                WG_LOGI(TAG, "  0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                        ols + (uint32_t)ob,
                                        olb[ob+0],olb[ob+1],olb[ob+2],olb[ob+3],
                                        olb[ob+4],olb[ob+5],olb[ob+6],olb[ob+7],
                                        olb[ob+8],olb[ob+9],olb[ob+10],olb[ob+11],
                                        olb[ob+12],olb[ob+13],olb[ob+14],olb[ob+15]);
                            }
                        }
                        if (prev_ebp == ebp || prev_ebp == 0) break;
                        ebp = prev_ebp;
                    }
                    // Dump locals + params of the innermost frame
                    if (ebp0 > 0x10000 && ebp0 < 0xF0000000u) {
                        // locals: EBP-44 .. EBP+0
                        uint32_t locals[12] = {0};
                        uint32_t lo = (ebp0 >= 44) ? ebp0 - 44 : 0;
                        wg_blink_read_mem(engine->blink, lo, locals, 48);
                        WG_LOGI(TAG, "  locals[EBP-44..EBP+4]:");
                        for (int li = 0; li < 12; li++) {
                            int off = (int)(lo + li*4) - (int)ebp0;
                            WG_LOGI(TAG, "    [EBP%+d]=0x%08X", off, locals[li]);
                        }
                        // params: EBP+8 .. EBP+28
                        uint32_t params[6] = {0};
                        wg_blink_read_mem(engine->blink, ebp0 + 8, params, 24);
                        WG_LOGI(TAG, "  params[EBP+8..EBP+28]: 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X",
                                params[0],params[1],params[2],params[3],params[4],params[5]);
                        // Dump the two monitored heap objects from EBP-20 and EBP-24
                        uint32_t ptr_a = locals[5] & ~1u; // EBP-24, strip tag
                        uint32_t ptr_b = locals[6];       // EBP-20
                        if (ptr_a > 0x1000000u && ptr_a < 0x80000000u) {
                            uint8_t objA[32] = {0};
                            wg_blink_read_mem(engine->blink, ptr_a, objA, 32);
                            WG_LOGI(TAG, "  [0x%X] A[0..31]: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    ptr_a,
                                    objA[0],objA[1],objA[2],objA[3], objA[4],objA[5],objA[6],objA[7],
                                    objA[8],objA[9],objA[10],objA[11], objA[12],objA[13],objA[14],objA[15]);
                            WG_LOGI(TAG, "                     %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    objA[16],objA[17],objA[18],objA[19], objA[20],objA[21],objA[22],objA[23],
                                    objA[24],objA[25],objA[26],objA[27], objA[28],objA[29],objA[30],objA[31]);
                        }
                        if (ptr_b > 0x1000000u && ptr_b < 0x80000000u) {
                            uint8_t objB[64] = {0};
                            wg_blink_read_mem(engine->blink, ptr_b, objB, 64);
                            WG_LOGI(TAG, "  [0x%X] B[0..31]: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    ptr_b,
                                    objB[0],objB[1],objB[2],objB[3], objB[4],objB[5],objB[6],objB[7],
                                    objB[8],objB[9],objB[10],objB[11], objB[12],objB[13],objB[14],objB[15]);
                            WG_LOGI(TAG, "                     %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    objB[16],objB[17],objB[18],objB[19], objB[20],objB[21],objB[22],objB[23],
                                    objB[24],objB[25],objB[26],objB[27], objB[28],objB[29],objB[30],objB[31]);
                            WG_LOGI(TAG, "             B[32..63]: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    objB[32],objB[33],objB[34],objB[35], objB[36],objB[37],objB[38],objB[39],
                                    objB[40],objB[41],objB[42],objB[43], objB[44],objB[45],objB[46],objB[47]);
                            WG_LOGI(TAG, "                        %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                                    objB[48],objB[49],objB[50],objB[51], objB[52],objB[53],objB[54],objB[55],
                                    objB[56],objB[57],objB[58],objB[59], objB[60],objB[61],objB[62],objB[63]);
                            // Dump vtable at B[0] — 8 function pointers
                            uint32_t vtbl_addr = 0;
                            memcpy(&vtbl_addr, objB, 4); // B[0] = vtable ptr
                            if (vtbl_addr > 0x400000u && vtbl_addr < 0x80000000u) {
                                uint32_t vfns[8] = {0};
                                wg_blink_read_mem(engine->blink, vtbl_addr, vfns, 32);
                                WG_LOGI(TAG, "  vtable[0x%X]: fn0=0x%X fn1=0x%X fn2=0x%X fn3=0x%X",
                                        vtbl_addr, vfns[0], vfns[1], vfns[2], vfns[3]);
                                WG_LOGI(TAG, "                fn4=0x%X fn5=0x%X fn6=0x%X fn7=0x%X",
                                        vfns[4], vfns[5], vfns[6], vfns[7]);
                                // Dump first 32 bytes at each fn to see what it does
                                for (int vi = 0; vi < 4 && vfns[vi] > 0x400000u && vfns[vi] < 0x80000000u; vi++) {
                                    uint8_t fn_bytes[16] = {0};
                                    wg_blink_read_mem(engine->blink, vfns[vi], fn_bytes, 16);
                                    WG_LOGI(TAG, "  vtable[%d]=0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                            vi, vfns[vi],
                                            fn_bytes[0],fn_bytes[1],fn_bytes[2],fn_bytes[3],
                                            fn_bytes[4],fn_bytes[5],fn_bytes[6],fn_bytes[7],
                                            fn_bytes[8],fn_bytes[9],fn_bytes[10],fn_bytes[11],
                                            fn_bytes[12],fn_bytes[13],fn_bytes[14],fn_bytes[15]);
                                }
                            }
                        }
                    }
                    // Dump all scheduler threads
                    static const char *state_names[] = {"FREE","RUNNING","READY","WAITING","SUSPENDED","EXITED","?"};
                    WG_LOGI(TAG, "  Scheduler threads (%d):", engine->scheduler->count);
                    for (int ti = 0; ti < WG_MAX_THREADS; ti++) {
                        WGThread *t = &engine->scheduler->threads[ti];
                        if (t->state == WG_THREAD_FREE) continue;
                        int sn = (t->state <= WG_THREAD_EXITED) ? (int)t->state : 6;
                        WG_LOGI(TAG, "    [%d] id=0x%X h=0x%X state=%s start=0x%X wait=0x%X rip=0x%X",
                                ti, t->id, t->handle, state_names[sn],
                                t->start_addr, t->wait_handle, t->regs.rip);
                    }
                }


                // Loop at 0x461F67 tests EBX (this), not object B:
                //   1) [EBX+0x38]->vtable[7]() != 0  -> exit (network pump)
                //   2) [EBX+0x4C] (byte) != 0        -> exit
                //   3) [EBX+0x18C] (dword) != 0      -> exit (result object)
                // It loops while ALL are "keep going". Probe EBX and the pump method.
                if (s_sleep_loop_cnt == 5) {
                    uint32_t ebx = (uint32_t)wg_blink_get_reg(engine->blink, 3);
                    WG_LOGI(TAG, "Loop-probe@5: EBX(this)=0x%X", ebx);
                    if (ebx > 0x1000000u && ebx < 0x80000000u) {
                        uint32_t subobj = 0, f4c = 0, f18c = 0, f188 = 0;
                        wg_blink_read_mem(engine->blink, ebx + 0x38, &subobj, 4);
                        wg_blink_read_mem(engine->blink, ebx + 0x4C, &f4c, 4);
                        wg_blink_read_mem(engine->blink, ebx + 0x188, &f188, 4);
                        wg_blink_read_mem(engine->blink, ebx + 0x18C, &f18c, 4);
                        WG_LOGI(TAG, "Loop-probe@5: [EBX+0x38]subobj=0x%X [EBX+0x4C]=0x%X [EBX+0x188]=0x%X [EBX+0x18C]=0x%X",
                                subobj, f4c, f188, f18c);
                        // subobj is a PE-data global (e.g. 0x7D7CB8), so accept the
                        // whole guest-mapped range, not just heap.
                        if (subobj >= 0x400000u && subobj < 0x80000000u) {
                            uint32_t vtbl = 0, m7 = 0;
                            wg_blink_read_mem(engine->blink, subobj, &vtbl, 4);
                            if (vtbl >= 0x400000u && vtbl < 0x80000000u) {
                                wg_blink_read_mem(engine->blink, vtbl + 0x1C, &m7, 4);
                                WG_LOGI(TAG, "Loop-probe@5: subobj vtable=0x%X pump=vtable[7]=0x%X", vtbl, m7);
                                if (m7 >= 0x400000u && m7 < 0x80000000u) {
                                    uint8_t mb[64] = {0};
                                    wg_blink_read_mem(engine->blink, m7, mb, 64);
                                    for (int mi = 0; mi < 4; mi++) {
                                        int b = mi * 16;
                                        WG_LOGI(TAG, "Loop-probe@5: pump@0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                                m7 + (uint32_t)b,
                                                mb[b+0],mb[b+1],mb[b+2],mb[b+3],mb[b+4],mb[b+5],mb[b+6],mb[b+7],
                                                mb[b+8],mb[b+9],mb[b+10],mb[b+11],mb[b+12],mb[b+13],mb[b+14],mb[b+15]);
                                    }
                                    // Follow every relative CALL (E8 rel32) in the first
                                    // 64 bytes — names the real routines the pump invokes.
                                    for (int k = 0; k < 59; k++) {
                                        if (mb[k] == 0xE8) {
                                            int32_t rel = (int32_t)(mb[k+1] | (mb[k+2]<<8) | (mb[k+3]<<16) | (mb[k+4]<<24));
                                            uint32_t tgt = m7 + (uint32_t)k + 5 + (uint32_t)rel;
                                            WG_LOGI(TAG, "Loop-probe@5:   pump CALL@+0x%X -> 0x%X", k, tgt);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Watchdog: if a thread spins on the same Sleep loop for a long
                // time, log it periodically (read-only) so a genuine deadlock is
                // visible. Includes the pump disasm so we don't have to scroll up to
                // the one-shot iter-5 probe in a huge log.
                if (s_sleep_loop_cnt > 0 && s_sleep_loop_cnt % 200 == 0) {
                    uint32_t ebx = (uint32_t)wg_blink_get_reg(engine->blink, 3);
                    WG_LOGW(TAG, "Sleep spin watchdog: ret=0x%X spun %d times, EBX(this)=0x%X",
                            cur_rip, s_sleep_loop_cnt, ebx);
                    if (ebx >= 0x400000u && ebx < 0x80000000u) {
                        uint32_t subobj = 0, f4c = 0, f18c = 0;
                        wg_blink_read_mem(engine->blink, ebx + 0x38, &subobj, 4);
                        wg_blink_read_mem(engine->blink, ebx + 0x4C, &f4c, 4);
                        wg_blink_read_mem(engine->blink, ebx + 0x18C, &f18c, 4);
                        WG_LOGW(TAG, "  [EBX+0x38]subobj=0x%X [EBX+0x4C]=0x%X [EBX+0x18C]=0x%X",
                                subobj, f4c, f18c);
                        if (subobj >= 0x400000u && subobj < 0x80000000u) {
                            uint32_t vtbl = 0, m7 = 0;
                            wg_blink_read_mem(engine->blink, subobj, &vtbl, 4);
                            if (vtbl >= 0x400000u && vtbl < 0x80000000u) {
                                wg_blink_read_mem(engine->blink, vtbl + 0x1C, &m7, 4);
                                WG_LOGW(TAG, "  pump=vtable[7]=0x%X (vtable=0x%X)", m7, vtbl);
                                if (m7 >= 0x400000u && m7 < 0x80000000u) {
                                    uint8_t mb[64] = {0};
                                    wg_blink_read_mem(engine->blink, m7, mb, 64);
                                    for (int mi = 0; mi < 4; mi++) {
                                        int b = mi * 16;
                                        WG_LOGW(TAG, "  pump@0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                                m7 + (uint32_t)b,
                                                mb[b+0],mb[b+1],mb[b+2],mb[b+3],mb[b+4],mb[b+5],mb[b+6],mb[b+7],
                                                mb[b+8],mb[b+9],mb[b+10],mb[b+11],mb[b+12],mb[b+13],mb[b+14],mb[b+15]);
                                    }
                                    for (int k = 0; k < 59; k++) {
                                        if (mb[k] == 0xE8) {
                                            int32_t rel = (int32_t)(mb[k+1] | (mb[k+2]<<8) | (mb[k+3]<<16) | (mb[k+4]<<24));
                                            uint32_t tgt = m7 + (uint32_t)k + 5 + (uint32_t)rel;
                                            WG_LOGW(TAG, "    pump CALL@+0x%X -> 0x%X", k, tgt);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                int sleep_nargs = (strcmp(fn, "SleepEx") == 0) ? 2 : 1;
                uint64_t new_rsp = rsp + ptr_size + ((uint64_t)sleep_nargs * ptr_size);
                wg_blink_set_reg(engine->blink, 4, new_rsp);
                wg_blink_set_rip(engine->blink, ret_addr);
                wg_blink_set_reg(engine->blink, 0, 0); // EAX = 0 (void)
                if (wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY)) {
                    return true; // switched; state saved at ret_addr
                }
                return true; // no other threads; state already cleaned
            }
            ret_val = 0;
        } else if (strcmp(fn, "ExitThread") == 0 && s_use_real_threads) {
            // Real-threads: setting rip=0 makes THIS pthread's blink loop return;
            // wg_worker_thread_entry then does wg_sync_thread_exit + frees the
            // Machine. (The main thread never calls ExitThread.)
            WG_LOGI(TAG, "[realthr] ExitThread(%u) tid=0x%X", args[0], s_cur_guest_tid);
            wg_blink_set_reg(engine->blink, 0, args[0]); // EAX = exit code
            wg_blink_set_rip(engine->blink, 0);
            return true;
        } else if (strcmp(fn, "ExitThread") == 0) {
            WG_LOGI(TAG, "ExitThread(%u)", args[0]);
            wg_dump_threads(engine, "ExitThread");
            wg_sched_exit_thread(engine->scheduler, engine->blink, args[0]);
            WGThread *cur_after = wg_sched_current(engine->scheduler);
            if (cur_after) {
                return true; // switched to another thread
            }
            // No more threads — halt
            wg_blink_set_rip(engine->blink, 0);
            return true;
        } else if (strcmp(fn, "GetCurrentProcess") == 0) {
            ret_val = (uint64_t)-1;
        } else if (strcmp(fn, "GetOverlappedResult") == 0) {
            // GetOverlappedResult(hFile, lpOverlapped, lpBytesTransferred, bWait)
            // Read InternalHigh (offset 4 in OVERLAPPED) as the byte count.
            if (args[1] && args[2]) {
                uint32_t bytes = 0;
                wg_blink_read_mem(engine->blink, args[1] + 4, &bytes, 4);
                wg_blink_write_mem(engine->blink, args[2], &bytes, 4);
            }
            ret_val = 1; // TRUE
        } else if (strcmp(fn, "CloseHandle") == 0) {
            if (s_use_real_threads && wg_sync_is_known(args[0])) wg_sync_close(args[0]);
            // Cooperative events: recycle the closed slot so the 256->3584 table can't
            // leak to overflow (the null-event deadlock). Only free real event handles.
            if (!s_use_real_threads && args[0] >= WG_EVENT_BASE &&
                args[0] < WG_EVENT_BASE + WG_MAX_EVENTS && s_event_free_n < WG_MAX_EVENTS) {
                s_event_free[s_event_free_n++] = args[0] - WG_EVENT_BASE;
            }
            wg_files_close(args[0]);
            ret_val = 1;
        } else if (strcmp(fn, "FindClose") == 0) {
            wg_findfile_close(args[0]);
            ret_val = 1;
        } else if (strcmp(fn, "GlobalUnlock") == 0) {
            ret_val = 1;
        } else if (strcmp(fn, "CreateFileW") == 0) {
            // CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecAttr,
            //             dwCreationDisposition, dwFlagsAndAttrs, hTemplate)
            // args[0]=filename, [1]=access, [4]=creation
            uint16_t wpath[260] = {0};
            char apath[260] = {0};
            if (args[0]) {
                wg_blink_read_mem(engine->blink, args[0], wpath, 518);
                for (int i = 0; i < 259 && wpath[i]; i++)
                    apath[i] = wpath[i] < 128 ? (char)wpath[i] : '_';
            }
            const char *real = wg_files_map_path(args[0], engine->blink, apath, sizeof(apath));
            // Bypass the reactor manifest download (GET-queue race): when Steam
            // opens steam_client_win32.manifest for read, native-fetch it first so
            // the open below finds a valid cached manifest.
            if (real && strstr(apath, "steam_client_win32.manifest"))
                wg_try_native_manifest_fetch(real);
            if (real) {
                ret_val = wg_files_create(real, args[1], args[4]);
            } else {
                ret_val = 0xFFFFFFFF;
            }
            // CreateFileW returns a HANDLE (pointer-width). On x64,
            // INVALID_HANDLE_VALUE is the full-width -1 (0xFFFFFFFFFFFFFFFF); a
            // bare 0x00000000FFFFFFFF fails the game's `== INVALID_HANDLE_VALUE`
            // check, so it treats a failed open as success and then queries a
            // bogus size (UE4 read a 4GB Visage.uproject -> OOM).
            if ((uint32_t)ret_val == 0xFFFFFFFFu && engine->pe_image &&
                engine->pe_image->is_64bit)
                ret_val = 0xFFFFFFFFFFFFFFFFULL;
            WG_LOGI(TAG, "CreateFileW('%s') -> 0x%llX", apath, (unsigned long long)ret_val);
            if (getenv("WG_FMT") && strstr(apath, ".pak"))
                WG_LOGW(TAG, "  ^pak opened by caller 0x%llX", (unsigned long long)ret_addr);

            // NSIS decompresses its whole data section (one solid raw-LZMA
            // stream) into a temp file, then reads each packed file from it by
            // offset. blink's in-guest decode of that stream truncates, so when
            // NSIS creates that data temp file (the first ns*.tmp opened
            // CREATE_ALWAYS that is NOT inside a plugin dir), we pre-fill it
            // with the correct full decompression done natively (LzmaDec), and
            // then ignore the guest's own (truncated) writes to it.
#if WG_NSIS_PREFILL_HACK
            if (ret_val != 0xFFFFFFFF && real &&
                s_nsis_data_tmp_handle == 0 && args[4] == 2 /* CREATE_ALWAYS */ &&
                strstr(apath, ".tmp") &&
                !strstr(apath, ".tmp\\") && !strstr(apath, ".tmp/")) {
                const char *exe_real = wg_files_map_path(0, engine->blink,
                                                         (char *)"C:\\a.exe", 260);
                if (exe_real && wg_nsis_prefill_datatmp(exe_real, real)) {
                    s_nsis_data_tmp_handle = (uint32_t)ret_val;
                    strncpy(s_nsis_data_tmp_path, real, sizeof(s_nsis_data_tmp_path) - 1);
                    // Reset the guest handle to offset 0 so the guest's reads
                    // (and ignored writes) line up; our data is already on disk.
                    wg_files_set_pointer((uint32_t)ret_val, 0, 0);
                    WG_LOGI(TAG, "NSIS data tmp pre-filled (handle=0x%X)",
                            s_nsis_data_tmp_handle);
                }
            }
#endif
#ifdef WG_DECODE_DIAG
            if (ret_val != 0xFFFFFFFF && args[4] == 2 && strstr(apath, ".tmp") &&
                !strstr(apath, ".tmp\\") && !strstr(apath, ".tmp/")) {
                s_diag_data_tmp_handle = (uint32_t)ret_val;
            }
#endif
        } else if (strcmp(fn, "CreateFileMappingW") == 0 || strcmp(fn, "CreateFileMappingA") == 0) {
            // CreateFileMapping(hFile, lpAttrs, flProtect, dwMaxSizeHigh, dwMaxSizeLow, lpName)
            // Record the underlying file handle; the actual data is served in MapViewOfFile.
            uint32_t hfile = args[0];
            ret_val = 0;
            if (hfile) {
                for (int i = 0; i < WG_MAX_FILEMAP; i++) if (!s_filemap[i].used) {
                    s_filemap[i].used = 1; s_filemap[i].file_handle = hfile;
                    ret_val = WG_FILEMAP_BASE + (uint32_t)i;
                    WG_LOGI(TAG, "CreateFileMapping(file=0x%X) -> mapping 0x%llX",
                            hfile, (unsigned long long)ret_val);
                    break;
                }
            }
            if (!ret_val) s_last_error = 8; // ERROR_NOT_ENOUGH_MEMORY
        } else if (strcmp(fn, "MapViewOfFile") == 0 || strcmp(fn, "MapViewOfFileEx") == 0) {
            // MapViewOfFile(hMap, dwAccess, dwOffHigh, dwOffLow, dwNumberOfBytes[, lpBase])
            uint32_t hmap = args[0];
            uint64_t offset = ((uint64_t)args[2] << 32) | args[3];
            uint32_t size = args[4];
            ret_val = 0;
            if (hmap >= WG_FILEMAP_BASE && hmap < WG_FILEMAP_BASE + WG_MAX_FILEMAP
                && s_filemap[hmap - WG_FILEMAP_BASE].used) {
                uint32_t fh = s_filemap[hmap - WG_FILEMAP_BASE].file_handle;
                uint32_t mapsize = size;
                if (mapsize == 0) {                              // 0 => map to EOF
                    uint64_t end = wg_files_set_pointer_64(fh, 0, 2); // SEEK_END
                    mapsize = (end > offset) ? (uint32_t)(end - offset) : 0x1000;
                }
                if (mapsize > 0x8000000u) mapsize = 0x8000000u;  // 128MB cap per view
                uint32_t gaddr = wg_guest_alloc_aligned(engine, mapsize, 0x10000);
                if (gaddr) {
                    wg_files_set_pointer_64(fh, (int64_t)offset, 0); // SEEK_SET
                    uint8_t *tmp = malloc(mapsize);
                    if (tmp) {
                        uint32_t nread = 0;
                        wg_files_read(fh, tmp, mapsize, &nread);
                        if (nread) wg_blink_write_mem(engine->blink, gaddr, tmp, nread);
                        free(tmp);
                        ret_val = gaddr;
                        WG_LOGI(TAG, "MapViewOfFile(map=0x%X off=%llu size=%u) -> 0x%X (read %u)",
                                hmap, (unsigned long long)offset, mapsize, gaddr, nread);
                    }
                }
            }
            if (!ret_val) s_last_error = 8;
        } else if (strcmp(fn, "UnmapViewOfFile") == 0) {
            // The view's guest memory is left mapped (bump-allocated; harmless). Succeed.
            ret_val = 1;
        } else if (strcmp(fn, "ReadFile") == 0) {
            uint32_t handle = args[0];
            // Buffer/out pointers MUST be the full 64-bit register values: the
            // 64-bit game reads level/streaming assets into region-3 buffers
            // (>4GB). Truncating buf_addr to 32-bit (args[1]) landed the data at
            // a wrong low address, leaving the real buffer garbage -> corrupt
            // object tables -> crash in the level-load hash walk (0x8f29c4).
            // args64[i] == args[i] for 32-bit guests, so this is universally safe.
            uint64_t buf_addr = args64[1];
            uint32_t nbytes = (uint32_t)args64[2];
            uint64_t bytes_read_addr = args64[3];
            uint64_t overlapped_addr = args64[4];
            // Cap per-read to bound the temp malloc, but 1MB was TOO SMALL: the SM5
            // global shader cache is ~5.7MB and UE4 reads it in one call — truncating
            // to 1MB dropped every shader past the first ~1MB, so the game reported
            // "Missing global shader ..._ES2..." and LowLevelFatalError'd before it
            // could render. Allow up to 128MB (malloc failure is handled below).
            if (nbytes > 0x8000000) nbytes = 0x8000000;
            // UE4's Windows file handle passes the read position via
            // OVERLAPPED.Offset/OffsetHigh (it never calls SetFilePointer).
            // Honor it — otherwise every read defaults to sequential-from-0 and
            // pak *footer* reads land at position 0, so the pak magic is never
            // found and no pak ever mounts (breaking all content + ICU).
            uint64_t pos_before;
            if (overlapped_addr) {
                bool is64 = engine->pe_image && engine->pe_image->is_64bit;
                uint32_t ofield = is64 ? 16 : 8, off_lo = 0, off_hi = 0;  // Offset field
                wg_blink_read_mem(engine->blink, overlapped_addr + ofield, &off_lo, 4);
                wg_blink_read_mem(engine->blink, overlapped_addr + ofield + 4, &off_hi, 4);
                uint64_t offset = ((uint64_t)off_hi << 32) | off_lo;
                pos_before = wg_files_set_pointer_64(handle, (int64_t)offset, 0); // SEEK_SET
            } else {
                pos_before = wg_files_set_pointer(handle, 0, 1); // SEEK_CUR
            }
            uint8_t *tmpbuf = malloc(nbytes);
            uint32_t first4 = 0, nread = 0;
            if (tmpbuf) {
                if (wg_files_read(handle, tmpbuf, nbytes, &nread)) {
                    wg_blink_write_mem(engine->blink, buf_addr, tmpbuf, nread);
                    if (bytes_read_addr) {
                        wg_blink_write_mem(engine->blink, bytes_read_addr, &nread, 4);
                    }
                    // Also record bytes transferred in OVERLAPPED.InternalHigh so
                    // a follow-up GetOverlappedResult reports the right count.
                    if (overlapped_addr) {
                        bool is64 = engine->pe_image && engine->pe_image->is_64bit;
                        uint64_t n64 = nread;
                        wg_blink_write_mem(engine->blink, overlapped_addr + (is64 ? 8 : 4),
                                           &n64, is64 ? 8 : 4);
                    }
                    if (nread >= 4) memcpy(&first4, tmpbuf, 4);
                    ret_val = 1;
                }
                free(tmpbuf);
            }
            // Log small control reads always, and ANY short read (nread <
            // nbytes) — a short read on the .exe would starve the decoder.
            if (nbytes <= 64 || nread < nbytes) {
                WG_LOGI(TAG, "ReadFile(h=0x%X, pos=%llu, n=%u) -> nread=%u first4=0x%08X",
                        handle, (unsigned long long)pos_before, nbytes, nread, first4);
            } else if (getenv("WG_FMT")) {  // diag: see full-buffer reads (pak mounting)
                WG_LOGI(TAG, "ReadFile(h=0x%X, pos=%llu, n=%u) -> nread=%u first4=0x%08X [full]",
                        handle, (unsigned long long)pos_before, nbytes, nread, first4);
            }
#ifdef WG_DECODE_DIAG
            else if (handle == 0x100 || handle == 0x101) {
                WG_LOGI("RD", "ReadFile(h=0x%X, pos=%u, n=%u) -> nread=%u first4=0x%08X",
                        handle, pos_before, nbytes, nread, first4);
            }
#endif
        } else if (strcmp(fn, "WriteFile") == 0) {
            uint32_t handle = args[0];
            // Full 64-bit pointers (see ReadFile): buffers may live in region-3
            // (>4GB). args64[i] == args[i] for 32-bit guests.
            uint64_t buf_addr = args64[1];
            uint32_t nbytes = (uint32_t)args64[2];
            uint64_t bytes_written_addr = args64[3];
            // NOTE: do NOT truncate nbytes here. A previous 1MB cap silently
            // dropped the tail of large writes; Steam's package save requires
            // WriteFile to report the FULL requested count written (it compares
            // bytes-written == buffer-size) or it logs "Saving package failed".
            // The real-file path below writes the whole buffer in bounded chunks.
            // stdout/stderr (from GetStdHandle) / pipes — surface the content;
            // Steam's networking spew (incl. the OpenSSL handshake error string
            // "COpenSSLConnection ... Error: %d - %s") goes here, not to the
            // bootstrap log, so we need to see it to diagnose the TLS failure.
            if (handle == 0xF1 || handle == 0xF2 || handle == 0x00007301) {
                if (nbytes > 0) {
                    uint32_t pl = nbytes < 512 ? nbytes : 512;
                    char *pv = malloc(pl + 1);
                    if (pv) {
                        wg_blink_read_mem(engine->blink, buf_addr, pv, pl);
                        pv[pl] = 0;
                        for (uint32_t i = 0; i < pl; i++) if (pv[i] == '\r') pv[i] = ' ';
                        WG_LOGI(TAG, "spew[0x%X]: %s", handle, pv);
                        free(pv);
                    }
                }
                if (bytes_written_addr)
                    wg_blink_write_mem(engine->blink, bytes_written_addr, &nbytes, 4);
                ret_val = 1;
                goto wf_done;
            }
            // Ignore writes to the pre-filled NSIS data tmp — our native
            // decompression already put the correct full data there; the
            // guest's own (truncated) decode would corrupt it.
            if (handle == s_nsis_data_tmp_handle && s_nsis_data_tmp_handle != 0) {
                if (bytes_written_addr)
                    wg_blink_write_mem(engine->blink, bytes_written_addr, &nbytes, 4);
                ret_val = 1;
                goto wf_done;
            }
            // Write the FULL buffer in bounded chunks so we never malloc the whole
            // (potentially 90MB+) write at once yet still write/report everything.
            const uint32_t WF_CHUNK = 0x100000; // 1MB working buffer
            uint32_t alloc = nbytes < WF_CHUNK ? (nbytes ? nbytes : 1) : WF_CHUNK;
            uint8_t *tmpbuf = malloc(alloc);
            if (tmpbuf) {
                uint32_t total_written = 0, off = 0;
                bool wrote_real = false, short_write = false;
                while (off < nbytes) {
                    uint32_t chunk = nbytes - off;
                    if (chunk > WF_CHUNK) chunk = WF_CHUNK;
                    wg_blink_read_mem(engine->blink, buf_addr + off, tmpbuf, chunk);
#ifdef WG_DECODE_DIAG
                    if (handle == s_diag_data_tmp_handle && s_diag_data_tmp_handle != 0) {
                        uint32_t pos = wg_files_set_pointer(handle, 0, 1); // SEEK_CUR
                        wg_diag_check(pos, tmpbuf, chunk);
                    }
#endif
                    uint32_t nw = 0;
                    if (!wg_files_write(handle, tmpbuf, chunk, &nw)) break; // unknown handle
                    wrote_real = true;
                    total_written += nw;
                    if (nw < chunk) { short_write = true; break; }
                    off += chunk;
                }
                if (wrote_real) {
                    if (bytes_written_addr)
                        wg_blink_write_mem(engine->blink, bytes_written_addr, &total_written, 4);
                    ret_val = 1;
                    WG_LOGI(TAG, "WriteFile(0x%X, %u bytes) -> wrote %u%s", handle, nbytes,
                            total_written, short_write ? " (SHORT)" : "");
                    if (total_written > 0 && total_written <= 512) {
                        char pv[514]; uint32_t pl = total_written < 513 ? total_written : 512;
                        memcpy(pv, tmpbuf, pl); pv[pl] = '\0';
                        for (uint32_t j = 0; j < pl; j++)
                            if ((unsigned char)pv[j] < 0x20 && pv[j] != '\n') pv[j] = '.';
                        WG_LOGI(TAG, "  >> %s", pv);
                    }
                } else {
                    // Succeed silently for unknown handles to prevent thread crashes
                    if (bytes_written_addr)
                        wg_blink_write_mem(engine->blink, bytes_written_addr, &nbytes, 4);
                    ret_val = 1;
                    WG_LOGW(TAG, "WriteFile(0x%X, %u bytes) -> sink (unknown handle)", handle, nbytes);
                }
                free(tmpbuf);
            }
            wf_done:;
        } else if (strcmp(fn, "GetFileType") == 0) {
            uint32_t h = args[0];
            if (h >= 0x100 && h < 0x200)
                ret_val = 1; // FILE_TYPE_DISK for real file handles
            else
                ret_val = 0; // FILE_TYPE_UNKNOWN for everything else
        } else if (strcmp(fn, "GetFileSize") == 0) {
            ret_val = wg_files_get_size(args[0]);
            // lpFileSizeHigh (arg1) gets the high dword if provided (we only have 32-bit sizes)
            if (args[1] > 0x10000u && args[1] < 0xF0000000u) {
                uint32_t hi = 0;
                wg_blink_write_mem(engine->blink, args[1], &hi, 4);
            }
            WG_LOGI(TAG, "GetFileSize(0x%X) -> %u", args[0], (uint32_t)ret_val);
        } else if (strcmp(fn, "GetFileSizeEx") == 0) {
            // BOOL GetFileSizeEx(HANDLE, PLARGE_INTEGER lpFileSize)
            // R1S stub never wrote the size — caller read stack garbage and
            // tried to allocate it (Steam: 563MB OOM). Write the real size.
            // Reject the invalid handle (0xFFFFFFFF): return FALSE instead of a
            // bogus 4GB size that sends the caller into a huge allocation.
            if (args[0] == 0xFFFFFFFFu) {
                ret_val = 0; s_last_error = 6; // ERROR_INVALID_HANDLE
                WG_LOGI(TAG, "GetFileSizeEx(INVALID) -> FALSE");
            } else {
                uint32_t sz = wg_files_get_size(args[0]);
                if (args[1] > 0x10000u && args[1] < 0xF0000000u) {
                    uint64_t sz64 = (uint64_t)sz; // low + high dwords
                    wg_blink_write_mem(engine->blink, args[1], &sz64, 8);
                }
                ret_val = 1;
                WG_LOGI(TAG, "GetFileSizeEx(0x%X) -> %u bytes", args[0], sz);
            }
        } else if (strcmp(fn, "SetFilePointer") == 0) {
            // Detect when NSIS finishes its truncated copy and patch the .tmp
            // Track seeks on the data .tmp to know extraction offsets.
            // Only capture large seeks (>1000) — small ones (0, 4) are header reads.
            if (s_nsis_data_tmp_handle != 0 && args[0] == s_nsis_data_tmp_handle &&
                args[3] == 0 && (int32_t)args[1] > 1000) {
                s_nsis_last_data_seek = (uint32_t)args[1];
                WG_LOGI(TAG, "NSIS data seek: offset=%u", s_nsis_last_data_seek);
            }

            // Track seeks on the EXE file (not .tmp) to find raw data offset.
            // Only consider seeks on files larger than 1MB (that's the .exe).
            if (!s_nsis_data_patched && args[3] == 0 &&
                (int32_t)args[1] > 100000 && (int32_t)args[1] < 2000000) {
                uint32_t file_size = wg_files_get_size(args[0]);
                if (file_size > 1000000) {
                    // This is the exe — track the last seek position
                    s_nsis_exe_data_offset = (uint32_t)args[1];
                }
            }

            if (false && !s_nsis_data_patched && s_nsis_exe_data_offset > 0) {
                // DISABLED: old .tmp patching — now using full outer stream decompression
                uint32_t current_size = wg_files_get_size(args[0]);
                if (current_size > 200000 && current_size < 500000) {
                    WG_LOGI(TAG, "Patching .tmp: handle=0x%X, size=%u, exe_data_off=%u",
                            args[0], current_size, s_nsis_exe_data_offset);
                    const char *exe_real = wg_files_map_path(0, engine->blink,
                        (char*)"C:\\a.exe", 260);
                    if (exe_real) {
                        FILE *exe_fp = fopen(exe_real, "rb");
                        if (exe_fp) {
                            fseek(exe_fp, 0, SEEK_END);
                            long exe_size = ftell(exe_fp);
                            fseek(exe_fp, 0, SEEK_SET);
                            uint8_t *exe_data = malloc(exe_size);
                            if (exe_data) {
                                fread(exe_data, 1, exe_size, exe_fp);
                                // Find the end of NSIS data (NullsoftInst + archive_size)
                                long nsi = -1;
                                for (long i = 0; i < exe_size - 16; i++) {
                                    if (memcmp(exe_data + i, "NullsoftInst", 12) == 0) {
                                        nsi = i; break;
                                    }
                                }
                                if (nsi >= 0) {
                                    uint32_t arc_size;
                                    memcpy(&arc_size, exe_data + nsi + 16, 4);
                                    long data_start = nsi + 20;
                                    // Raw data starts at s_nsis_exe_data_offset in the exe
                                    // NSIS copied current_size bytes from that position
                                    // Total raw data: from exe_data_offset to data_start + arc_size
                                    long total_raw = (data_start + arc_size) - s_nsis_exe_data_offset;
                                    if ((long)current_size < total_raw) {
                                        long src_offset = s_nsis_exe_data_offset + current_size;
                                        long append_size = total_raw - current_size;
                                        if (src_offset + append_size <= exe_size && append_size > 0) {
                                            wg_files_set_pointer(args[0], 0, 2);
                                            uint32_t written = 0;
                                            wg_files_write(args[0], exe_data + src_offset, (uint32_t)append_size, &written);
                                            WG_LOGI(TAG, "Patched .tmp: appended %u bytes (was %u, now %u, from exe@%ld)",
                                                    written, current_size, current_size + written, src_offset);
                                            s_nsis_data_patched = true;
                                        }
                                    }
                                }
                                free(exe_data);
                            }
                            fclose(exe_fp);
                        }
                    }
                }
            }
            ret_val = wg_files_set_pointer(args[0], (int32_t)args[1], args[3]);
            WG_LOGI(TAG, "SetFilePointer(h=0x%X, dist=%d, method=%u) -> %u",
                    args[0], (int32_t)args[1], args[3], (uint32_t)ret_val);
        } else if (strcmp(fn, "GetFileAttributesW") == 0) {
            uint16_t wpath[260] = {0};
            char apath[260] = {0};
            if (args[0]) {
                wg_blink_read_mem(engine->blink, args[0], wpath, 518);
                for (int i = 0; i < 259 && wpath[i]; i++)
                    apath[i] = wpath[i] < 128 ? (char)wpath[i] : '_';
            }

            const char *real = wg_files_map_path(args[0], engine->blink, apath, sizeof(apath));
            if (real) {
                struct stat st;
                // If Steam is checking for a client package that's missing (or a
                // wrong-size leftover), fetch it natively so it finds it present and
                // skips its own (flaky) reactor download. No-op for non-packages and
                // for packages already present at the correct size.
                wg_try_native_package_fetch(real);
                if (stat(real, &st) == 0) {
                    ret_val = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
                } else {
                    ret_val = 0xFFFFFFFF;
                    s_last_error = 2;
                }
            } else {
                ret_val = 0xFFFFFFFF;
                s_last_error = 2;
            }
            if (getenv("WG_FMT") && (strstr(apath, "Content") || strstr(apath, "nternational")))
                WG_LOGW(TAG, "GetFileAttributesW('%s') -> 0x%llX", apath, (unsigned long long)ret_val);
        } else if (strcmp(fn, "DeleteFileW") == 0) {
            uint16_t wpath[260] = {0};
            char apath[260] = {0};
            if (args[0]) {
                wg_blink_read_mem(engine->blink, args[0], wpath, 518);
                for (int i = 0; i < 259 && wpath[i]; i++)
                    apath[i] = wpath[i] < 128 ? (char)wpath[i] : '_';
            }
            const char *real = wg_files_map_path(args[0], engine->blink, apath, sizeof(apath));
            if (real) {
                ret_val = (unlink(real) == 0) ? 1 : 0;
            } else {
                ret_val = 1;
            }
        } else if (strcmp(fn, "FindFirstFileW") == 0) {
            // FindFirstFileW(lpFileName, lpFindFileData)
            ret_val = wg_findfile_first(engine, args[0], args[1]);
        } else if (strcmp(fn, "FindFirstFileExW") == 0) {
            // FindFirstFileExW(name, InfoLevel, lpFindData, SearchOp, filter, flags)
            ret_val = wg_findfile_first(engine, args[0], args[2]);
        } else if (strcmp(fn, "FindNextFileW") == 0) {
            // FindNextFileW(hFindFile, lpFindFileData)
            ret_val = wg_findfile_next(engine, args[0], args[1]);
        } else if (strcmp(fn, "GlobalAlloc") == 0) {
            uint32_t size = args[1];
            if (size == 0) size = 4096;
            // A size with the top bit set (or one that's absurdly large) is
            // garbage that NSIS's broken in-guest LZMA decoder derived. NSIS
            // uses it as a real length (it allocs then zero-terminates at
            // buf[size-1]), so no buffer we return can satisfy it — masking the
            // size and handing back a smaller buffer just turns the clean
            // "Error decompressing data" abort into an out-of-bounds SIGSEGV.
            // Return NULL and let NSIS take its own error path.
            if ((size & 0x80000000u) || size > 512u * 1024 * 1024) {
                WG_LOGW(TAG, "GlobalAlloc FAILED: corrupt size %u (in-guest "
                        "decoder produced garbage)", size);
#ifdef WG_DECODE_DIAG
                // Who called GlobalAlloc with the bad size, and what's on the
                // guest stack right now? (32-bit: args already read from stack.)
                WG_LOGE("DIAG", "corrupt GlobalAlloc: caller RIP=0x%llx "
                        "EAX=0x%llx ECX=0x%llx EDX=0x%llx EBX=0x%llx "
                        "EBP=0x%llx ESP=0x%llx",
                        (unsigned long long)ret_addr,
                        wg_blink_get_reg(engine->blink, 0),
                        wg_blink_get_reg(engine->blink, 1),
                        wg_blink_get_reg(engine->blink, 2),
                        wg_blink_get_reg(engine->blink, 3),
                        wg_blink_get_reg(engine->blink, 5),
                        wg_blink_get_reg(engine->blink, 4));
                uint32_t stk[12] = {0};
                wg_blink_read_mem(engine->blink, rsp, stk, sizeof(stk));
                for (int si = 0; si < 12; si += 4)
                    WG_LOGE("DIAG", "  [ESP+%2d]: %08X %08X %08X %08X",
                            si*4, stk[si], stk[si+1], stk[si+2], stk[si+3]);
                // The size came from 4 bytes at [ebp-0x70]. Dump that pointer and
                // the bytes around it to see where the garbage lives.
                uint32_t ebp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                uint32_t pInput = 0, vAccum = 0, vCount = 0;
                wg_blink_read_mem(engine->blink, ebp - 0x70, &pInput, 4);
                wg_blink_read_mem(engine->blink, ebp - 0x40, &vAccum, 4);
                wg_blink_read_mem(engine->blink, ebp - 0x6c, &vCount, 4);
                WG_LOGE("DIAG", "locals: [ebp-0x70](inPtr)=0x%08X [ebp-0x40](accum)=0x%08X [ebp-0x6c]=0x%08X",
                        pInput, vAccum, vCount);
                if (pInput) {
                    uint8_t around[32] = {0};
                    wg_blink_read_mem(engine->blink, pInput - 8, around, 32);
                    WG_LOGE("DIAG", "  inPtr-8: %02X %02X %02X %02X %02X %02X %02X %02X | "
                            "%02X %02X %02X %02X %02X %02X %02X %02X",
                            around[0],around[1],around[2],around[3],around[4],around[5],around[6],around[7],
                            around[8],around[9],around[10],around[11],around[12],around[13],around[14],around[15]);
                }
#endif
                ret_val = 0;
            } else {
                size = (size + 0xFFF) & ~0xFFF;
                if (s_heap_ptr + size > 0x5F000000u && s_heap_ptr < 0xA0000000u)
                    s_heap_ptr = 0xA0000000u;   // hop to region 2 (see wg_guest_alloc)
                if (s_heap_ptr < (uint32_t)(WG_THUNK_BASE + 0x20000u) &&
                    s_heap_ptr + size > (uint32_t)WG_THUNK_BASE)
                    s_heap_ptr = (uint32_t)(WG_THUNK_BASE + 0x20000u);  // skip thunk hole
                uint32_t hi7 = (s_heap_ptr >= 0xA0000000u) ? 0xFFFF0000u : 0x5F000000u;
                if (s_heap_ptr + size > hi7 || s_heap_ptr + size < s_heap_ptr) {
                    ret_val = 0; // heap full (both regions)
                } else {
                uint32_t addr = s_heap_ptr;
                uint8_t *zeros = calloc(1, size);
                if (zeros) {
                    wg_blink_load_code(engine->blink, addr, zeros, size, 0);
                    free(zeros);
                    s_heap_ptr += size;
                    s_heap_ptr = (s_heap_ptr + 0xFFF) & ~0xFFF;
                    ret_val = addr;
                }
                }
            }
        } else if (strcmp(fn, "GlobalLock") == 0) {
            ret_val = args[0]; // GMEM_FIXED: handle == pointer
        } else if (strcmp(fn, "LocalAlloc") == 0) {
            // LocalAlloc(uFlags=args[0], uBytes=args[1])
            uint32_t size = args[1];
            ret_val = wg_guest_alloc(engine, size ? size : 1);
        } else if (strcmp(fn, "LocalFree") == 0) {
            ret_val = 0; // success
        }

        // WS2_32 / WSOCK32 dispatch — map ordinal names to function names
        // and forward to the winsock handler.
        if (entry && entry->dll_name &&
            (strcasecmp(entry->dll_name, "WS2_32.dll") == 0 ||
             strcasecmp(entry->dll_name, "WSOCK32.dll") == 0)) {
            const char *ws_fn = fn;
            // Map WS2_32 ordinals to function names
            if (strncmp(fn, "Ordinal_", 8) == 0) {
                int ord = atoi(fn + 8);
                switch (ord) {
                    case 1: ws_fn = "accept"; break;
                    case 2: ws_fn = "bind"; break;
                    case 3: ws_fn = "closesocket"; break;
                    case 4: ws_fn = "connect"; break;
                    case 5: ws_fn = "getpeername"; break;
                    case 6: ws_fn = "getsockname"; break;
                    case 7: ws_fn = "getsockopt"; break;
                    case 8: ws_fn = "htonl"; break;
                    case 9: ws_fn = "htons"; break;
                    case 10: ws_fn = "ioctlsocket"; break;
                    case 11: ws_fn = "inet_addr"; break;
                    case 12: ws_fn = "inet_ntoa"; break;
                    case 13: ws_fn = "listen"; break;
                    case 14: ws_fn = "ntohl"; break;
                    case 15: ws_fn = "ntohs"; break;
                    case 16: ws_fn = "recv"; break;
                    case 17: ws_fn = "recvfrom"; break;
                    case 18: ws_fn = "select"; break;
                    case 19: ws_fn = "send"; break;
                    case 20: ws_fn = "sendto"; break;
                    case 21: ws_fn = "setsockopt"; break;
                    case 22: ws_fn = "shutdown"; break;
                    case 23: ws_fn = "socket"; break;
                    case 52: ws_fn = "gethostbyname"; break;
                    case 57: ws_fn = "gethostname"; break;
                    // WS2_32 fixed ordinals (Microsoft): 111=WSAGetLastError,
                    // 112=WSASetLastError. WSAEnumNetworkEvents/WSAEventSelect are
                    // imported by NAME, not these ordinals — the old mapping made
                    // Steam's post-connect WSAGetLastError() run the wrong handler
                    // and crash.
                    case 111: ws_fn = "WSAGetLastError"; break;
                    case 112: ws_fn = "WSASetLastError"; break;
                    case 151: ws_fn = "__WSAFDIsSet"; break; // FD_ISSET backing fn
                    case 115: ws_fn = "WSAStartup"; break;
                    case 116: ws_fn = "WSACleanup"; break;
                    case 1142: ws_fn = "WSAStartup"; break; // WSOCK32
                    default: break;
                }
            }
            // Intercept WSAIoctl SIO_GET_EXTENSION_FUNCTION_POINTER
            // to return thunk addresses for ConnectEx, DisconnectEx, etc.
            bool ws_handled = false;
            if (strcmp(ws_fn, "WSAIoctl") == 0 && args[1] == 0xC8000006) {
                uint8_t guid[16] = {0};
                if (args[2] && args[3] >= 16)
                    wg_blink_read_mem(engine->blink, args[2], guid, 16);
                static const uint8_t CONNECTEX_GUID[]    = {0xb9,0x07,0xa2,0x25,0xf3,0xdd,0x60,0x46,0x8e,0xe9,0x76,0xe5,0x8c,0x74,0x06,0x3e};
                static const uint8_t DISCONNECTEX_GUID[] = {0x11,0x2e,0xda,0x7f,0x30,0x86,0x6f,0x43,0xa0,0x31,0xf5,0x36,0xa6,0xee,0xc1,0x57};
                static const uint8_t ACCEPTEX_GUID[]     = {0xb5,0x36,0x7e,0xb1,0x11,0xab,0x0f,0x00,0xd9,0xc9,0x00,0xa0,0x24,0x16,0x09,0x93};
                const char *ext = NULL;
                if (memcmp(guid, CONNECTEX_GUID, 16) == 0)    ext = "ConnectEx";
                else if (memcmp(guid, DISCONNECTEX_GUID, 16) == 0) ext = "DisconnectEx";
                else if (memcmp(guid, ACCEPTEX_GUID, 16) == 0)     ext = "AcceptEx";
                if (ext) {
                    uint64_t thunk = wg_dll_mapper_find_any(engine->dll_mapper, ext);
                    if (!thunk) thunk = wg_dll_mapper_resolve(engine->dll_mapper, "WS2_32.dll", ext);
                    if (thunk >= 0xC00000 && thunk < 0xC20000) {
                        uint8_t hlt = 0xF4;
                        wg_blink_write_mem(engine->blink, (uint32_t)thunk, &hlt, 1);
                    }
                    uint32_t addr = (uint32_t)thunk;
                    if (args[4] && args[5] >= 4)
                        wg_blink_write_mem(engine->blink, args[4], &addr, 4);
                    if (args[6]) { uint32_t r = 4; wg_blink_write_mem(engine->blink, args[6], &r, 4); }
                    WG_LOGI(TAG, "WSAIoctl: %s -> thunk 0x%X", ext, addr);
                    ret_val = 0;
                    ws_handled = true;
                }
            }

            if (!ws_handled) {
                uint64_t ws_ret = 0;
                // Don't log the per-iteration polling calls (select / __WSAFDIsSet):
                // during a wait they fire hundreds of thousands of times and flood
                // the device log's ring buffer, scrolling off the actual download
                // activity we need to see. Log everything else.
                if (strcmp(ws_fn, "select") != 0 && strcmp(ws_fn, "__WSAFDIsSet") != 0)
                    WG_LOGI(TAG, "WS2_32 dispatch: %s -> %s", fn, ws_fn);
                // Trace the connection object at connect() so we can correlate it
                // with the NULL connection at the later send(0,0,0). The owning
                // CTCPConnection 'this' is usually held in a callee-saved reg
                // (ESI/EDI/EBX). Identify it by a vtable pointer in the PE range.
                if (strcmp(ws_fn, "connect") == 0) {
                    s_connect_calls++;
                    s_last_conn_sock = args[0];
                    uint32_t r_ecx = (uint32_t)wg_blink_get_reg(engine->blink, 1);
                    uint32_t r_ebx = (uint32_t)wg_blink_get_reg(engine->blink, 3);
                    uint32_t r_esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    uint32_t r_edi = (uint32_t)wg_blink_get_reg(engine->blink, 7);
                    WG_LOGW(TAG, "connect #%d: sock=0x%X ECX=%08X EBX=%08X ESI=%08X EDI=%08X",
                            s_connect_calls, args[0], r_ecx, r_ebx, r_esi, r_edi);
                    uint32_t cand[4] = { r_ecx, r_ebx, r_esi, r_edi };
                    const char *cn[4] = { "ECX", "EBX", "ESI", "EDI" };
                    for (int ci = 0; ci < 4; ci++) {
                        if (cand[ci] >= 0x10000u && cand[ci] < 0xF0000000u) {
                            uint32_t vt = 0;
                            wg_blink_read_mem(engine->blink, cand[ci], &vt, 4);
                            if (vt >= 0x400000u && vt < 0x800000u) {
                                WG_LOGW(TAG, "  %s=0x%X looks like an object (vtable=0x%X)",
                                        cn[ci], cand[ci], vt);
                                if (!s_last_conn_obj) s_last_conn_obj = cand[ci];
                            }
                        }
                    }
                    uint32_t cbp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                    for (int fi = 0; fi < 6 && cbp > 0x10000 && cbp < 0xF0000000u; fi++) {
                        uint32_t ra = 0, prev = 0;
                        wg_blink_read_mem(engine->blink, cbp + 4, &ra, 4);
                        wg_blink_read_mem(engine->blink, cbp, &prev, 4);
                        WG_LOGW(TAG, "  connect [%d] EBP=0x%X ret=0x%X", fi, cbp, ra);
                        if (prev <= cbp || prev == 0) break;
                        cbp = prev;
                    }
                }
                // Diagnostic: a send on socket 0 is the symptom of Steam's TCP
                // layer bailing. Dump the guest caller chain so we can see which
                // code path issues it (real TLS send vs assert/cleanup).
                if (strcmp(ws_fn, "send") == 0 && args[0] == 0) {
                    uint32_t ebp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                    uint32_t eax = (uint32_t)wg_blink_get_reg(engine->blink, 0);
                    uint32_t ecx = (uint32_t)wg_blink_get_reg(engine->blink, 1);
                    uint32_t edx = (uint32_t)wg_blink_get_reg(engine->blink, 2);
                    uint32_t ebx = (uint32_t)wg_blink_get_reg(engine->blink, 3);
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    uint32_t edi = (uint32_t)wg_blink_get_reg(engine->blink, 7);
                    WG_LOGW(TAG, "send(s=0!) buf=0x%X len=0x%X flags=0x%X", args[1], args[2], args[3]);
                    WG_LOGW(TAG, "  EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESI=%08X EDI=%08X",
                            eax, ecx, edx, ebx, esi, edi);
                    // Correlation (in the block that always gets pasted): did a
                    // connect() ever happen, and what connection object did it use?
                    WG_LOGW(TAG, "  CORRELATION: connect_calls=%d last_conn_obj=0x%X last_conn_sock=0x%X (this send: ESI/this=0x%X)",
                            s_connect_calls, s_last_conn_obj, s_last_conn_sock, esi);
                    // Dump the guest SEH chain (fs:[0] = TEB[0]). If Steam has a
                    // __try handler in its .text around this code, delivering the
                    // NULL-deref AV to it could let it recover. Each
                    // EXCEPTION_REGISTRATION_RECORD = { Next, Handler }.
                    {
                        uint32_t seh = 0;
                        wg_blink_read_mem(engine->blink, s_main_teb + 0, &seh, 4);
                        WG_LOGW(TAG, "  SEH chain head (fs:[0])=0x%X:", seh);
                        for (int si = 0; si < 16 && seh > 0x1000u && seh < 0xFFFFFFFEu; si++) {
                            uint32_t rec[2] = {0};
                            wg_blink_read_mem(engine->blink, seh, rec, 8);
                            WG_LOGW(TAG, "    [%d] frame=0x%X handler=0x%X", si, seh, rec[1]);
                            if (rec[0] <= seh) break; // chain walks up the stack
                            seh = rec[0];
                        }
                    }
                    uint32_t ra0 = 0;
                    wg_blink_read_mem(engine->blink, ebp + 4, &ra0, 4);
                    // Disassemble the send call site (how the socket arg was pushed).
                    if (ra0 >= 0x401000u && ra0 < 0x800000u) {
                        uint32_t cs = (ra0 >= 40) ? ra0 - 40 : 0;
                        uint8_t cb[48] = {0};
                        wg_blink_read_mem(engine->blink, cs, cb, 48);
                        for (int ci = 0; ci < 3; ci++) {
                            int b = ci * 16;
                            WG_LOGW(TAG, "  callsite 0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                    cs + (uint32_t)b,
                                    cb[b+0],cb[b+1],cb[b+2],cb[b+3],cb[b+4],cb[b+5],cb[b+6],cb[b+7],
                                    cb[b+8],cb[b+9],cb[b+10],cb[b+11],cb[b+12],cb[b+13],cb[b+14],cb[b+15]);
                        }
                    }
                    // Dump the likely connection object (ECX = thiscall 'this').
                    if (ecx >= 0x10000u && ecx < 0xF0000000u) {
                        uint32_t obj[24] = {0};
                        wg_blink_read_mem(engine->blink, ecx, obj, sizeof(obj));
                        for (int oi = 0; oi < 6; oi++)
                            WG_LOGW(TAG, "  [ECX+0x%02X]: %08X %08X %08X %08X",
                                    oi*16, obj[oi*4],obj[oi*4+1],obj[oi*4+2],obj[oi*4+3]);
                    }
                    for (int fi = 0; fi < 14 && ebp > 0x10000 && ebp < 0xF0000000u; fi++) {
                        uint32_t ra = 0, prev = 0, a1 = 0, a2 = 0;
                        wg_blink_read_mem(engine->blink, ebp + 4, &ra, 4);
                        wg_blink_read_mem(engine->blink, ebp, &prev, 4);
                        wg_blink_read_mem(engine->blink, ebp + 8, &a1, 4);   // arg1 / stdcall 'this'
                        wg_blink_read_mem(engine->blink, ebp + 12, &a2, 4);  // arg2
                        WG_LOGW(TAG, "  [%d] EBP=0x%X ret=0x%X arg1=0x%X arg2=0x%X",
                                fi, ebp, ra, a1, a2);
                        // For the inner frames, dump 0x40 bytes of code before the
                        // return address — how this call set up ECX('this') and
                        // the args (i.e. where the NULL `this` came from).
                        if (ra >= 0x401000u && ra < 0x800000u && fi < 4) {
                            for (int r = 0; r < 4; r++) {
                                uint8_t pb[16] = {0};
                                wg_blink_read_mem(engine->blink, ra - 0x40 + r*16, pb, 16);
                                WG_LOGW(TAG, "      @0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                    ra - 0x40 + r*16, pb[0],pb[1],pb[2],pb[3],pb[4],pb[5],pb[6],pb[7],
                                    pb[8],pb[9],pb[10],pb[11],pb[12],pb[13],pb[14],pb[15]);
                            }
                        }
                        if (prev <= ebp || prev == 0) break;
                        ebp = prev;
                    }
                    // Is the real (connected) connection object still reachable on
                    // the stack near ESP? If it is but `this`=0, the send path
                    // loaded the wrong value — that pinpoints where the NULL came
                    // from. Also scan its presence anywhere in the stack window.
                    if (s_last_conn_obj) {
                        uint32_t sp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                        uint32_t base = (sp > 0x4000) ? sp - 0x400 : 0; // a little below too
                        uint32_t top  = (sp & ~0xFFFu) + 0x6000;
                        int hits = 0;
                        for (uint32_t a = base; a < top && hits < 12; a += 4) {
                            uint32_t v = 0;
                            wg_blink_read_mem(engine->blink, a, &v, 4);
                            if (v == s_last_conn_obj) {
                                WG_LOGW(TAG, "  conn 0x%X on stack @0x%X (ESP%+d)",
                                        s_last_conn_obj, a, (int)(a - sp));
                                hits++;
                            }
                        }
                        if (!hits)
                            WG_LOGW(TAG, "  conn 0x%X NOT found in stack window", s_last_conn_obj);
                        // Dump the whole connection object so we can spot a
                        // member that SHOULD point to a sub-object (the send
                        // method's owner / 'this') but is NULL because we skipped
                        // the Windows init that populates it.
                        for (int row = 0; row < 8; row++) {
                            uint32_t hdr[4] = {0};
                            wg_blink_read_mem(engine->blink, s_last_conn_obj + row*16, hdr, 16);
                            WG_LOGW(TAG, "  conn[0x%02X]: %08X %08X %08X %08X",
                                    row*16, hdr[0],hdr[1],hdr[2],hdr[3]);
                        }
                    }
                    // Dump a wide code window before the send-method call so we
                    // can see where ESI ('this', which should be the connection
                    // 0x22D16000) was loaded as 0 — the getter/member/global that
                    // produced the NULL.
                    {
                        uint32_t ra1 = 0;
                        wg_blink_read_mem(engine->blink, ((uint32_t)wg_blink_get_reg(engine->blink,5)) + 4, &ra1, 4);
                        if (ra1 >= 0x401000u && ra1 < 0x800000u) {
                            uint32_t cs = ra1 - 0x150;
                            for (int row = 0; row < 21; row++) {
                                uint8_t cb[16] = {0};
                                wg_blink_read_mem(engine->blink, cs + row*16, cb, 16);
                                WG_LOGW(TAG, "  code 0x%X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                    cs+row*16, cb[0],cb[1],cb[2],cb[3],cb[4],cb[5],cb[6],cb[7],
                                    cb[8],cb[9],cb[10],cb[11],cb[12],cb[13],cb[14],cb[15]);
                            }
                        }
                        // esi (the connection 'this') is non-volatile but gets
                        // clobbered to 0 by the call to 0x4E9280 just before the
                        // send. Dump 0x4E9280 so we can see which Win32 call it
                        // makes (likely with a wrong stack-cleanup arg count that
                        // corrupts the saved esi) — that's OUR bug to fix.
                        for (int row = 0; row < 12; row++) {
                            uint8_t cb[16] = {0};
                            wg_blink_read_mem(engine->blink, 0x4E9280 + row*16, cb, 16);
                            WG_LOGW(TAG, "  fn4E9280+0x%02X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                row*16, cb[0],cb[1],cb[2],cb[3],cb[4],cb[5],cb[6],cb[7],
                                cb[8],cb[9],cb[10],cb[11],cb[12],cb[13],cb[14],cb[15]);
                        }
                    }
                    // socket 0 == NULL connection. On Windows the earlier
                    // this->m_socket read (this=NULL) faults into Steam's active
                    // __except, which abandons this send and recovers. Our
                    // zero-page map masked that read, so deliver the AV here
                    // instead: dispatch STATUS_ACCESS_VIOLATION to the fs:[0]
                    // chain. If a handler exists, skip the doomed send.
                    if (s_seh_trigger_send && !s_seh_active && s_seh_send_triggers < 64) {
                        s_seh_send_triggers++;
                        if (wg_raise_guest_exception(engine, 0xC0000005u, 0,
                                                     (uint32_t)ret_addr, false))
                            return true; // SEH dispatched; bypass the send
                    }
                }
                if (wg_winsock_handle(engine->winsock, ws_fn, args, &ws_ret, engine->blink)) {
                    ret_val = ws_ret;
                    // Real select() timeout (device-gated). Our winsock select polls
                    // without honoring the timeval, so a select-based poll loop that
                    // finds nothing ready busy-spins at JIT speed instead of waiting.
                    // Steam's main thread does exactly this after the handshake — it
                    // hammers select tens of thousands of times and never advances to
                    // spawn the request-sender. Honor the timeout: when nothing is
                    // ready and a timeout was given, cooperatively yield and only
                    // return 0 once the real wall-clock deadline passes, so the loop
                    // paces like real hardware (matching the slow-interpreter Mac path
                    // that does get Steam through). NULL timeval = block until ready.
                    // Only PACE the MAIN thread's (tid=1) select loop. Its
                    // message-pump busy-spin was what stalled the reactor and
                    // pacing it is what let the state machine advance to spawn the
                    // request-sender. Worker threads (the download workers) must
                    // recv as fast as possible — pacing their select loop throttles
                    // transfers enough to intermittently trip Steam's own download
                    // timeout (empty buffer -> "http error 0"). So workers keep the
                    // instant-select behaviour.
                    if (s_real_timeouts && strcmp(ws_fn, "select") == 0 &&
                        wg_sched_current_tid(engine->scheduler) == 1) {
                        WGThread *scur = wg_sched_current(engine->scheduler);
                        if (ws_ret != 0) {
                            if (scur && scur->wait_handle == WG_SELECT_WAIT) scur->wait_handle = 0;
                        } else if (scur) {
                            bool infinite = (args[4] == 0);
                            uint64_t tmo_ms = 0;
                            if (!infinite) {
                                uint32_t sec = 0, usec = 0;
                                wg_blink_read_mem(engine->blink, args[4], &sec, 4);
                                wg_blink_read_mem(engine->blink, args[4] + 4, &usec, 4);
                                tmo_ms = (uint64_t)sec * 1000 + usec / 1000;
                            }
                            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                            uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
                            if (scur->wait_handle != WG_SELECT_WAIT) {
                                scur->wait_handle = WG_SELECT_WAIT; scur->wait_start_ms = now;
                            }
                            bool expired = !infinite && (now - scur->wait_start_ms >= tmo_ms);
                            if (!expired) {
                                if (wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY))
                                    return true; // paced wait — re-poll select next turn
                                // alone: fall through, return 0 (nothing ready)
                            } else {
                                scur->wait_handle = 0; // deadline hit
                            }
                        }
                    }
                    // If the socket has an OVERLAPPED argument and the call succeeded,
                    // post an IOCP completion so GetQueuedCompletionStatus fires.
                    // ConnectEx: args[6]=overlapped, success = ret==1
                    if (strcmp(ws_fn, "ConnectEx") == 0 && ws_ret == 1 && args[6]) {
                        uint32_t ckey = iocp_comp_key(args[0]);
                        iocp_post(0, ckey, args[6]);
                        wg_sched_wake(engine->scheduler, WG_IOCP_HANDLE);
                        // Write STATUS_SUCCESS to OVERLAPPED so polling code sees completion.
                        uint32_t zero = 0;
                        wg_blink_write_mem(engine->blink, args[6] + 0, &zero, 4); // Internal
                        wg_blink_write_mem(engine->blink, args[6] + 4, &zero, 4); // InternalHigh
                        // Windows ConnectEx with IOCP returns FALSE + ERROR_IO_PENDING
                        wg_winsock_set_last_error(engine->winsock, 997 /*ERROR_IO_PENDING*/);
                        ret_val = 0; // FALSE = async
                    }
                    // WSASend: args[0]=socket, args[5]=overlapped, success = ret==0
                    else if ((strcmp(ws_fn, "WSASend") == 0 || strcmp(ws_fn, "WSASendTo") == 0)
                             && ws_ret == 0 && args[5]) {
                        uint32_t bytes = 0;
                        if (args[3]) wg_blink_read_mem(engine->blink, args[3], &bytes, 4);
                        uint32_t ckey = iocp_comp_key(args[0]);
                        iocp_post(bytes, ckey, args[5]);
                        wg_sched_wake(engine->scheduler, WG_IOCP_HANDLE);
                        uint32_t zero = 0;
                        wg_blink_write_mem(engine->blink, args[5] + 0, &zero, 4);
                        wg_blink_write_mem(engine->blink, args[5] + 4, &bytes, 4);
                        wg_winsock_set_last_error(engine->winsock, 997);
                        ret_val = (uint64_t)(uint32_t)-1; // SOCKET_ERROR
                    }
                    // WSARecv: args[0]=socket, args[5]=overlapped, success = ret==0
                    else if ((strcmp(ws_fn, "WSARecv") == 0 || strcmp(ws_fn, "WSARecvFrom") == 0)
                             && ws_ret == 0 && args[5]) {
                        uint32_t bytes = 0;
                        if (args[3]) wg_blink_read_mem(engine->blink, args[3], &bytes, 4);
                        uint32_t ckey = iocp_comp_key(args[0]);
                        iocp_post(bytes, ckey, args[5]);
                        wg_sched_wake(engine->scheduler, WG_IOCP_HANDLE);
                        uint32_t zero = 0;
                        wg_blink_write_mem(engine->blink, args[5] + 0, &zero, 4);
                        wg_blink_write_mem(engine->blink, args[5] + 4, &bytes, 4);
                        wg_winsock_set_last_error(engine->winsock, 997);
                        ret_val = (uint64_t)(uint32_t)-1; // SOCKET_ERROR
                    }
                } else {
                    WG_LOGW(TAG, "WS2_32 unhandled: %s", ws_fn);
                }
            }
        }

        // WINHTTP.dll dispatch
        if (engine->winhttp && entry && entry->dll_name &&
            strcasecmp(entry->dll_name, "winhttp.dll") == 0) {
            uint64_t wh_ret = 0;
            if (wg_winhttp_handle(engine->winhttp, fn, args, &wh_ret, engine->blink)) {
                ret_val = wh_ret;
            } else {
                WG_LOGW(TAG, "WINHTTP unhandled: %s", fn);
            }
        }

        // SECUR32.dll / SCHANNEL dispatch — TLS via SecureTransport
        if (engine->schannel && entry && entry->dll_name &&
            (strcasecmp(entry->dll_name, "secur32.dll") == 0 ||
             strcasecmp(entry->dll_name, "sspicli.dll") == 0 ||
             strcasecmp(entry->dll_name, "schannel.dll") == 0)) {
            uint64_t sc_ret = 0;
            if (wg_schannel_handle(engine->schannel, fn, args, &sc_ret, engine->blink)) {
                ret_val = sc_ret;
            } else {
                WG_LOGW(TAG, "SECUR32 unhandled: %s", fn);
            }
        }

        // WININET.dll dispatch — connectivity checks + HTTP via WinHTTP backend
        if (entry && entry->dll_name &&
            strcasecmp(entry->dll_name, "wininet.dll") == 0) {
            WG_LOGI(TAG, "WININET: %s", fn);
            if (strcmp(fn, "InternetGetConnectedState") == 0 ||
                strcmp(fn, "InternetGetConnectedStateExW") == 0) {
                // Report "connected via LAN"
                if (args[0]) {
                    uint32_t flags = 0x01; // INTERNET_CONNECTION_LAN
                    wg_blink_write_mem(engine->blink, args[0], &flags, 4);
                }
                ret_val = 1; // TRUE = connected
            } else if (strcmp(fn, "InternetAttemptConnect") == 0) {
                ret_val = 0; // ERROR_SUCCESS
            } else if (strcmp(fn, "InternetCheckConnectionA") == 0 ||
                       strcmp(fn, "InternetCheckConnectionW") == 0) {
                ret_val = 1; // TRUE = connected
            } else if (strcmp(fn, "InternetOpenA") == 0 ||
                       strcmp(fn, "InternetOpenW") == 0) {
                uint64_t wh_ret = 0;
                const char *mapped_fn = "WinHttpOpen";
                if (engine->winhttp && wg_winhttp_handle(engine->winhttp, mapped_fn, args, &wh_ret, engine->blink))
                    ret_val = wh_ret;
                else
                    ret_val = 0x5000; // fake session handle
            } else if (strcmp(fn, "InternetConnectA") == 0 ||
                       strcmp(fn, "InternetConnectW") == 0) {
                uint64_t wh_ret = 0;
                if (engine->winhttp && wg_winhttp_handle(engine->winhttp, "WinHttpConnect", args, &wh_ret, engine->blink))
                    ret_val = wh_ret;
                else
                    ret_val = 0x5001;
            } else if (strcmp(fn, "InternetCloseHandle") == 0) {
                uint64_t wh_ret = 0;
                if (engine->winhttp) wg_winhttp_handle(engine->winhttp, "WinHttpCloseHandle", args, &wh_ret, engine->blink);
                ret_val = 1;
            } else if (strcmp(fn, "InternetSetStatusCallbackA") == 0 ||
                       strcmp(fn, "InternetSetStatusCallbackW") == 0) {
                ret_val = 0; // NULL = no previous callback
            }
        }

        // IPHLPAPI.dll dispatch — network adapter enumeration
        if (entry && entry->dll_name &&
            strcasecmp(entry->dll_name, "IPHLPAPI.DLL") == 0) {
            WG_LOGI(TAG, "IPHLPAPI: %s", fn);
            if (strcmp(fn, "GetAdaptersAddresses") == 0) {
                // GetAdaptersAddresses(Family, Flags, Reserved, AdapterAddresses, SizePointer)
                // args[3]=pAdapterAddresses, args[4]=pSizePointer
                // Build a minimal IP_ADAPTER_ADDRESSES at args[3]
                uint32_t size_ptr = args[4];
                uint32_t buf_ptr = args[3];
                uint32_t avail = 0;
                if (size_ptr) wg_blink_read_mem(engine->blink, size_ptr, &avail, 4);
                uint32_t needed = 376; // approximate IP_ADAPTER_ADDRESSES size (32-bit)
                if (!buf_ptr || avail < needed) {
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 111; // ERROR_BUFFER_OVERFLOW — tell caller how much to allocate
                } else {
                    // Write a fake adapter: Ethernet, status UP, has IPv4
                    uint8_t aa[376];
                    memset(aa, 0, sizeof(aa));
                    // Length (offset 0)
                    uint32_t len = needed;
                    memcpy(aa + 0, &len, 4);
                    // IfIndex (offset 4)
                    uint32_t idx = 1;
                    memcpy(aa + 4, &idx, 4);
                    // Next (offset 8) = NULL (only one adapter)
                    // AdapterName (offset 12) = pointer (we'll skip string pointers)
                    // IfType (offset 260) = 6 (IF_TYPE_ETHERNET_CSMACD)
                    uint32_t iftype = 6;
                    memcpy(aa + 260, &iftype, 4);
                    // OperStatus (offset 264) = 1 (IfOperStatusUp)
                    uint32_t oper = 1;
                    memcpy(aa + 264, &oper, 4);
                    wg_blink_write_mem(engine->blink, buf_ptr, aa, needed);
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 0; // NO_ERROR
                }
            } else if (strcmp(fn, "GetAdaptersInfo") == 0) {
                // GetAdaptersInfo(pAdapterInfo, pOutBufLen)
                uint32_t size_ptr = args[1];
                uint32_t buf_ptr = args[0];
                uint32_t avail = 0;
                if (size_ptr) wg_blink_read_mem(engine->blink, size_ptr, &avail, 4);
                uint32_t needed = 640; // IP_ADAPTER_INFO size
                if (!buf_ptr || avail < needed) {
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 111; // ERROR_BUFFER_OVERFLOW
                } else {
                    uint8_t ai[640];
                    memset(ai, 0, sizeof(ai));
                    // Type (offset 260) = MIB_IF_TYPE_ETHERNET (6)
                    uint32_t iftype = 6;
                    memcpy(ai + 260, &iftype, 4);
                    // IpAddressList.IpAddress (offset 432) = "192.168.1.100"
                    const char *ip = "192.168.1.100";
                    memcpy(ai + 432, ip, strlen(ip));
                    // IpAddressList.IpMask (offset 448) = "255.255.255.0"
                    const char *mask = "255.255.255.0";
                    memcpy(ai + 448, mask, strlen(mask));
                    // GatewayList.IpAddress (offset 472) = "192.168.1.1"
                    const char *gw = "192.168.1.1";
                    memcpy(ai + 472, gw, strlen(gw));
                    wg_blink_write_mem(engine->blink, buf_ptr, ai, needed);
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 0; // NO_ERROR
                }
            } else if (strcmp(fn, "GetNetworkParams") == 0) {
                // GetNetworkParams(pFixedInfo, pOutBufLen)
                uint32_t size_ptr = args[1];
                uint32_t buf_ptr = args[0];
                uint32_t avail = 0;
                if (size_ptr) wg_blink_read_mem(engine->blink, size_ptr, &avail, 4);
                uint32_t needed = 312; // FIXED_INFO size
                if (!buf_ptr || avail < needed) {
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 111;
                } else {
                    uint8_t fi[312];
                    memset(fi, 0, sizeof(fi));
                    // DnsServerList.IpAddress (offset 260) = "8.8.8.8"
                    const char *dns = "8.8.8.8";
                    memcpy(fi + 260, dns, strlen(dns));
                    wg_blink_write_mem(engine->blink, buf_ptr, fi, needed);
                    if (size_ptr) wg_blink_write_mem(engine->blink, size_ptr, &needed, 4);
                    ret_val = 0;
                }
            } else if (strcmp(fn, "GetBestInterface") == 0) {
                // GetBestInterface(dwDestAddr, pdwBestIfIndex)
                if (args[1]) {
                    uint32_t ifidx = 1;
                    wg_blink_write_mem(engine->blink, args[1], &ifidx, 4);
                }
                ret_val = 0; // NO_ERROR
            }
        }
    }

    // Stdcall (32-bit): callee pops return address + all arguments.
    // x64: caller-clean — the callee pops ONLY the return address; popping
    // num_args*8 here would corrupt the caller's frame on every call.
    int num_args = entry ? entry->num_args : 0;
    uint64_t new_rsp = is_32bit ? rsp + ptr_size + (num_args * ptr_size)
                                : rsp + 8;
    wg_blink_set_reg(engine->blink, 4, new_rsp); // RSP
    wg_blink_set_rip(engine->blink, ret_addr);
    wg_blink_set_reg(engine->blink, 0, ret_val); // EAX = return value

    wg_call_ring_push(entry ? entry->func_name : "?", ret_val);
    return true;
}

bool wg_engine_init(WGEngine *engine) {
    WG_LOGI(TAG, "Initializing translation engine...");

    engine->memory = wg_memory_create(0x100000000ULL);
    if (!engine->memory) {
        WG_LOGE(TAG, "Failed to create virtual memory space");
        engine->state = WG_ENGINE_ERROR;
        return false;
    }
    WG_LOGI(TAG, "Virtual memory: 4GB address space created");

    engine->cpu = wg_x86_state_create();
    if (!engine->cpu) {
        WG_LOGE(TAG, "Failed to create CPU state");
        engine->state = WG_ENGINE_ERROR;
        return false;
    }
    WG_LOGI(TAG, "Builtin x86-64 interpreter ready (fallback)");

    // Blink VM creation is deferred until we know if the PE is 32 or 64-bit
    engine->blink = NULL;
    engine->backend = WG_BACKEND_BLINK;

    engine->dll_mapper = wg_dll_mapper_create();
    if (!engine->dll_mapper) {
        WG_LOGE(TAG, "Failed to create DLL mapper");
        engine->state = WG_ENGINE_ERROR;
        return false;
    }
    wg_dll_mapper_register_defaults(engine->dll_mapper);
    WG_LOGI(TAG, "Win32 DLL mapper initialized");

    // Blink init is deferred — thunks will be mapped when a PE is loaded

    WG_LOGI(TAG, "NSIS data mode: %s",
            WG_NSIS_PREFILL_HACK ? "native LzmaDec prefill (workaround)"
                                 : "guest in-VM decode (prefill DISABLED)");
    WG_LOGI(TAG, "Engine ready");
    return true;
}

static bool ensure_blink_vm(WGEngine *engine, bool is_64bit) {
    // Always create a fresh VM for each PE load
    if (engine->blink) {
        wg_blink_destroy(engine->blink);
        engine->blink = NULL;
        engine->thunks_mapped = false;
    }

    if (is_64bit) {
        engine->blink = wg_blink_create();
    } else {
        engine->blink = wg_blink_create32();
    }

    if (!engine->blink) {
        WG_LOGW(TAG, "Blink unavailable, falling back to builtin");
        engine->backend = WG_BACKEND_BUILTIN;
        return false;
    }

    // Warm-up: NOP then HLT (not RET — a RET pops from an unset stack, which
    // faults; harmless in the software-MMU path but an uncaught host SIGSEGV
    // under linear memory / JIT). HLT cleanly returns WG_BLINK_HALT.
    uint8_t warmup[] = { 0x90, 0xF4 };
    wg_blink_load_code(engine->blink, 0x3F0000, warmup, sizeof(warmup), 0x3F0000);
    WGBlinkResult wr = wg_blink_run(engine->blink, 10);
    WG_LOGI(TAG, "Blink JIT warm-up: %s",
            wr == WG_BLINK_HALT ? "OK" : "absorbed first-run init");

    map_thunks_to_blink(engine);
    return true;
}

// Build a minimal x64 TEB/PEB + TLS and point GS at the TEB. x64 Windows keeps
// the TEB at GS (not FS) with 64-bit-wide fields at different offsets than the
// 32-bit TEB. The MSVC CRT and UE4 read gs:[0x30] (TEB self), gs:[0x58]
// (ThreadLocalStoragePointer) and gs:[0x60] (PEB). In particular UE4's
// GCreateMalloc reads a __declspec(thread) guard via gs:[0x58][_tls_index] — so
// without a real GS base + TLS array the global allocator (GMalloc) is never
// created and every allocation returns null, which is exactly what stalled
// static init. Also sets the PE's static TLS block in slot 0.
static void wg_setup_win32_teb64(WGEngine *engine) {
    WGPEImage *pe = engine->pe_image;
    void *bl = engine->blink;
    uint32_t teb = wg_guest_alloc(engine, 0x2000);   // x64 TEB is ~0x1800
    uint32_t peb = wg_guest_alloc(engine, 0x1000);
    uint32_t tls_array = wg_guest_alloc(engine, 0x400);   // 128 slots * 8
    if (!teb || !peb || !tls_array) return;

    uint32_t image_base  = (uint32_t)pe->image_base;
    uint64_t stack_base  = 0x7FFF0000, stack_limit = 0x7EFF0000;
    uint64_t v64; uint32_t v32;

    // NT_TIB (x64, 8-byte fields)
    v64 = 0;                wg_blink_write_mem(bl, teb + 0x00, &v64, 8); // ExceptionList
    wg_blink_write_mem(bl, teb + 0x08, &stack_base, 8);                  // StackBase
    wg_blink_write_mem(bl, teb + 0x10, &stack_limit, 8);                 // StackLimit
    v64 = teb;              wg_blink_write_mem(bl, teb + 0x30, &v64, 8);  // Self (gs:[0x30])
    v64 = 0x1000;           wg_blink_write_mem(bl, teb + 0x40, &v64, 8);  // ClientId.UniqueProcess
    v64 = 0x1004;           wg_blink_write_mem(bl, teb + 0x48, &v64, 8);  // ClientId.UniqueThread
    v64 = tls_array;        wg_blink_write_mem(bl, teb + 0x58, &v64, 8);  // ThreadLocalStoragePointer
    v64 = peb;              wg_blink_write_mem(bl, teb + 0x60, &v64, 8);  // PEB (gs:[0x60])
    v32 = 0;                wg_blink_write_mem(bl, teb + 0x68, &v32, 4);  // LastErrorValue

    // PEB (x64 offsets)
    uint8_t bd = 0;         wg_blink_write_mem(bl, peb + 0x02, &bd, 1);   // BeingDebugged
    v64 = image_base;       wg_blink_write_mem(bl, peb + 0x10, &v64, 8);  // ImageBaseAddress
    v64 = 0x00D00000;       wg_blink_write_mem(bl, peb + 0x30, &v64, 8);  // ProcessHeap
    v32 = 10;               wg_blink_write_mem(bl, peb + 0x118, &v32, 4); // OSMajorVersion
    v32 = 0;                wg_blink_write_mem(bl, peb + 0x11C, &v32, 4); // OSMinorVersion
    uint16_t bld = 19045;   wg_blink_write_mem(bl, peb + 0x120, &bld, 2); // OSBuildNumber
    v32 = 2;                wg_blink_write_mem(bl, peb + 0x124, &v32, 4); // OSPlatformId (NT)

    // PE static TLS (IMAGE_TLS_DIRECTORY64 — 8-byte fields). Allocate + copy the
    // template into a data block and put its pointer in TLS array slot 0, with
    // the module's _tls_index (at AddressOfIndex) set to 0 so gs:[0x58][0] hits it.
    uint32_t tls_data = 0;
    if (pe->tls_rva) {
        uint64_t raw_start = 0, raw_end = 0, addr_index = 0; uint32_t zerofill = 0;
        wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x00, &raw_start, 8);
        wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x08, &raw_end, 8);
        wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x10, &addr_index, 8);
        wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x20, &zerofill, 4);
        uint32_t tpl = (raw_end > raw_start) ? (uint32_t)(raw_end - raw_start) : 0;
        uint32_t data_size = tpl + zerofill; if (!data_size) data_size = 8;
        tls_data = wg_guest_alloc(engine, data_size);
        if (tls_data && tpl) {
            uint8_t *tmp = malloc(tpl);
            if (tmp) { wg_blink_read_mem(bl, (uint32_t)raw_start, tmp, tpl);
                       wg_blink_write_mem(bl, tls_data, tmp, tpl); free(tmp); }
        }
        if (addr_index) { uint32_t z = 0; wg_blink_write_mem(bl, (uint32_t)addr_index, &z, 4); }
        WG_LOGI(TAG, "TLS64: data@0x%X size 0x%X (index 0)", tls_data, data_size);
    }
    if (!tls_data) tls_data = wg_guest_alloc(engine, 0x100); // valid zeroed block regardless
    v64 = tls_data; wg_blink_write_mem(bl, tls_array, &v64, 8);  // array[0]

    wg_blink_set_gs_base(engine->blink, teb);
    s_main_teb = teb;
    WG_LOGI(TAG, "Win32 x64 TEB@0x%X PEB@0x%X TLS@0x%X gs-base set", teb, peb, tls_array);
}

// Build a minimal 32-bit TEB/PEB + TLS and point FS at the TEB. MSVC's CRT reads
// fs:[0x18] (TEB self), fs:[0x2C] (TLS pointer) and fs:[0x30] (PEB) during
// startup, plus PEB->ProcessHeap / OS version fields — without these a real
// app (steam.exe) faults in CRT init. NSIS never touched FS so it ran without.
static void wg_setup_win32_teb(WGEngine *engine) {
    WGPEImage *pe = engine->pe_image;
    if (!pe) return;
    if (pe->is_64bit) { wg_setup_win32_teb64(engine); return; }

    uint32_t teb = wg_guest_alloc(engine, 0x1000);
    uint32_t peb = wg_guest_alloc(engine, 0x1000);
    uint32_t tls_array = wg_guest_alloc(engine, 0x400);   // 256 TLS slots
    if (!teb || !peb || !tls_array) return;

    uint32_t image_base  = (uint32_t)pe->image_base;
    uint32_t stack_base  = 0x7FFF0000, stack_limit = 0x7EFF0000;
    uint32_t heap_handle = 0x00D00000;   // fake ProcessHeap handle
    uint32_t v;

    // TEB (NT_TIB first)
    v = 0xFFFFFFFF; wg_blink_write_mem(engine->blink, teb + 0x00, &v, 4); // ExceptionList
    wg_blink_write_mem(engine->blink, teb + 0x04, &stack_base, 4);        // StackBase
    wg_blink_write_mem(engine->blink, teb + 0x08, &stack_limit, 4);       // StackLimit
    wg_blink_write_mem(engine->blink, teb + 0x18, &teb, 4);              // Self
    v = 0x1000; wg_blink_write_mem(engine->blink, teb + 0x20, &v, 4);     // ClientId.UniqueProcess
    v = 0x1004; wg_blink_write_mem(engine->blink, teb + 0x24, &v, 4);     // ClientId.UniqueThread
    wg_blink_write_mem(engine->blink, teb + 0x2C, &tls_array, 4);         // ThreadLocalStoragePointer
    wg_blink_write_mem(engine->blink, teb + 0x30, &peb, 4);               // ProcessEnvironmentBlock
    v = 0; wg_blink_write_mem(engine->blink, teb + 0x34, &v, 4);          // LastErrorValue

    // PEB
    uint8_t bd = 0; wg_blink_write_mem(engine->blink, peb + 0x02, &bd, 1);   // BeingDebugged
    wg_blink_write_mem(engine->blink, peb + 0x08, &image_base, 4);           // ImageBaseAddress
    wg_blink_write_mem(engine->blink, peb + 0x18, &heap_handle, 4);          // ProcessHeap
    v = wg_ncpu(); wg_blink_write_mem(engine->blink, peb + 0x64, &v, 4);     // NumberOfProcessors (WG_NCPU; 1 steers Steam to sync I/O, games need >1)
    v = 10;    wg_blink_write_mem(engine->blink, peb + 0xA4, &v, 4);         // OSMajorVersion
    v = 0;     wg_blink_write_mem(engine->blink, peb + 0xA8, &v, 4);         // OSMinorVersion
    uint16_t bld = 19045; wg_blink_write_mem(engine->blink, peb + 0xAC, &bld, 2); // OSBuildNumber
    v = 2;     wg_blink_write_mem(engine->blink, peb + 0xB0, &v, 4);         // OSPlatformId (NT)

    // PE TLS directory (IMAGE_TLS_DIRECTORY32; fields are VAs at the preferred
    // base, where the main exe is loaded). Allocate the TLS data block, copy the
    // template, put its pointer in slot 0 of the TLS array, and write index 0.
    if (pe->tls_rva) {
        uint32_t tls[6] = {0};
        wg_blink_read_mem(engine->blink, image_base + pe->tls_rva, tls, 24);
        uint32_t raw_start = tls[0], raw_end = tls[1], addr_index = tls[2];
        uint32_t zerofill = tls[4];
        uint32_t tpl = (raw_end > raw_start) ? raw_end - raw_start : 0;
        uint32_t data_size = tpl + zerofill; if (!data_size) data_size = 4;
        uint32_t tls_data = wg_guest_alloc(engine, data_size);
        if (tls_data && tpl) {
            uint8_t *tmp = malloc(tpl);
            if (tmp) {
                wg_blink_read_mem(engine->blink, raw_start, tmp, tpl);
                wg_blink_write_mem(engine->blink, tls_data, tmp, tpl);
                free(tmp);
            }
        }
        wg_blink_write_mem(engine->blink, tls_array, &tls_data, 4);   // array[0]
        if (addr_index) { uint32_t z = 0; wg_blink_write_mem(engine->blink, addr_index, &z, 4); }
        WG_LOGI(TAG, "TLS: data@0x%X size 0x%X (index 0)", tls_data, data_size);
    }

    wg_blink_set_fs_base(engine->blink, teb);
    s_main_teb = teb;
    WG_LOGI(TAG, "Win32 TEB@0x%X PEB@0x%X fs-base set", teb, peb);
}

// Allocate a TEB for a newly-created thread. Each Windows thread MUST have its
// own TEB: it carries the thread's stack bounds, ClientId, LastError, and TLS
// pointer. Sharing the main TEB (the old behavior) corrupted per-thread state.
// Shares the process PEB (read from the main TEB) and re-instantiates the static
// TLS data block so __declspec(thread) data is per-thread too.
static uint32_t wg_alloc_thread_teb(WGEngine *engine, uint32_t stack_base,
                                    uint32_t stack_limit, uint32_t tid) {
    WGPEImage *pe = engine->pe_image;
    if (!pe || !s_main_teb) return 0;

    if (pe->is_64bit) {
        // x64 per-thread TEB (GS-based, 8-byte fields). Shares the process PEB.
        void *bl = engine->blink;
        uint32_t teb = wg_guest_alloc(engine, 0x2000);
        uint32_t tls_array = wg_guest_alloc(engine, 0x400);
        if (!teb || !tls_array) return 0;
        uint64_t peb = 0;
        wg_blink_read_mem(bl, s_main_teb + 0x60, &peb, 8);
        uint64_t v64;
        v64 = 0;              wg_blink_write_mem(bl, teb + 0x00, &v64, 8); // ExceptionList
        v64 = stack_base;     wg_blink_write_mem(bl, teb + 0x08, &v64, 8); // StackBase
        v64 = stack_limit;    wg_blink_write_mem(bl, teb + 0x10, &v64, 8); // StackLimit
        v64 = teb;            wg_blink_write_mem(bl, teb + 0x30, &v64, 8); // Self
        v64 = 0x1000;         wg_blink_write_mem(bl, teb + 0x40, &v64, 8); // ClientId.Process
        v64 = tid;            wg_blink_write_mem(bl, teb + 0x48, &v64, 8); // ClientId.Thread
        v64 = tls_array;      wg_blink_write_mem(bl, teb + 0x58, &v64, 8); // TLS pointer
        v64 = peb;            wg_blink_write_mem(bl, teb + 0x60, &v64, 8); // PEB
        uint32_t z = 0;       wg_blink_write_mem(bl, teb + 0x68, &z, 4);   // LastError
        // Per-thread static TLS block (__declspec(thread)) from the TLS dir.
        uint32_t tls_data = 0;
        if (pe->tls_rva) {
            uint32_t image_base = (uint32_t)pe->image_base;
            uint64_t raw_start = 0, raw_end = 0; uint32_t zerofill = 0;
            wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x00, &raw_start, 8);
            wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x08, &raw_end, 8);
            wg_blink_read_mem(bl, image_base + pe->tls_rva + 0x20, &zerofill, 4);
            uint32_t tpl = (raw_end > raw_start) ? (uint32_t)(raw_end - raw_start) : 0;
            uint32_t data_size = tpl + zerofill; if (!data_size) data_size = 8;
            tls_data = wg_guest_alloc(engine, data_size);
            if (tls_data && tpl) { uint8_t *tmp = malloc(tpl);
                if (tmp) { wg_blink_read_mem(bl, (uint32_t)raw_start, tmp, tpl);
                           wg_blink_write_mem(bl, tls_data, tmp, tpl); free(tmp); } }
        }
        if (!tls_data) tls_data = wg_guest_alloc(engine, 0x100);
        v64 = tls_data; wg_blink_write_mem(bl, tls_array, &v64, 8);
        WG_LOGI(TAG, "Thread TEB64@0x%X tid=0x%X stack=0x%X-0x%X", teb, tid, stack_limit, stack_base);
        return teb;
    }

    uint32_t teb = wg_guest_alloc(engine, 0x1000);
    uint32_t tls_array = wg_guest_alloc(engine, 0x400);   // 256 TLS slots
    if (!teb || !tls_array) return 0;

    // Share the process PEB with the main thread.
    uint32_t peb = 0;
    wg_blink_read_mem(engine->blink, s_main_teb + 0x30, &peb, 4);

    uint32_t v;
    v = 0xFFFFFFFF; wg_blink_write_mem(engine->blink, teb + 0x00, &v, 4); // ExceptionList
    wg_blink_write_mem(engine->blink, teb + 0x04, &stack_base, 4);        // StackBase
    wg_blink_write_mem(engine->blink, teb + 0x08, &stack_limit, 4);       // StackLimit
    wg_blink_write_mem(engine->blink, teb + 0x18, &teb, 4);               // Self
    v = 0x1000; wg_blink_write_mem(engine->blink, teb + 0x20, &v, 4);     // ClientId.UniqueProcess
    wg_blink_write_mem(engine->blink, teb + 0x24, &tid, 4);               // ClientId.UniqueThread
    wg_blink_write_mem(engine->blink, teb + 0x2C, &tls_array, 4);         // ThreadLocalStoragePointer
    wg_blink_write_mem(engine->blink, teb + 0x30, &peb, 4);               // PEB
    v = 0; wg_blink_write_mem(engine->blink, teb + 0x34, &v, 4);          // LastErrorValue

    // Per-thread static TLS data block (__declspec(thread)). Re-instantiate from
    // the PE TLS directory template so each thread has its own copy.
    if (pe->tls_rva) {
        uint32_t image_base = (uint32_t)pe->image_base;
        uint32_t tls[6] = {0};
        wg_blink_read_mem(engine->blink, image_base + pe->tls_rva, tls, 24);
        uint32_t raw_start = tls[0], raw_end = tls[1], zerofill = tls[4];
        uint32_t tpl = (raw_end > raw_start) ? raw_end - raw_start : 0;
        uint32_t data_size = tpl + zerofill; if (!data_size) data_size = 4;
        uint32_t tls_data = wg_guest_alloc(engine, data_size);
        if (tls_data && tpl) {
            uint8_t *tmp = malloc(tpl);
            if (tmp) {
                wg_blink_read_mem(engine->blink, raw_start, tmp, tpl);
                wg_blink_write_mem(engine->blink, tls_data, tmp, tpl);
                free(tmp);
            }
        }
        wg_blink_write_mem(engine->blink, tls_array, &tls_data, 4);   // array[0]
    }

    WG_LOGI(TAG, "Thread TEB@0x%X tid=0x%X stack=0x%X-0x%X", teb, tid, stack_limit, stack_base);
    return teb;
}

// ── Real-threads worker (docs/threads_rearchitect.md P2c) ────────────────────
// Each guest thread is a real pthread driving its OWN blink Machine over the
// shared System. Guest code runs concurrently (blink's System locks cover shared
// memory); Win32 thunks serialise on s_thunk_lock so the many shared s_* statics
// + file/socket/heap tables stay safe.

typedef struct {
    WGEngine *engine;
    void    *machine;    // from wg_blink_new_thread_machine
    uint32_t start;      // guest entry point
    uint32_t param;      // thread argument
    uint32_t stack_top;  // high end of the guest stack
    uint32_t teb;        // guest TEB
    uint32_t thread_h;   // wg_sync THREAD handle (for join/WFSO)
    uint32_t tid;        // guest thread id
    int      tls_slot;   // per-thread index into s_tls_slots/s_fls_slots
    uint32_t flags;      // CreateThread flags (bit 0x4 = CREATE_SUSPENDED)
} WGWorkerArgs;

// CONSTRUCT-PIN (WG_CONSTRUCT_PIN): the UObject register fn at runtime 0xA58060
// PUBLISHES the freshly-allocated object (0xA580B5 `mov [r14],rax`) BEFORE it
// constructs/links the object's fields — a recursion-safety pattern that is only
// correct SINGLE-THREADED. Under real threads a WORKER reads the early-published,
// half-built object via the LOCK-FREE getter 0xB40690 and walks its not-yet-linked
// TIntrusiveLinkedList (nodes init to next=self) -> the self-loop / O(N^2) name scan
// that blocks the render. Fix: while ANY guest thread is inside 0xA58060's call tree
// (its code range appears as a return address on the thread's stack, or the RIP is
// in it), PIN the GIL (reuse s_spin_pin) so no other guest thread runs during the
// construction — honoring the single-threaded assumption. Scoped to 0xA58060 ONLY,
// so it does NOT freeze the 2.618M coordination (which is different code).
static _Thread_local int s_construct_win = -1;
static signed char s_construct_on = -1;
static uint64_t s_construct_lo = 0, s_construct_hi = 0;
static void wg_update_construct_pin(WGEngine *engine) {
    if (s_construct_on < 0) {
        s_construct_on = getenv("WG_CONSTRUCT_PIN") ? 1 : 0;
        const char *lo = getenv("WG_CONSTRUCT_LO"), *hi = getenv("WG_CONSTRUCT_HI");
        s_construct_lo = lo ? strtoull(lo, 0, 16) : 0xA58060ULL;
        s_construct_hi = hi ? strtoull(hi, 0, 16) : 0xA58200ULL;
    }
    if (!s_construct_on || !s_use_real_threads) return;
    uint64_t rip = wg_blink_get_rip(engine->blink);
    int in_c = (rip >= s_construct_lo && rip <= s_construct_hi);
    if (!in_c) {
        uint32_t sp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
        for (int w = 0; w < 200; w++) {
            uint32_t v = 0; wg_blink_read_mem(engine->blink, sp + (uint32_t)w * 8, &v, 4);
            if ((uint64_t)v >= s_construct_lo && (uint64_t)v <= s_construct_hi) { in_c = 1; break; }
        }
    }
    if (in_c) {
        if (s_construct_win < 0) { const char *e = getenv("WG_SPIN_WINDOW"); s_construct_win = e ? atoi(e) : 24; }
        s_spin_pin = s_construct_win;   // dir_unlock pins the GIL while s_spin_pin > 0
    }
}

static void *wg_worker_thread_entry(void *arg) {
    WGWorkerArgs wa = *(WGWorkerArgs *)arg;
    free(arg);
    WGEngine *engine = wa.engine;

    s_cur_guest_tid = wa.tid;
    s_tls_slot = wa.tls_slot;             // this pthread's TLS/FLS shadow slot
    wg_blink_adopt_machine(wa.machine);   // g_machine = this pthread's Machine

    // Seed the entry frame UNDER THE GIL: wg_blink_write_mem walks the shared
    // System page tables (CopyToUser), which races with other threads executing
    // if unlocked — a corrupted return-address write makes the thread proc `ret`
    // to a garbage rip (0x1) and crash the instant it starts.
    wg_thunk_lock();
    bool g64 = engine->pe_image && engine->pe_image->is_64bit;
    if (g64) {
        // x64 thread proc: DWORD WINAPI Proc(LPVOID param) — param in RCX, GS at
        // the TEB, [rsp]=return address 0 (a `ret` lands at rip 0 = exit
        // sentinel). rsp%16==8 after the pushed return address; leave 32B shadow.
        uint32_t sp = (wa.stack_top - 0x100) & ~0xFu;
        sp -= 8;
        uint64_t zero64 = 0;
        wg_blink_write_mem(engine->blink, sp, &zero64, 8);
        wg_blink_set_reg(engine->blink, 4, sp);            // RSP
        wg_blink_set_reg(engine->blink, 1, wa.param);      // RCX = param
        wg_blink_set_gs_base(engine->blink, wa.teb);
        wg_blink_set_rip(engine->blink, wa.start);
    } else {
        // Seed the stdcall entry frame: [esp]=return addr 0 (a plain `ret` from
        // the thread proc lands at rip 0 = our exit sentinel), [esp+4]=arg.
        uint32_t sp = (wa.stack_top - 0x100) & ~0xFu;
        uint32_t zero = 0;
        wg_blink_write_mem(engine->blink, sp,     &zero,     4);
        wg_blink_write_mem(engine->blink, sp + 4, &wa.param, 4);
        wg_blink_set_reg(engine->blink, 4, sp);       // ESP
        wg_blink_set_reg(engine->blink, 5, sp);       // EBP
        wg_blink_set_fs_base(engine->blink, wa.teb);
        wg_blink_set_rip(engine->blink, wa.start);
    }
    wg_thunk_unlock();

    // CREATE_SUSPENDED: park until ResumeThread(handle). UE4 fills in this
    // thread's context between CreateThread and ResumeThread; running before
    // that reads garbage and crashes.
    if (wa.flags & 0x4u) {
        WG_LOGI(TAG, "[realthr] worker tid=0x%X SUSPENDED — waiting for ResumeThread", wa.tid);
        wg_resume_gate_wait(wa.thread_h);
    }

    WG_LOGI(TAG, "[realthr] worker tid=0x%X start=0x%X rsp=0x%llX teb=0x%X %s running",
            wa.tid, wa.start, (unsigned long long)wg_blink_get_reg(engine->blink, 4),
            wa.teb, g64 ? "x64" : "x86");

    uint32_t exit_code = 0;
    // WG_BLOCK_WORKERS gate config (see s_main_rip_stall). Only the UE4 task-graph
    // pool workers (start 0x9FFC70) are gated; the named/render thread (0x9EA6B0)
    // and any other threads run normally.
    static signed char s_blockw = -1;
    if (s_blockw < 0) s_blockw = getenv("WG_BLOCK_WORKERS") ? 1 : 0;
    long blockw_stall = getenv("WG_BLOCKW_STALL") ? atol(getenv("WG_BLOCKW_STALL")) : 8;
    for (;;) {
        // Park this pool worker while the main is making forward progress, so boot
        // registration/loading runs single-threaded (no construction race). Holds NO
        // GIL while parked. Released the moment the main's RIP pins (waiting for us).
        // WG_BLOCKW_STAYON: keep gating through BOTH registration phases (the early
        // engine-init AND the post-swapchain level load) instead of ungating at
        // ShowWindow — turning off at ShowWindow re-exposed the level-load
        // registration to the construction race (the O(N^2) scan returned post-
        // swapchain). With STAYON, workers stay gated whenever the main is making
        // progress and are released only via the RIP-stall/block valve (so the
        // swapchain's brief worker needs are still met). Lower WG_BLOCKW_STALL makes
        // the valve fire sooner (better for reaching the swapchain).
        static signed char s_stayon = -1;
        if (s_stayon < 0) s_stayon = getenv("WG_BLOCKW_STAYON") ? 1 : 0;
        int gate_off = s_stayon ? 0 : s_past_init;
        if (s_blockw && !gate_off && wa.start == 0x9FFC70) {
            // Park until the main genuinely BLOCKS (s_main_blocked — waiting on a
            // worker) OR, as a rare fallback, its RIP stalls for blockw_stall slices
            // (keep this HIGH so it doesn't misfire on registration/scan compute).
            while (!s_main_blocked && s_main_rip_stall < (unsigned)blockw_stall
                   && !(s_stayon ? 0 : s_past_init)) usleep(300);
        }
        // GIL: blink's System (guest memory / page tables) is NOT thread-safe
        // (built --disable-threads), so no two guest threads may execute at
        // once. Serialize ALL execution on s_thunk_lock — thunk dispatch below
        // already holds it, and a blocking thunk (WFSO/Sleep/CV) releases it via
        // wg_thunk_block_begin so other threads make progress. sched_yield after
        // each slice keeps the lock fair (no single thread hogs it).
        wg_thunk_lock();
        // DECOUPLED + ADAPTIVE WORKER SLICE (WG_SLICEMULT): a worker runs a BIG atomic
        // slice by DEFAULT so its own UObject constructions complete under one GIL
        // hold and no other thread interleaves a half-built class (the drain
        // corruption). But a worker also spins in the task-graph "get-next-task" loop;
        // holding the GIL for a full big slice while spinning starves the thread that
        // would produce work (the 0xB40735 livelock). So — exactly like the main —
        // shrink this worker to the small base after a SUSTAINED pin (WG_PINTHR slices
        // at one RIP = a spin/busy-wait, never a construction, which advances the RIP).
        // Default mult=1 (no change).
        int wmult = getenv("WG_SLICEMULT") ? atoi(getenv("WG_SLICEMULT")) : 1;
        if (wmult < 1) wmult = 1;
        if (wmult > 1 && getenv("WG_ADASLICE")) {
            static __thread uint64_t w_prevrip = 0; static __thread int w_pin = 0;
            uint64_t wr = wg_blink_get_rip(engine->blink);
            uint64_t wd = wr >= w_prevrip ? wr - w_prevrip : w_prevrip - wr;
            w_prevrip = wr;
            long pinthr = getenv("WG_PINTHR") ? atol(getenv("WG_PINTHR")) : 20;
            if (wd < 0x1000) { if (w_pin < 1000000) ++w_pin; } else w_pin = 0;
            int wcontended = getenv("WG_NOCONTEND") ? 1 : (s_gil_waiters > 0);
            if (w_pin >= pinthr && wcontended) wmult = 1;  // pinned + contended => spin => small
        }
        wg_update_construct_pin(engine);   // GIL-pin while this worker is mid UObject construction
        WGBlinkResult r = wg_blink_run(engine->blink, engine->instructions_per_tick * wmult);
        wg_thunk_unlock();
        sched_yield();
        // Busy-wait breaker (same as the main tick): a run of thunk-less slices =
        // this worker is spinning on a flag another thread must set; sched_yield is
        // too weak (it re-grabs the GIL before the other thread is scheduled).
        // After a run, usleep with the GIL released so the awaited thread gets a
        // guaranteed window. Breaks the worker-side coordination stall (0xa3).
        {
            static __thread int w_okrun = 0;
            static int w_sy = -1;
            if (w_sy < 0) w_sy = getenv("WG_NO_SPINYIELD") ? 0 : 1;
            if (w_sy && r == WG_BLINK_OK) {
                ++w_okrun;   // adaptive: longer spin -> more GIL time to the others
                if (w_okrun >= 32 && (w_okrun & 31) == 0) {
                    int us = w_okrun < 256 ? 150 : (w_okrun < 4096 ? 1500 : 9000);
                    usleep((useconds_t)us);
                }
            } else w_okrun = 0;
        }
        uint32_t rip = (uint32_t)wg_blink_get_rip(engine->blink);
        if (rip == 0) {   // thread proc returned -> exit
            exit_code = (uint32_t)wg_blink_get_reg(engine->blink, 0); // EAX
            break;
        }
        // A HLT thunk (HALT) OR a fault (ERROR) both dispatch through
        // handle_blink_thunk — same as the main tick. (Bug fixed: the worker
        // used to handle only ERROR, so every Win32 call from a worker spun
        // forever on its HLT at 0xC000xx.)
        if (r == WG_BLINK_HALT || r == WG_BLINK_ERROR) {
            wg_thunk_lock();
            bool handled = handle_blink_thunk(engine);
            wg_thunk_unlock();
            if (!handled) {
                uint32_t frip = (uint32_t)wg_blink_get_rip(engine->blink);
                // Auto-recover a call through a bad function pointer / vtable, the
                // SAME as the main tick: if rip landed outside the image and [rsp]
                // is a valid .text return address, this was an indirect CALL through
                // garbage — return 0 to the caller and keep running. Killing the
                // worker instead hangs the main thread waiting on its task.
                bool g64 = engine->pe_image && engine->pe_image->is_64bit;
                uint64_t img_lo = engine->pe_image ? engine->pe_image->image_base + 0x1000 : 0x401000;
                uint64_t img_hi = engine->pe_image ? engine->pe_image->image_base + engine->pe_image->size_of_image : 0x8C0000;
                bool recovered = false;
                if (frip < img_lo || frip >= img_hi) {
                    wg_thunk_lock();   // read_mem walks the shared System page tables
                    uint64_t sp = wg_blink_get_reg(engine->blink, 4);
                    uint64_t ret = 0;
                    wg_blink_read_mem(engine->blink, (uint32_t)sp, &ret, g64 ? 8 : 4);
                    // [vtdiag] Dump the object+vtable that produced the bad call.
                    // frip==-1 means call [rax+off] read -1; rax still holds the
                    // vtable pointer. Reveals: valid static vtable (0x27xxxxx) with a
                    // -1 hole vs a heap-garbage vtable pointer (wrong/UAF object).
                    {
                        static int _vd = 0;
                        if (_vd++ < 4) {
                            uint64_t vt = wg_blink_get_reg(engine->blink, 0); // RAX
                            uint64_t rcxo = wg_blink_get_reg(engine->blink, 1); // RCX (object)
                            uint64_t vc[12] = {0};
                            wg_blink_read_mem(engine->blink, (uint32_t)vt, vc, sizeof(vc));
                            WG_LOGW(TAG, "[vtdiag] obj(rcx)=0x%llx vtable(rax)=0x%llx [+0]=0x%llx [+8]=0x%llx [+0x18]=0x%llx [+0x38]=0x%llx [+0x40]=0x%llx [+0x48]=0x%llx [+0x50]=0x%llx",
                                (unsigned long long)rcxo, (unsigned long long)vt,
                                (unsigned long long)vc[0], (unsigned long long)vc[1],
                                (unsigned long long)vc[3], (unsigned long long)vc[7],
                                (unsigned long long)vc[8], (unsigned long long)vc[9],
                                (unsigned long long)vc[10]);
                        }
                    }
                    if (ret >= img_lo && ret < img_hi && wg_recover_ok(ret)) {
                        wg_blink_set_reg(engine->blink, 0, 0);                  // RAX = 0
                        wg_blink_set_reg(engine->blink, 4, sp + (g64 ? 8 : 4)); // pop return addr
                        wg_blink_set_rip(engine->blink, (uint32_t)ret);
                        recovered = true;
                    }
                    wg_thunk_unlock();
                    if (recovered) {
                        static int _wr = 0;
                        if (_wr++ < 8) WG_LOGW(TAG, "[realthr] worker tid=0x%X auto-recovered bad-addr call 0x%X ret=0x%llx", wa.tid, frip, (unsigned long long)ret);
                    }
                }
                if (!recovered) {
                    WG_LOGE(TAG, "[realthr] worker tid=0x%X unhandled halt/fault at rip=0x%X — exiting", wa.tid, frip);
                    exit_code = (uint32_t)-1;
                    break;
                }
            }
        }
        if (engine->state == WG_ENGINE_STOPPED) break;
    }

    WG_LOGI(TAG, "[realthr] worker tid=0x%X exited code=%u", wa.tid, exit_code);
    wg_sync_thread_exit(wa.thread_h, exit_code);
    wg_free_tls_slot(wa.tls_slot);
    // FreeMachine unlinks this Machine from blink's shared System machine list.
    // NewMachine (spawn) holds the GIL; blink has no internal lock (--disable-
    // threads), so a worker exiting while another spawns corrupts the list and
    // the new thread gets a bad Machine (startup crash at ~21 threads). Serialize.
    wg_thunk_lock();
    wg_blink_free_thread_machine(wa.machine);
    wg_thunk_unlock();
    return NULL;
}

static uint32_t wg_spawn_real_thread(WGEngine *engine, uint32_t start,
                                     uint32_t param, uint32_t flags,
                                     uint32_t *out_tid) {
    // CREATE_SUSPENDED (bit 0x4) is honoured via a resume gate (see below).
    if (!engine->pe_image) return 0;   // 64-bit workers now supported (x64 TEB/GS)

    // Allocate + map a 1MB guest stack from the shared thread-stack region
    // (same bump allocator the cooperative scheduler uses, so no collision).
    uint32_t stack_base = engine->scheduler->next_stack_addr;
    engine->scheduler->next_stack_addr += WG_THREAD_STACK + 0x1000;
    uint8_t *zstack = calloc(1, WG_THREAD_STACK);
    if (zstack) { wg_blink_load_code(engine->blink, stack_base, zstack, WG_THREAD_STACK, 0); free(zstack); }
    uint32_t stack_top = stack_base + WG_THREAD_STACK;

    uint32_t tid = engine->scheduler->next_id++;
    uint32_t teb = wg_alloc_thread_teb(engine, stack_top, stack_base, tid);

    void *machine = wg_blink_new_thread_machine(engine->blink);
    if (!machine) return 0;

    uint32_t thread_h = wg_sync_create_thread_obj(tid);

    WGWorkerArgs *wa = calloc(1, sizeof(*wa));
    if (!wa) { wg_blink_free_thread_machine(machine); return 0; }
    wa->engine = engine; wa->machine = machine; wa->start = start;
    wa->param = param; wa->stack_top = stack_top; wa->teb = teb;
    wa->thread_h = thread_h; wa->tid = tid; wa->tls_slot = wg_alloc_tls_slot();
    wa->flags = flags;
    if (flags & 0x4u) wg_resume_gate_create(thread_h);  // park until ResumeThread

    pthread_t pt;
    if (pthread_create(&pt, NULL, wg_worker_thread_entry, wa) != 0) {
        free(wa); wg_blink_free_thread_machine(machine);
        return 0;
    }
    pthread_detach(pt);

    WG_LOGI(TAG, "[realthr] spawned tid=0x%X handle=0x%X start=0x%X", tid, thread_h, start);
    if (out_tid) *out_tid = tid;
    return thread_h;
}

static bool load_pe_blink(WGEngine *engine) {
    WGPEImage *pe = engine->pe_image;

    // Real-threads mode (docs/threads_rearchitect.md): opt-in via env on the
    // harness. Initialise the pthread-backed sync subsystem + the global thunk
    // lock, and mark the main guest thread's id.
    if (getenv("WG_REAL_THREADS")) s_use_real_threads = true;
    if (getenv("WG_NO_REAL_THREADS")) s_use_real_threads = false;
    // Cooperative finite-timeout waits: without real timeouts, a worker's timed
    // WaitForSingleObject (e.g. UE4's 500ms task-wait) yields forever and never
    // advances, so it never signals the main thread (which then blocks INFINITE)
    // -> deadlock. Off by default (device/Steam legacy path); WG_REAL_TIMEOUTS=1
    // turns the wall-clock deadline path on for cooperative multi-thread guests.
    if (getenv("WG_REAL_TIMEOUTS")) s_real_timeouts = true;
    if (s_use_real_threads && !wg_sync_init) {
        // wg_sync.c didn't get linked (stale Xcode project — the app's
        // -undefined dynamic_lookup masks the missing symbols as NULL). Don't
        // crash: stay on the cooperative path and tell the user how to fix it.
        WG_LOGE(TAG, "[realthr] wg_sync NOT LINKED — disabling real-threads. "
                "Run 'xcodegen generate', reopen Xcode, Clean Build Folder, rebuild.");
        s_use_real_threads = false;
    }
    if (s_use_real_threads) {
        wg_thunk_lock_init();
        wg_sync_init();
        s_cur_guest_tid = 1;
        if (wg_spinpin_on() || getenv("WG_SPINLOG")) {
            wg_spinlock_acquire_hook = wg_on_guest_spinlock;   // blink -> GIL-pin / logging on guest spinlock acquire
            WG_LOGW(TAG, "[realthr] guest-spinlock hook installed (pin=%d window=%ld)",
                    wg_spinpin_on(), s_spin_window < 0 ? 24 : s_spin_window);
        }
        WG_LOGW(TAG, "[realthr] REAL-THREADS mode ENABLED");
    }

    if (!ensure_blink_vm(engine, pe->is_64bit)) {
        return false;
    }

    WG_LOGI(TAG, "Loading %s PE via blink", pe->is_64bit ? "64-bit" : "32-bit");
    s_nsis_data_patched = false;
    s_nsis_exe_data_offset = 0;

    // Map PE headers at image base — programs walk their own MZ/PE header
    if (pe->num_sections > 0 && pe->sections[0].virtual_address > 0) {
        uint32_t hdr_size = pe->sections[0].virtual_address;
        if (hdr_size > pe->raw_size) hdr_size = (uint32_t)pe->raw_size;
        wg_blink_load_code(engine->blink, pe->image_base, pe->raw_data, hdr_size, 0);
    }

    for (int i = 0; i < pe->num_sections; i++) {
        WGPESection *sec = &pe->sections[i];
        uint64_t base = pe->image_base + sec->virtual_address;

        WG_LOGI(TAG, "  Section '%s': VA=0x%llx Size=0x%x",
                sec->name, (unsigned long long)base, sec->virtual_size);

        if (sec->virtual_size > 0) {
            uint8_t *zeros = calloc(1, sec->virtual_size);
            if (zeros) {
                wg_blink_load_code(engine->blink, base, zeros, sec->virtual_size, 0);
                free(zeros);
            }
            if (sec->data && sec->raw_size > 0) {
                uint32_t copy_size = sec->raw_size < sec->virtual_size
                    ? sec->raw_size : sec->virtual_size;
                wg_blink_write_mem(engine->blink, base, sec->data, copy_size);
            }
        }
    }

    // Pick where the fixed guest scratch pages live. Legacy low addresses
    // (0xA00000/0xB00000/0xC30000) sit inside a large rebased 64-bit image and
    // would clobber its .text, so relocate them just above the image. Must be
    // done AFTER sections load (the trampoline/getaddrinfo maps come next) and
    // BEFORE the cmdline page is lazily mapped on first GetCommandLine.
    s_scratch_base = 0;
    s_cmdline_page = 0x00A00000u;
    s_tramp_addr   = 0x00C30000u;
    s_gai_base     = 0x00B00000u;
    if (pe->is_64bit) {
        uint64_t img_end = pe->image_base + pe->size_of_image;
        if (img_end > 0x00A00000ULL) {
            uint64_t b = (img_end + 0xFFFFFULL) & ~0xFFFFFULL; // round up to 1MB
            b += 0x100000ULL;                                  // 1MB gap after image
            s_scratch_base = (uint32_t)b;
            s_cmdline_page = s_scratch_base + 0x00000;         // 4KB page
            s_tramp_addr   = s_scratch_base + 0x01000;         // 4KB page
            s_gai_base     = s_scratch_base + 0x100000;        // 1MB region
            WG_LOGI(TAG, "Scratch relocated above image: cmdline=0x%X tramp=0x%X gai=0x%X",
                    s_cmdline_page, s_tramp_addr, s_gai_base);
        }
    }
    if (pe->is_64bit) map_initterm_tramp(engine, s_tramp_addr);
    wg_winsock_set_gai_base(s_gai_base);

    // Experimental (WG_UPROJ_PATCH=1): Visage's FEngineLoop::PreInit aborts when
    // IProjectManager::LoadProjectFile fails to validate the .uproject descriptor
    // (its FileVersion check rejects our parsed JSON). Force PreInit to treat the
    // load as successful: patch `sete bl` (0F 94 C3) at 0x691a91 to `xor ebx,ebx;
    // nop` (31 DB 90) so bl=0 (success). Game-specific probe.
    if (pe->is_64bit && getenv("WG_UPROJ_PATCH")) {
        uint8_t cur[3] = {0};
        wg_blink_read_mem(engine->blink, 0x691a91, cur, 3);
        if (cur[0] == 0x0F && cur[1] == 0x94 && cur[2] == 0xC3) {
            uint8_t patch[3] = { 0x31, 0xDB, 0x90 };
            wg_blink_write_mem(engine->blink, 0x691a91, patch, 3);
            WG_LOGW(TAG, "WG_UPROJ_PATCH: forced LoadProjectFile success @0x691a91");
        }
    }

    // WG_CTOR_HOOK: arm the UObject base-constructor serialization hook (see the
    // s_ctor_* declarations + the handler in handle_blink_thunk). Writes a 1-byte HLT
    // over the ctor entry so every construction traps and GIL-pins its thread.
    s_ctor_armed = false;
    if (pe->is_64bit && getenv("WG_CTOR_HOOK")) {
        const char *a = getenv("WG_CTOR_ADDR");
        s_ctor_addr = a ? strtoull(a, 0, 16) : 0x9E36E0ULL;
        if (wg_blink_read_mem(engine->blink, s_ctor_addr, &s_ctor_orig, 1)) {
            uint8_t hlt = 0xF4;
            wg_blink_write_mem(engine->blink, s_ctor_addr, &hlt, 1);
            s_ctor_armed = true;
            WG_LOGW(TAG, "WG_CTOR_HOOK: armed UObject-ctor serialization @0x%llX (orig=0x%02X)",
                    (unsigned long long)s_ctor_addr, s_ctor_orig);
        }
    }

    // WG_LOOPPROBE: arm the self-loop breakers at every UObject +0x28 chain-walk site.
    s_loop_armed = false;
    if (pe->is_64bit && getenv("WG_LOOPPROBE")) {
        const char *a = getenv("WG_LOOP_ADDR");
        if (a) { s_loops[0].addr = strtoull(a, 0, 16); }   // override the primary site
        for (int li = 0; li < WG_NLOOPS; li++) {
            if (wg_blink_read_mem(engine->blink, s_loops[li].addr, &s_loops[li].orig, 1)) {
                uint8_t hlt = 0xF4;
                wg_blink_write_mem(engine->blink, s_loops[li].addr, &hlt, 1);
                s_loops[li].armed = true;
                s_loop_armed = true;
                WG_LOGW(TAG, "WG_LOOPPROBE: armed self-loop breaker @0x%llX (orig=0x%02X)",
                        (unsigned long long)s_loops[li].addr, s_loops[li].orig);
            }
        }
    }

    // WG_ANIMFIX (default ON for 64-bit): arm the anim curve-compression config-getter
    // hook so the boot doesn't fatal on the missing Engine default. Disable with
    // WG_NO_ANIMFIX.
    s_anim_armed = false;
    if (pe->is_64bit && !getenv("WG_NO_ANIMFIX")) {
        uint8_t hlt = 0xF4;
        bool ok1 = wg_blink_read_mem(engine->blink, s_anim_addr, &s_anim_orig, 1) && s_anim_orig == 0xE8;
        bool ok2 = wg_blink_read_mem(engine->blink, s_anim_addr2, &s_anim_orig2, 1) && s_anim_orig2 == 0xE8;
        if (ok1 && ok2) {
            wg_blink_write_mem(engine->blink, s_anim_addr,  &hlt, 1);
            wg_blink_write_mem(engine->blink, s_anim_addr2, &hlt, 1);
            s_anim_armed = true;
            WG_LOGW(TAG, "WG_ANIMFIX: armed curve-compression config hooks @0x%llX (section) + 0x%llX (value)",
                    (unsigned long long)s_anim_addr2, (unsigned long long)s_anim_addr);
        } else {
            WG_LOGW(TAG, "WG_ANIMFIX: NOT armed (orig bytes: value=0x%02X section=0x%02X, expected 0xE8)",
                    s_anim_orig, s_anim_orig2);
        }
    }

    // General guest-address trace (WG_TRACE): add wg_trace_add(addr,label) calls
    // here to breakpoint + log register state at guest addresses. Kept as a
    // reusable diagnostic; no addresses armed by default.
    s_trace_count = 0;
    if (pe->is_64bit && getenv("WG_TRACE")) {
        for (int i = 0; i < s_trace_count; i++) {
            if (wg_blink_read_mem(engine->blink, s_trace[i].addr, &s_trace[i].orig, 1)) {
                uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_trace[i].addr, &hlt, 1);
                s_trace[i].armed = true;
            }
        }
        WG_LOGW(TAG, "WG_TRACE: armed %d breakpoints", s_trace_count);
    }

    // Force UE4's simple ANSI allocator on 64-bit games (see the cmdline builder).
    s_cmdline_extra[0] = 0;
    if (pe->is_64bit) strncpy(s_cmdline_extra, " -ansimalloc", sizeof(s_cmdline_extra) - 1);
    // Extra UE4 switches for experiments (e.g. WG_UE_ARGS="-sm5 -nohmd").
    if (getenv("WG_UE_ARGS")) {
        size_t n = strlen(s_cmdline_extra);
        snprintf(s_cmdline_extra + n, sizeof(s_cmdline_extra) - n, " %s", getenv("WG_UE_ARGS"));
    }

    // Resolve imports — write thunk addresses into the IAT
    if (pe->num_imports > 0) {
        WG_LOGI(TAG, "Resolving %d DLL imports...", pe->num_imports);
        for (int i = 0; i < pe->num_imports; i++) {
            WGPEImportDll *imp = &pe->imports[i];
            WG_LOGI(TAG, "  %s: %d functions", imp->dll_name, imp->num_functions);

            for (int j = 0; j < imp->num_functions; j++) {
                uint64_t stub_addr = wg_dll_mapper_resolve(
                    engine->dll_mapper, imp->dll_name, imp->functions[j].name);
                if (stub_addr) {
                    uint64_t iat_entry = pe->image_base + imp->functions[j].iat_rva;
                    uint8_t addr_bytes[8];
                    memcpy(addr_bytes, &stub_addr, 8);
                    wg_blink_write_mem(engine->blink, iat_entry, addr_bytes, 8);
                }
            }
        }
    }

    uint64_t entry = pe->image_base + pe->entry_point;

    // Set up stack BEFORE switching to 32-bit (stack setup uses ReserveVirtual)
    if (!wg_blink_setup_stack(engine->blink, entry)) {
        WG_LOGE(TAG, "Failed to set up stack");
        return false;
    }

    // NOW switch to 32-bit mode if this is a 32-bit PE
    if (!pe->is_64bit) {
        wg_blink_switch_to_32bit(engine->blink);
    }

    // Set up the Win32 thread environment (TEB/PEB/TLS + FS base) so real MSVC
    // CRT startup (steam.exe) doesn't fault reading fs:[…].
    wg_setup_win32_teb(engine);

    // Map the getaddrinfo result scratch region (1MB @ s_gai_base) for THIS VM.
    // wg_winsock serializes addrinfo chains here; it must be mapped per VM (the
    // blink VM is recreated per PE load, so a one-shot static flag in winsock
    // would skip re-mapping on the 2nd load and the guest faults reading it).
    {
        uint8_t *zeros = calloc(1, 0x100000);
        if (zeros) {
            wg_blink_load_code(engine->blink, s_gai_base, zeros, 0x100000u, 0);
            free(zeros);
        }
    }

    // Map zero pages so reads from unmapped addresses return 0 instead of
    // faulting. Real Windows catches these via SEH; without it, programs
    // crash on NULL dereferences and stale pointers (e.g. vtable entries
    // referencing unloaded DLLs). We map:
    //   0x00000000-0x0000FFFF  — NULL dereferences
    //   0x10000000-0x10000FFF  — low DLL range (fake load addresses)
    //   0x50000000-0x5FFFFFFF  — covers stale DLL pointers from .rdata
    //     (e.g. 0x53572073 "CNet::BFrame..." network handler tables)
    {
        uint8_t *zp = calloc(1, 0x10000);
        if (zp) {
            // NULL dereferences. The zero-page map is load-bearing: it absorbs
            // NULL reads that come from our own 0-returning stubs (where real
            // Windows would have a valid pointer) during CRT init. Unmapping it
            // globally turns those into fatal faults, so we keep it mapped and
            // instead deliver a guest AV only at the specific Steam send-on-NULL
            // site (see the WS2_32 send handler -> wg_raise_guest_exception).
            wg_blink_load_code(engine->blink, 0, zp, 0x10000, 0);
            // Stale DLL pointers: map specific 4KB pages covering known
            // addresses from steam's .rdata (0x53572073 etc.)
            wg_blink_load_code(engine->blink, 0x53572000u, zp, 0x1000, 0);
            wg_blink_load_code(engine->blink, 0x5ECF7000u, zp, 0x1000, 0);
            // Top of 32-bit address space (GetCurrentThread returns 0xFFFFFFFE)
            wg_blink_load_code(engine->blink, 0xFFFFF000u, zp, 0x1000, 0);
            free(zp);
        }
    }

    // Map a minimal PE stub at the fake system-DLL handle (0xBFFF0000) so
    // programs that walk kernel32's PE export table (GetModuleHandle +
    // manual export parsing) find valid headers but an empty export dir.
    {
        uint8_t fake[0x200];
        memset(fake, 0, sizeof(fake));
        // DOS header
        fake[0] = 'M'; fake[1] = 'Z';
        // e_lfanew at offset 0x3C -> PE header at 0x80
        uint32_t pe_off = 0x80;
        memcpy(fake + 0x3C, &pe_off, 4);
        // PE signature
        fake[0x80] = 'P'; fake[0x81] = 'E';
        // COFF: Machine=0x14C (i386), NumberOfSections=0
        uint16_t machine = 0x14C;
        memcpy(fake + 0x84, &machine, 2);
        // SizeOfOptionalHeader = 0xE0 (standard PE32)
        uint16_t opt_sz = 0xE0;
        memcpy(fake + 0x94, &opt_sz, 2);
        // Optional header magic = PE32 (0x10B)
        uint16_t magic = 0x10B;
        memcpy(fake + 0x98, &magic, 2);
        // Export directory RVA at OptionalHeader + 0x60 (offset 0x98+0x60=0xF8)
        // Leave it 0 (no exports)
        wg_blink_load_code(engine->blink, 0xBFFF0000u, fake, sizeof(fake), 0);
    }

    // Register the main thread (thread 0) with the scheduler. Clear the ENTIRE
    // thread table first: the blink VM is recreated per PE load, so any worker
    // threads from a previous PE (e.g. SteamSetup.exe's installer threads, before
    // the user picks Steam.exe) are stale — their saved rip/stack point into the
    // destroyed VM. Leaving them non-FREE made the round-robin scheduler resume a
    // stale thread in the new VM -> SIGSEGV (seen at 0x53d58a). Also reset the
    // id/handle/stack allocators so the new PE's threads start fresh + deterministic.
    if (engine->scheduler) {
        memset(engine->scheduler->threads, 0, sizeof(engine->scheduler->threads));
        engine->scheduler->next_id = 0x1000;
        engine->scheduler->next_handle = 0x7100;
        engine->scheduler->next_stack_addr = 0x60000000u; // above the heap's growth (see wg_sched_create)
        WGThread *mt = &engine->scheduler->threads[0];
        mt->state = WG_THREAD_RUNNING;
        mt->id = 1;
        mt->handle = 0x7000;
        mt->exit_code = 259;
        // Save FS base from the TEB we just set up
        mt->regs.fs_base = s_main_teb;
        mt->teb = s_main_teb;
        engine->scheduler->current = 0;
        engine->scheduler->count = 1;
    }

    WG_LOGI(TAG, "PE mapped via blink. Entry: 0x%llx", (unsigned long long)entry);
    return true;
}

static bool load_pe_builtin(WGEngine *engine) {
    WGPEImage *pe = engine->pe_image;

    for (int i = 0; i < pe->num_sections; i++) {
        WGPESection *sec = &pe->sections[i];
        uint64_t base = pe->image_base + sec->virtual_address;

        uint32_t prot = WG_MEM_READ;
        if (sec->characteristics & 0x20000000) prot |= WG_MEM_EXEC;
        if (sec->characteristics & 0x80000000) prot |= WG_MEM_WRITE;

        if (!wg_memory_map(engine->memory, base, sec->virtual_size, prot))
            return false;
        if (sec->data && sec->raw_size > 0) {
            uint32_t copy_size = sec->raw_size < sec->virtual_size
                ? sec->raw_size : sec->virtual_size;
            wg_memory_write(engine->memory, base, sec->data, copy_size);
        }
    }

    if (pe->num_imports > 0) {
        for (int i = 0; i < pe->num_imports; i++) {
            WGPEImportDll *imp = &pe->imports[i];
            for (int j = 0; j < imp->num_functions; j++) {
                uint64_t stub_addr = wg_dll_mapper_resolve(
                    engine->dll_mapper, imp->dll_name, imp->functions[j].name);
                if (stub_addr) {
                    uint64_t iat_entry = pe->image_base + imp->functions[j].iat_rva;
                    wg_memory_write_u64(engine->memory, iat_entry, stub_addr);
                }
            }
        }
    }

    uint64_t entry = pe->image_base + pe->entry_point;
    wg_x86_set_rip(engine->cpu, entry);
    uint64_t stack_base = 0x7FFE0000;
    wg_memory_map(engine->memory, stack_base - 0x100000, 0x100000,
                  WG_MEM_READ | WG_MEM_WRITE);
    wg_x86_set_reg(engine->cpu, WG_REG_RSP, stack_base - 0x100);
    wg_x86_set_reg(engine->cpu, WG_REG_RBP, stack_base - 0x100);

    return true;
}

bool wg_engine_load_pe(WGEngine *engine, const char *path) {
    WG_LOGI(TAG, "Loading PE: %s", path);

    // Clear any windows left over from a previous program so the UI doesn't
    // get stuck behind a lingering window.
    wg_wm_reset();

    // Reset the guest heap so every run has an identical, deterministic layout.
    s_heap_ptr = WG_GUEST_HEAP_BASE;
    s_nsis_data_patched = false;
    s_last_error = 0;
    s_nsis_data_tmp_handle = 0;
    s_nsis_data_tmp_path[0] = 0;
    s_nsis_last_data_seek = 0;
    s_dlg_active = false;
    s_dlg_hwnd = 0;
    s_dlg_proc = 0;
    s_ctrl_count = 0;
    s_callstack_depth = 0;
    s_detail_count = 0;
    s_pb_pos = 0; s_pb_max = 100;
    s_page_hwnd = 0;
    s_com_shelllink = 0; s_com_persistfile = 0;  // rebuilt in the fresh heap
    s_pending_exec[0] = 0;
    s_null_call_recover = 0;
    s_recover_last_addr = 0;
    s_recover_streak = 0;
    s_recover_total = 0;
    s_crt_errno = s_crt_commode = s_crt_fmode = 0;  // re-alloc in the fresh heap
    wg_d3d11_init();                                  // rebuild COM vtables in the fresh VM
    s_tls_next = 0;
    memset(s_tls_slots, 0, sizeof(s_tls_slots));
    s_fls_next = 0;
    memset(s_fls_slots, 0, sizeof(s_fls_slots));
    s_event_next = 0;
    memset(s_event_signalled, 0, sizeof(s_event_signalled));
    memset(s_event_manual, 0, sizeof(s_event_manual));
    // Re-arm the steam.exe TLS/cert/manifest patch + diagnostic traps on EVERY PE
    // load. These were one-shot (guarded by s_errstr_armed) which is wrong when a
    // session loads two PEs with fresh VMs each — e.g. SteamSetup.exe (installer)
    // then Steam.exe. The first load consumed the arming; the second (the real
    // Steam that does the manifest download) never got the cipher-string patch, so
    // its ClientHello collapsed to NO_CIPHERS -> internal_error. The Mac harness
    // loads Steam.exe directly (one PE) so it never hit this. Resetting here makes
    // wg_engine_run patch + arm the traps fresh for each new VM.
    s_errstr_armed = s_watch_armed = s_cloop_armed = s_fac_armed = false;
    s_pe_armed = s_hs_armed = s_disp_armed = s_sslw_armed = false;
    s_watch_count = s_cloop_count = s_fac_count = s_errput_count = 0;
    s_pe_count = s_hs_count = s_disp_count = s_snd_count = 0;
    s_sslw_count = s_sndchk_count = 0;
    s_tls_setup_done = false;   // re-patch the TLS strings/cert/manifest for each new PE
    s_tmsg_head = 0; s_tmsg_tail = 0;
    s_comp_head = 0; s_comp_tail = 0;
    s_iocp_binding_count = 0;
    s_iocp_created = false;
    s_tp_work_count = 0;
    s_alloc_count = 0;
    s_cmdpage_mapped = false;
    wg_bitmap_reset_all();

    // Reset the loaded-DLL table (fresh VM => previous mappings are gone).
    for (int i = 0; i < 16; i++) {
        if (s_modules[i].in_use && s_modules[i].img) wg_pe_image_free(s_modules[i].img);
        s_modules[i].in_use = false;
        s_modules[i].img = NULL;
    }
    s_dll_next_base = 0x60000000u;

    // Set the exe path for file I/O mapping (also anchors the bottle's drive_c).
    wg_files_set_exe_path(path);
    // Mirror the full engine log into the bottle so it rides along when drive_c is
    // copied off the device (the Xcode console ring-buffers and the .xcresult
    // doesn't reliably sync). This is the untruncated source of truth.
    {
        char logpath[1200];
        snprintf(logpath, sizeof(logpath), "%s/wineglass_engine.log", wg_files_drive_c());
        wg_log_set_file(logpath);
        WG_LOGI(TAG, "Engine log mirrored -> %s", logpath);
    }
    // Clear stale NSIS plug-in dirs from the bottle's Temp so this run gets a
    // fresh plug-ins directory (NSIS bails if one already exists).
    wg_files_reset_temp();

    engine->pe_image = wg_pe_load_file(path);
    if (!engine->pe_image) {
        WG_LOGE(TAG, "Failed to parse PE file");
        engine->state = WG_ENGINE_ERROR;
        return false;
    }

    WG_LOGI(TAG, "PE parsed: %d sections, entry=0x%x, image_base=0x%llx",
            engine->pe_image->num_sections,
            engine->pe_image->entry_point,
            (unsigned long long)engine->pe_image->image_base);

    // PE32+ images prefer 0x140000000 — above 4GB. Every Win32 handler carries
    // guest pointers through 32-bit args (and the PEB/thunk plumbing assumes a
    // sub-4GB guest), so rebase such images down to the classic 0x400000.
    if (engine->pe_image->is_64bit &&
        engine->pe_image->image_base + engine->pe_image->size_of_image > 0xE0000000ULL) {
        if (!wg_pe_rebase(engine->pe_image, 0x00400000ULL)) {
            WG_LOGW(TAG, "64-bit image base 0x%llx not rebasable — pointers may "
                    "truncate in Win32 handlers",
                    (unsigned long long)engine->pe_image->image_base);
        }
    }

    bool ok = false;
    if (engine->backend == WG_BACKEND_BLINK) {
        ok = load_pe_blink(engine); // this creates the blink VM on demand
    }
    if (!ok) {
        ok = load_pe_builtin(engine);
    }

    if (ok) engine->state = WG_ENGINE_LOADED;
    else    engine->state = WG_ENGINE_ERROR;
    return ok;
}

bool wg_engine_load_pe_memory(WGEngine *engine, const uint8_t *data, size_t size) {
    WG_LOGI(TAG, "Loading PE from memory (%zu bytes)", size);

    engine->pe_image = wg_pe_load_memory(data, size);
    if (!engine->pe_image) {
        WG_LOGE(TAG, "Failed to parse PE");
        engine->state = WG_ENGINE_ERROR;
        return false;
    }

    WG_LOGI(TAG, "PE parsed: %d sections, entry=0x%x, imports=%d",
            engine->pe_image->num_sections,
            engine->pe_image->entry_point,
            engine->pe_image->num_imports);

    // Same sub-4GB rebase as wg_engine_load_pe (see comment there).
    if (engine->pe_image->is_64bit &&
        engine->pe_image->image_base + engine->pe_image->size_of_image > 0xE0000000ULL) {
        wg_pe_rebase(engine->pe_image, 0x00400000ULL);
    }

    bool ok = false;
    if (engine->backend == WG_BACKEND_BLINK) {
        ok = load_pe_blink(engine);
    }
    if (!ok) {
        ok = load_pe_builtin(engine);
    }

    if (ok) engine->state = WG_ENGINE_LOADED;
    else    engine->state = WG_ENGINE_ERROR;
    return ok;
}

// Deadlock watchdog: a separate pthread (the main tick blocks inside the guest's
// WaitForSingleObject during a deadlock, so it can't self-check). When s_thunk_progress
// stops advancing for WG_DEADLOCK_DUMP seconds, dump every live wait + its event's
// signalled state — pinpoints whether a wait is on an unsignalled event (dispatch
// break) or a signalled one (a wake bug).
static void *wg_deadlock_watchdog(void *arg) {
    (void)arg;
    int secs = atoi(getenv("WG_DEADLOCK_DUMP") ? getenv("WG_DEADLOCK_DUMP") : "0");
    if (secs <= 0) secs = 15;
    int do_kick = getenv("WG_DEADLOCK_KICK") != NULL;
    fprintf(stderr, "[watchdog] started (fires after ~%ds near-zero thunk rate, kick=%d)\n", secs, do_kick);
    unsigned long long last = 0; int low = 0, dumped = 0;
    unsigned long long last_main = 0;
    for (;;) {
        struct timespec ts = {3, 0}; nanosleep(&ts, NULL);
        unsigned long long cur = s_thunk_progress, delta = cur - last; last = cur;
        // The MAIN tick heartbeat: if it froze, the main is deadlocked EVEN IF workers
        // spin and keep the global thunk count high (the SetEvent-livelock that masked
        // the deadlock so the kick never fired). Treat a frozen main as stalled too.
        unsigned long long cur_main = s_main_tick_pub, dmain = cur_main - last_main; last_main = cur_main;
        if (delta < 3000 || dmain == 0) {
            low += 3;
            if (low >= secs) {
                // Always reprint the MAIN/GIL line so ripStall/tick can be watched over
                // time (climbing ripStall + frozen tick = the main is spinning one spot).
                fprintf(stderr, "[watchdog] MAIN: rip=0x%llX ripStall=%u tick=%llu blocked=%d | GIL: owned=%d owner_tid=0x%X prefer=0x%X waiters=%d rec=%d\n",
                        (unsigned long long)s_main_rip_pub, s_main_rip_stall,
                        (unsigned long long)s_main_tick_pub, s_main_blocked,
                        s_dir_owned, s_dir_owner_tid, s_dir_prefer, s_gil_waiters, s_dir_rec);
                if (!dumped) {
                    fprintf(stderr, "\n[watchdog] near-zero progress (%llu thunks/3s) ~%ds — DEADLOCK; live waits:\n", delta, low);
                    wg_sync_dump_waits();
                    wg_dump_synctrace();
                    dumped = 1;
                }
                if (do_kick) {
                    int k = wg_sync_kick_workers();
                    int ks = wg_srw_kick();
                    fprintf(stderr, "[watchdog] kicked %d parked worker events, reset %d stuck SRW locks\n", k, ks);
                    low = secs > 6 ? secs - 6 : 0;   // re-check in ~6s; kick again if still stalled
                }
            }
        } else { low = 0; dumped = 0; }
    }
    return NULL;
}

bool wg_engine_run(WGEngine *engine) {
    if (engine->state != WG_ENGINE_LOADED) {
        WG_LOGE(TAG, "Cannot run: no PE loaded (state=%d)", engine->state);
        return false;
    }
    if (getenv("WG_DEADLOCK_DUMP")) {
        pthread_t wt; if (pthread_create(&wt, NULL, wg_deadlock_watchdog, NULL) == 0) pthread_detach(wt);
    }
    // WG_MOVIE_DIR: play the game's startup movies natively on the window RIGHT NOW,
    // at engine start — independent of the guest. UE4's boot loads a required Engine
    // asset (anim curve compression settings) and fatals before it reaches its own
    // movie-player enumeration, so the guest-triggered WG_NATIVE_MOVIE path never
    // fires. Playing the logo movie directly puts the game's actual startup frames on
    // screen while the guest boot is worked on. Point WG_MOVIE_DIR at Content/Movies.
    if (getenv("WG_MOVIE_DIR")) {
        WG_LOGW(TAG, "WG_MOVIE_DIR: playing startup movies natively from '%s'", getenv("WG_MOVIE_DIR"));
        wg_gpu_play_movie(getenv("WG_MOVIE_DIR"));
    }
    engine->state = WG_ENGINE_RUNNING;
    WG_LOGI(TAG, "Execution started (backend: %s)",
            engine->backend == WG_BACKEND_BLINK ? "blink" : "builtin");
    // DIAG: trap steam.exe's ERR_error_string_n (0x5FDFC0) to print the exact
    // OpenSSL handshake error code (lib/reason). Steam drains its error queue
    // through this fn before a level-filtered spew, so it's the only reliable
    // way to learn WHY SSL_do_handshake fails. Guarded to steam's image base.
    // (s_tls_setup_done is file-scope + reset per PE load, so SteamSetup.exe
    // running first no longer consumes the patch before the real Steam.exe.)
    // Only the real Steam.exe (~4MB, image_base 0x400000). The size gate is
    // essential: the iOS self-test runs tiny 32-bit PEs (also image_base
    // 0x400000) BEFORE Steam, and without it one of them would trip the
    // once-guard (set unconditionally below) so Steam skipped the whole TLS
    // patch/trap block -> empty cipher list -> fatal internal_error alert
    // instead of a ClientHello. (The macOS harness loads Steam directly, so it
    // never hit this — device-only regression.)
    // 32-bit ONLY: these patches/traps write HLT + patched bytes at Steam.exe's
    // hardcoded 32-bit addresses (0x6BB882, 0x69B720, …). A 64-bit game rebased
    // to the same 0x400000 base (e.g. Visage, 55MB) would otherwise get them
    // stamped into its own .text — corrupting code (0x24 -> 0xF4) and crashing.
    if (engine->blink && engine->pe_image && !engine->pe_image->is_64bit &&
        engine->pe_image->image_base == 0x400000 &&
        engine->pe_image->raw_size > 0x100000 && !s_tls_setup_done) {
      // The armed HLT diagnostic traps (below) are handled only by the main tick,
      // not by worker pthreads — a worker hitting one (e.g. 0x6BB882) would take an
      // unhandled halt. They're cooperative-era diagnostics + the cipher max_ver
      // force (now redundant with the max_proto_version cap + config-string
      // rewrite). So skip them in real-threads mode; the functional config patch
      // (cipher/curve/cert/manifest, further below) still runs.
      if (!s_use_real_threads) {
        if (wg_blink_read_mem(engine->blink, s_errstr_bp, &s_errstr_orig, 1)) {
            uint8_t hlt = 0xF4;
            wg_blink_write_mem(engine->blink, s_errstr_bp, &hlt, 1);
            s_errstr_armed = true;
            WG_LOGI(TAG, "Armed ERR_error_string_n trap @0x%X (orig=0x%02X)",
                    s_errstr_bp, s_errstr_orig);
        }
        if (wg_blink_read_mem(engine->blink, s_cloop_addr, &s_cloop_orig, 1)) {
            uint8_t hlt = 0xF4;
            wg_blink_write_mem(engine->blink, s_cloop_addr, &hlt, 1);
            s_cloop_armed = true;
        }
        if (wg_blink_read_mem(engine->blink, s_fac_addr, &s_fac_orig, 1)) {
            uint8_t hlt = 0xF4;
            wg_blink_write_mem(engine->blink, s_fac_addr, &hlt, 1);
            s_fac_armed = true;
        }
        // Package-save error-byte root-cause traps (see decls). All cold paths.
        if (!s_pe_armed) {
            uint8_t hlt = 0xF4;
            if (wg_blink_read_mem(engine->blink, s_pe_ext_addr, &s_pe_ext_orig, 1))
                wg_blink_write_mem(engine->blink, s_pe_ext_addr, &hlt, 1);
            if (wg_blink_read_mem(engine->blink, s_pe_alloc_addr, &s_pe_alloc_orig, 1))
                wg_blink_write_mem(engine->blink, s_pe_alloc_addr, &hlt, 1);
            if (wg_blink_read_mem(engine->blink, s_pe_chk_addr, &s_pe_chk_orig, 1))
                wg_blink_write_mem(engine->blink, s_pe_chk_addr, &hlt, 1);
            s_pe_armed = true;
            WG_LOGI(TAG, "Armed package-save traps: ext@0x%X alloc@0x%X chk@0x%X",
                    s_pe_ext_addr, s_pe_alloc_addr, s_pe_chk_addr);
        }
        if (!s_hs_armed) {
            uint8_t hlt = 0xF4;
            if (wg_blink_read_mem(engine->blink, s_hs_ret_addr, &s_hs_ret_orig, 1))
                wg_blink_write_mem(engine->blink, s_hs_ret_addr, &hlt, 1);
            if (wg_blink_read_mem(engine->blink, s_hs_gate_addr, &s_hs_gate_orig, 1))
                wg_blink_write_mem(engine->blink, s_hs_gate_addr, &hlt, 1);
            s_hs_armed = true;
            WG_LOGI(TAG, "Armed handshake traps: ret@0x%X gate@0x%X",
                    s_hs_ret_addr, s_hs_gate_addr);
        }
        if (!s_disp_armed) {
            uint8_t hlt = 0xF4;
            if (wg_blink_read_mem(engine->blink, s_disp_chk_addr, &s_disp_chk_orig, 1))
                wg_blink_write_mem(engine->blink, s_disp_chk_addr, &hlt, 1);
            if (wg_blink_read_mem(engine->blink, s_disp_pump_addr, &s_disp_pump_orig, 1))
                wg_blink_write_mem(engine->blink, s_disp_pump_addr, &hlt, 1);
            s_disp_armed = true;
            WG_LOGI(TAG, "Armed dispatch traps: chk@0x%X pump@0x%X",
                    s_disp_chk_addr, s_disp_pump_addr);
        }
        if (!s_snd_armed) {
            if (wg_blink_read_mem(engine->blink, s_snd_addr, &s_snd_orig, 1)) {
                uint8_t hlt = 0xF4;
                wg_blink_write_mem(engine->blink, s_snd_addr, &hlt, 1);
                s_snd_armed = true;
                WG_LOGI(TAG, "Armed send-pump trap: snd@0x%X", s_snd_addr);
            }
        }
        if (!s_sslw_armed) {
            uint8_t hlt = 0xF4;
            if (wg_blink_read_mem(engine->blink, s_sslw_addr, &s_sslw_orig, 1))
                wg_blink_write_mem(engine->blink, s_sslw_addr, &hlt, 1);
            if (wg_blink_read_mem(engine->blink, s_sndchk_addr, &s_sndchk_orig, 1))
                wg_blink_write_mem(engine->blink, s_sndchk_addr, &hlt, 1);
            s_sslw_armed = true;
            WG_LOGI(TAG, "Armed send traps: write@0x%X gate@0x%X", s_sslw_addr, s_sndchk_addr);
        }
      } // end if(!s_use_real_threads) — trap arming
        // Pragmatic TLS fix: blink miscomputes OpenSSL's multi-token cipher/curve
        // list construction, so Steam's ClientHello offers only static-RSA + P-521
        // and the CDN rejects it. Overwrite the two config strings (.rdata) with
        // forms that parse to ECDHE-RSA ciphers + a real curve. Addresses are from
        // the Jun-2024 steam.exe (guarded to image_base 0x400000).
        {
            // blink miscomputes OpenSSL's cipher-rule parser (ssl_create_cipher_list)
            // for multi-token strings -> only one cipher survives. Use a single
            // token so an ECDHE-RSA suite is actually offered. (The curve parser,
            // CONF_parse_list, works fine for multi-token.)
            static const char ciphers[] = "ECDHE-RSA-AES128-GCM-SHA256";
            static const char curves[]  = "X25519:P-256";
            static const char tls13[]   = "TLS_AES_128_GCM_SHA256"; // TLS1.3 suite (single token)
            wg_blink_write_mem(engine->blink, 0x709908, ciphers, sizeof(ciphers));
            wg_blink_write_mem(engine->blink, 0x709A7C, curves, sizeof(curves));
            wg_blink_write_mem(engine->blink, 0x7A3168, tls13, sizeof(tls13));
            // Steam's CustomVerifyCertificate (0x4F0BF0) builds a Windows cert
            // store via CRYPT32 (CertOpenStore/...) which we auto-stub to NULL, so
            // it always fails -> connection aborts with "http error 0". The live
            // TLS handshake already used the real CDN cert, so short-circuit the
            // function to return success (mov eax,1; ret). cdecl/thiscall (plain ret).
            static const uint8_t verify_ok[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
            wg_blink_write_mem(engine->blink, 0x4F0BF0, verify_ok, sizeof(verify_ok));
            // Steam's manifest signature check (CheckManifestSignature, 0x4611E0,
            // cdecl 1 arg) verifies a "kvsign2" RSA signature over the manifest
            // KeyValues. It fails under blink; the manifest was fetched over the
            // real TLS connection to the real CDN, so accept it. (mov eax,1; ret)
            wg_blink_write_mem(engine->blink, 0x4611E0, verify_ok, sizeof(verify_ok));
            WG_LOGI(TAG, "Patched TLS strings + cert + manifest verify");
            // s_watch (cipher_list_to_bytes max_ver force) is FUNCTIONAL, not
            // diagnostic: without it the cipher list is empty -> NO_CIPHERS ->
            // BoringSSL sends a fatal internal_error alert instead of a
            // ClientHello (seen on device real-threads). Arm it in BOTH modes.
            // In real-threads it's handled in handle_blink_thunk (runs on the
            // worker that builds the ClientHello); in cooperative, in the tick.
            if (!s_watch_armed &&
                wg_blink_read_mem(engine->blink, s_watch_addr, &s_watch_orig, 1)) {
                uint8_t hlt = 0xF4;
                wg_blink_write_mem(engine->blink, s_watch_addr, &hlt, 1);
                s_watch_armed = true;
                WG_LOGW(TAG, "Armed cipher-list max_ver trap @0x%X (orig=0x%02X)",
                        s_watch_addr, s_watch_orig);
            } else if (!s_watch_armed) {
                WG_LOGE(TAG, "s_watch ARM FAILED: cannot read 0x%X", s_watch_addr);
            }
            // s_cloop (SSL_CTX_set_cipher_list) is ALSO functional in real-threads:
            // it caps ctx max_proto_version so the ClientHello's supported_versions
            // stops advertising TLS1.3. Cooperative arms it above (in the
            // !s_use_real_threads block) and handles it in the tick; for real
            // threads arm it here and handle it in handle_blink_thunk. Without this
            // the hello is inconsistent (TLS1.2 cipher + TLS1.3 versions) and the
            // CDN rejects it with fatal handshake_failure.
            if (s_use_real_threads && !s_cloop_armed &&
                wg_blink_read_mem(engine->blink, s_cloop_addr, &s_cloop_orig, 1)) {
                uint8_t hlt = 0xF4;
                wg_blink_write_mem(engine->blink, s_cloop_addr, &hlt, 1);
                s_cloop_armed = true;
                WG_LOGW(TAG, "Armed ctx max_proto_version cap @0x%X (orig=0x%02X)",
                        s_cloop_addr, s_cloop_orig);
            }
        }
        s_tls_setup_done = true;
    }
    return true;
}

void wg_engine_tick(WGEngine *engine) {
    if (!engine) return;
    // Allow ticking when PAUSED if worker threads need to run
    if (engine->state == WG_ENGINE_PAUSED && engine->scheduler) {
        bool has_ready = false;
        for (int ti = 0; ti < WG_MAX_THREADS; ti++) {
            if (engine->scheduler->threads[ti].state == WG_THREAD_READY) {
                has_ready = true;
                break;
            }
        }
        if (has_ready) {
            // Switch to a worker thread and run it
            wg_sched_save_current(engine->scheduler, engine->blink, WG_THREAD_WAITING);
            if (wg_sched_switch_next(engine->scheduler, engine->blink)) {
                engine->state = WG_ENGINE_RUNNING;
            }
        }
    }
    if (engine->state != WG_ENGINE_RUNNING) return;

    engine->tick_count++;

    if (getenv("WG_SPINLOG") && (engine->tick_count % 2000) == 0) {
        extern unsigned long long wg_lockcas_total, wg_lockcas_acq, wg_cx16_total, wg_cx16_ok, wg_store_total;
        WG_LOGW("Engine", "LOCKCAS=%llu acq=%llu | CX16=%llu | STORES=%llu (sanity)",
                wg_lockcas_total, wg_lockcas_acq, wg_cx16_total, wg_store_total);
        wg_dump_spinrips();
    }

    // Diagnostic: sample the guest RIP periodically to locate a slow tight loop
    // (the post-Slate compute/busy-wait phase). WG_RIPSAMPLE=1.
    if (getenv("WG_RIPSAMPLE") && (engine->tick_count % 2000) == 0) {
        uint64_t rip = wg_blink_get_rip(engine->blink);
        // Walk the stack for .text return addrs so we see the caller chain of
        // the hot loop (the guest allocator alone doesn't identify the workload).
        char chain[300]; int ci = 0, found = 0; uint32_t prev = 0;
        uint32_t sp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
        uint32_t lo = engine->pe_image ? (uint32_t)engine->pe_image->image_base + 0x1000 : 0x401000;
        uint32_t hi = engine->pe_image ? (uint32_t)engine->pe_image->image_base + 0x2358000 : 0x2758000;
        for (int w = 0; w < 400 && found < 10; w++) {
            uint32_t v = 0; wg_blink_read_mem(engine->blink, sp + (uint32_t)w * 8, &v, 4);
            if (v >= lo && v < hi && v != prev) {
                ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v); found++; prev = v;
            }
        }
        WG_LOGW(TAG, "RIPSAMPLE tick=%llu rip=0x%llX callers: %s",
                (unsigned long long)engine->tick_count, (unsigned long long)rip, chain);
        // STRLENPROBE: in the hot chunked string-builder (0x9F4D80-0x9F4F30) rbp is
        // the FString/buffer; [rbp+0xc]=length, [rbp+8]=chunk index. If length grows
        // UNBOUNDED across samples => a circular Outer chain / GetPathName cycle
        // building an infinite path (residual construction corruption); bounded =>
        // a normal per-object string. Pinpoints the post-swapchain O(N^2) stall.
        if (getenv("WG_STRLENPROBE") && rip >= 0x9F4D80 && rip <= 0x9F4F30) {
            uint32_t rbp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
            uint32_t len = 0, chunks = 0;
            wg_blink_read_mem(engine->blink, rbp + 0xc, &len, 4);
            wg_blink_read_mem(engine->blink, rbp + 8, &chunks, 4);
            WG_LOGW(TAG, "STRLENPROBE rip=0x%llX rbp=0x%X len=%u chunks=%u",
                    (unsigned long long)rip, rbp, len, chunks);
        }
        // DRAIN PROBE: when the hot rip is inside the FArchive::Serialize frontier
        // function (~0x815800-0x8158a0), dump the FArchive (RBX) + its buffer write
        // position [RBX+0x90] + base [RBX+0x98] + the source ptr [RBX+0x98]-deref.
        // If RBX changes across samples (distinct archives) OR the position advances
        // monotonically => FINITE PROGRESS. If RBX constant AND position repeats =>
        // busy-wait SPIN. Resolves the multi-session slow-vs-deadlock crux.
        if (getenv("WG_DRAINPROBE") && rip >= 0x815700 && rip <= 0x8159a0) {
            uint64_t rbx = wg_blink_get_reg(engine->blink, 3);
            uint64_t rsi = wg_blink_get_reg(engine->blink, 6);
            uint64_t pos = 0, base = 0, srcbuf = 0;
            wg_blink_read_mem(engine->blink, rbx + 0x90, &pos, 8);
            wg_blink_read_mem(engine->blink, rbx + 0x98, &base, 8);
            if (base) wg_blink_read_mem(engine->blink, base, &srcbuf, 8);
            WG_LOGW(TAG, "DRAINPROBE tick=%llu FArchive(rbx)=0x%llX pos[+0x90]=0x%llX base[+0x98]=0x%llX src=0x%llX rsi=0x%llX",
                    (unsigned long long)engine->tick_count, (unsigned long long)rbx,
                    (unsigned long long)pos, (unsigned long long)base,
                    (unsigned long long)srcbuf, (unsigned long long)rsi);
        }
        // LINKPROBE: the 0xA5AC30 list-walk (rbx=node, rbx=[rbx+0x28] until null,
        // calls the 0xB66D50 setter per node) that BOTH Path A and Path B grind for
        // 2M+ ticks (>whole native boot). Dump the node (rbx), its next[+0x28], the
        // filter cmp[+0x20], rsi. If node values are DISTINCT + monotone => huge-but-
        // finite; if they REPEAT/cycle => circular walk (the real stuck bug).
        if (getenv("WG_DRAINPROBE") && rip >= 0xA5AC00 && rip <= 0xA5AC80) {
            uint64_t rbx = wg_blink_get_reg(engine->blink, 3);
            uint64_t rsi = wg_blink_get_reg(engine->blink, 6);
            uint64_t next = 0, cmp = 0;
            wg_blink_read_mem(engine->blink, rbx + 0x28, &next, 8);
            wg_blink_read_mem(engine->blink, rbx + 0x20, &cmp, 8);
            WG_LOGW(TAG, "LINKPROBE tick=%llu node(rbx)=0x%llX next[+0x28]=0x%llX cmp[+0x20]=0x%llX rsi=0x%llX",
                    (unsigned long long)engine->tick_count, (unsigned long long)rbx,
                    (unsigned long long)next, (unsigned long long)cmp,
                    (unsigned long long)rsi);
        }
        // WORKAROUND (WG_BREAK_SELFLOOP): the corrupted UObject registration list
        // has nodes whose next-ptr points to ITSELF ([rbx+0x28]==rbx), so the walk
        // 0xA5AC30 `rbx=[rbx+0x28]; test rbx; jne` spins forever. RIPSAMPLE catches
        // the SETTER 0xB66D50 (deep in the per-node call) far more than the walk, and
        // rbx (callee-saved) is still the walked node there — so check at BOTH sites.
        // Write next=0 to terminate the walk so the boot proceeds past the stuck reg.
        if (getenv("WG_BREAK_SELFLOOP") &&
            ((rip >= 0xA5AC00 && rip <= 0xA5AC80) ||
             (rip >= 0xB66D40 && rip <= 0xB66D80))) {
            uint64_t rbx = wg_blink_get_reg(engine->blink, 3);
            uint64_t next = 0;
            if (rbx) wg_blink_read_mem(engine->blink, rbx + 0x28, &next, 8);
            if (rbx != 0 && next == rbx) {
                // RETRY instead of truncate: next==self is the UNLINKED state — a
                // concurrent constructor (a worker) simply hasn't LINKED this node
                // yet when our walk reached it. Truncating (next=0) corrupts the
                // list (loses the tail) -> the downstream garbage-name scans. So
                // instead YIELD here (the GIL is free at this point in the tick) so
                // the constructor runs and sets next=successor, then re-check next
                // tick. Only truncate if the SAME node stays self across many retries
                // (genuinely dead, not just in-flight).
                static uint64_t s_rn = 0; static int s_rc = 0;
                if (rbx == s_rn) ++s_rc; else { s_rn = rbx; s_rc = 1; }
                if (s_rc < 400 && !getenv("WG_NO_SELFLOOP_RETRY")) {
                    usleep(300);   // let the constructor link this node
                } else {
                    uint64_t zero = 0;
                    wg_blink_write_mem(engine->blink, rbx + 0x28, &zero, 8);
                    WG_LOGW(TAG, "BREAK_SELFLOOP: node 0x%llX stayed self %d retries; truncated",
                            (unsigned long long)rbx, s_rc);
                    s_rn = 0; s_rc = 0;
                }
            }
        }
        // STALLPROBE: dump the state at the post-drain 0xA5C7E7 stall (string scan /
        // wait). rdi/rsi = scan counters (HUGE = runaway scan on a corrupt string);
        // r12/r14 = the strings. Tells whether to terminate a scan or it's a real wait.
        if (getenv("WG_STALLPROBE") && rip >= 0xA5C700 && rip <= 0xA5C900) {
            uint64_t r12 = wg_blink_get_reg(engine->blink, 12);
            uint64_t r14 = wg_blink_get_reg(engine->blink, 14);
            uint64_t rdi = wg_blink_get_reg(engine->blink, 7);
            uint64_t rsi = wg_blink_get_reg(engine->blink, 6);
            uint64_t s12 = 0, s14 = 0;
            if (r12) wg_blink_read_mem(engine->blink, r12, &s12, 8);
            if (r14) wg_blink_read_mem(engine->blink, r14, &s14, 8);
            WG_LOGW(TAG, "STALLPROBE rip=0x%llX rdi=0x%llX rsi=0x%llX r12=0x%llX([r12]=0x%llX) r14=0x%llX([r14]=0x%llX)",
                    (unsigned long long)rip, (unsigned long long)rdi, (unsigned long long)rsi,
                    (unsigned long long)r12, (unsigned long long)s12,
                    (unsigned long long)r14, (unsigned long long)s14);
        }
        // Cascade breaker: after the list is un-stuck, a corrupt (huge) string LENGTH
        // makes the wide-string scan at 0xB89D60 (`for(r8=start;r8<end=start+len*2;r8+=2)
        // if(*r8=='\'')`) run effectively forever (Path B reads demand-zero past the
        // string). Force r8=rax(end) so `cmp r8,rax; jne` exits and the boot continues.
        if (getenv("WG_BREAK_SELFLOOP") && rip >= 0xB89D40 && rip <= 0xB89D80) {
            uint64_t rax = wg_blink_get_reg(engine->blink, 0);  // scan end
            wg_blink_set_reg(engine->blink, 8, rax);            // r8 = end -> loop exits
            WG_LOGW(TAG, "BREAK_SCAN: terminated runaway string scan at 0xB89D60");
        }
    }

    if (engine->blink) {
        // Adaptive slice (real-threads): after a run of thunk-free (compute-bound)
        // slices the guest is grinding pure computation (the UObject drain). Give it
        // a MUCH bigger instruction budget so it holds the GIL longer — otherwise the
        // polling workers (WAITCAP_INF) contend the GIL away every 100k instructions
        // and the game thread stalls re-acquiring it (_pthread_mutex_..lock_slow, the
        // ~40 ticks/s coordination phase). Reset to the small slice the instant a
        // thunk/coordination point is hit (a HALT), so workers get the GIL promptly.
        // WG_NO_BIGSLICE disables.
        static int s_okstreak = 0;
        int slice = engine->instructions_per_tick;
        if (s_use_real_threads && getenv("WG_DIVSLICE")) {
            // DIVERSITY-ADAPTIVE SLICE: a CONSTRUCTION visits MANY distinct code
            // addresses across recent slices (10+); a BUSY-WAIT (even one spanning
            // a wide address range, e.g. the 2.7MB main<->worker handshake) cycles
            // only a FEW distinct RIPs (3-4). Distinct-COUNT separates them where
            // RIP-range/pinned/RSS could not. Many distinct => big atomic slice
            // (worker can't interleave the construction). Few distinct => small base
            // slice => frequent handoffs => the mutual busy-wait resolves.
            static uint64_t s_ring[32]; static int s_ri = 0;
            uint64_t cr = wg_blink_get_rip(engine->blink);
            s_ring[s_ri % 32] = cr & ~0xFFFULL;  /* page-granular so a loop's body counts once */
            ++s_ri;
            int nfill = s_ri < 32 ? s_ri : 32, ndist = 0;
            for (int _i = 0; _i < nfill; ++_i) {
                int seen = 0;
                for (int _j = 0; _j < _i; ++_j) if (s_ring[_j] == s_ring[_i]) { seen = 1; break; }
                if (!seen) ++ndist;
            }
            long thr = getenv("WG_DIVTHR") ? atol(getenv("WG_DIVTHR")) : 6;
            if (s_ri < 32 || ndist > thr) slice *= 48;  /* diverse => construction => big */
            /* else few distinct => busy-wait => small base slice */
            (void)s_okstreak;
        } else if (s_use_real_threads && getenv("WG_RSSSLICE")) {
            // RSS-ADAPTIVE SLICE (the clean fix for construction-vs-coordination): a
            // UObject CONSTRUCTION allocates (process RSS grows) -> BIG atomic slice so
            // no worker interleaves BETWEEN slices and uses the half-built class (the
            // drain corruption). A BUSY-WAIT / coordination point does NOT allocate (RSS
            // flat) -> small base slice for frequent GIL handoffs. RSS growth cleanly
            // separates the two (the drain grows 0.4->2.8GB; coordination stalls are
            // flat) where RIP/thunk heuristics couldn't. WG_IPT = small base (e.g. 300000).
            static long s_lastrss = 0; static int s_flat = 0;
            struct rusage _ru; getrusage(RUSAGE_SELF, &_ru);
            long _rss = _ru.ru_maxrss;   /* macOS: bytes (high-water) */
            if (_rss > s_lastrss) { s_flat = 0; s_lastrss = _rss; }
            else if (s_flat < 200) ++s_flat;
            /* Also big-slice during the registration tick WINDOW: some constructions
               link PRE-ALLOCATED nodes (flat RSS but still >8M compute), which RSS
               growth misses. The window (env-tunable) spans past the 2.76M coordination
               through the drain so those constructions are atomic too. */
            static long _lo = -1, _hi = -1;
            if (_lo < 0) { const char *a = getenv("WG_BIGTICK_LO"); _lo = a ? atol(a) : 2765000;
                           const char *b = getenv("WG_BIGTICK_HI"); _hi = b ? atol(b) : 3300000; }
            int _inwin = (long)engine->tick_count >= _lo && (long)engine->tick_count < _hi;
            if (s_flat < 24 || _inwin) slice *= 48;  /* construction => big atomic slice */
            /* else RSS flat + outside window => busy-wait/coordination => small slice */
            (void)s_okstreak;
        } else if (s_use_real_threads && getenv("WG_RIPSLICE")) {
            // RIP-ADAPTIVE SLICE (the fix for the boot's construction-vs-coordination
            // tension): a UObject class CONSTRUCTION advances the guest RIP across
            // slices -> give it a BIG atomic slice so no worker can interleave and use
            // the half-built class (the registration corruption). A BUSY-WAIT keeps the
            // RIP pinned -> use the SMALL base slice so workers get frequent GIL
            // handoffs to break the coordination spin. Picking per-slice from RIP
            // movement gets BOTH (big slices alone starve coordination; small slices
            // alone corrupt constructions). Set WG_IPT to the small base (e.g. 500000).
            static uint64_t s_prevrip = 0; static int s_pinned = 0;
            uint64_t crip = wg_blink_get_rip(engine->blink);
            uint64_t dd = crip >= s_prevrip ? crip - s_prevrip : s_prevrip - crip;
            s_prevrip = crip;
            if (dd < 0x1000) { if (s_pinned < 64) ++s_pinned; } else s_pinned = 0;
            if (s_pinned < 3) slice *= 32;   // advancing => construction => big atomic
            // else pinned => busy-wait => small base slice (frequent handoffs)
            (void)s_okstreak;
        } else if (s_use_real_threads && getenv("WG_ADASLICE")) {
            // ADAPTIVE, DECOUPLED (the coordination-vs-construction resolution the 7
            // slice heuristics missed). The main runs a BIG atomic slice by DEFAULT
            // (mult*base) so ITS constructions are atomic, and shrinks to the small
            // base ONLY after a SUSTAINED pin — WG_PINTHR consecutive slices at one
            // RIP. A construction ADVANCES the RIP every slice (never sustains a pin),
            // so it always gets the big slice; only a real coordination BUSY-WAIT
            // sustains a pin, and a pinned main is NOT constructing, so shrinking there
            // is safe. Workers ALWAYS run big (WG_SLICEMULT above), so the main going
            // small at a coordination point can't corrupt a worker's construction
            // (the GIL still serializes; the worker's big slice stays atomic). Small
            // main slices at the busy-wait => many fast GIL handoffs => the worker the
            // main waits on runs a full slice and produces => the 2.618M stall clears.
            static uint64_t s_prevrip2 = 0; static int s_pin2 = 0;
            uint64_t crip = wg_blink_get_rip(engine->blink);
            uint64_t dd = crip >= s_prevrip2 ? crip - s_prevrip2 : s_prevrip2 - crip;
            s_prevrip2 = crip;
            long pinthr = getenv("WG_PINTHR") ? atol(getenv("WG_PINTHR")) : 20;
            long mult = getenv("WG_SLICEMULT") ? atol(getenv("WG_SLICEMULT")) : 32;
            if (mult < 1) mult = 1;
            if (dd < 0x1000) { if (s_pin2 < 1000000) ++s_pin2; } else s_pin2 = 0;
            // Shrink ONLY when pinned AND another thread is contending for the GIL (a
            // starved worker the main is busy-waiting on). A pinned main with no
            // contender is a tight construction loop (workers blocked on events) —
            // keep it BIG so it races to the SetEvent that wakes them. WG_NOCONTEND
            // reverts to pin-only shrinking.
            int contended = getenv("WG_NOCONTEND") ? 1 : (s_gil_waiters > 0);
            if (s_pin2 < pinthr || !contended) slice *= (int)mult;  // big atomic
            // else sustained pin + contended => busy-wait => small base => fast handoffs
            (void)s_okstreak;
        } else if (s_use_real_threads && s_okstreak >= 4 && !getenv("WG_NO_BIGSLICE"))
            slice *= 12;
        // GIL (real-threads): serialize guest execution with the workers — see
        // wg_worker_thread_entry. No-op in cooperative mode (lock is a no-op).
        wg_thunk_lock();
        wg_update_construct_pin(engine);   // GIL-pin while the main is mid UObject construction
        WGBlinkResult r = wg_blink_run(engine->blink, slice);
        // Publish the main's RIP for the WG_BLOCK_WORKERS gate: a PINNED main (same
        // RIP across slices) means it is waiting for a worker -> release the gated
        // workers; an ADVANCING RIP means productive single-threaded boot work.
        { uint64_t mr = wg_blink_get_rip(engine->blink);
          if (mr == s_main_rip_pub) { if (s_main_rip_stall < 1000000) s_main_rip_stall++; }
          else { s_main_rip_pub = mr; s_main_rip_stall = 0; } }
        s_main_tick_pub++;   // main-tick heartbeat for the deadlock watchdog
        wg_thunk_unlock();
        s_okstreak = (r == WG_BLINK_OK) ? s_okstreak + 1 : 0;
        // Fairness (real-threads): the main guest thread runs here; without a yield
        // it re-grabs the GIL immediately and can starve a worker. If the main
        // thread is busy-waiting (esp. JIT, where a slice runs fast) for a result a
        // worker must produce, that's a livelock. Let a waiting worker take the GIL.
        if (s_use_real_threads) sched_yield();
        // consecutive thunk-less slices (real-threads busy-wait detector, below)
        static int s_okrun = 0;
        switch (r) {
            case WG_BLINK_OK:
                // Cooperative time-slice preemption: this slice ran to the
                // instruction budget WITHOUT hitting a thunk — the guest is in a
                // tight loop with no Win32 calls (e.g. a spinlock/busy-wait on a
                // flag another thread must set). Thunk-boundary preemption can't
                // fire here, so the spinning thread would starve the others forever
                // (the deadlock that stalls UE4 boot). Yield to any other runnable
                // thread so it can make progress and release the spin.
                if (!s_use_real_threads && engine->scheduler &&
                    wg_sched_other_ready(engine->scheduler)) {
                    wg_sched_yield(engine->scheduler, engine->blink, WG_THREAD_READY);
                }
                // Real-threads: the plain sched_yield above is too weak for a PURE
                // guest-code busy-wait (the main spins on a flag a worker must set,
                // with NO thunk to yield at — the async-loading stall at 0x815879
                // where the HB freezes). The main re-grabs the GIL before the OS
                // schedules the worker -> livelock. Count consecutive thunk-less
                // slices; after a run of them, SLEEP briefly with the GIL released
                // (it's unlocked here) so a worker is GUARANTEED a window to run,
                // produce, and break the spin. A legit compute burst makes a Win32
                // call (WG_BLINK_HALT) well within the threshold, resetting the
                // counter, so it isn't penalized. WG_NO_SPINYIELD disables.
                else if (s_use_real_threads && !getenv("WG_NO_SPINYIELD")) {
                    // ADAPTIVE: s_okrun counts CONSECUTIVE thunk-less slices (reset on
                    // any thunk). A short run = a compute burst (yield little). A LONG
                    // run = the main is busy-waiting for a worker result (0xB66D50
                    // task-graph spin) and making zero progress — so the longer it
                    // spins, the MORE GIL time we hand the workers so they can process
                    // and signal it, breaking the spin much faster than a fixed 150us.
                    ++s_okrun;
                    // WG_RIPYIELD: yield ONLY when the guest RIP is STUCK (a genuine
                    // busy-wait spinning in a tiny range). A UObject class CONSTRUCTION
                    // is also thunk-less but ADVANCES the RIP — yielding mid-construction
                    // lets a worker interleave and use the half-built class (the boot
                    // corruption). So keep the GIL (atomic) while the RIP advances, and
                    // only release it once the RIP is pinned = a real spin. This breaks
                    // busy-waits (0xA5C7E7) without corrupting constructions.
                    if (getenv("WG_RANGEYIELD")) {
                        // BOUNDED-RANGE busy-wait detector: a coordination spin cycles a
                        // FEW nearby code blocks (RIP min..max stays within a small window
                        // over many slices); a CONSTRUCTION advances through much wider code
                        // (calls the allocator etc., MB apart). Yield only for the bounded
                        // case -> breaks the 2.618M coordination WITHOUT interrupting
                        // constructions, so big atomic slices can run (zero corruption).
                        static uint64_t s_win[24]; static int s_wi = 0;
                        uint64_t cr = wg_blink_get_rip(engine->blink);
                        s_win[s_wi % 24] = cr; ++s_wi;
                        if (s_wi >= 24) {
                            uint64_t mn = ~0ULL, mx = 0;
                            for (int _i = 0; _i < 24; ++_i) {
                                if (s_win[_i] < mn) mn = s_win[_i];
                                if (s_win[_i] > mx) mx = s_win[_i];
                            }
                            long rng = getenv("WG_RANGE") ? atol(getenv("WG_RANGE")) : 0x40000;
                            if ((long)(mx - mn) < rng) usleep(3000);  // bounded => busy-wait
                        }
                    } else if (getenv("WG_RIPYIELD")) {
                        static uint64_t s_lastrip = 0; static int s_ripstuck = 0;
                        uint64_t cr = wg_blink_get_rip(engine->blink);
                        uint64_t dd = cr >= s_lastrip ? cr - s_lastrip : s_lastrip - cr;
                        s_lastrip = cr;
                        // A construction ADVANCES the RIP across slices (dd large) => reset,
                        // stay atomic. A busy-wait keeps the RIP pinned (dd small) => count.
                        // Once pinned, yield on EVERY slice (escalating) so even big atomic
                        // slices hand the workers a frequent window to break the coordination
                        // spin, while never yielding mid-construction (RIP moving).
                        if (dd < 0x1000) { ++s_ripstuck; } else { s_ripstuck = 0; }
                        if (s_ripstuck >= 3) {
                            int us = s_ripstuck < 12 ? 500 : (s_ripstuck < 96 ? 3000 : 12000);
                            usleep((useconds_t)us);
                        }
                    } else if (s_okrun >= 32 && (s_okrun & 31) == 0) {
                        int us = s_okrun < 256 ? 150 : (s_okrun < 4096 ? 1500 : 9000);
                        usleep((useconds_t)us);
                    }
                }
                break;
            case WG_BLINK_HALT: {
                s_okrun = 0;   // a thunk fired -> not a pure busy-wait; reset detector
                uint32_t pre_tid = (!s_use_real_threads && engine->scheduler)
                                       ? wg_sched_current_tid(engine->scheduler) : 0;
                { wg_thunk_lock(); bool _htk = handle_blink_thunk(engine); wg_thunk_unlock();
                  if (_htk) {
                    // Cooperative preemption (path B): a thunk (HLT) is a safe path
                    // boundary. Threads otherwise switch only at blocking calls, so a
                    // long non-blocking stretch starves the others — the render thread
                    // (id=0x1000) sat READY for ~1M Win32 calls during init. Every
                    // WG_PREEMPT thunks, if another thread is runnable AND the handler
                    // didn't already switch, yield so all threads make progress.
                    if (!s_use_real_threads && engine->scheduler &&
                        wg_sched_current_tid(engine->scheduler) == pre_tid) {
                        static uint64_t s_pc = 0;
                        static int s_pq = -1;
                        if (s_pq < 0) {
                            const char *e = getenv("WG_PREEMPT");
                            s_pq = e ? atoi(e) : 1024;
                            if (s_pq < 1) s_pq = 1;
                        }
                        if ((++s_pc % (uint64_t)s_pq) == 0 &&
                            wg_sched_other_ready(engine->scheduler)) {
                            wg_sched_yield(engine->scheduler, engine->blink,
                                           WG_THREAD_READY);
                        }
                    }
                    break;
                  }
                }
                uint64_t halt_rip = wg_blink_get_rip(engine->blink);
                if (s_watch_armed && halt_rip == s_watch_addr) {
                    // ssl_cipher_list_to_bytes, before the cipher loop: esi=s,
                    // s3=[s+0x7c]. The loop's "edi" flag only sets when a cipher has
                    // max_tls >= s3->tmp.max_ver. blink leaves max_ver=0x304 (TLS1.3)
                    // but the iterated list has no TLS1.3 suite -> edi stays 0 ->
                    // NO_CIPHERS. Cap max_ver at 0x303 (TLS1.2) so TLS1.2 ciphers
                    // satisfy it and a real ClientHello gets built.
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    uint32_t sub = 0; wg_blink_read_mem(engine->blink, esi + 0x7c, &sub, 4);
                    if (sub) {
                        uint32_t ver = 0;
                        wg_blink_read_mem(engine->blink, sub + 0x2ac, &ver, 4);
                        if (ver > 0x303) {
                            uint32_t v12 = 0x303;
                            wg_blink_write_mem(engine->blink, sub + 0x2ac, &v12, 4);
                        }
                        if (s_watch_count < 6)
                            WG_LOGW(TAG, "*** cipher_list_to_bytes: max_ver 0x%X -> 0x303", ver);
                    }
                    s_watch_count++;
                    wg_blink_write_mem(engine->blink, s_watch_addr, &s_watch_orig, 1);
                    wg_blink_set_rip(engine->blink, s_watch_addr);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_watch_addr, &hlt, 1);
                    break;
                }
                if (s_cloop_armed && halt_rip == s_cloop_addr) {
                    // SET_GROUPS_LIST handler `call eax`: eax = parse fn target,
                    // [esp+? ] = the "P-521:P-384:P-256" string. Capture target.
                    uint32_t eax = (uint32_t)wg_blink_get_reg(engine->blink, 0);
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    uint32_t a0=0,a1=0,a2=0,a3=0;
                    wg_blink_read_mem(engine->blink, esp + 0, &a0, 4);
                    wg_blink_read_mem(engine->blink, esp + 4, &a1, 4);
                    wg_blink_read_mem(engine->blink, esp + 8, &a2, 4);
                    wg_blink_read_mem(engine->blink, esp + 12, &a3, 4);
                    if (s_cloop_count < 12) {
                        char cstr[160] = {0};
                        if (a2) wg_blink_read_mem(engine->blink, a2, cstr, 159);
                        WG_LOGW(TAG, "  set_cipher_list(ctx=0x%X, '%s')", a1, cstr);
                    }
                    // Cap CTX max_proto_version (ctx+0x118) to TLS1.2 so the
                    // ClientHello's supported_versions stops advertising TLS1.3
                    // (we have no TLS1.3 ciphersuite — blink's cipher parser drops
                    // them). SSL_new inherits this. Leaves min (ctx+0x114) alone.
                    if (a1) {
                        uint32_t maxv = 0;
                        wg_blink_read_mem(engine->blink, a1 + 0xb8, &maxv, 4);
                        if (maxv == 0 || maxv > 0x303) {
                            uint32_t v12 = 0x303;
                            wg_blink_write_mem(engine->blink, a1 + 0xb8, &v12, 4);
                            WG_LOGW(TAG, "  capped ctx max_proto_version 0x%X -> 0x303", maxv);
                        }
                    }
                    s_cloop_count++;
                    wg_blink_write_mem(engine->blink, s_cloop_addr, &s_cloop_orig, 1);
                    wg_blink_set_rip(engine->blink, s_cloop_addr);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_cloop_addr, &hlt, 1);
                    break;
                }
                if (s_fac_armed && halt_rip == s_fac_addr) {
                    // factory(this=ecx, type=[esp+4]). At entry [esp]=caller ret.
                    uint32_t ecx = (uint32_t)wg_blink_get_reg(engine->blink, 1);
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    if (ecx < 0x10000 || ecx >= 0xFFFFF000) {
                        uint32_t ret = 0, type = 0;
                        wg_blink_read_mem(engine->blink, esp, &ret, 4);
                        wg_blink_read_mem(engine->blink, esp + 4, &type, 4);
                        if (s_fac_count < 8)
                            WG_LOGW(TAG, "  manifest factory BAD this=0x%X type=0x%X caller_ret=0x%X", ecx, type, ret);
                        s_fac_count++;
                    }
                    wg_blink_write_mem(engine->blink, s_fac_addr, &s_fac_orig, 1);
                    wg_blink_set_rip(engine->blink, s_fac_addr);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_fac_addr, &hlt, 1);
                    break;
                }
                if (s_pe_armed && (halt_rip == s_pe_ext_addr ||
                                   halt_rip == s_pe_alloc_addr ||
                                   halt_rip == s_pe_chk_addr)) {
                    // CUtlBuffer entered the 0xc0 (HasError) state, or CheckError
                    // is asserting on it. Dump the buffer object + the caller chain
                    // (stack-scan for .text return addrs) so we can see which HTTP
                    // read/put overflowed the package buffer. Correlate the three
                    // trap sites by 'this' (esi).
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    uint32_t ebp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                    const char *what = halt_rip == s_pe_ext_addr   ? "EXTERNAL/cant-grow"
                                     : halt_rip == s_pe_alloc_addr ? "REALLOC-FAIL"
                                     :                               "CHECKERROR-ASSERT";
                    if (s_pe_count < 24) {
                        uint32_t b0=0,b4=0,b8=0,bc=0,b10=0; uint8_t flag=0;
                        wg_blink_read_mem(engine->blink, esi + 0x00, &b0, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x04, &b4, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x08, &b8, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x0c, &bc, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x10, &b10, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x0f, &flag, 1);
                        WG_LOGW(TAG, "*** PKG-BUF %s this=0x%X flag=0x%02X mem=0x%X "
                                "+4=0x%X +8=0x%X +c=0x%X +10=0x%X",
                                what, esi, flag, b0, b4, b8, bc, b10);
                        char chain[256] = {0}; int ci = 0, found = 0;
                        for (int k = 0; k < 64 && found < 8; k++) {
                            uint32_t v = 0;
                            wg_blink_read_mem(engine->blink, esp + k * 4, &v, 4);
                            if (v >= 0x401000 && v < 0x700000) {
                                ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v);
                                found++;
                            }
                        }
                        WG_LOGW(TAG, "    callchain(esp=0x%X ebp=0x%X): %s", esp, ebp, chain);
                    }
                    s_pe_count++;
                    uint8_t orig = halt_rip == s_pe_ext_addr   ? s_pe_ext_orig
                                 : halt_rip == s_pe_alloc_addr ? s_pe_alloc_orig
                                 :                               s_pe_chk_orig;
                    wg_blink_write_mem(engine->blink, halt_rip, &orig, 1);
                    wg_blink_set_rip(engine->blink, halt_rip);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, halt_rip, &hlt, 1);
                    break;
                }
                if (s_hs_armed && (halt_rip == s_hs_ret_addr ||
                                   halt_rip == s_hs_gate_addr)) {
                    uint32_t eax = (uint32_t)wg_blink_get_reg(engine->blink, 0);
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    if (halt_rip == s_hs_ret_addr) {
                        uint32_t step = 0, ssl = 0;
                        wg_blink_read_mem(engine->blink, esi + 0x1a0, &step, 4);
                        wg_blink_read_mem(engine->blink, esi + 0x298, &ssl, 4);
                        WG_LOGW(TAG, "*** HS_RET=%d (1=complete,<=0=want/err) conn=0x%X ssl=0x%X step=%u",
                                (int)eax, esi, ssl, step);
                    } else {
                        WG_LOGW(TAG, "*** HS_GATE=%d (nonzero=still-in-init, 0=>done)", (int)eax);
                    }
                    uint8_t orig = (halt_rip == s_hs_ret_addr) ? s_hs_ret_orig : s_hs_gate_orig;
                    wg_blink_write_mem(engine->blink, halt_rip, &orig, 1);
                    wg_blink_set_rip(engine->blink, halt_rip);
                    wg_blink_step(engine->blink);
                    s_hs_count++;
                    if (s_hs_count < 80) {
                        uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, halt_rip, &hlt, 1);
                    } // else leave disarmed to bound cost
                    break;
                }
                if (s_disp_armed && (halt_rip == s_disp_chk_addr ||
                                     halt_rip == s_disp_pump_addr)) {
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    uint32_t tid = wg_sched_current_tid(engine->scheduler);
                    // Stack-scan for the driver: who pumps this connection's RunFrame?
                    char chain[200] = {0}; int ci = 0, found = 0;
                    for (int k = 0; k < 96 && found < 9; k++) {
                        uint32_t v = 0;
                        wg_blink_read_mem(engine->blink, esp + k * 4, &v, 4);
                        if (v >= 0x401000 && v < 0x700000) {
                            ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v);
                            found++;
                        }
                    }
                    if (halt_rip == s_disp_chk_addr) {
                        uint8_t f22 = 0;
                        wg_blink_read_mem(engine->blink, esi + 0x22, &f22, 1);
                        WG_LOGW(TAG, "*** DISP[tid=%X] post-HS conn=0x%X [+0x22]=%u -> %s | %s",
                                tid, esi, f22, f22 ? "RUN pump" : "SKIP", chain);
                    } else {
                        WG_LOGW(TAG, "*** DISP[tid=%X] pump RUNS conn=0x%X | %s", tid, esi, chain);
                    }
                    uint8_t orig = (halt_rip == s_disp_chk_addr) ? s_disp_chk_orig : s_disp_pump_orig;
                    wg_blink_write_mem(engine->blink, halt_rip, &orig, 1);
                    wg_blink_set_rip(engine->blink, halt_rip);
                    wg_blink_step(engine->blink);
                    s_disp_count++;
                    if (s_disp_count < 60) {
                        uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, halt_rip, &hlt, 1);
                    }
                    break;
                }
                if (s_snd_armed && halt_rip == s_snd_addr) {
                    // Send pump @0x4F3D16: eax = total bytes queued in the send
                    // buffer ([edi+0x230]); [edi+0x244] = bytes already sent.
                    uint32_t eax = (uint32_t)wg_blink_get_reg(engine->blink, 0);
                    uint32_t edi = (uint32_t)wg_blink_get_reg(engine->blink, 7);
                    uint32_t sent = 0; wg_blink_read_mem(engine->blink, edi + 0x244, &sent, 4);
                    if (s_snd_count < 30)
                        WG_LOGW(TAG, "*** SENDPUMP conn=0x%X queued=%u sent=%u remaining=%d",
                                edi, eax, sent, (int)(eax - sent));
                    s_snd_count++;
                    wg_blink_write_mem(engine->blink, s_snd_addr, &s_snd_orig, 1);
                    wg_blink_set_rip(engine->blink, s_snd_addr);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_snd_addr, &hlt, 1);
                    break;
                }
                if (s_sslw_armed && (halt_rip == s_sslw_addr || halt_rip == s_sndchk_addr)) {
                    uint32_t edi = (uint32_t)wg_blink_get_reg(engine->blink, 7);
                    if (halt_rip == s_sndchk_addr) {
                        uint32_t pend = 0;
                        wg_blink_read_mem(engine->blink, edi + 0x134, &pend, 4);
                        if (s_sndchk_count < 12)
                            WG_LOGW(TAG, "*** SENDGATE conn=0x%X [+0x134]=%u %s", edi, pend,
                                    pend ? "(has pending send)" : "-> SKIP (no GET queued)");
                        s_sndchk_count++;
                        wg_blink_write_mem(engine->blink, s_sndchk_addr, &s_sndchk_orig, 1);
                        wg_blink_set_rip(engine->blink, s_sndchk_addr);
                        wg_blink_step(engine->blink);
                        uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_sndchk_addr, &hlt, 1);
                        break;
                    }
                    // 0x4F40DC: the SSL_write send call. esp=[ssl][buf][len].
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    uint32_t sslp = 0, buf = 0, len = 0, b0 = 0;
                    wg_blink_read_mem(engine->blink, esp + 0, &sslp, 4);
                    wg_blink_read_mem(engine->blink, esp + 4, &buf, 4);
                    wg_blink_read_mem(engine->blink, esp + 8, &len, 4);
                    if (buf) wg_blink_read_mem(engine->blink, buf, &b0, 4);
                    if (s_sslw_count < 20)
                        WG_LOGW(TAG, "*** SSL_write SEND conn=0x%X ssl=0x%X buf=0x%X len=%u first4=0x%08X%s",
                                edi, sslp, buf, len, b0, b0 == 0x20544547 ? " ('GET ')" : "");
                    s_sslw_count++;
                    wg_blink_write_mem(engine->blink, s_sslw_addr, &s_sslw_orig, 1);
                    wg_blink_set_rip(engine->blink, s_sslw_addr);
                    wg_blink_step(engine->blink);
                    uint8_t hlt = 0xF4; wg_blink_write_mem(engine->blink, s_sslw_addr, &hlt, 1);
                    break;
                }
                if (s_errstr_armed && halt_rip == s_errstr_bp) {
                    // void ERR_put_error(int lib,int func,int reason,const char*file,int line) [cdecl]
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    uint32_t ret = 0, lib = 0, func = 0, reason = 0, file = 0, line = 0;
                    wg_blink_read_mem(engine->blink, esp, &ret, 4);
                    wg_blink_read_mem(engine->blink, esp + 4, &lib, 4);
                    wg_blink_read_mem(engine->blink, esp + 8, &func, 4);
                    wg_blink_read_mem(engine->blink, esp + 12, &reason, 4);
                    wg_blink_read_mem(engine->blink, esp + 16, &file, 4);
                    wg_blink_read_mem(engine->blink, esp + 20, &line, 4);
                    char fbuf[80] = {0};
                    if (file) wg_blink_read_mem(engine->blink, file, fbuf, 79);
                    if (s_errput_count < 30) {
                        WG_LOGW(TAG, "*** ERR_put_error lib=%u func=%u reason=%u  %s:%u  [caller=0x%X]",
                                lib, func, reason, fbuf, line, ret);
                    }
                    if (reason == 181) {
                        // Called via SSLfatal(0x69ED30): one frame up holds the real
                        // caller (ssl_cipher_list_to_bytes) + the SSL*. SSLfatal did
                        // 5 pushes before this call -> its entry esp = esp+24.
                        uint32_t up_caller = 0, sslp = 0, s3 = 0;
                        wg_blink_read_mem(engine->blink, esp + 24, &up_caller, 4);
                        wg_blink_read_mem(engine->blink, esp + 28, &sslp, 4);
                        if (sslp) wg_blink_read_mem(engine->blink, sslp + 0x7c, &s3, 4);
                        WG_LOGW(TAG, "    NO_CIPHERS: cipher_list_to_bytes ret=0x%X SSL=0x%X s3=0x%X",
                                up_caller, sslp, s3);
                        if (s3) {
                            uint32_t mask_k=0, mask_a=0, min_ver=0, max_ver=0;
                            wg_blink_read_mem(engine->blink, s3 + 0x2a0, &mask_k, 4);
                            wg_blink_read_mem(engine->blink, s3 + 0x2a4, &mask_a, 4);
                            wg_blink_read_mem(engine->blink, s3 + 0x2a8, &min_ver, 4);
                            wg_blink_read_mem(engine->blink, s3 + 0x2ac, &max_ver, 4);
                            WG_LOGW(TAG, "    masks: mask_k=0x%X mask_a=0x%X min_ver=0x%X max_ver=0x%X",
                                    mask_k, mask_a, min_ver, max_ver);
                        }
                        // ssl_security: cert=[s+0x404]; sec_cb=[cert+0xf8]; ctx=[cert+0x100]
                        uint32_t cert=0, sec_cb=0, sec_f8c=0, sec_100=0;
                        if (sslp) wg_blink_read_mem(engine->blink, sslp + 0x404, &cert, 4);
                        if (cert) {
                            wg_blink_read_mem(engine->blink, cert + 0xf8, &sec_cb, 4);
                            wg_blink_read_mem(engine->blink, cert + 0xfc, &sec_f8c, 4);
                            wg_blink_read_mem(engine->blink, cert + 0x100, &sec_100, 4);
                        }
                        WG_LOGW(TAG, "    cert=0x%X sec_cb=0x%X [cert+0xfc]=0x%X(level?) [cert+0x100]=0x%X",
                                cert, sec_cb, sec_f8c, sec_100);
                    }
                    s_errput_count++;
                    // No-op stub (void cdecl): pop return addr only (caller cleans args).
                    wg_blink_set_reg(engine->blink, 4, esp + 4);
                    wg_blink_set_rip(engine->blink, ret);
                    break;
                }
                if (halt_rip == 0) {
                    // RIP=0 can mean: (a) ExitProcess set it, or (b) a
                    // worker thread returned from its start function.
                    // Check if there are other threads to run.
                    WGThread *cur = wg_sched_current(engine->scheduler);
                    if (cur && cur->id != 1) {
                        // Worker thread returned — exit it and switch
                        uint32_t eax = (uint32_t)wg_blink_get_reg(engine->blink, 0);
                        // DIAG: the Steam download orchestrator/worker threads start
                        // at 0x53CEE0. On device they return prematurely (orchestrator
                        // code 0, then workers time out 258) leaving the package
                        // download incomplete (no .vz saved). Capture the residual
                        // .text call chain on the thread's stack at exit so we can see
                        // WHAT decided to return. image_base guard to steam.exe.
                        if (cur->start_addr == 0x53CEE0u && engine->pe_image &&
                            engine->pe_image->image_base == 0x400000) {
                            uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                            char chain[300] = {0}; int ci = 0, found = 0;
                            for (int k = 0; k < 160 && found < 12; k++) {
                                uint32_t v = 0;
                                wg_blink_read_mem(engine->blink, esp + k * 4, &v, 4);
                                if (v >= 0x401000 && v < 0x700000) {
                                    ci += snprintf(chain + ci, sizeof(chain) - ci, "0x%X ", v);
                                    found++;
                                }
                            }
                            WG_LOGW(TAG, "*** DLTHREAD-EXIT tid=0x%X code=%u esp=0x%X "
                                    "wait_h=0x%X chain: %s", cur->id, eax, esp,
                                    cur->wait_handle, chain);
                        }
                        wg_dump_threads(engine, "thread-return");
                        // Wake any joiners FIRST (threads WaitForSingleObject-ing this
                        // thread's handle) so they're READY before we switch. Then
                        // exit_thread marks this thread EXITED and ALREADY switches to
                        // the next runnable thread (restoring its regs). Do NOT call
                        // switch_next again afterward: the thread it switched to is now
                        // RUNNING (not READY), so a second switch_next would find
                        // nothing and we'd wrongly declare "program exited" — killing
                        // the process whenever a worker returns while another thread is
                        // still alive (this froze the reactor test + likely Steam).
                        wg_sched_wake(engine->scheduler, cur->handle);
                        wg_sched_exit_thread(engine->scheduler, engine->blink, eax);
                        if (wg_sched_current(engine->scheduler))
                            break; // a runnable thread is now active — keep going
                    }
                    // Main-thread jump-to-null recovery. A UE4 static constructor
                    // (or any guest code) that calls through an uninitialized
                    // function pointer lands at RIP=0. If the word on top of the
                    // stack is a return address a CALL pushed — i.e. it points
                    // into the image's .text — this was a call-to-null, NOT a real
                    // exit (an entry RET-to-0 leaves a non-.text word there).
                    // Return 0 to the caller and keep running so the program makes
                    // progress instead of dying on the first null indirect call.
                    if (engine->pe_image) {
                        bool g64 = engine->pe_image->is_64bit;
                        uint64_t sp = wg_blink_get_reg(engine->blink, 4);
                        uint64_t ret = 0;
                        wg_blink_read_mem(engine->blink, sp, &ret, g64 ? 8 : 4);
                        uint64_t img_lo = engine->pe_image->image_base + 0x1000;
                        uint64_t img_hi = engine->pe_image->image_base +
                                          engine->pe_image->size_of_image;
                        if (ret >= img_lo && ret < img_hi && wg_recover_ok(ret)) {
                            s_null_call_recover++;
                            if (getenv("WG_BADVTBL")) {
                                static int _bv = 0;
                                if (_bv++ < 3) {
                                    uint64_t r14 = wg_blink_get_reg(engine->blink, 14);
                                    uint64_t rcx = wg_blink_get_reg(engine->blink, 1);
                                    uint64_t vt = 0; wg_blink_read_mem(engine->blink, r14, &vt, 8);
                                    WG_LOGW(TAG, "[badvtbl] bad=0x%llx ret=0x%llx r14(obj)=0x%llx rcx=0x%llx vtbl=[r14]=0x%llx",
                                            (unsigned long long)halt_rip, (unsigned long long)ret,
                                            (unsigned long long)r14, (unsigned long long)rcx,
                                            (unsigned long long)vt);
                                    for (int _k = 0; _k < 8; _k++) {
                                        uint64_t e = 0; wg_blink_read_mem(engine->blink, vt + _k * 8, &e, 8);
                                        WG_LOGW(TAG, "   vtbl[+0x%x]=0x%llx", _k * 8, (unsigned long long)e);
                                    }
                                }
                            }
                            if ((s_null_call_recover % 1000) == 1)
                                WG_LOGW(TAG, "Recover null indirect call -> return 0 "
                                        "to 0x%llX (count=%llu)",
                                        (unsigned long long)ret,
                                        (unsigned long long)s_null_call_recover);
                            // One-shot: identify the null-vtable object being polled
                            // (e.g. the RHI device-lost spin at 0xE1D98A).
                            { static uint64_t s_seen = 0; if (getenv("WG_FMT") && ret != s_seen) {
                                s_seen = ret;
                                uint64_t r9 = wg_blink_get_reg(engine->blink, 9);
                                uint64_t rcx = wg_blink_get_reg(engine->blink, 1);
                                uint64_t rdi = wg_blink_get_reg(engine->blink, 7);
                                uint32_t vt0 = 0, m7 = 0;
                                wg_blink_read_mem(engine->blink, (uint32_t)r9, &vt0, 4);
                                wg_blink_read_mem(engine->blink, (uint32_t)r9 + 0x38, &m7, 4);
                                WG_LOGW(TAG, "  null-vtable @0x%llX: R9(vtbl)=0x%llX [vtbl+0]=0x%X "
                                        "[vtbl+0x38]=0x%X RCX=0x%llX RDI=0x%llX",
                                        (unsigned long long)ret, (unsigned long long)r9, vt0, m7,
                                        (unsigned long long)rcx, (unsigned long long)rdi);
                            } }
                            wg_blink_set_reg(engine->blink, 0, 0);            // RAX = 0
                            wg_blink_set_reg(engine->blink, 4, sp + (g64 ? 8 : 4));
                            wg_blink_set_rip(engine->blink, (uint32_t)ret);
                            break;
                        }
                        if (s_recover_streak > WG_RECOVER_SPIN_LIMIT)
                            WG_LOGE(TAG, "Null-call spin at 0x%llX exceeded %d "
                                    "recoveries — stopping (needs real runtime)",
                                    (unsigned long long)ret, WG_RECOVER_SPIN_LIMIT);
                    }
                    WG_LOGI(TAG, "Program exited normally after %llu ticks",
                            (unsigned long long)engine->tick_count);
                } else {
                    // Try auto-recovery for calls to a bad address (uninitialized
                    // function pointer / bad vtable): if RIP landed OUTSIDE the
                    // image and the word on top of the stack is a return address
                    // into .text, this was an indirect CALL through garbage —
                    // return 0 to the caller and keep going. A fault INSIDE the
                    // image is a real data access (e.g. NULL deref) and falls
                    // through to the SEH path below. Uses the real image bounds
                    // (a 64-bit UE4 image is tens of MB — the old fixed
                    // image_base+0x4C0000 window was far too small).
                    bool g64 = engine->pe_image && engine->pe_image->is_64bit;
                    uint64_t img_lo = engine->pe_image
                        ? engine->pe_image->image_base + 0x1000 : 0x401000;
                    uint64_t img_hi = engine->pe_image
                        ? engine->pe_image->image_base + engine->pe_image->size_of_image
                        : 0x8C0000;
                    if (halt_rip < img_lo || halt_rip >= img_hi) {
                        uint64_t sp = wg_blink_get_reg(engine->blink, 4);
                        uint64_t ret = 0;
                        wg_blink_read_mem(engine->blink, sp, &ret, g64 ? 8 : 4);
                        if (ret >= img_lo && ret < img_hi && wg_recover_ok(ret)) {
                            s_null_call_recover++;
                            if (getenv("WG_BADVTBL")) {
                                static int _bv = 0;
                                if (_bv++ < 3) {
                                    uint64_t r14 = wg_blink_get_reg(engine->blink, 14);
                                    uint64_t rcx = wg_blink_get_reg(engine->blink, 1);
                                    uint64_t vt = 0; wg_blink_read_mem(engine->blink, r14, &vt, 8);
                                    WG_LOGW(TAG, "[badvtbl] bad=0x%llx ret=0x%llx r14(obj)=0x%llx rcx=0x%llx vtbl=[r14]=0x%llx",
                                            (unsigned long long)halt_rip, (unsigned long long)ret,
                                            (unsigned long long)r14, (unsigned long long)rcx,
                                            (unsigned long long)vt);
                                    for (int _k = 0; _k < 8; _k++) {
                                        uint64_t e = 0; wg_blink_read_mem(engine->blink, vt + _k * 8, &e, 8);
                                        WG_LOGW(TAG, "   vtbl[+0x%x]=0x%llx", _k * 8, (unsigned long long)e);
                                    }
                                }
                            }
                            if ((s_null_call_recover % 1000) == 1)
                                WG_LOGW(TAG, "Auto-recover: call to bad addr 0x%llx "
                                        "-> return 0 to 0x%llX (count=%llu)",
                                        (unsigned long long)halt_rip,
                                        (unsigned long long)ret,
                                        (unsigned long long)s_null_call_recover);
                            wg_blink_set_reg(engine->blink, 0, 0); // RAX = 0
                            wg_blink_set_reg(engine->blink, 4, sp + (g64 ? 8 : 4));
                            wg_blink_set_rip(engine->blink, (uint32_t)ret);
                            break;
                        }
                    }
                    // A real memory fault (e.g. NULL deref): raise a Windows
                    // STATUS_ACCESS_VIOLATION into the guest's SEH chain so it
                    // recovers like on Windows. This is what lets Steam's
                    // bootstrapper survive its NULL-connection dereference
                    // instead of our zero-page map silently swallowing it.
                    {
                        int stop = wg_blink_get_stop_reason(engine->blink);
                        if (s_enable_guest_seh && (stop == -4 /*segfault*/ ||
                                                    stop == -8 /*#GP*/)) {
                            uint32_t fa = (uint32_t)wg_blink_get_fault_addr(engine->blink);
                            if (wg_raise_guest_exception(engine, 0xC0000005u, fa,
                                                         (uint32_t)halt_rip, false))
                                break;
                        }
                    }
                    // WG_ABORT_CONTINUE: limp past a real memory fault too (same as the
                    // WG_BLINK_ERROR path) — unwind the faulting function (pop return addr,
                    // null result) so the boot keeps going past a non-critical crash toward
                    // a rendered frame. Bounded so a genuine crash-storm still stops.
                    if (getenv("WG_ABORT_CONTINUE")) {
                        static int s_limpsv = 0;
                        uint64_t img_lo = engine->pe_image ? engine->pe_image->image_base + 0x1000 : 0x401000;
                        uint64_t img_hi = engine->pe_image ? engine->pe_image->image_base + engine->pe_image->size_of_image : 0x8C0000;
                        if (s_limpsv < 100000) {
                            uint64_t rsp = wg_blink_get_reg(engine->blink, 4);
                            uint64_t ret = 0; wg_blink_read_mem(engine->blink, rsp, &ret, 8);
                            if (ret >= img_lo && ret < img_hi) {
                                if ((s_limpsv++ % 1000) == 0)
                                    WG_LOGW(TAG, "WG_ABORT_CONTINUE(sv): unwind fault #%d @0x%llx -> ret 0x%llx",
                                            s_limpsv, (unsigned long long)halt_rip, (unsigned long long)ret);
                                wg_blink_set_reg(engine->blink, 0, 0);
                                wg_blink_set_reg(engine->blink, 4, rsp + 8);
                                wg_blink_set_rip(engine->blink, ret);
                                break;
                            }
                            if ((s_limpsv++ % 1000) == 0)
                                WG_LOGW(TAG, "WG_ABORT_CONTINUE(sv): skip fault #%d @0x%llx (no ret)",
                                        s_limpsv, (unsigned long long)halt_rip);
                            wg_blink_set_rip(engine->blink, halt_rip + 1);
                            break;
                        }
                    }
                    WG_LOGE(TAG, "Crash at RIP=0x%llx (SIGSEGV — bad pointer or unmapped memory)",
                            (unsigned long long)halt_rip);
                    // 64-bit register dump — the level-load crash (0x8f29c4) walks a
                    // hash table via rdi=[rsi+0x460]+idx*32; a truncated region-3
                    // (>4GB) pointer shows here as a low/garbage table base.
                    {
                        uint64_t r64[16];
                        for (int i = 0; i < 16; i++) r64[i] = wg_blink_get_reg(engine->blink, i);
                        WG_LOGE(TAG, "  RAX=%016llx RCX=%016llx RDX=%016llx RBX=%016llx",
                            (unsigned long long)r64[0],(unsigned long long)r64[1],
                            (unsigned long long)r64[2],(unsigned long long)r64[3]);
                        WG_LOGE(TAG, "  RSP=%016llx RBP=%016llx RSI=%016llx RDI=%016llx",
                            (unsigned long long)r64[4],(unsigned long long)r64[5],
                            (unsigned long long)r64[6],(unsigned long long)r64[7]);
                        WG_LOGE(TAG, "  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx",
                            (unsigned long long)r64[8],(unsigned long long)r64[9],
                            (unsigned long long)r64[10],(unsigned long long)r64[11]);
                        uint64_t rsi = r64[6], tbl = 0;
                        wg_blink_read_mem(engine->blink, rsi + 0x460, &tbl, 8);
                        WG_LOGE(TAG, "  [RSI+0x460](table base)=%016llx  [RSI+0x18](mask)=?",
                            (unsigned long long)tbl);
                    }
                    // Dump registers for debugging
                    WG_LOGE(TAG, "  EAX=%08X ECX=%08X EDX=%08X EBX=%08X",
                        (uint32_t)wg_blink_get_reg(engine->blink, 0),
                        (uint32_t)wg_blink_get_reg(engine->blink, 1),
                        (uint32_t)wg_blink_get_reg(engine->blink, 2),
                        (uint32_t)wg_blink_get_reg(engine->blink, 3));
                    WG_LOGE(TAG, "  ESP=%08X EBP=%08X ESI=%08X EDI=%08X",
                        (uint32_t)wg_blink_get_reg(engine->blink, 4),
                        (uint32_t)wg_blink_get_reg(engine->blink, 5),
                        (uint32_t)wg_blink_get_reg(engine->blink, 6),
                        (uint32_t)wg_blink_get_reg(engine->blink, 7));
                    // Bytes at crash RIP
                    uint8_t code[16] = {0};
                    wg_blink_read_mem(engine->blink, halt_rip, code, 16);
                    WG_LOGE(TAG, "  code@RIP: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                        code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7],
                        code[8],code[9],code[10],code[11],code[12],code[13],code[14],code[15]);
                    // Stack dump
                    uint32_t stk[8] = {0};
                    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
                    wg_blink_read_mem(engine->blink, esp, stk, sizeof(stk));
                    WG_LOGE(TAG, "  stack: %08X %08X %08X %08X  %08X %08X %08X %08X",
                        stk[0],stk[1],stk[2],stk[3],stk[4],stk[5],stk[6],stk[7]);
                    // Bytes before return address (call instruction)
                    if (stk[0] >= 8 && stk[0] < 0x8C0000) {
                        uint8_t caller[16] = {0};
                        wg_blink_read_mem(engine->blink, stk[0] - 8, caller, 16);
                        WG_LOGE(TAG, "  caller@%08X-8: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                            stk[0], caller[0],caller[1],caller[2],caller[3],caller[4],caller[5],caller[6],caller[7],
                            caller[8],caller[9],caller[10],caller[11],caller[12],caller[13],caller[14],caller[15]);
                    }
                    // EBP-based frame: dump caller's return addr and locals
                    uint32_t ebp = (uint32_t)wg_blink_get_reg(engine->blink, 5);
                    if (ebp >= 0x7FFE0000 && ebp < 0x80000000) {
                        uint32_t frame[8] = {0};
                        wg_blink_read_mem(engine->blink, ebp, frame, 32);
                        WG_LOGE(TAG, "  [EBP+00]: %08X %08X %08X %08X  %08X %08X %08X %08X",
                            frame[0],frame[1],frame[2],frame[3],frame[4],frame[5],frame[6],frame[7]);
                        // Dump caller's code at return address
                        if (frame[1] >= 0x401000 && frame[1] < 0x8C0000) {
                            uint8_t callercode[16] = {0};
                            wg_blink_read_mem(engine->blink, frame[1] - 8, callercode, 16);
                            WG_LOGE(TAG, "  retaddr@%08X-8: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                                frame[1], callercode[0],callercode[1],callercode[2],callercode[3],
                                callercode[4],callercode[5],callercode[6],callercode[7],
                                callercode[8],callercode[9],callercode[10],callercode[11],
                                callercode[12],callercode[13],callercode[14],callercode[15]);
                        }
                    }
                    // Dump [EDI] (often the heap buffer being operated on)
                    uint32_t edi = (uint32_t)wg_blink_get_reg(engine->blink, 7);
                    if (edi >= 0x20000000 && edi < 0x30000000) {
                        uint32_t dibuf[8] = {0};
                        wg_blink_read_mem(engine->blink, edi, dibuf, 32);
                        WG_LOGE(TAG, "  [EDI+00]: %08X %08X %08X %08X  %08X %08X %08X %08X",
                            dibuf[0],dibuf[1],dibuf[2],dibuf[3],dibuf[4],dibuf[5],dibuf[6],dibuf[7]);
                    }
                    // Memory at ESI (likely object with bad vtable)
                    uint32_t esi = (uint32_t)wg_blink_get_reg(engine->blink, 6);
                    if (esi) {
                        uint32_t obj[16] = {0};
                        wg_blink_read_mem(engine->blink, esi, obj, sizeof(obj));
                        WG_LOGE(TAG, "  [ESI+00]: %08X %08X %08X %08X  %08X %08X %08X %08X",
                            obj[0],obj[1],obj[2],obj[3],obj[4],obj[5],obj[6],obj[7]);
                        WG_LOGE(TAG, "  [ESI+20]: %08X %08X %08X %08X  %08X %08X %08X %08X",
                            obj[8],obj[9],obj[10],obj[11],obj[12],obj[13],obj[14],obj[15]);
                    }
                    // Last Win32 API calls before crash
                    WG_LOGE(TAG, "  last API calls:");
                    for (int ri = 0; ri < WG_CALL_RING_SIZE; ri++) {
                        int idx = (s_call_ring_idx - WG_CALL_RING_SIZE + ri) % WG_CALL_RING_SIZE;
                        if (idx < 0) idx += WG_CALL_RING_SIZE;
                        if (s_call_ring[idx].fn)
                            WG_LOGE(TAG, "    %s -> 0x%llX",
                                s_call_ring[idx].fn,
                                (unsigned long long)s_call_ring[idx].ret);
                    }
                }
                engine->state = WG_ENGINE_STOPPED;
                break;
            }
            case WG_BLINK_SYSCALL:
                WG_LOGD(TAG, "Syscall intercepted (blink)");
                break;
            case WG_BLINK_ERROR:
                { wg_thunk_lock(); bool _htk = handle_blink_thunk(engine); wg_thunk_unlock(); if (_htk) break; }
                // WG_ABORT_CONTINUE: limp past a guest abort/crash instead of stopping.
                // The Visage boot reaches Slate UI init then LowLevelFatalError's on a
                // missing Engine asset (curve compression settings) whose handler then
                // crashes. Those assets aren't needed to draw the title-screen UI, so
                // skip the faulting instruction and keep running to try to reach a
                // rendered frame. Bounded so a genuine infinite crash-loop still stops.
                if (getenv("WG_ABORT_CONTINUE")) {
                    static int s_limp = 0;
                    uint64_t crip = wg_blink_get_rip(engine->blink);
                    uint64_t img_lo = engine->pe_image ? engine->pe_image->image_base + 0x1000 : 0x401000;
                    uint64_t img_hi = engine->pe_image ? engine->pe_image->image_base + engine->pe_image->size_of_image : 0x8C0000;
                    if (s_limp < 100000) {
                        // RETURN from the faulting function instead of skipping a byte: pop
                        // the return address off the stack and continue at the caller with a
                        // null result (rax=0). The crash is in the fatal's error-message
                        // builder (an outer-chain walk over corrupt data); unwinding it lets
                        // the boot keep going past a non-critical missing-asset fatal.
                        uint64_t rsp = wg_blink_get_reg(engine->blink, 4);
                        uint64_t ret = 0; wg_blink_read_mem(engine->blink, rsp, &ret, 8);
                        if (ret >= img_lo && ret < img_hi) {
                            if ((s_limp++ % 1000) == 0)
                                WG_LOGW(TAG, "WG_ABORT_CONTINUE: unwinding crash #%d @0x%llx -> ret 0x%llx",
                                        s_limp, (unsigned long long)crip, (unsigned long long)ret);
                            wg_blink_set_reg(engine->blink, 0, 0);          // rax = 0 (null result)
                            wg_blink_set_reg(engine->blink, 4, rsp + 8);    // pop return addr
                            wg_blink_set_rip(engine->blink, ret);
                            break;
                        }
                        // No valid return addr on the stack — skip the faulting byte instead.
                        if ((s_limp++ % 1000) == 0)
                            WG_LOGW(TAG, "WG_ABORT_CONTINUE: skip crash #%d @0x%llx (no ret)",
                                    s_limp, (unsigned long long)crip);
                        wg_blink_set_rip(engine->blink, crip + 1);
                        break;
                    }
                }
                WG_LOGE(TAG, "Crash at RIP=0x%llx",
                        (unsigned long long)wg_blink_get_rip(engine->blink));
                engine->state = WG_ENGINE_STOPPED;
                break;
        }
    } else {
        WGInterpResult r = wg_x86_exec_block(
            engine->cpu, engine->memory, engine->instructions_per_tick);
        switch (r) {
            case WG_INTERP_OK: break;
            case WG_INTERP_HALT:
                engine->state = WG_ENGINE_STOPPED;
                break;
            case WG_INTERP_SYSCALL:
                break;
            case WG_INTERP_ERROR:
                engine->state = WG_ENGINE_ERROR;
                break;
        }
    }
}

// Run the engine synchronously until it halts or errors.
// Uses small instruction batches so thunks are caught promptly.
WGEngineState wg_engine_run_sync(WGEngine *engine, int max_ticks) {
    if (!wg_engine_run(engine)) return engine->state;

    int saved = engine->instructions_per_tick;
    engine->instructions_per_tick = 1000; // smaller batches for thunk detection

    for (int i = 0; i < max_ticks; i++) {
        wg_engine_tick(engine);
        if (engine->state != WG_ENGINE_RUNNING) break;
    }

    engine->instructions_per_tick = saved;
    return engine->state;
}

void wg_engine_resume(WGEngine *engine) {
    if (!engine || engine->state != WG_ENGINE_PAUSED) return;
    WG_LOGI(TAG, "Resuming from dialog pause");
    engine->state = WG_ENGINE_RUNNING;
}

// True when a modal dialog is up and waiting for input (Next/Cancel/...).
bool wg_engine_dialog_active(WGEngine *engine) {
    (void)engine;
    return s_dlg_active;
}

// Deliver a button click to the modal dialog: WM_COMMAND(ctrl_id, BN_CLICKED).
// Re-enters the dialog proc so NSIS advances the wizard (and eventually calls
// EndDialog). Common ids: 1 = Next/Install (IDOK), 2 = Cancel, 3 = Back.
void wg_engine_dialog_command(WGEngine *engine, uint32_t ctrl_id) {
    if (!engine || engine->state != WG_ENGINE_PAUSED) return;
    // If we paused inside nsDialogs::Show, the guest is suspended right after the
    // Show call. NSIS drives custom-page transitions itself, so just resume and
    // let it advance to the next page (which pauses again) — injecting a
    // WM_COMMAND here would nest onto a half-finished WM_INITDIALOG and stall.
    if (s_nsd_show_pause) {
        s_nsd_show_pause = false;
        WG_LOGI(TAG, "nsDialogs page: resume (tap id=%u)", ctrl_id);
        engine->state = WG_ENGINE_RUNNING;
        return;
    }
    if (!s_dlg_active || !s_dlg_proc) return;
    WGDlgCtrl *c = wg_find_ctrl(s_dlg_hwnd, ctrl_id);
    uint32_t ctrl_hwnd = c ? (WG_CTRL_HWND_BASE + (uint32_t)(c - s_ctrls)) : 0;
    uint32_t esp = (uint32_t)wg_blink_get_reg(engine->blink, 4);
    uint32_t new_rsp = esp - 20;
    uint32_t stack_data[5] = {
        WG_DLG_SENTINEL,          // return trap
        s_dlg_hwnd,               // hwnd
        0x0111,                   // WM_COMMAND
        ctrl_id,                  // wParam = MAKEWPARAM(id, BN_CLICKED=0)
        ctrl_hwnd                 // lParam = control handle
    };
    wg_blink_write_mem(engine->blink, new_rsp, stack_data, 20);
    wg_blink_set_reg(engine->blink, 4, new_rsp);
    wg_blink_set_rip(engine->blink, s_dlg_proc);
    wg_blink_set_reg(engine->blink, 0, 0);
    engine->state = WG_ENGINE_RUNNING;
    WG_LOGI(TAG, "Dialog command: WM_COMMAND id=%u -> dlgproc 0x%X", ctrl_id, s_dlg_proc);
}

// Hit-test a point (in the compositor's 800x600 virtual space) against the
// modal dialog's buttons. Returns the button control id under the point (so the
// caller can wg_engine_dialog_command it), or 0. Lets the user tap the actual
// Back/Next/Cancel rendered in the window instead of native overlay buttons.
uint32_t wg_engine_hit_test(WGEngine *engine, int virt_x, int virt_y) {
    (void)engine;
    if (!s_dlg_active || !s_dlg_hwnd) return 0;
    WGWin32Window *w = wg_wm_find(s_dlg_hwnd);
    if (!w) return 0;
    int32_t cw = 0, ch = 0;
    if (!wg_wm_get_client(s_dlg_hwnd, &cw, &ch)) return 0;
    int tb = (w->parent == 0) ? WG_TITLEBAR_H : 0;
    int cx = virt_x - w->x;
    int cy = virt_y - (w->y + tb);            // into client coords
    if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) return 0;
    for (int i = 0; i < s_ctrl_count; i++) {
        WGDlgCtrl *c = &s_ctrls[i];
        if (c->hwnd != s_dlg_hwnd || c->cls != 0x0080) continue;  // buttons only
        if (!(c->style & 0x10000000u)) continue;                  // WS_VISIBLE
        float sx = c->dlg_cx ? (float)cw / c->dlg_cx : 1.0f;
        float sy = c->dlg_cy ? (float)ch / c->dlg_cy : 1.0f;
        int px = (int)(c->x * sx), py = (int)(c->y * sy);
        int pw = (int)(c->cx * sx), ph = (int)(c->cy * sy);
        if (cx >= px && cx < px + pw && cy >= py && cy < py + ph) return c->id;
    }
    return 0;
}

void wg_engine_stop(WGEngine *engine) {
    if (!engine) return;
    if (engine->state == WG_ENGINE_RUNNING) {
        engine->state = WG_ENGINE_STOPPED;
    }
}

WGEngineState wg_engine_get_state(const WGEngine *engine) {
    return engine ? engine->state : WG_ENGINE_ERROR;
}

const char *wg_engine_take_pending_exec(WGEngine *engine) {
    (void)engine;
    if (!s_pending_exec[0]) return NULL;
    static char path[1024];
    strncpy(path, s_pending_exec, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    s_pending_exec[0] = 0;   // one-shot
    return path;
}

// Accessors for the D3D11/DXGI layer (wg_d3d11.c).
void *wg_engine_blink(WGEngine *engine) { return engine ? engine->blink : NULL; }
uint32_t wg_engine_guest_alloc(WGEngine *engine, uint32_t size) {
    return engine ? wg_guest_alloc(engine, size) : 0;
}
