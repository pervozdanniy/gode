#ifndef SRC_GOROUTINE_SCHEDULER_H_
#define SRC_GOROUTINE_SCHEDULER_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
#include <vector>
#include "g.h"
#include "node_mutex.h"
#include "v8.h"

namespace node {
namespace goroutine {

// Forward declarations
class P;
class M;
class Runtime;

// Runnable queue size (matching Go)
constexpr uint32_t kRunqSize = 256;

// Processor (P) - execution context with local runnable queue + V8 state.
// Following Go: P owns mcache (≈ IsolateData with LABs) and local runqueue.
class P {
 public:
  explicit P(uint32_t id);
  ~P();

  uint32_t id() const { return id_; }

  // V8 state lifecycle (called from main thread)
  void InitV8State(v8::Isolate* isolate);
  void DestroyV8State();

  // V8 state activation (called from M-thread)
  void Activate();    // M acquires this P
  void Deactivate();  // M releases this P

  void* v8_state() const { return v8_state_; }

  // Local runnable queue operations (lock-free)
  bool PushLocal(G* g);
  G* PopLocal();
  G* StealHalf(std::vector<G*>& stolen);

  // Stats
  uint32_t runq_size() const;

 private:
  uint32_t id_;

  // Per-P V8 state (opaque handle to GoroutinePState)
  void* v8_state_ = nullptr;

  // Lock-free circular buffer for runnable goroutines
  std::atomic<uint32_t> runq_head_{0};
  std::atomic<uint32_t> runq_tail_{0};
  G* runq_[kRunqSize];

  // TODO: Per-P memory cache (mcache)
};

// Global scheduler
class Scheduler {
 public:
  static Scheduler* GetInstance();

  // Lifecycle
  void Init(uint32_t gomaxprocs);
  void Shutdown();
  bool IsInitialized() const { return initialized_.load(); }

  // Schedule goroutine
  void Schedule(G* g);

  // Find runnable goroutine (called by M)
  G* FindRunnable(P* p);

  // Park current goroutine (block)
  void Park(G* g, const char* reason);

  // Ready a goroutine (unblock)
  void Ready(G* g);

  // Explicit yield
  void Yield();

  // Get processor for current thread
  P* GetP();

 private:
  Scheduler() = default;
  ~Scheduler() = default;

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Global run queue
  void PushGlobal(G* g);
  G* PopGlobal();
  bool StealFromGlobal(P* p, uint32_t batch_size);

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  // Processors
  std::vector<P*> procs_;
  uint32_t gomaxprocs_ = 0;

  // Global runnable queue
  Mutex global_mutex_;
  std::vector<G*> global_runq_;

  // Runtime reference
  Runtime* runtime_ = nullptr;

  friend class Runtime;
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_SCHEDULER_H_

