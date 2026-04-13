// Goroutine stack scanning for GC root iteration.
// See goroutine-gc-roots.h for design overview.

#include "src/execution/goroutine-gc-roots.h"

#include "src/execution/frames.h"
#include "src/execution/isolate.h"
#include "src/execution/thread-local-top.h"
#include "src/objects/slots.h"
#include "src/objects/visitors.h"
#include "src/roots/roots.h"

#include <algorithm>
#include <cstring>

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
  // Snapshot the current M-thread's ThreadLocalTop into goroutine's GC state.
  // c_entry_fp_ in the snapshot is the start of the goroutine's V8 frame chain
  // on its mmap stack — used by StackFrameIterator during GC.
  std::memcpy(state->saved_tlt, isolate->thread_local_top(),
              sizeof(ThreadLocalTop));
  state->yielded = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    yielded_.push_back(state);
  }
}

void GoroutineGCRegistry::Unpark(GoroutineGCState* state) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    yielded_.erase(std::remove(yielded_.begin(), yielded_.end(), state),
                   yielded_.end());
  }
  // Zero out saved TLT so we don't hold stale pointers.
  std::memset(state->saved_tlt, 0, sizeof(ThreadLocalTop));
  state->yielded = false;
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
    //    StackFrameIterator uses tlt->c_entry_fp_ to find the frame chain.
    //    frame->Iterate(visitor) visits AND updates (in-place) all tagged
    //    pointers in interpreter register files — this is the critical fix
    //    that prevents stale pointers after GC evacuation.
    if (tlt->c_entry_fp_ != kNullAddress) {
      StackFrameIterator it(isolate, tlt);
      for (; !it.done(); it.Advance()) {
        it.frame()->Iterate(visitor);
      }
    }
  }
}

}  // namespace internal
}  // namespace v8

// ---- C API ----
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

}  // extern "C"
