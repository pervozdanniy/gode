// Goroutine stack scanning for GC root iteration (Phase 1.2).
//
// Problem: goroutine interpreter frames live on fcontext mmap stacks that GC
// doesn't know about. When a goroutine yields, its register file (containing
// raw tagged V8 pointers) is frozen on the mmap stack. If GC evacuates those
// objects, the pointers become stale → use-after-free on resume.
//
// Solution: at yield time, save the goroutine's ThreadLocalTop (which contains
// c_entry_fp_ — the start of the V8 frame chain). During GC root iteration,
// use StackFrameIterator(isolate, saved_tlt) to walk the goroutine's mmap stack
// frames and visit/update all live V8 object references in-place.
//
// Result: GC can safely evacuate objects referenced by yielded goroutines,
// and goroutines resume with correct (updated) pointers.

#ifndef V8_EXECUTION_GOROUTINE_GC_ROOTS_H_
#define V8_EXECUTION_GOROUTINE_GC_ROOTS_H_

#include <atomic>
#include <vector>

#include "src/handles/handles.h"  // HandleScopeData

namespace v8 {
class Isolate;
namespace internal {
class RootVisitor;
class Isolate;
class ThreadLocalTop;
class HandleScopeImplementer;
}  // namespace internal
}  // namespace v8

namespace v8 {
namespace internal {

// Maximum number of M-threads (goroutine workers). Indexed by M::id().
static constexpr int kMaxGoroutineM = 256;

// Per-goroutine GC scanning state.
// Allocated when goroutine is created, freed when it dies.
// Populated at yield (Park), cleared at resume (Unpark).
struct GoroutineGCState {
  // ...existing fields unchanged...
  ThreadLocalTop* saved_tlt = nullptr;
  ThreadLocalTop* live_tlt = nullptr;
  HandleScopeImplementer* hsi = nullptr;
  HandleScopeData saved_hsd = {};
  bool yielded = false;
};

// Global registry of yielded goroutines for GC root scanning.
// Lock-free: uses a fixed array of atomic pointers indexed by M-thread id.
class GoroutineGCRegistry {
 public:
  static GoroutineGCRegistry& Get();

  // Allocate GC state for a goroutine. Call once at goroutine creation.
  GoroutineGCState* Allocate();

  // Free GC state. Call at goroutine destruction.
  void Free(GoroutineGCState* state);

  // Park: snapshot current ThreadLocalTop and register goroutine for GC.
  // Called from YieldG() before jump_fcontext.
  // Uses tls_m_id to index into atomic array — no mutex.
  void Park(GoroutineGCState* state, Isolate* isolate);

  // Unpark: unregister goroutine from GC scanning.
  // Called when goroutine resumes (after jump_fcontext returns in YieldG).
  void Unpark(GoroutineGCState* state);

  // Called from Heap::IterateRoots during GC.
  void IterateRoots(Isolate* isolate, RootVisitor* visitor);

 private:
  GoroutineGCRegistry();

  // Fixed array indexed by M-thread id (0..kMaxGoroutineM-1).
  // Each slot holds a GoroutineGCState* when that M's goroutine is yielded,
  // nullptr otherwise. GC iterates all slots under safepoint.
  std::atomic<GoroutineGCState*> per_m_state_[kMaxGoroutineM];
};

}  // namespace internal
}  // namespace v8

// ---- C API for src/goroutine/ (crosses V8 internal boundary) ----
extern "C" {
  // Allocate/free per-goroutine GC state.
  void* v8_goroutine_gc_alloc();
  void  v8_goroutine_gc_free(void* state);

  // Park: save ThreadLocalTop and register goroutine for GC scanning.
  void v8_goroutine_gc_park(v8::Isolate* isolate, void* state);

  // Unpark: unregister goroutine from GC scanning.
  void v8_goroutine_gc_unpark(void* state);

  // Set/clear TLS pointer to current goroutine's GC state.
  void v8_goroutine_set_current_gc_state(void* state);

  // Set the M-thread id for this thread (called once from ThreadLoop).
  // Used by Park/Unpark to index into per_m_state_[].
  void v8_goroutine_gc_set_m_id(uint32_t id);
}

#endif  // V8_EXECUTION_GOROUTINE_GC_ROOTS_H_
