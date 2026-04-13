// Goroutine M-thread GC safepoint registry.
//
// When GC needs to stop the world (IsolateSafepoint::EnterLocalSafepointScope),
// it must also pause goroutine worker M-threads that are currently executing
// JS code. This registry tracks those threads and coordinates with GC.
//
// Lifecycle:
//   M thread start → v8_goroutine_safepoint_register() → entry (kParked)
//   Before goroutine loop → v8_goroutine_safepoint_set_running(entry)
//   Between goroutines → v8_goroutine_safepoint_check(entry)
//   Back to sem_wait → v8_goroutine_safepoint_set_parked(entry)
//   M thread stop → v8_goroutine_safepoint_unregister(entry)
//
// GC integration (called from safepoint.cc):
//   GoroutineSafepointRegistry::Get().RequestAndWait()
//   ... GC runs ...
//   GoroutineSafepointRegistry::Get().Resume()

#ifndef V8_EXECUTION_GOROUTINE_SAFEPOINT_H_
#define V8_EXECUTION_GOROUTINE_SAFEPOINT_H_

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace v8 {
namespace internal {

// State of a registered M thread from GC's perspective.
enum class ExternalThreadState : int {
  kParked = 0,       // In sem_wait, not accessing heap. GC-safe.
  kRunning = 1,      // Executing goroutines. Must stop for GC.
  kInSafepoint = 2,  // Acknowledged GC request. Waiting for resume.
};

// Per-M-thread entry in the registry.
struct ExternalSafepointEntry {
  std::atomic<int> state{static_cast<int>(ExternalThreadState::kParked)};

  void SetParked() {
    state.store(static_cast<int>(ExternalThreadState::kParked),
                std::memory_order_release);
  }
  void SetRunning() {
    state.store(static_cast<int>(ExternalThreadState::kRunning),
                std::memory_order_release);
  }
  ExternalThreadState Load() const {
    return static_cast<ExternalThreadState>(
        state.load(std::memory_order_acquire));
  }
};

// Global registry of goroutine M-threads for GC safepoint coordination.
// Singleton — one per process (we have one V8 isolate).
class GoroutineSafepointRegistry {
 public:
  static GoroutineSafepointRegistry& Get();

  // Called from M thread on start. Returns an owned entry pointer.
  ExternalSafepointEntry* Register();

  // Called from M thread on stop.
  void Unregister(ExternalSafepointEntry* entry);

  // Called from M thread: mark as running (before goroutine loop).
  void SetRunning(ExternalSafepointEntry* entry);

  // Called from M thread: mark as parked (before sem_wait / idle).
  void SetParked(ExternalSafepointEntry* entry);

  // Called from M thread between goroutines.
  // If a GC safepoint is requested, blocks until GC is done.
  void CheckSafepoint(ExternalSafepointEntry* entry);

  // === Called from GC (safepoint.cc) under local_heaps_mutex_ ===

  // Signal all Running M threads to stop.
  // Returns count of threads in kRunning state that must acknowledge.
  int RequestSafepoint();

  // Wait until all RequestSafepoint()-counted threads are in kInSafepoint.
  void WaitForAll(int running_count);

  // Resume all M threads after GC is done.
  void Resume();

 private:
  GoroutineSafepointRegistry() = default;

  std::mutex mutex_;
  std::condition_variable cv_stopped_;   // GC waits here for M threads
  std::condition_variable cv_resumed_;   // M threads wait here for GC done
  std::vector<ExternalSafepointEntry*> entries_;

  bool safepoint_requested_ = false;
  int stopped_count_ = 0;
};

}  // namespace internal
}  // namespace v8

// C API called from src/goroutine/runtime.cc
extern "C" {
  // Register current M thread. Returns opaque handle (ExternalSafepointEntry*).
  void* v8_goroutine_safepoint_register();

  // Unregister when M thread exits.
  void v8_goroutine_safepoint_unregister(void* handle);

  // Mark M thread as running (before executing goroutines).
  void v8_goroutine_safepoint_set_running(void* handle);

  // Mark M thread as parked (before sem_wait).
  void v8_goroutine_safepoint_set_parked(void* handle);

  // Check safepoint between goroutines. Blocks if GC is requested.
  void v8_goroutine_safepoint_check(void* handle);
}

#endif  // V8_EXECUTION_GOROUTINE_SAFEPOINT_H_
