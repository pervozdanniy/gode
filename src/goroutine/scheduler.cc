#include "scheduler.h"
#include "runtime.h"
#include <algorithm>
#include <random>

// C-linkage API from V8 goroutine patches (goroutine-thread-state.cc)
extern "C" void* v8_goroutine_p_state_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_activate(void* p_state);
extern "C" void  v8_goroutine_p_state_deactivate();
extern "C" void  v8_goroutine_p_state_destroy(void* p_state);

namespace node {
namespace goroutine {

// P implementation
P::P(uint32_t id) : id_(id), v8_state_(nullptr) {
    for (uint32_t i = 0; i < kRunqSize; i++) {
      runq_[i] = nullptr;
    }
}

P::~P() {
    DestroyV8State();
    while (G* g = PopLocal()) {
      delete g;
    }
}

void P::InitV8State(v8::Isolate* isolate) {
  if (v8_state_) return;  // already initialized
  v8_state_ = v8_goroutine_p_state_create(isolate);
  fprintf(stderr, "[P%u] V8 state initialized\n", id_);
  fflush(stderr);
}

void P::DestroyV8State() {
  if (!v8_state_) return;
  v8_goroutine_p_state_destroy(v8_state_);
  v8_state_ = nullptr;
}

void P::Activate() {
  if (v8_state_) {
    v8_goroutine_p_state_activate(v8_state_);
  }
}

void P::Deactivate() {
  v8_goroutine_p_state_deactivate();
}

bool P::PushLocal(G* g) {
  uint32_t tail = runq_tail_.load(std::memory_order_relaxed);
  uint32_t head = runq_head_.load(std::memory_order_acquire);

  // Check if queue is full
  if (tail - head >= kRunqSize) {
    return false;
  }

  runq_[tail % kRunqSize] = g;
  runq_tail_.store(tail + 1, std::memory_order_release);
  return true;
}

G* P::PopLocal() {
  uint32_t tail = runq_tail_.load(std::memory_order_relaxed);
  uint32_t head = runq_head_.load(std::memory_order_acquire);

  // Check if queue is empty
  if (head >= tail) {
    return nullptr;
  }

  // Pop from head (FIFO)
  G* g = runq_[head % kRunqSize];
  runq_head_.store(head + 1, std::memory_order_release);
  return g;
}

G* P::StealHalf(std::vector<G*>& stolen) {
  uint32_t tail = runq_tail_.load(std::memory_order_acquire);
  uint32_t head = runq_head_.load(std::memory_order_acquire);

  uint32_t size = tail - head;
  if (size == 0) {
    return nullptr;
  }

  // Steal half
  uint32_t n = size / 2;
  if (n == 0) {
    n = 1;
  }

  // Try to update head
  uint32_t new_head = head + n;
  if (!runq_head_.compare_exchange_strong(head, new_head)) {
    return nullptr;  // Someone else modified it
  }

  // Copy stolen goroutines
  for (uint32_t i = 0; i < n; i++) {
    stolen.push_back(runq_[(head + i) % kRunqSize]);
  }

  return stolen.empty() ? nullptr : stolen[0];
}

uint32_t P::runq_size() const {
  uint32_t tail = runq_tail_.load(std::memory_order_acquire);
  uint32_t head = runq_head_.load(std::memory_order_acquire);
  return tail > head ? tail - head : 0;
}

// Scheduler implementation
Scheduler* Scheduler::GetInstance() {
  static Scheduler instance;
  return &instance;
}

void Scheduler::Init(uint32_t gomaxprocs) {
  if (initialized_.exchange(true)) {
    return;  // Already initialized
  }

  gomaxprocs_ = gomaxprocs;

  // Create processors
  procs_.resize(gomaxprocs);
  for (uint32_t i = 0; i < gomaxprocs; i++) {
    procs_[i] = new P(i);
  }

  runtime_ = Runtime::GetInstance();
}

void Scheduler::Shutdown() {
  if (!initialized_.load() || shutdown_.exchange(true)) {
    return;
  }

  // Clean up processors
  for (P* p : procs_) {
    delete p;
  }
  procs_.clear();

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

  // Try to add to current P's local queue
  // For now, add to global queue (will improve with thread-local P)
  PushGlobal(g);
}

G* Scheduler::FindRunnable(P* p) {
  if (!p) return nullptr;

  // 1. Check local runq
  G* g = p->PopLocal();
  if (g) {
    return g;
  }

  // 2. Check global runq
  if (StealFromGlobal(p, 10)) {
    g = p->PopLocal();
    if (g) return g;
  }

  // 3. Work stealing from random P
  if (procs_.size() > 1) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, procs_.size() - 1);

    // Try stealing from random processors
    for (uint32_t i = 0; i < procs_.size(); i++) {
      uint32_t victim_id = dis(gen);
      P* victim = procs_[victim_id];

      if (victim != p) {
        std::vector<G*> stolen;
        victim->StealHalf(stolen);

        if (!stolen.empty()) {
          // Add all but first to local queue
          for (size_t j = 1; j < stolen.size(); j++) {
            p->PushLocal(stolen[j]);
          }
          return stolen[0];
        }
      }
    }
  }

  return nullptr;
}

void Scheduler::Park(G* g, const char* reason) {
  if (!g) return;
  g->SetState(GState::Gwaiting);
  // TODO: Add to waiting structures
}

void Scheduler::Ready(G* g) {
  if (!g) return;
  Schedule(g);
}

void Scheduler::Yield() {
  // TODO: Implement cooperative yield
  // Will require context switching
}

P* Scheduler::GetP() {
  // TODO: Implement thread-local P retrieval
  // For now, return first P
  return procs_.empty() ? nullptr : procs_[0];
}

// Global queue operations
void Scheduler::PushGlobal(G* g) {
  Mutex::ScopedLock lock(global_mutex_);
  global_runq_.push_back(g);
}

G* Scheduler::PopGlobal() {
  Mutex::ScopedLock lock(global_mutex_);
  if (global_runq_.empty()) {
    return nullptr;
  }

  G* g = global_runq_.front();
  global_runq_.erase(global_runq_.begin());
  return g;
}

bool Scheduler::StealFromGlobal(P* p, uint32_t batch_size) {
  Mutex::ScopedLock lock(global_mutex_);

  if (global_runq_.empty()) {
    return false;
  }

  // Take up to batch_size from global
  uint32_t n = std::min(batch_size, static_cast<uint32_t>(global_runq_.size()));

  for (uint32_t i = 0; i < n; i++) {
    if (!global_runq_.empty()) {
      G* g = global_runq_.front();
      global_runq_.erase(global_runq_.begin());

      if (!p->PushLocal(g)) {
        // Local queue full, put back
        global_runq_.insert(global_runq_.begin(), g);
        break;
      }
    }
  }

  return true;
}

}  // namespace goroutine
}  // namespace node

