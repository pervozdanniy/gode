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
#include "src/heap/local-heap-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/feedback-vector-inl.h"
#include "src/objects/feedback-cell-inl.h"
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
  // Fast lookup-only path for builtin call. This function is called directly
  // from InterpreterEntryTrampoline asm, so it MUST be minimal — no heavy
  // allocations, no pulling in factory.cc or heap-allocator.cc.
  //
  // Extract unique_id from closure for cache key.
  Tagged<JSFunction> function = Cast<JSFunction>(Tagged<Object>(closure_raw));
  Tagged<SharedFunctionInfo> sfi = function->shared();
  int32_t unique_id = sfi->unique_id();

  // Check cache: if we've already cloned this FV for this M-thread, use it.
  auto it = fv_cache_.find(unique_id);
  if (it != fv_cache_.end()) {
    // Cache hit: return the per-M FV (dereference PersistentHandle).
    return *it->second;
  }

  // Cache miss: return canonical FV (shared between threads).
  // The per-M FV will be created lazily later in a safe context (not from
  // builtin asm call path) via CreatePerMFeedbackVector().
  return canonical_fv_raw;
}

void GoroutineFeedbackState::CreatePerMFeedbackVector(
    uintptr_t closure_raw, uintptr_t canonical_fv_raw) {
  // Heavy allocation path: create a new FeedbackVector for this M-thread.
  // Called from C++ runtime code (BytecodeBudgetInterrupt or G::Execute),
  // NOT from builtin asm → safe to call FeedbackVector::New.

  Tagged<JSFunction> function = Cast<JSFunction>(Tagged<Object>(closure_raw));
  Tagged<SharedFunctionInfo> sfi = function->shared();
  int32_t unique_id = sfi->unique_id();

  // Already created? Skip.
  if (fv_cache_.count(unique_id)) return;

  Tagged<FeedbackVector> canonical_fv =
      Cast<FeedbackVector>(Tagged<Object>(canonical_fv_raw));

  LocalHeap* lh = LocalHeap::Current();
  if (!lh || lh->is_main_thread()) return;

  // Extract parameters needed for FeedbackVector::New.
  DirectHandle<SharedFunctionInfo> sfi_handle(sfi, isolate_);
  DirectHandle<ClosureFeedbackCellArray> cell_array(
      canonical_fv->closure_feedback_cell_array(), isolate_);
  DirectHandle<FeedbackCell> parent_cell(
      canonical_fv->parent_feedback_cell(), isolate_);

  IsCompiledScope is_compiled_scope(sfi, isolate_);
  if (!is_compiled_scope.is_compiled()) return;

  Handle<FeedbackVector> per_m_fv = FeedbackVector::New(
      isolate_, sfi_handle, cell_array, parent_cell, &is_compiled_scope);

  // Store as PersistentHandle so GC auto-updates on evacuation.
  IndirectHandle<FeedbackVector> persistent_fv =
      lh->NewPersistentHandle(per_m_fv);
  fv_cache_[unique_id] = persistent_fv.location();
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
// Returns tagged ptr to per-M FeedbackVector (or fv_raw if no cache hit).
uintptr_t v8_goroutine_resolve_feedback(uintptr_t closure_raw,
                                         uintptr_t fv_raw) {
  if (!tls_feedback_state) {
    // Fallback: no feedback state initialized (shouldn't happen on M-threads,
    // but safe fallback is to return canonical FV).
    return fv_raw;
  }
  // Fast lookup path: check if we have a per-M FV cached for this function.
  // If yes, return it; if no, return canonical FV (shared between threads).
  return tls_feedback_state->GetOrCreate(closure_raw, fv_raw);
}


// Called from BytecodeBudgetInterrupt on M-threads to create per-M FV.
// This runs in C++ runtime context — safe for FeedbackVector::New.
void v8_goroutine_create_per_m_feedback(uintptr_t closure_raw,
                                         uintptr_t fv_raw) {
  if (!tls_feedback_state) return;
  tls_feedback_state->CreatePerMFeedbackVector(closure_raw, fv_raw);
}


}  // extern "C"

