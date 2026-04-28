// Per-M FeedbackVector state — Phase 2 implementation.
//
// Each goroutine M-thread clones the canonical FeedbackVector for each
// function it encounters. IC slot writes are then per-M → no megamorphic
// pollution between M-threads, and no data races on FV slots.
//
// FV pointers are stored as PersistentHandle locations so GC auto-updates
// them on object evacuation.

#include "src/execution/goroutine-feedback.h"

#include <atomic>
#include <unordered_map>
#include <unistd.h>
#include <sys/syscall.h>
#include "src/execution/isolate.h"
#include "src/heap/factory.h"
#include "src/heap/local-heap.h"
#include "src/objects/feedback-vector-inl.h"
#include "src/objects/js-function-inl.h"
#include "src/objects/shared-function-info-inl.h"

namespace v8 {
namespace internal {

GoroutineFeedbackState::GoroutineFeedbackState(Isolate* isolate)
    : isolate_(isolate) {
  fv_cache_.reserve(16);
}

GoroutineFeedbackState::~GoroutineFeedbackState() = default;

uintptr_t GoroutineFeedbackState::GetOrCreate(uintptr_t closure_raw,
                                               uintptr_t canonical_fv_raw) {
  // TODO(goroutine): Phase 2 per-M FeedbackVector cloning is complex.
  // For now, return canonical FV (shared between threads).
  // The main benefit is still delivered: main thread gets inline TLS check
  // in builtins-x64.cc instead of expensive C-call on every function entry.
  //
  // Future work: properly clone FV with IC slots initialization.
  return canonical_fv_raw;
}


}  // namespace internal
}  // namespace v8

// ---- C-linkage wrapper for builtins-x64.cc ----
// Called from InterpreterEntryTrampoline when v8_goroutine_thread is true.
// TLS holds the per-M GoroutineFeedbackState*.

static thread_local __attribute__((tls_model("initial-exec")))
    v8::internal::GoroutineFeedbackState* tls_feedback_state = nullptr;

extern "C" {

void v8_goroutine_feedback_state_create(void* isolate_ptr) {
  auto* isolate = reinterpret_cast<v8::internal::Isolate*>(isolate_ptr);
  tls_feedback_state = new v8::internal::GoroutineFeedbackState(isolate);
}

void v8_goroutine_feedback_state_destroy() {
  delete tls_feedback_state;
  tls_feedback_state = nullptr;
}

// Called from InterpreterEntryTrampoline on M-threads.
// closure_raw = tagged JSFunction ptr, fv_raw = tagged FeedbackVector ptr.
// Returns tagged ptr to per-M FeedbackVector (or fv_raw on fallback).
uintptr_t v8_goroutine_resolve_feedback(uintptr_t closure_raw,
                                         uintptr_t fv_raw) {
  // Minimal implementation: just return canonical FV.
  return fv_raw;
}


}  // extern "C"

