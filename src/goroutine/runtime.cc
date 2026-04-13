#include "runtime.h"
#include "scheduler.h"
#include "context.h"

#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>

#define GTRACE(fmt, ...) \
  fprintf(stderr, "[GTRACE tid=%ld] " fmt "\n", \
          (long)syscall(SYS_gettid), ##__VA_ARGS__)

// V8 goroutine-thread TLS flag (defined in deps/v8/src/execution/goroutine-thread.cc)
extern thread_local bool v8_goroutine_thread;

// Per-M V8 IsolateData state (goroutine-thread-state.cc)
extern "C" void* v8_goroutine_p_state_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_activate(void* state);
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

void M::StopThread() {
  if (!running_.exchange(false)) return;
  uv_sem_post(&runtime_->goroutine_sem_);
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
  v8_goroutine_p_state_activate(v8_state_);

  // Link LocalHeap to per-M StackGuard so GC safepoints can interrupt this
  // goroutine worker thread's interpreter via RequestInterruptUnsafe().
  uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
  isolate->SetStackLimit(sp - (900 * 1024));

  while (running_.load()) {
    // ---- Parked: waiting for work signal (GC-safe) ----
    GTRACE("Worker M(id=%u) waiting on sem...", id_);
    uv_sem_wait(&rt->goroutine_sem_);
    if (!running_.load()) break;

    GTRACE("Worker M(id=%u) woke up, draining queue", id_);

    // Inner loop: drain goroutines while available.
    int count = 0;
    while (running_.load()) {
      Scheduler* sched = Scheduler::GetInstance();
      G* g = sched->FindRunnable(id_);
      if (!g) break;

      GTRACE("Worker M(id=%u) picked G%llu", id_, (unsigned long long)g->goid());
      count++;

      current_g_ = g;
      g->SetState(GState::Grunning);

      v8_goroutine_local_heap_unpark(local_heap_);
      GTRACE("Worker M(id=%u) running G%llu", id_, (unsigned long long)g->goid());
      RunG(g, isolate);
      GTRACE("Worker M(id=%u) G%llu done/yielded", id_, (unsigned long long)g->goid());
      v8_goroutine_local_heap_park(local_heap_);

      current_g_ = nullptr;
      if (g->state() == GState::Gdead) {
        delete g;
      }
    }

    GTRACE("Worker M(id=%u) inner loop done (ran %d goroutines)", id_, count);

    if (rt->async_init_) {
      uv_async_send(&rt->async_handle_);
    }
  }

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

  // Stop worker M-threads.
  for (M* m : workers_) {
    m->StopThread();
    delete m;
  }
  workers_.clear();

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
  // No-op. Wakes epoll so the event loop can process callbacks.
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
    if (sem_init_) {
      GTRACE("NotifyGoroutineAvailable: sem_post (num_threads=%u)", num_threads_);
      uv_sem_post(&goroutine_sem_);
    }
  }
}

}  // namespace goroutine
}  // namespace node
