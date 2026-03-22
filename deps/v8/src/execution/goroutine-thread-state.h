// Goroutine per-P V8 state
//
// Following Go's GMP model: memory state (IsolateData, HandleScopeImplementer)
// belongs to P (Processor), not M (Machine thread).
//
// - P owns IsolateData (including LABs for allocation) + HandleScopeImplementer
// - When M acquires P, it activates P's state (sets thread_locals)
// - When M releases P, it deactivates (clears thread_locals)
// - This allows P (with its memory cache) to migrate between M's
//
// Lifecycle:
//   Runtime::Init()  → v8_goroutine_p_state_create()   for each P
//   M::Run()         → v8_goroutine_p_state_activate()  when M acquires P
//   M parks          → v8_goroutine_p_state_deactivate() when M releases P
//   Runtime::Shutdown → v8_goroutine_p_state_destroy()  for each P

#ifndef V8_EXECUTION_GOROUTINE_THREAD_STATE_H_
#define V8_EXECUTION_GOROUTINE_THREAD_STATE_H_

#include "src/execution/goroutine-thread.h"

namespace v8 {
namespace internal {

class Isolate;
class IsolateData;
class HandleScopeImplementer;

// Per-P V8 state. Owned by P, activated on M-thread.
struct GoroutinePState {
  IsolateData* isolate_data;
  HandleScopeImplementer* handle_scope_impl;
};

// Static accessors for the currently-active P state on this thread.
// Called from patched V8 code (isolate.h, etc.)
class GoroutineThreadState {
 public:
  // Get active P's IsolateData for current thread. nullptr if not active.
  static IsolateData* GetIsolateData();

  // Get active P's HandleScopeImplementer for current thread.
  static HandleScopeImplementer* GetHandleScopeImplementer();

  // Check if current thread has an active P state.
  static bool IsActive();

  // P-state lifecycle (called from C-linkage wrappers)
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
