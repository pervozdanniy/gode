#include "context.h"
#include "g.h"
#include "scheduler.h"
#include "runtime.h"
#include <cstdint>
#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>

#define GCTX_TRACE(fmt, ...) \
  fprintf(stderr, "[GCTX   tid=%ld] " fmt "\n", \
          (long)syscall(SYS_gettid), ##__VA_ARGS__)

// ---- Boost.Context fcontext C API (transfer_t version) ----
// The actual ABI: jump_fcontext returns {fctx, data} in RAX+RDX,
// and the entry function receives the same struct in RDI+RSI.
extern "C" {
  typedef void* fcontext_t;

  struct fctx_transfer_t {
    fcontext_t  fctx;
    void*       data;
  };

  extern fctx_transfer_t jump_fcontext(fcontext_t to, void* vp);
  extern fcontext_t make_fcontext(void* sp, std::size_t size,
                                  void (*fn)(fctx_transfer_t));
}

// V8 HandleScopeData save/restore (defined in goroutine-thread-state.cc).
extern "C" void v8_goroutine_save_hsd(v8::Isolate* isolate, void* buf);
extern "C" void v8_goroutine_restore_hsd(v8::Isolate* isolate, const void* buf);
extern "C" void v8_goroutine_force_new_handle_block(v8::Isolate* isolate);
extern "C" void v8_goroutine_gc_park(v8::Isolate* isolate, void* state);
extern "C" void v8_goroutine_gc_unpark(void* state);
// Phase 1.3: SeqLock — wait until no shape transition is in flight.
extern "C" void v8_goroutine_shape_seqlock_wait();

namespace node {
namespace goroutine {

// ---- Thread-local per-M state ----
// g0 (scheduler) context that the current goroutine can jump back to.
static thread_local fcontext_t tls_sched_ctx = nullptr;
// Currently executing goroutine on this M-thread.
static thread_local G* tls_current_g = nullptr;

// ---- Goroutine entry point (runs on G's own 64 KB stack) ----
static void goroutine_entry(fctx_transfer_t t) {
  // t.fctx = scheduler (g0) context that jumped to us.
  // t.data = G* pointer.
  GCTX_TRACE("goroutine_entry: entered");
  tls_sched_ctx = t.fctx;
  G* g = static_cast<G*>(t.data);
  GCTX_TRACE("goroutine_entry: G%llu, calling GetCurrent()", (unsigned long long)g->goid());
  v8::Isolate* iso = v8::Isolate::GetCurrent();
  GCTX_TRACE("goroutine_entry: G%llu, isolate=%p, calling Execute()", (unsigned long long)g->goid(), (void*)iso);
  g->Execute(iso);
  GCTX_TRACE("goroutine_entry: G%llu Execute() done", (unsigned long long)g->goid());
  g->SetState(GState::Gdead);

  // Return to scheduler.  Never returns.
  jump_fcontext(tls_sched_ctx, static_cast<void*>(g));
  __builtin_unreachable();
}

// ---- Public API ----

void* InitContext(G* g, void* stack_top) {
  if (!g || !stack_top) return nullptr;
  return make_fcontext(stack_top, g->stack()->size(), goroutine_entry);
}

void RunG(G* g, v8::Isolate* isolate) {
  GCTX_TRACE("RunG: starting G%llu", (unsigned long long)g->goid());

  // ---- Save g0's V8 HandleScopeData ----
  char g0_hsd[64];
  GCTX_TRACE("RunG: G%llu -> save_hsd", (unsigned long long)g->goid());
  v8_goroutine_save_hsd(isolate, g0_hsd);
  GCTX_TRACE("RunG: G%llu -> save_hsd done", (unsigned long long)g->goid());

  // Restore G's saved HSD (resuming), or force a fresh handle block (new G).
  if (g->has_saved_hsd()) {
    GCTX_TRACE("RunG: G%llu -> restore_hsd", (unsigned long long)g->goid());
    v8_goroutine_restore_hsd(isolate, g->saved_hsd_buf());
    GCTX_TRACE("RunG: G%llu -> restore_hsd done", (unsigned long long)g->goid());
  } else {
    GCTX_TRACE("RunG: G%llu -> force_new_handle_block", (unsigned long long)g->goid());
    v8_goroutine_force_new_handle_block(isolate);
    GCTX_TRACE("RunG: G%llu -> force_new_handle_block done", (unsigned long long)g->goid());
  }

  // Set V8 stack limit for the goroutine's small stack.
  uintptr_t g_stack_bottom = reinterpret_cast<uintptr_t>(g->stack()->top());
  GCTX_TRACE("RunG: G%llu -> SetStackLimit(%p)", (unsigned long long)g->goid(), (void*)g_stack_bottom);
  isolate->SetStackLimit(g_stack_bottom + 8192);
  GCTX_TRACE("RunG: G%llu -> jump_fcontext (ctx=%p)", (unsigned long long)g->goid(), g->stack_context());

  tls_current_g = g;

  fctx_transfer_t result = jump_fcontext(
      static_cast<fcontext_t>(g->stack_context()),
      static_cast<void*>(g));

  // Back on g0 stack.
  G* returned_g = static_cast<G*>(result.data);
  returned_g->SaveContext(result.fctx);

  GCTX_TRACE("RunG: G%llu returned (state=%d)", (unsigned long long)returned_g->goid(), (int)returned_g->state());

  // ---- Save G's HSD, restore g0's ----
  v8_goroutine_save_hsd(isolate, returned_g->saved_hsd_buf());
  returned_g->mark_saved_hsd();
  v8_goroutine_restore_hsd(isolate, g0_hsd);

  tls_current_g = nullptr;

  // Restore V8 stack limit for the caller's stack.
  uintptr_t sp = reinterpret_cast<uintptr_t>(&result);
  isolate->SetStackLimit(sp - (900 * 1024));
}

void YieldG() {
  G* g = tls_current_g;
  if (!g) return;

  GCTX_TRACE("YieldG: G%llu yielding...", (unsigned long long)g->goid());

  g->SetState(GState::Grunnable);
  Scheduler::GetInstance()->Schedule(g);
  Runtime::GetInstance()->NotifyGoroutineAvailable();

  // Park: snapshot goroutine's TLT onto gc_state so GC can scan its mmap
  // stack while it's yielded. Must happen BEFORE jump to scheduler.
  v8_goroutine_gc_park(v8::Isolate::GetCurrent(), g->gc_state());
  // Jump back to scheduler (g0).
  fctx_transfer_t t = jump_fcontext(tls_sched_ctx, static_cast<void*>(g));
  // Goroutine resumes here (RunG called jump_fcontext back to us).
  // Phase 1.3: SeqLock — before touching V8 heap, ensure no shape transition
  // is in progress on the main thread. Spins with cpu_relax (fast path ~1ns).
  v8_goroutine_shape_seqlock_wait();
  // Unpark: remove from GC scanning registry — we're running again.
  v8_goroutine_gc_unpark(g->gc_state());
  // Update g0 context for next yield.
  tls_sched_ctx = t.fctx;
}

G* CurrentG() {
  return tls_current_g;
}

}  // namespace goroutine
}  // namespace node

