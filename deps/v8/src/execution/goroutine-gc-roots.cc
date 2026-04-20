// Goroutine stack scanning for GC root iteration.
// See goroutine-gc-roots.h for design overview.

#include "src/execution/goroutine-gc-roots.h"

#include "src/execution/frames.h"
#include "src/execution/isolate.h"
#include "src/execution/thread-local-top.h"
#include "src/objects/slots.h"
#include "src/objects/visitors.h"
#include "src/roots/roots.h"
#include "src/api/api.h"  // HandleScopeImplementer::IterateThis

#include <algorithm>
#include <cstring>

// Forward declaration: returns current M-thread's HSI (from goroutine-thread-state.cc).
extern "C" void* v8_goroutine_get_current_hsi();

namespace v8 {
namespace internal {

// static
GoroutineGCRegistry& GoroutineGCRegistry::Get() {
  static GoroutineGCRegistry instance;
  return instance;
}

GoroutineGCState* GoroutineGCRegistry::Allocate() {
  auto* state = new GoroutineGCState();
  // Allocate and zero-initialize the TLT storage.
  state->saved_tlt = new ThreadLocalTop();
  std::memset(state->saved_tlt, 0, sizeof(ThreadLocalTop));
  state->yielded = false;
  return state;
}

void GoroutineGCRegistry::Free(GoroutineGCState* state) {
  if (!state) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    yielded_.erase(std::remove(yielded_.begin(), yielded_.end(), state),
                   yielded_.end());
  }
  delete state->saved_tlt;
  delete state;
}

void GoroutineGCRegistry::Park(GoroutineGCState* state, Isolate* isolate) {
  // Snapshot the current M-thread's HandleScopeImplementer.
  // This captures all Handle<T> objects created in V8 runtime functions
  // running on this goroutine M-thread (e.g. Handle<JSAny> receiver in
  // Runtime_StoreIC_Miss). These handles are stored in the per-M HSI which
  // is never visited by the main-thread GC via isolate->handle_scope_implementer()
  // (that path returns the MAIN HSI, not per-M). Without visiting the per-M HSI,
  // GC evacuation leaves handles stale → use-after-free → crash.
  state->hsi = static_cast<v8::internal::HandleScopeImplementer*>(
      v8_goroutine_get_current_hsi());

  // Snapshot the per-M HandleScopeData.
  // hsi->Iterate() calls isolate_->handle_scope_data() to get the active-block
  // limit (current->next). On the GC main thread, isolate_->handle_scope_data()
  // returns the MAIN thread's HSD — wrong block limit for the per-M HSI.
  // We capture the per-M HSD here (Park runs on the M-thread where
  // isolate->isolate_data() returns the per-M IsolateData) so IterateRoots()
  // can temporarily swap it in before calling hsi->Iterate().
  state->saved_hsd = *isolate->handle_scope_data();

  // Snapshot the current M-thread's ThreadLocalTop into goroutine's GC state.
  // c_entry_fp_ in the snapshot is the start of the goroutine's V8 frame chain
  // on its mmap stack — used by StackFrameIterator during GC.
  // isolate->thread_local_top() returns the per-M IsolateData TLT (due to
  // our patched isolate_data()) which holds the goroutine's c_entry_fp_.
  ThreadLocalTop* live = isolate->thread_local_top();
  state->live_tlt = live;
  std::memcpy(state->saved_tlt, live, sizeof(ThreadLocalTop));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state->yielded) return;  // Idempotent: already registered for GC.
    state->yielded = true;
    yielded_.push_back(state);
  }
}

void GoroutineGCRegistry::Unpark(GoroutineGCState* state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state->yielded) return;  // Idempotent: not registered.
  yielded_.erase(std::remove(yielded_.begin(), yielded_.end(), state),
                 yielded_.end());
  // GC visited saved_tlt and updated all tagged pointer fields in-place.
  // Copy those updated fields back to the live per-M TLT so the goroutine
  // resumes with correct (post-GC) pointers instead of stale pre-GC addresses.
  if (state->live_tlt && state->saved_tlt) {
    state->live_tlt->context_         = state->saved_tlt->context_;
    state->live_tlt->exception_       = state->saved_tlt->exception_;
    state->live_tlt->pending_message_ = state->saved_tlt->pending_message_;
  }
  // Zero out saved TLT so we don't hold stale pointers.
  std::memset(state->saved_tlt, 0, sizeof(ThreadLocalTop));
  state->yielded = false;
  state->live_tlt = nullptr;
  state->hsi = nullptr;
}

void GoroutineGCRegistry::IterateRoots(Isolate* isolate,
                                        RootVisitor* visitor) {
  // GC runs under safepoint (all M-threads stopped), so no concurrent
  // Park/Unpark calls. Mutex is still held for safety.
  std::lock_guard<std::mutex> lock(mutex_);

  for (GoroutineGCState* state : yielded_) {
    if (!state->yielded || !state->saved_tlt) continue;
    ThreadLocalTop* tlt = state->saved_tlt;

    // 1. Visit tagged V8 object references stored in the goroutine's TLT.
    //    These are live roots that must survive GC.
    //    Cast to Address* to use the defined FullObjectSlot(Address*) ctor
    //    rather than the declared-but-undefined FullObjectSlot(TaggedBase*).
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->exception_)));
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->pending_message_)));
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->context_)));

    // 2. Visit TryCatch exception/message chains (if any active handlers).
    //    TryCatch blocks are C++ stack-allocated on the goroutine's mmap stack,
    //    so they remain valid while goroutine is yielded.
    //    ForEachTryCatchField uses ThreadLocalTop's friend access to TryCatch
    //    private fields (next_, exception_, message_obj_).
    tlt->ForEachTryCatchField([&](Address* slot) {
      visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                                FullObjectSlot(slot));
    });

    // 3. Walk all V8 interpreter frames on the goroutine's mmap stack.
    if (tlt->c_entry_fp_ != kNullAddress) {
      StackFrameIterator it(isolate, tlt);
      for (; !it.done(); it.Advance()) {
        it.frame()->Iterate(visitor);
      }
    }

    // 4. Visit per-M HandleScopeImplementer handles.
    //    hsi->Iterate(visitor) internally reads isolate_->handle_scope_data()
    //    to know the limit of the current (last) handle block. On the GC main
    //    thread, isolate_->handle_scope_data() returns the MAIN thread's HSD
    //    with a completely different next/limit — wrong block boundary for the
    //    per-M HSI's blocks. Without the swap, the last per-M block is scanned
    //    with the wrong upper limit: handles allocated in it (e.g. the receiver
    //    Handle<JSAny> in Runtime_StoreIC_Miss) are missed → stale after GC
    //    evacuation → GetRootForNonJSReceiver crash.
    //
    //    Fix: temporarily replace the main isolate's handle_scope_data_ with
    //    the per-M HSD snapshot taken at Park() time, call Iterate, then
    //    restore. This is safe because GC runs under full safepoint (all
    //    M-threads are stopped; no concurrent handle allocations).
    if (state->hsi) {
      HandleScopeData* main_hsd = isolate->handle_scope_data();
      HandleScopeData saved_main = *main_hsd;
      *main_hsd = state->saved_hsd;
      state->hsi->Iterate(visitor);
      *main_hsd = saved_main;
    }
  }
}

}  // namespace internal
}  // namespace v8

// ---- C API ----

// TLS: current goroutine's GC state while it's running on this M-thread.
// Set by context.cc before jump_fcontext, cleared after goroutine returns to g0.
// Used by safepoint hooks (local-heap.cc) to register goroutine for GC scanning
// when GC fires mid-goroutine (without an explicit YieldG call).
static thread_local v8::internal::GoroutineGCState* tls_goroutine_gc_state =
    nullptr;

extern "C" {

void* v8_goroutine_gc_alloc() {
  return v8::internal::GoroutineGCRegistry::Get().Allocate();
}

void v8_goroutine_gc_free(void* state) {
  if (!state) return;
  v8::internal::GoroutineGCRegistry::Get().Free(
      static_cast<v8::internal::GoroutineGCState*>(state));
}

void v8_goroutine_gc_park(v8::Isolate* isolate, void* state) {
  if (!state || !isolate) return;
  v8::internal::GoroutineGCRegistry::Get().Park(
      static_cast<v8::internal::GoroutineGCState*>(state),
      reinterpret_cast<v8::internal::Isolate*>(isolate));
}

void v8_goroutine_gc_unpark(void* state) {
  if (!state) return;
  v8::internal::GoroutineGCRegistry::Get().Unpark(
      static_cast<v8::internal::GoroutineGCState*>(state));
}

// Called by context.cc to track which goroutine is running on this M-thread.
// Must be set BEFORE jump_fcontext to goroutine, cleared AFTER returning to g0.
void v8_goroutine_set_current_gc_state(void* state) {
  tls_goroutine_gc_state =
      static_cast<v8::internal::GoroutineGCState*>(state);
}

// Called from local-heap.cc when M-thread parks for GC safepoint while
// a goroutine is running (SleepInSafepoint / ParkSlowPath non-main path).
// Registers goroutine's mmap stack frames for GC root scanning.
void v8_goroutine_safepoint_park(v8::internal::Isolate* isolate) {
  if (!tls_goroutine_gc_state || !isolate) return;
  v8::internal::GoroutineGCRegistry::Get().Park(tls_goroutine_gc_state,
                                                  isolate);
}

// Called from local-heap.cc after safepoint ends (GC is done).
// Unregisters goroutine from GC scanning — it will resume execution.
void v8_goroutine_safepoint_unpark() {
  if (!tls_goroutine_gc_state) return;
  v8::internal::GoroutineGCRegistry::Get().Unpark(tls_goroutine_gc_state);
}

}  // extern "C"
