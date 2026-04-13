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
  schedtick_.resize(num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    schedtick_[i].store(0);
  }

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

  // TODO: try to add to current thread's local queue first
  PushGlobal(g);
}

G* Scheduler::FindRunnable(uint32_t tid) {
  if (tid >= local_queues_.size()) return nullptr;
  LocalQueue* lq = local_queues_[tid];

  // 1. Every 61st tick → check global first (prevents starvation)
  uint32_t tick = schedtick_[tid].fetch_add(1, std::memory_order_relaxed);
  if (tick % 61 == 0) {
    G* g = PopGlobal();
    if (g) return g;
  }

  // 2. Own local queue
  G* g = lq->Pop();
  if (g) return g;

  // 3. Global queue
  if (StealFromGlobal(tid, 10)) {
    g = lq->Pop();
    if (g) return g;
  }

  // 4. Work stealing — steal half from a random thread
  if (local_queues_.size() > 1) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, local_queues_.size() - 1);

    for (uint32_t i = 0; i < local_queues_.size(); i++) {
      uint32_t victim = dis(gen);
      if (victim == tid) continue;

      std::vector<G*> stolen;
      local_queues_[victim]->StealHalf(stolen);

      if (!stolen.empty()) {
        // Put all but first into our local queue
        for (size_t j = 1; j < stolen.size(); j++) {
          lq->Push(stolen[j]);
        }
        return stolen[0];
      }
    }
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
  global_runq_.erase(global_runq_.begin());
  return g;
}

bool Scheduler::StealFromGlobal(uint32_t tid, uint32_t batch_size) {
  Mutex::ScopedLock lock(global_mutex_);
  if (global_runq_.empty()) return false;

  LocalQueue* lq = local_queues_[tid];
  uint32_t n = std::min(batch_size, static_cast<uint32_t>(global_runq_.size()));

  for (uint32_t i = 0; i < n; i++) {
    if (global_runq_.empty()) break;

    G* g = global_runq_.front();
    global_runq_.erase(global_runq_.begin());

    if (!lq->Push(g)) {
      global_runq_.insert(global_runq_.begin(), g);
      break;
    }
  }

  return true;
}

}  // namespace goroutine
}  // namespace node

