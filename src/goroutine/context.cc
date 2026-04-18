#include "context.h"
#include "g.h"
#include "scheduler.h"
#include "runtime.h"
#include <cstdint>
#include <cstdio>


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
// Set TLS pointer so local-heap.cc safepoint hooks can find goroutine gc_state.
extern "C" void v8_goroutine_set_current_gc_state(void* state);
// Phase 1.3: SeqLock — wait until no shape transition is in flight.
extern "C" void v8_goroutine_shape_seqlock_wait();
// Old-Space LAB sync: borrow LocalHeap LAB into per-M IsolateData before run,
// flush updated top back to LocalHeap after run.
extern "C" void v8_goroutine_lab_sync_before_run();
extern "C" void v8_goroutine_lab_sync_after_run();
// Returns per-M IsolateData* — used to set kRootRegister (r13) on goroutine entry.
extern "C" void* v8_goroutine_get_isolate_data();
// Set per-M StackGuard stack limit directly (does NOT touch shared stack_size_).
extern "C" void v8_goroutine_set_stack_limit(uintptr_t limit);

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

  // Set kRootRegister (r13) to per-M IsolateData so V8 fast paths work.
  // New fcontext stacks have r13=0; we must set it before any V8 generated code.
  void* iso_data = v8_goroutine_get_isolate_data();
  __asm__ volatile("movq %0, %%r13" : : "r"(iso_data) : "r13");
  v8::Isolate* iso = v8::Isolate::GetCurrent();
  g->Execute(iso);
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

  // Set stack limit for the goroutine's small mmap stack via per-M StackGuard.
  // Use v8_goroutine_set_stack_limit (not isolate->SetStackLimit) to avoid
  // corrupting the shared Isolate::stack_size_ field.
  uintptr_t g_stack_bottom = reinterpret_cast<uintptr_t>(g->stack()->top());
  v8_goroutine_set_stack_limit(g_stack_bottom + 8192);

  tls_current_g = g;
  // Register goroutine's GC state so local-heap.cc safepoint hooks can find
  // it when GC fires mid-goroutine (without explicit YieldG).
  v8_goroutine_set_current_gc_state(g->gc_state());

  v8_goroutine_lab_sync_before_run();
  fctx_transfer_t result = jump_fcontext(
      static_cast<fcontext_t>(g->stack_context()),
      static_cast<void*>(g));
  v8_goroutine_lab_sync_after_run();

  // Back on g0 stack — goroutine is no longer running on this M-thread.
  v8_goroutine_set_current_gc_state(nullptr);

  // Back on g0 stack.
  G* returned_g = static_cast<G*>(result.data);
  returned_g->SaveContext(result.fctx);


  // ---- Save G's HSD, restore g0's ----
  v8_goroutine_save_hsd(isolate, returned_g->saved_hsd_buf());
  returned_g->mark_saved_hsd();
  v8_goroutine_restore_hsd(isolate, g0_hsd);

  tls_current_g = nullptr;

  // Restore stack limit to M-thread's OS stack via per-M StackGuard.
  uintptr_t sp = reinterpret_cast<uintptr_t>(&result);
  v8_goroutine_set_stack_limit(sp - (900 * 1024));
}

void YieldG() {
  G* g = tls_current_g;
  if (!g) return;


  g->SetState(GState::Grunnable);
  Scheduler::GetInstance()->Schedule(g);
  Runtime::GetInstance()->NotifyGoroutineAvailable();

  // Park: snapshot goroutine's TLT onto gc_state so GC can scan its mmap
  // stack while it's yielded. Must happen BEFORE jump to scheduler.
  v8_goroutine_gc_park(v8::Isolate::GetCurrent(), g->gc_state());
  // Flush LAB back to LocalHeap before yielding (goroutine stops touching heap).
  v8_goroutine_lab_sync_after_run();
  // Jump back to scheduler (g0).
  fctx_transfer_t t = jump_fcontext(tls_sched_ctx, static_cast<void*>(g));
  // Goroutine resumes here (RunG called jump_fcontext back to us).
  // Restore kRootRegister (r13) — may have been clobbered by scheduler context.
  {
    void* iso_data = v8_goroutine_get_isolate_data();
    __asm__ volatile("movq %0, %%r13" : : "r"(iso_data) : "r13");
  }
  // Phase 1.3: SeqLock — before touching V8 heap, ensure no shape transition
  // is in progress on the main thread. Spins with cpu_relax (fast path ~1ns).
  v8_goroutine_shape_seqlock_wait();
  // Steal LocalHeap LAB again now that we're running.
  v8_goroutine_lab_sync_before_run();
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

