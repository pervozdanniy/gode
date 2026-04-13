#ifndef SRC_GOROUTINE_RUNTIME_H_
#define SRC_GOROUTINE_RUNTIME_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
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
// GM model (no P): M has a thread id used to index into the scheduler's
// per-thread local queues. V8 state is per-M (thread_local).
// M0 is the main thread. Worker Ms are OS threads.
class M {
 public:
  explicit M(uint32_t id);
  ~M();

  uint32_t id() const { return id_; }

  G* current_g() const { return current_g_; }

  // V8 per-M state lifecycle
  void InitV8State(v8::Isolate* isolate);
  void DestroyV8State();
  void ActivateV8State();
  void DeactivateV8State();

  // Pick one runnable G and execute it on the calling thread.
  bool ExecuteOne(v8::Isolate* isolate);

  // Check GC safepoint between goroutines (blocks if GC is in progress).
  void CheckSafepoint();

  // Start / stop this M as a worker OS thread.
  void StartThread(Runtime* rt);
  void StopThread();

 private:
  uint32_t id_;
  G* current_g_ = nullptr;

  // Per-M V8 state (opaque handle)
  void* v8_state_ = nullptr;

  // GC safepoint entry for this M thread (registered with V8 GC).
  void* safepoint_entry_ = nullptr;

  // Worker thread state.
  Runtime* runtime_ = nullptr;
  uv_thread_t thread_;
  std::atomic<bool> running_{false};

  static void ThreadEntry(void* arg);
  void ThreadLoop();
};

// Runtime — global goroutine runtime.
//
// V8 execution is protected by v8_mutex_:
//   • M0 holds the mutex while the event loop processes JS callbacks.
//   • uv_prepare releases it  → worker Ms can run goroutines during epoll.
//   • uv_check   reacquires it → M0 also drains the run queue.
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

  static void OnPrepare(uv_prepare_t* handle);
  static void OnCheck(uv_check_t* handle);
  static void OnIdle(uv_idle_t* handle);
  static void OnAsync(uv_async_t* handle);
  void DrainRunQueue();

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  uint32_t num_threads_ = 0;
  v8::Isolate* isolate_ = nullptr;
  uv_loop_t* loop_ = nullptr;
  Scheduler* scheduler_ = nullptr;

  M* m0_ = nullptr;

  // Event-loop hooks.
  uv_prepare_t prepare_handle_;
  uv_check_t check_handle_;
  uv_idle_t idle_handle_;
  uv_async_t async_handle_;
  bool prepare_active_ = false;
  bool check_active_ = false;
  bool idle_active_ = false;
  bool async_init_ = false;

  // V8 execution token — only one M in V8 at a time.
  uv_mutex_t v8_mutex_;
  bool v8_mutex_init_ = false;

  // Semaphore: posted when a goroutine becomes runnable.
  uv_sem_t goroutine_sem_;
  bool sem_init_ = false;

  // Worker M-threads (M1, M2, …).
  std::vector<M*> workers_;

  friend class M;
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_RUNTIME_H_

