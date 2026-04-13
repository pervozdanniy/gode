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

#include <mutex>
#include <vector>

namespace v8 {
class Isolate;
namespace internal {
class RootVisitor;
class Isolate;
class ThreadLocalTop;
}  // namespace internal
}  // namespace v8

namespace v8 {
namespace internal {

// Per-goroutine GC scanning state.
// Allocated when goroutine is created, freed when it dies.
// Populated at yield (Park), cleared at resume (Unpark).
struct GoroutineGCState {
  // Full ThreadLocalTop snapshot taken at yield time.
  // Contains c_entry_fp_, context_, exception_, try_catch_handler_, etc.
  // nullptr-equivalent (zeroed) when goroutine is running.
  ThreadLocalTop* saved_tlt = nullptr;

  // True when this goroutine is yielded and registered for GC scanning.
  bool yielded = false;
};

// Global registry of yielded goroutines for GC root scanning.
// Thread-safe: protected by mutex_.
class GoroutineGCRegistry {
 public:
  static GoroutineGCRegistry& Get();

  // Allocate GC state for a goroutine. Call once at goroutine creation.
  GoroutineGCState* Allocate();

  // Free GC state. Call at goroutine destruction.
  void Free(GoroutineGCState* state);

  // Park: snapshot current ThreadLocalTop and register goroutine for GC.
  // Called from YieldG() before jump_fcontext.
  void Park(GoroutineGCState* state, Isolate* isolate);

  // Unpark: unregister goroutine from GC scanning.
  // Called when goroutine resumes (after jump_fcontext returns in YieldG).
  void Unpark(GoroutineGCState* state);

  // Called from Heap::IterateRoots during GC.
  // For each yielded goroutine:
  //   1. Visits tagged roots in saved TLT (exception, context, etc.)
  //   2. Uses StackFrameIterator to walk mmap stack frames and visit/update
  //      all V8 object references in the interpreter register files.
  void IterateRoots(Isolate* isolate, RootVisitor* visitor);

 private:
  GoroutineGCRegistry() = default;

  std::mutex mutex_;
  std::vector<GoroutineGCState*> yielded_;
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
}

#endif  // V8_EXECUTION_GOROUTINE_GC_ROOTS_H_
