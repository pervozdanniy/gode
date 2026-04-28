// Per-M FeedbackVector state — Phase 2 groundwork.
//
// Each goroutine M-thread owns a GoroutineFeedbackState that maps
// (script_id, function_literal_id) → per-M FeedbackVector.
//
// Per-M FVs are isolated from the shared canonical FV so that IC slots
// from different goroutines don't pollute each other (megamorphic slowdown).
//
// FV pointers are stored as PersistentHandle locations
// (LocalHeap::NewPersistentHandle), so GC automatically updates them on
// object evacuation. No manual GC root registration is needed.

#ifndef V8_EXECUTION_GOROUTINE_FEEDBACK_H_
#define V8_EXECUTION_GOROUTINE_FEEDBACK_H_

#include "src/common/globals.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace v8 {
namespace internal {

class Isolate;

// Per-M FeedbackVector state.
// Owns per-M FeedbackVector handles for each JS function called on this M-thread.
class GoroutineFeedbackState {
 public:
  explicit GoroutineFeedbackState(Isolate* isolate);
  ~GoroutineFeedbackState();

  // Get or create a per-M FeedbackVector for the given JSFunction.
  //   closure_raw      — raw tagged pointer to JSFunction
  //   canonical_fv_raw — raw tagged pointer to the shared canonical FeedbackVector
  // Returns raw tagged pointer to per-M FV, or canonical_fv_raw on failure.
  // Called from InterpreterEntryTrampoline via C stub on goroutine M-threads.
  uintptr_t GetOrCreate(uintptr_t closure_raw, uintptr_t canonical_fv_raw);

 private:
  // O(1) lookup: unique_id → PersistentHandle location (GC auto-updates)
  std::unordered_map<int32_t, Address*> fv_cache_;
  Isolate* isolate_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_EXECUTION_GOROUTINE_FEEDBACK_H_

