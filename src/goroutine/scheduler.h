#ifndef SRC_GOROUTINE_SCHEDULER_H_
#define SRC_GOROUTINE_SCHEDULER_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
#include <deque>
#include <vector>
#include "g.h"
#include "node_mutex.h"
#include "v8.h"

namespace node {
namespace goroutine {

// Forward declarations
class M;
class Runtime;

// Per-thread local runnable queue size
constexpr uint32_t kRunqSize = 256;

// Per-thread local queue (owned by Scheduler, indexed by thread id).
// No P abstraction — queues belong directly to M threads.
//
// Cache-line aligned to avoid false sharing between worker threads.
// Each LocalQueue sits on its own 64-byte cache line.
struct alignas(64) LocalQueue {
  std::atomic<uint32_t> head{0};
  std::atomic<uint32_t> tail{0};
  G* runq[kRunqSize];

  LocalQueue() {
    for (uint32_t i = 0; i < kRunqSize; i++) runq[i] = nullptr;
  }

  bool Push(G* g);
  G* Pop();
  G* StealHalf(std::vector<G*>& stolen);
  uint32_t size() const;
};

// Global scheduler — GM model (no P).
//
// Local queues are per-M (per-thread), stored in the scheduler
// and indexed by thread id. Work stealing operates on thread ids.
class Scheduler {
 public:
  static Scheduler* GetInstance();

  // Lifecycle
  void Init(uint32_t num_threads);
  void Shutdown();
  bool IsInitialized() const { return initialized_.load(); }

  // Schedule goroutine (adds to global queue)
  void Schedule(G* g);

  // Find runnable goroutine for thread tid
  G* FindRunnable(uint32_t tid);

  // Park current goroutine (block)
  void Park(G* g, const char* reason);

  // Ready a goroutine (unblock)
  void Ready(G* g);

  // Explicit yield
  void Yield();

  // Get/allocate local queue for thread
  LocalQueue* GetLocalQueue(uint32_t tid);

  uint32_t num_threads() const { return num_threads_; }

 private:
  Scheduler() = default;
  ~Scheduler() = default;

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Global run queue
  void PushGlobal(G* g);
  G* PopGlobal();
  bool StealFromGlobal(uint32_t tid, uint32_t batch_size);

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  // Per-thread local queues (indexed by thread id)
  std::vector<LocalQueue*> local_queues_;
  uint32_t num_threads_ = 0;

  // Schedtick per thread (for global queue fairness).
  // Plain uint32_t: each slot is accessed only by its owning thread.
  std::vector<uint32_t> schedtick_;

  // Global runnable queue
  Mutex global_mutex_;
  std::deque<G*> global_runq_;

  // Round-robin dispatch counter (used by Schedule() to distribute to local queues)
  std::atomic<uint32_t> dispatch_tid_{0};

  // Runtime reference
  Runtime* runtime_ = nullptr;

  friend class Runtime;
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_SCHEDULER_H_

