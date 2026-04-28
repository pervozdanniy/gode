#include "scheduler.h"
#include "runtime.h"
#include <algorithm>
#include <random>

namespace node {
namespace goroutine {

// ---- LocalQueue implementation ----

bool LocalQueue::Push(G* g) {
  uint32_t t = tail.load(std::memory_order_relaxed);
  uint32_t h = head.load(std::memory_order_acquire);

  if (t - h >= kRunqSize) {
    return false;  // full
  }

  runq[t % kRunqSize] = g;
  tail.store(t + 1, std::memory_order_release);
  return true;
}

G* LocalQueue::Pop() {
  uint32_t t = tail.load(std::memory_order_relaxed);
  uint32_t h = head.load(std::memory_order_acquire);

  if (h >= t) {
    return nullptr;  // empty
  }

  G* g = runq[h % kRunqSize];
  head.store(h + 1, std::memory_order_release);
  return g;
}

G* LocalQueue::StealHalf(std::vector<G*>& stolen) {
  uint32_t t = tail.load(std::memory_order_acquire);
  uint32_t h = head.load(std::memory_order_acquire);

  uint32_t sz = t - h;
  if (sz == 0) return nullptr;

  uint32_t n = sz / 2;
  if (n == 0) n = 1;

  uint32_t new_head = h + n;
  if (!head.compare_exchange_strong(h, new_head)) {
    return nullptr;  // contention
  }

  for (uint32_t i = 0; i < n; i++) {
    stolen.push_back(runq[(h + i) % kRunqSize]);
  }
  return stolen.empty() ? nullptr : stolen[0];
}

uint32_t LocalQueue::size() const {
  uint32_t t = tail.load(std::memory_order_acquire);
  uint32_t h = head.load(std::memory_order_acquire);
  return t > h ? t - h : 0;
}

// ---- Scheduler implementation ----

Scheduler* Scheduler::GetInstance() {
  static Scheduler instance;
  return &instance;
}

void Scheduler::Init(uint32_t num_threads) {
  if (initialized_.exchange(true)) return;

  num_threads_ = num_threads;

  // Create per-thread local queues
  local_queues_.resize(num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    local_queues_[i] = new LocalQueue();
  }

  // Per-thread schedtick counters
  schedtick_.assign(num_threads, 0);

  runtime_ = Runtime::GetInstance();
}

void Scheduler::Shutdown() {
  if (!initialized_.load() || shutdown_.exchange(true)) return;

  // Clean up local queues
  for (LocalQueue* lq : local_queues_) {
    while (G* g = lq->Pop()) {
      delete g;
    }
    delete lq;
  }
  local_queues_.clear();

  // Clean up global queue
  {
    Mutex::ScopedLock lock(global_mutex_);
    for (G* g : global_runq_) {
      delete g;
    }
    global_runq_.clear();
  }
}

void Scheduler::Schedule(G* g) {
  if (!g) return;

  g->SetState(GState::Grunnable);

  // Fast path: try one target queue via atomic round-robin.
  // Avoid iterating all N queues (O(n) cost + false sharing).
  if (num_threads_ > 0) {
    uint32_t target = dispatch_tid_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
    if (local_queues_[target]->Push(g)) {
      return;
    }
    // Target full: try ONE more (next in round-robin).
    target = (target + 1) % num_threads_;
    if (local_queues_[target]->Push(g)) {
      return;
    }
  }

  // Both attempts failed → fall back to global queue.
  // Work stealing will redistribute from global later.
  PushGlobal(g);
}

G* Scheduler::FindRunnable(uint32_t tid) {
  if (tid >= local_queues_.size()) return nullptr;
  LocalQueue* lq = local_queues_[tid];
  uint32_t tick = schedtick_[tid]++;

  // 1. Every 61st tick — check global queue FIRST (prevents starvation).
  // This matches Go's algorithm: global every 61 calls, then local, then steal.
  if (tick % 61 == 0) {
    if (StealFromGlobal(tid, 10)) {
      G* g = lq->Pop();
      if (g) return g;
    }
  }

  // 2. Own local queue (no mutex, FIFO for our ring buffer).
  G* g = lq->Pop();
  if (g) return g;

  // 3. Global queue batch steal (if not just checked in step 1).
  if (tick % 61 != 0) {
    if (StealFromGlobal(tid, 10)) {
      g = lq->Pop();
      if (g) return g;
    }
  }

  // 4. Work stealing — try to steal from ALL other threads in round-robin.
  // Go's algorithm: try every P once, starting from last steal victim.
  // We use a simpler approach: try all threads once in order.
  if (local_queues_.size() > 1) {
    // Start from (tid+1) to avoid checking self first.
    for (uint32_t i = 1; i < local_queues_.size(); i++) {
      uint32_t victim = (tid + i) % local_queues_.size();

      std::vector<G*> stolen;
      local_queues_[victim]->StealHalf(stolen);

      if (!stolen.empty()) {
        // Put all but first into our local queue.
        for (size_t j = 1; j < stolen.size(); j++) {
          lq->Push(stolen[j]);
        }
        return stolen[0];
      }
    }
  }

  // 5. Final global check after failed steal (Go does this too).
  if (StealFromGlobal(tid, 10)) {
    g = lq->Pop();
    if (g) return g;
  }

  return nullptr;
}

void Scheduler::Park(G* g, const char* reason) {
  if (!g) return;
  g->SetState(GState::Gwaiting);
}

void Scheduler::Ready(G* g) {
  if (!g) return;
  Schedule(g);
}

void Scheduler::Yield() {
  // TODO: cooperative yield with context switching
}

LocalQueue* Scheduler::GetLocalQueue(uint32_t tid) {
  if (tid >= local_queues_.size()) return nullptr;
  return local_queues_[tid];
}

// ---- Global queue operations ----

void Scheduler::PushGlobal(G* g) {
  Mutex::ScopedLock lock(global_mutex_);
  global_runq_.push_back(g);
}

G* Scheduler::PopGlobal() {
  Mutex::ScopedLock lock(global_mutex_);
  if (global_runq_.empty()) return nullptr;

  G* g = global_runq_.front();
  global_runq_.pop_front();  // O(1) with deque
  return g;
}

bool Scheduler::StealFromGlobal(uint32_t tid, uint32_t batch_size) {
  Mutex::ScopedLock lock(global_mutex_);
  if (global_runq_.empty()) return false;

  LocalQueue* lq = local_queues_[tid];
  uint32_t n = std::min(batch_size, static_cast<uint32_t>(global_runq_.size()));

  uint32_t pushed = 0;
  for (uint32_t i = 0; i < n; i++) {
    G* g = global_runq_.front();
    if (!lq->Push(g)) break;
    global_runq_.pop_front();  // O(1) with deque
    pushed++;
  }

  return pushed > 0;
}

}  // namespace goroutine
}  // namespace node

