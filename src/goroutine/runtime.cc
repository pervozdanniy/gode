#include "runtime.h"
#include "scheduler.h"
#include "context.h"

// V8 goroutine-thread TLS flag (defined in deps/v8).
extern thread_local bool v8_goroutine_thread;

// C-linkage API from V8 goroutine patches (goroutine-thread-state.cc)
extern "C" void* v8_goroutine_p_state_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_activate(void* state);
extern "C" void  v8_goroutine_p_state_deactivate();
extern "C" void  v8_goroutine_p_state_destroy(void* state);

// C-linkage API from V8 goroutine GC safepoint (goroutine-safepoint.cc)
extern "C" void* v8_goroutine_safepoint_register();
extern "C" void  v8_goroutine_safepoint_unregister(void* handle);
extern "C" void  v8_goroutine_safepoint_set_running(void* handle);
extern "C" void  v8_goroutine_safepoint_set_parked(void* handle);
extern "C" void  v8_goroutine_safepoint_check(void* handle);

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

void M::ActivateV8State() {
  if (v8_state_) {
    v8_goroutine_p_state_activate(v8_state_);
  }
}

void M::DeactivateV8State() {
  v8_goroutine_p_state_deactivate();
}

void M::CheckSafepoint() {
  v8_goroutine_safepoint_check(safepoint_entry_);
}

bool M::ExecuteOne(v8::Isolate* isolate) {
  Scheduler* sched = Scheduler::GetInstance();
  G* g = sched->FindRunnable(id_);
  if (!g) return false;

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

  v8_goroutine_thread = true;
  isolate->Enter();

  // Register this M thread with the GC safepoint registry.
  safepoint_entry_ = v8_goroutine_safepoint_register();

  while (running_.load()) {
    // M is parked (in sem_wait) — GC-safe.
    v8_goroutine_safepoint_set_parked(safepoint_entry_);
    uv_sem_wait(&rt->goroutine_sem_);
    if (!running_.load()) break;

    uv_mutex_lock(&rt->v8_mutex_);

    uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
    isolate->SetStackLimit(sp - (900 * 1024));

    // M is now running — notify GC safepoint registry.
    v8_goroutine_safepoint_set_running(safepoint_entry_);

    while (running_.load()) {
      if (!ExecuteOne(isolate)) break;

      // Between goroutines: check if GC needs us to stop.
      CheckSafepoint();

      // Briefly release V8 so M0 can reacquire after epoll.
      uv_mutex_unlock(&rt->v8_mutex_);
      uv_mutex_lock(&rt->v8_mutex_);
    }

    // Back to parked before releasing mutex.
    v8_goroutine_safepoint_set_parked(safepoint_entry_);
    uv_mutex_unlock(&rt->v8_mutex_);

    // Wake event loop so callbacks can observe goroutine results.
    if (rt->async_init_) {
      uv_async_send(&rt->async_handle_);
    }
  }

  // Unregister from GC safepoint registry before exiting.
  v8_goroutine_safepoint_unregister(safepoint_entry_);
  safepoint_entry_ = nullptr;

  v8_goroutine_thread = false;
  isolate->Exit();
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

  // V8 execution token — M0 starts as the owner.
  uv_mutex_init(&v8_mutex_);
  v8_mutex_init_ = true;
  uv_mutex_lock(&v8_mutex_);

  // Goroutine notification semaphore.
  uv_sem_init(&goroutine_sem_, 0);
  sem_init_ = true;

  // M0 = main thread (tid=0), no separate V8 state needed.
  m0_ = new M(0);

  // uv_prepare: release V8 before the event loop enters I/O poll.
  uv_prepare_init(loop_, &prepare_handle_);
  prepare_handle_.data = this;
  uv_prepare_start(&prepare_handle_, OnPrepare);
  uv_unref(reinterpret_cast<uv_handle_t*>(&prepare_handle_));
  prepare_active_ = true;

  // uv_check: reacquire V8 after I/O poll.
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

  // Release V8 mutex so workers can finish.
  if (v8_mutex_init_) {
    uv_mutex_unlock(&v8_mutex_);
  }

  // Stop worker M-threads.
  for (M* m : workers_) {
    m->StopThread();
    delete m;
  }
  workers_.clear();

  if (prepare_active_) {
    uv_prepare_stop(&prepare_handle_);
    prepare_active_ = false;
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

  if (v8_mutex_init_) {
    uv_mutex_destroy(&v8_mutex_);
    v8_mutex_init_ = false;
  }

  if (sem_init_) {
    uv_sem_destroy(&goroutine_sem_);
    sem_init_ = false;
  }

  delete m0_;
  m0_ = nullptr;

  if (scheduler_) scheduler_->Shutdown();
}

void Runtime::OnPrepare(uv_prepare_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);
  uv_mutex_unlock(&rt->v8_mutex_);
}

void Runtime::OnCheck(uv_check_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);
  uv_mutex_lock(&rt->v8_mutex_);

  uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
  rt->isolate_->SetStackLimit(sp - (900 * 1024));

  // Single-thread mode: M0 must drain goroutines.
  // Multi-thread: workers handle goroutines — M0 only does callbacks.
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
      uv_sem_post(&goroutine_sem_);
    }
  }
}

}  // namespace goroutine
}  // namespace node

