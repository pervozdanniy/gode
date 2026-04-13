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
//   M::ThreadLoop()  → v8_goroutine_p_state_activate()  when M starts running
//   M stops          → v8_goroutine_p_state_deactivate() when M finishes
//   Runtime::Shutdown → v8_goroutine_p_state_destroy()  for each M

#ifndef V8_EXECUTION_GOROUTINE_THREAD_STATE_H_
#define V8_EXECUTION_GOROUTINE_THREAD_STATE_H_

#include "src/execution/goroutine-thread.h"

namespace v8 {
namespace internal {

class Isolate;
class IsolateData;
class HandleScopeImplementer;

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
};

}  // namespace internal
}  // namespace v8

// No C-linkage declarations here — V8 internal headers don't see v8::Isolate.
// C-linkage wrappers are declared via `extern "C"` where used (scheduler.cc, etc.)

#endif  // V8_EXECUTION_GOROUTINE_THREAD_STATE_H_
