// Per-M LocalHeap implementation.
// See goroutine-local-heap.h for design overview.
#include "src/execution/goroutine-local-heap.h"
#include "src/execution/isolate.h"
#include "src/execution/isolate-data.h"
#include "src/execution/goroutine-thread-state.h"
#include "src/heap/local-heap.h"
#include "src/heap/heap-allocator.h"
#include "src/heap/linear-allocation-area.h"
namespace v8 {
namespace internal {
// Helper class that has friend access to LocalHeap::Park()/Unpark().
// Declared as friend in local-heap.h.
struct GoroutineLocalHeapHelper {
  static void Park(LocalHeap* lh) { lh->Park(); }
  static void Unpark(LocalHeap* lh) { lh->Unpark(); }
};
}  // namespace internal
}  // namespace v8
extern "C" {
void* v8_goroutine_local_heap_create(v8::Isolate* isolate) {
  auto* i_isolate = reinterpret_cast<v8::internal::Isolate*>(isolate);
  // LocalHeap constructor:
  //   - registers with heap->safepoint() (participates in StopTheWorld)
  //   - calls Isolate::SetCurrent(isolate) so GetCurrent() works
  //   - starts in Parked state
  auto* lh = new v8::internal::LocalHeap(
      i_isolate->heap(), v8::internal::ThreadKind::kBackground);
  return static_cast<void*>(lh);
}
void v8_goroutine_local_heap_destroy(void* lh) {
  if (!lh) return;
  // Destructor calls EnsureParkedBeforeDestruction() and unregisters from GC.
  delete static_cast<v8::internal::LocalHeap*>(lh);
}
void v8_goroutine_local_heap_park(void* lh) {
  if (!lh) return;
  v8::internal::GoroutineLocalHeapHelper::Park(
      static_cast<v8::internal::LocalHeap*>(lh));
}
void v8_goroutine_local_heap_unpark(void* lh) {
  if (!lh) return;
  // Blocks until any ongoing GC safepoint completes.
  v8::internal::GoroutineLocalHeapHelper::Unpark(
      static_cast<v8::internal::LocalHeap*>(lh));
}
void v8_goroutine_local_heap_safepoint(void* lh) {
  if (!lh) return;
  // Fast path (no GC): single atomic load. No cost when GC not running.
  static_cast<v8::internal::LocalHeap*>(lh)->Safepoint();
}

void v8_goroutine_local_heap_replace_old_lab(void* lh, void* isolate_data_ptr) {
  if (!lh || !isolate_data_ptr) return;
  auto* local_heap = static_cast<v8::internal::LocalHeap*>(lh);
  auto* iso_data = static_cast<v8::internal::IsolateData*>(isolate_data_ptr);
  auto* lab = v8::internal::GoroutineThreadState::GetOldAllocationInfo(iso_data);
  local_heap->allocator()->ReplaceOldSpaceLAB(lab);
}

}  // extern "C"
