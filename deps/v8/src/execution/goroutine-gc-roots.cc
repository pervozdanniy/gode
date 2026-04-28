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

GoroutineGCRegistry::GoroutineGCRegistry() {
  for (int i = 0; i < kMaxGoroutineM; i++) {
    per_m_state_[i].store(nullptr, std::memory_order_relaxed);
  }
}

GoroutineGCState* GoroutineGCRegistry::Allocate() {
  auto* state = new GoroutineGCState();
  state->saved_tlt = new ThreadLocalTop();
  std::memset(state->saved_tlt, 0, sizeof(ThreadLocalTop));
  state->yielded = false;
  return state;
}

void GoroutineGCRegistry::Free(GoroutineGCState* state) {
  if (!state) return;
  // No need to search/remove from per_m_state_ — if the goroutine is dead,
  // its M-thread has already Unparked it (set slot to nullptr).
  delete state->saved_tlt;
  delete state;
}

void GoroutineGCRegistry::Park(GoroutineGCState* state, Isolate* isolate) {
  // Snapshot the current M-thread's HandleScopeImplementer.
  state->hsi = static_cast<v8::internal::HandleScopeImplementer*>(
      v8_goroutine_get_current_hsi());

  // Snapshot the per-M HandleScopeData.
  state->saved_hsd = *isolate->handle_scope_data();

  // Snapshot the current M-thread's ThreadLocalTop into goroutine's GC state.
  ThreadLocalTop* live = isolate->thread_local_top();
  state->live_tlt = live;
  std::memcpy(state->saved_tlt, live, sizeof(ThreadLocalTop));

  if (state->yielded) return;  // Idempotent: already registered for GC.
  state->yielded = true;

  // Atomic write to per-M slot — no mutex needed.
  // tls_m_id is set once per M-thread at startup via v8_goroutine_gc_set_m_id().
  extern thread_local uint32_t tls_goroutine_m_id;
  per_m_state_[tls_goroutine_m_id].store(state, std::memory_order_release);
}

void GoroutineGCRegistry::Unpark(GoroutineGCState* state) {
  if (!state->yielded) return;  // Idempotent: not registered.

  // Clear per-M slot atomically.
  extern thread_local uint32_t tls_goroutine_m_id;
  per_m_state_[tls_goroutine_m_id].store(nullptr, std::memory_order_release);

  // GC visited saved_tlt and updated all tagged pointer fields in-place.
  if (state->live_tlt && state->saved_tlt) {
    state->live_tlt->context_         = state->saved_tlt->context_;
    state->live_tlt->exception_       = state->saved_tlt->exception_;
    state->live_tlt->pending_message_ = state->saved_tlt->pending_message_;
  }
  std::memset(state->saved_tlt, 0, sizeof(ThreadLocalTop));
  state->yielded = false;
  state->live_tlt = nullptr;
  state->hsi = nullptr;
}

void GoroutineGCRegistry::IterateRoots(Isolate* isolate,
                                        RootVisitor* visitor) {
  // GC runs under safepoint (all M-threads stopped), so no concurrent
  // Park/Unpark calls. Iterate the fixed array — no mutex needed.
  for (int i = 0; i < kMaxGoroutineM; i++) {
    GoroutineGCState* state =
        per_m_state_[i].load(std::memory_order_acquire);
    if (!state || !state->yielded || !state->saved_tlt) continue;
    ThreadLocalTop* tlt = state->saved_tlt;

    // 1. Visit tagged V8 object references stored in the goroutine's TLT.
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->exception_)));
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->pending_message_)));
    visitor->VisitRootPointer(Root::kStackRoots, nullptr,
                              FullObjectSlot(reinterpret_cast<Address*>(&tlt->context_)));

    // 2. Visit TryCatch exception/message chains.
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
    if (state->hsi) {
      HandleScopeData* main_hsd = isolate->handle_scope_data();
      HandleScopeData saved_main = *main_hsd;
      *main_hsd = state->saved_hsd;
      state->hsi->Iterate(visitor);
      *main_hsd = saved_main;
    }
  }
}

// TLS: M-thread id for indexing per_m_state_[]. Set once at M-thread start.
thread_local uint32_t tls_goroutine_m_id = 0;

}  // namespace internal
}  // namespace v8

// ---- C API ----

// TLS: current goroutine's GC state while it's running on this M-thread.
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

void v8_goroutine_set_current_gc_state(void* state) {
  tls_goroutine_gc_state =
      static_cast<v8::internal::GoroutineGCState*>(state);
}

void v8_goroutine_gc_set_m_id(uint32_t id) {
  v8::internal::tls_goroutine_m_id = id;
}

void v8_goroutine_safepoint_park(v8::internal::Isolate* isolate) {
  if (!tls_goroutine_gc_state || !isolate) return;
  v8::internal::GoroutineGCRegistry::Get().Park(tls_goroutine_gc_state,
                                                  isolate);
}

void v8_goroutine_safepoint_unpark() {
  if (!tls_goroutine_gc_state) return;
  v8::internal::GoroutineGCRegistry::Get().Unpark(tls_goroutine_gc_state);
}

}  // extern "C"
