// Goroutine per-M V8 state
//
// GM model (no P abstraction): V8 state (IsolateData, HandleScopeImplementer)
// belongs directly to M (Machine thread), not to a separate P entity.
//
// - Each worker M owns its own IsolateData (including LABs) + HandleScopeImplementer
// - When M starts, it creates and activates its V8 state (sets thread_locals)
// - When M stops, it deactivates and destroys V8 state
//
// Lifecycle:
//   Runtime::Init()  → v8_goroutine_p_state_create()   for each M
//   M::ThreadLoop()  → v8_goroutine_p_state_activate_with_isolate()  when M starts running
//   M stops          → v8_goroutine_p_state_deactivate() when M finishes
//   Runtime::Shutdown → v8_goroutine_p_state_destroy()  for each M

#ifndef V8_EXECUTION_GOROUTINE_THREAD_STATE_H_
#define V8_EXECUTION_GOROUTINE_THREAD_STATE_H_

#include "src/execution/goroutine-flag.h"

namespace v8 {
namespace internal {

class Isolate;
class IsolateData;
class HandleScopeImplementer;
class LinearAllocationArea;
class StackGuard;

// Per-M IsolateData pointer — fast TLS for hot-path dispatch in isolate.h.
// Set by GoroutineThreadState::ActivatePState(), cleared by DeactivatePState().
// nullptr on all non-goroutine threads (main thread, worker threads, etc.).
// Using initial-exec TLS model for single-instruction access on x86_64.
extern thread_local __attribute__((tls_model("initial-exec")))
    IsolateData* tls_per_m_isolate_data;

// Per-M V8 state. Owned by M thread.
struct GoroutinePState {
  IsolateData* isolate_data;
  HandleScopeImplementer* handle_scope_impl;
};

// Static accessors for the currently-active M state on this thread.
// Called from patched V8 code (isolate.h, etc.)
class GoroutineThreadState {
 public:
  // Get active M's IsolateData for current thread. nullptr if not active.
  static IsolateData* GetIsolateData();

  // Get active M's HandleScopeImplementer for current thread.
  static HandleScopeImplementer* GetHandleScopeImplementer();

  // Check if current thread has an active M state.
  static bool IsActive();

  // Per-M state lifecycle (called from C-linkage wrappers)
  static GoroutinePState* CreatePState(Isolate* isolate);
  static void DestroyPState(GoroutinePState* state);
  static void ActivatePState(GoroutinePState* state);
  static void DeactivatePState();

  // Per-G HandleScopeData save/restore for goroutine context switching.
  // buf must be at least 64 bytes.
  static void SaveHSD(Isolate* isolate, void* buf);
  static void RestoreHSD(Isolate* isolate, const void* buf);
  // Force next handle allocation into a fresh block (prevents overlap).
  static void ForceNewHandleBlock(Isolate* isolate);

  // Old-Space LAB synchronisation around goroutine execution.
  // LabSyncBeforeRun: steal LocalHeap LAB into per-M IsolateData so JIT fast path works.
  // LabSyncAfterRun:  flush updated top back to LocalHeap, reset IsolateData LAB.
  static void LabSyncBeforeRun();
  static void LabSyncAfterRun();

  // Get pointer to IsolateData::old_allocation_info_ (private field).
  // Used by goroutine-local-heap.cc to point LocalHeap's allocator
  // at per-M IsolateData's LAB for zero-sync r13 fast path.
  static LinearAllocationArea* GetOldAllocationInfo(IsolateData* data);

  // Poison a per-M StackGuard's jslimit so the next backward branch triggers
  // HandleInterrupts → Safepoint(). Uses friend access to StackGuard.
  static void PoisonStackLimit(StackGuard* sg);
};

}  // namespace internal
}  // namespace v8

// No C-linkage declarations here — V8 internal headers don't see v8::Isolate.
// C-linkage wrappers are declared via `extern "C"` where used (scheduler.cc, etc.)

#endif  // V8_EXECUTION_GOROUTINE_THREAD_STATE_H_
