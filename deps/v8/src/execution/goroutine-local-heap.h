// Per-M LocalHeap C API for goroutine worker threads.
//
// Each M worker thread owns a LocalHeap which registers it with V8's GC
// safepoint mechanism, replacing GoroutineSafepointRegistry entirely.
//
// Lifecycle on M thread:
//   start  → v8_goroutine_local_heap_create()  [starts Parked]
//   loop   → unpark → run goroutines → safepoint between each → park → sleep
//   end    → v8_goroutine_local_heap_destroy()
#ifndef V8_EXECUTION_GOROUTINE_LOCAL_HEAP_H_
#define V8_EXECUTION_GOROUTINE_LOCAL_HEAP_H_
#include "v8.h"
extern "C" {
  // Create a kBackground LocalHeap for the current M thread.
  // Must be called ON the thread that will use it. Starts Parked.
  void* v8_goroutine_local_heap_create(v8::Isolate* isolate);
  // Destroy. Thread must be Parked. Restores saved Isolate::Current.
  void v8_goroutine_local_heap_destroy(void* lh);
  // Park: thread will not access heap (safe for GC stop-the-world).
  void v8_goroutine_local_heap_park(void* lh);
  // Unpark: thread will access heap. Blocks if GC in progress until done.
  void v8_goroutine_local_heap_unpark(void* lh);
  // Cooperative safepoint check between goroutines.
  // Blocks until GC completes if a safepoint was requested.
  void v8_goroutine_local_heap_safepoint(void* lh);
}
#endif  // V8_EXECUTION_GOROUTINE_LOCAL_HEAP_H_
