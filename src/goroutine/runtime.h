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

class P;
class Scheduler;
class Runtime;

// M (Machine) — OS thread that executes goroutines.
//
// Go model: an M must acquire a P before it can run goroutines.
// M0 is the main thread.  Worker Ms are OS threads that compete
// for the V8 execution token (v8_mutex_) to run goroutines.
class M {
 public:
  explicit M(uint32_t id);
  ~M();

  uint32_t id() const { return id_; }

  P* p() const { return p_; }
  void set_p(P* p) { p_ = p; }

  G* current_g() const { return current_g_; }

  // Pick one runnable G from P's queue and execute it on the calling thread.
  bool ExecuteOne(v8::Isolate* isolate);

  // Start / stop this M as a worker OS thread.
  void StartThread(Runtime* rt);
  void StopThread();

 private:
  uint32_t id_;
  P* p_ = nullptr;
  G* current_g_ = nullptr;

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

  void Init(uint32_t gomaxprocs, uv_loop_t* loop, v8::Isolate* isolate);
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
  void DrainRunQueue();

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  uint32_t gomaxprocs_ = 0;
  v8::Isolate* isolate_ = nullptr;
  uv_loop_t* loop_ = nullptr;
  Scheduler* scheduler_ = nullptr;

  M* m0_ = nullptr;

  // Event-loop hooks.
  uv_prepare_t prepare_handle_;
  uv_check_t check_handle_;
  uv_idle_t idle_handle_;
  bool prepare_active_ = false;
  bool check_active_ = false;
  bool idle_active_ = false;

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

