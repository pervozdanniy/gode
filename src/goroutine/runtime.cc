#include "runtime.h"
#include "scheduler.h"
#include "context.h"

#include <cstdio>
#include <mutex>
#include <string>

// Global V8 Lock — serialises concurrent goroutine execution until
// per-thread JIT cache (Phase 2) makes true parallelism safe.
// GVL removed: workers run V8 concurrently via per-M IsolateData + LocalHeap
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>


#define GTRACE(fmt, ...) \
  fprintf(stderr, "[GTRACE tid=%ld] " fmt "\n", \
          (long)syscall(SYS_gettid), ##__VA_ARGS__)

// Per-M V8 IsolateData state (goroutine-thread-state.cc)
extern "C" void* v8_goroutine_p_state_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_activate_with_isolate(void* state, v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_deactivate();
extern "C" void  v8_goroutine_p_state_destroy(void* state);

// Per-M LocalHeap for GC safepoint coordination (goroutine-local-heap.cc).
// LocalHeap also calls Isolate::SetCurrent() so v8::Isolate::GetCurrent()
// works on worker threads without isolate->Enter().
extern "C" void* v8_goroutine_local_heap_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_local_heap_destroy(void* lh);
extern "C" void  v8_goroutine_local_heap_park(void* lh);
extern "C" void  v8_goroutine_local_heap_unpark(void* lh);
extern "C" void  v8_goroutine_local_heap_safepoint(void* lh);

namespace node {
namespace goroutine {

// ---- M (Machine) ----

M::M(uint32_t id) : id_(id) {}
M::~M() { DestroyV8State(); }

void M::InitV8State(v8::Isolate* isolate) {
  if (v8_state_) return;
  v8_state_ = v8_goroutine_p_state_create(isolate);
}

void M::DestroyV8State() {
  if (!v8_state_) return;
  v8_goroutine_p_state_destroy(v8_state_);
  v8_state_ = nullptr;
}

bool M::ExecuteOne(v8::Isolate* isolate) {
  Scheduler* sched = Scheduler::GetInstance();
  G* g = sched->FindRunnable(id_);
  if (!g) return false;

  GTRACE("M0(id=%u) picked G%llu", id_, (unsigned long long)g->goid());

  current_g_ = g;
  g->SetState(GState::Grunning);

  // M0 is the main thread — always Running, no LocalHeap park/unpark needed.
  RunG(g, isolate);

  current_g_ = nullptr;

  if (g->state() == GState::Gdead) {
    delete g;
  }

  return true;
}

void M::StartThread(Runtime* rt) {
  runtime_ = rt;
  running_.store(true);
  uv_thread_create(&thread_, ThreadEntry, this);
}

// Phase 1: mark this M as stopping and post to the shared sem to wake it.
// MUST be called for ALL workers BEFORE joining ANY — because the semaphore
// is shared. If we signal M1 then immediately join M1, another worker (M2)
// might steal the sem_post, leaving M1 stuck in uv_sem_wait forever.
void M::SignalStop() {
  running_.store(false, std::memory_order_release);
}

// Phase 2: wait for this M to exit. Call only after SignalStop() has been
// called for ALL workers.
void M::JoinThread() {
  uv_thread_join(&thread_);
}

void M::ThreadEntry(void* arg) {
  static_cast<M*>(arg)->ThreadLoop();
}

void M::ThreadLoop() {
  Runtime* rt = runtime_;
  v8::Isolate* isolate = rt->isolate();

  GTRACE("Worker M(id=%u) started", id_);

  // Create LocalHeap — starts Parked (GC-safe).
  local_heap_ = v8_goroutine_local_heap_create(isolate);
  v8_goroutine_p_state_activate_with_isolate(v8_state_, isolate);

  // Link LocalHeap to per-M StackGuard so GC safepoints can interrupt this
  // goroutine worker thread's interpreter via RequestInterruptUnsafe().
  uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
  isolate->SetStackLimit(sp - (900 * 1024));

  while (running_.load()) {
    // ---- Parked: waiting for work signal (GC-safe) ----
    GTRACE("Worker M(id=%u) waiting on sem...", id_);
    rt->sleeping_workers_.fetch_add(1, std::memory_order_relaxed);
    uv_sem_wait(&rt->goroutine_sem_);
    rt->sleeping_workers_.fetch_sub(1, std::memory_order_relaxed);
    if (!running_.load()) break;

    GTRACE("Worker M(id=%u) woke up, draining queue", id_);

    // Inner loop: drain goroutines while available.
    int count = 0;
    while (running_.load()) {
      // FindRunnable while Parked — no V8 heap access, GC-safe.
      Scheduler* sched = Scheduler::GetInstance();
      G* g = sched->FindRunnable(id_);
      if (!g) break;

      GTRACE("Worker M(id=%u) picked G%llu", id_, (unsigned long long)g->goid());
      count++;

      current_g_ = g;
      g->SetState(GState::Grunning);

      // Unpark: signals V8 heap access begins; GC must wait for safepoint.
      v8_goroutine_local_heap_unpark(local_heap_);
      GTRACE("Worker M(id=%u) running G%llu", id_, (unsigned long long)g->goid());

      RunG(g, isolate);

      GTRACE("Worker M(id=%u) G%llu done/yielded", id_, (unsigned long long)g->goid());
      v8_goroutine_local_heap_park(local_heap_);

      current_g_ = nullptr;
      if (g->state() == GState::Gdead) {
        // v8::Global::~G() calls Reset() which is NOT thread-safe.
        // Enqueue for deletion on main thread (drained in OnAsync).
        std::lock_guard<std::mutex> lock(rt->dead_mutex_);
        rt->dead_queue_.push_back(g);
      }
    }

    GTRACE("Worker M(id=%u) inner loop done (ran %d goroutines)", id_, count);

    if (rt->async_init_) {
      uv_async_send(&rt->async_handle_);
    }
  }

  // ThreadLoop exiting. Destroy LocalHeap from THIS (worker) thread — required
  // because LocalHeap::~LocalHeap() uses thread-local write barriers and
  // Isolate::Current() that belong to this M thread.
  // This is safe because Shutdown() calls SignalStop() for ALL workers first,
  // then joins them all — so the main thread is NOT blocked waiting for us
  // while we're here doing cleanup.
  v8_goroutine_p_state_deactivate();
  v8_goroutine_local_heap_destroy(local_heap_);
  local_heap_ = nullptr;
}

// ---- Runtime ----

Runtime* Runtime::GetInstance() {
  static Runtime instance;
  return &instance;
}

void Runtime::Init(uint32_t num_threads, uv_loop_t* loop,
                   v8::Isolate* isolate) {
  if (initialized_.exchange(true)) return;

  num_threads_ = num_threads;
  loop_ = loop;
  isolate_ = isolate;

  scheduler_ = Scheduler::GetInstance();
  scheduler_->Init(num_threads);

  // Goroutine notification semaphore.
  uv_sem_init(&goroutine_sem_, 0);
  sem_init_ = true;

  // M0 = main thread (no LocalHeap needed — it already has the main one).
  m0_ = new M(0);

  // uv_check: M0 drains goroutine queue after I/O poll (GOMAXPROCS=1 mode).
  uv_check_init(loop_, &check_handle_);
  check_handle_.data = this;
  uv_check_start(&check_handle_, OnCheck);
  uv_unref(reinterpret_cast<uv_handle_t*>(&check_handle_));
  check_active_ = true;

  // uv_idle: spins while goroutines are pending (single-thread only).
  uv_idle_init(loop_, &idle_handle_);
  idle_handle_.data = this;
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_handle_));

  // uv_async: workers use this to wake the event loop.
  uv_async_init(loop_, &async_handle_, OnAsync);
  async_handle_.data = this;
  uv_unref(reinterpret_cast<uv_handle_t*>(&async_handle_));
  async_init_ = true;

  // Worker M-threads (M1 … M_{num_threads-1}).
  for (uint32_t i = 1; i < num_threads; i++) {
    M* m = new M(i);
    m->InitV8State(isolate);
    m->StartThread(this);
    workers_.push_back(m);
  }
}

void Runtime::Shutdown() {
  if (!initialized_.load() || shutdown_.exchange(true)) return;

  // Phase 1: mark ALL workers as stopped first (no sem_post yet).
  // The goroutine_sem_ is shared — if we post once per worker, a wrong
  // worker can steal the post, leaving the target stuck in uv_sem_wait.
  for (M* m : workers_) {
    m->SignalStop();
  }

  // Phase 2: post num_threads wakeups — enough to wake every sleeping worker.
  // Threads that get "extra" posts will simply loop, see running_=false, exit.
  for (size_t i = 0; i < workers_.size(); i++) {
    uv_sem_post(&goroutine_sem_);
  }

  // Phase 2: join all workers and clean up.
  for (M* m : workers_) {
    m->JoinThread();
    delete m;
  }
  workers_.clear();

  // Drain any remaining dead goroutines (workers may have enqueued some
  // after the last OnAsync fired but before they exited).
  {
    std::lock_guard<std::mutex> lock(dead_mutex_);
    for (G* g : dead_queue_) delete g;
    dead_queue_.clear();
  }

  if (check_active_) {
    uv_check_stop(&check_handle_);
    check_active_ = false;
  }
  if (idle_active_) {
    uv_idle_stop(&idle_handle_);
    idle_active_ = false;
  }
  if (async_init_) {
    uv_close(reinterpret_cast<uv_handle_t*>(&async_handle_), nullptr);
    async_init_ = false;
  }

  if (sem_init_) {
    uv_sem_destroy(&goroutine_sem_);
    sem_init_ = false;
  }

  delete m0_;
  m0_ = nullptr;

  if (scheduler_) scheduler_->Shutdown();
}

void Runtime::OnCheck(uv_check_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);

  uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
  rt->isolate_->SetStackLimit(sp - (900 * 1024));

  // In single-thread mode (GOMAXPROCS=1) M0 runs all goroutines.
  // In multi-thread mode workers own the goroutine queue; M0 stays out.
  if (rt->num_threads_ == 1) {
    rt->DrainRunQueue();
  }
}

void Runtime::OnIdle(uv_idle_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);
  rt->DrainRunQueue();
}

void Runtime::OnAsync(uv_async_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);

  // Drain dead goroutine queue — delete G objects on main thread so that
  // v8::Global::Reset() (in G::~G) runs with V8 context, not from worker.
  std::vector<G*> dead;
  {
    std::lock_guard<std::mutex> lock(rt->dead_mutex_);
    dead.swap(rt->dead_queue_);
  }
  for (G* g : dead) delete g;

  // Drain the goroutine print queue on the main thread.
  std::vector<std::string> local;
  {
    std::lock_guard<std::mutex> lock(rt->print_mutex_);
    local.swap(rt->print_queue_);
  }
  for (const auto& msg : local) {
    fwrite(msg.c_str(), 1, msg.size(), stdout);
  }
  if (!local.empty()) fflush(stdout);
}

void Runtime::EnqueuePrint(std::string msg) {
  if (!async_init_) {
    // Runtime not initialized (called from main thread before any go()).
    // Print directly.
    fwrite(msg.c_str(), 1, msg.size(), stdout);
    fflush(stdout);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(print_mutex_);
    print_queue_.push_back(std::move(msg));
  }
  // uv_async_send is thread-safe by spec — safe to call from goroutine threads.
  uv_async_send(&async_handle_);
}

void Runtime::DrainRunQueue() {
  if (!m0_ || !isolate_) return;

  constexpr int kBudget = 128;
  bool found = false;
  for (int i = 0; i < kBudget; i++) {
    if (!m0_->ExecuteOne(isolate_)) break;
    found = true;
  }

  if (!found && idle_active_) {
    uv_idle_stop(&idle_handle_);
    idle_active_ = false;
  }
}

void Runtime::NotifyGoroutineAvailable() {
  if (num_threads_ == 1) {
    if (!idle_active_ && loop_) {
      uv_idle_start(&idle_handle_, OnIdle);
      idle_active_ = true;
    }
  } else {
    if (sem_init_ && sleeping_workers_.load(std::memory_order_relaxed) > 0) {
      GTRACE("NotifyGoroutineAvailable: sem_post (num_threads=%u, sleeping=%d)",
             num_threads_, sleeping_workers_.load());
      uv_sem_post(&goroutine_sem_);
    }
  }
}

}  // namespace goroutine
}  // namespace node
