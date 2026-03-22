#include "context.h"
#include "g.h"
#include "scheduler.h"
#include "runtime.h"
#include <cstdint>

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
  tls_sched_ctx = t.fctx;
  G* g = static_cast<G*>(t.data);

  g->Execute(v8::Isolate::GetCurrent());
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
  // ---- Save g0's V8 HandleScopeData ----
  char g0_hsd[64];
  v8_goroutine_save_hsd(isolate, g0_hsd);

  // Restore G's saved HSD (resuming), or force a fresh handle block (new G).
  if (g->has_saved_hsd()) {
    v8_goroutine_restore_hsd(isolate, g->saved_hsd_buf());
  } else {
    v8_goroutine_force_new_handle_block(isolate);
  }

  // Set V8 stack limit for the goroutine's small stack.
  uintptr_t g_stack_bottom = reinterpret_cast<uintptr_t>(g->stack()->top());
  isolate->SetStackLimit(g_stack_bottom + 8192);

  tls_current_g = g;

  fctx_transfer_t result = jump_fcontext(
      static_cast<fcontext_t>(g->stack_context()),
      static_cast<void*>(g));

  // Back on g0 stack.
  G* returned_g = static_cast<G*>(result.data);
  returned_g->SaveContext(result.fctx);

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

  g->SetState(GState::Grunnable);
  Scheduler::GetInstance()->Schedule(g);
  Runtime::GetInstance()->NotifyGoroutineAvailable();

  // Jump back to scheduler (g0).
  fctx_transfer_t t = jump_fcontext(tls_sched_ctx, static_cast<void*>(g));
  // Resumed — update g0 context for next yield.
  tls_sched_ctx = t.fctx;
}

G* CurrentG() {
  return tls_current_g;
}

}  // namespace goroutine
}  // namespace node

