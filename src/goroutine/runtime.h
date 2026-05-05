#ifndef SRC_GOROUTINE_RUNTIME_H_
#define SRC_GOROUTINE_RUNTIME_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "uv.h"
#include "v8.h"
#include "g.h"

namespace node {
namespace goroutine {

class Scheduler;
class Runtime;

// M (Machine) — OS thread that executes goroutines.
//
// Main thread (M0) NEVER executes goroutines.
// GOMAXPROCS = number of worker M-threads spawned.
class M {
 public:
  explicit M(uint32_t id);
  ~M();

  uint32_t id() const { return id_; }

  G* current_g() const { return current_g_; }

  // Flush any accumulated dead goroutines to the global queue.
  // Called during shutdown to avoid leaking goroutines in local batch.
  void FlushDeadBatch();

  // V8 per-M state lifecycle (per-M IsolateData copy for handles/LABs).
  void InitV8State(v8::Isolate* isolate);
  void DestroyV8State();

  // Start / stop this M as a worker OS thread.
  void StartThread(Runtime* rt);
  void SignalStop();   // Phase 1: set running_=false, wake via shared sem
  void JoinThread();  // Phase 2: wait for thread exit

 private:
  uint32_t id_;
  G* current_g_ = nullptr;

  // Per-M V8 state (opaque GoroutinePState handle).
  void* v8_state_ = nullptr;

  // Per-M LocalHeap: registers this thread with V8 GC and sets
  // Isolate::Current() so v8::Isolate::GetCurrent() works on worker threads.
  void* local_heap_ = nullptr;

  // Worker thread state.
  Runtime* runtime_ = nullptr;
  uv_thread_t thread_;
  std::atomic<bool> running_{false};

  // Local dead goroutine batch: accumulate up to kDeadBatchSize dead G* here
  // before flushing to the global dead_queue_. Reduces mutex contention and
  // async overhead by batching cleanup operations.
  static constexpr uint32_t kDeadBatchSize = 16;
  std::vector<G*> dead_batch_;

  static void ThreadEntry(void* arg);
  void ThreadLoop();
};

// Runtime — global goroutine runtime.
class Runtime {
 public:
  static Runtime* GetInstance();

  void Init(uint32_t num_threads, uv_loop_t* loop, v8::Isolate* isolate);
  void Shutdown();
  bool IsInitialized() const { return initialized_.load(); }

  Scheduler* scheduler() const { return scheduler_; }
  v8::Isolate* isolate() const { return isolate_; }

  // Wake worker M-threads after a goroutine is scheduled.
  void NotifyGoroutineAvailable();


 private:
  Runtime() = default;
  ~Runtime() = default;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  static void OnCheck(uv_check_t* handle);
  static void OnAsync(uv_async_t* handle);

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  uint32_t num_threads_ = 0;
  v8::Isolate* isolate_ = nullptr;
  uv_loop_t* loop_ = nullptr;
  Scheduler* scheduler_ = nullptr;

  // Event-loop hooks.
  uv_check_t check_handle_;
  uv_async_t async_handle_;
  bool check_active_ = false;
  bool async_init_ = false;

  // Semaphore: posted when a goroutine becomes runnable.
  uv_sem_t goroutine_sem_;
  bool sem_init_ = false;

  // Number of worker M-threads currently blocked on uv_sem_wait.
  // Used by NotifyGoroutineAvailable to avoid spurious sem_post.
  std::atomic<int> sleeping_workers_{0};


  // Dead goroutine queue: worker threads enqueue finished G* here instead of
  // calling `delete g` directly. v8::Global::Reset() (in G::~G) is NOT
  // thread-safe — it must run on the main thread which holds the V8 context.
  std::mutex dead_mutex_;
  std::vector<G*> dead_queue_;

  // Worker M-threads (M1, M2, …).
  std::vector<M*> workers_;

  friend class M;
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_RUNTIME_H_
