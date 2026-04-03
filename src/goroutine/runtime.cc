#include "runtime.h"
#include "scheduler.h"
#include "context.h"
#include <cstdio>

// V8 goroutine-thread TLS flag (defined in deps/v8).
extern thread_local bool v8_goroutine_thread;

namespace node {
namespace goroutine {

// ---- M (Machine) ----

M::M(uint32_t id) : id_(id) {}
M::~M() = default;

bool M::ExecuteOne(v8::Isolate* isolate) {
  if (!p_) return false;

  Scheduler* sched = Scheduler::GetInstance();
  G* g = sched->FindRunnable(p_);
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
  // Wake the thread so it can observe running_ == false.
  uv_sem_post(&runtime_->goroutine_sem_);
  uv_thread_join(&thread_);
}

void M::ThreadEntry(void* arg) {
  static_cast<M*>(arg)->ThreadLoop();
}

void M::ThreadLoop() {
  Runtime* rt = runtime_;
  v8::Isolate* isolate = rt->isolate();

  // Activate V8 goroutine-thread patches BEFORE Enter() so that
  // isolate_data() redirects to per-thread data and Enter() doesn't
  // overwrite the main thread's thread_id_ in shared IsolateData.
  v8_goroutine_thread = true;
  // Register this OS thread with V8.
  isolate->Enter();

  while (running_.load()) {
    // Sleep until a goroutine is available.
    uv_sem_wait(&rt->goroutine_sem_);
    if (!running_.load()) break;

    // Acquire V8.
    uv_mutex_lock(&rt->v8_mutex_);

    // Set stack limit for this worker thread's stack.
    uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
    isolate->SetStackLimit(sp - (900 * 1024));

    // Run goroutines until the queue is empty.
    while (running_.load()) {
      if (!ExecuteOne(isolate)) break;

      // Briefly release V8 so M0 can reacquire after epoll.
      uv_mutex_unlock(&rt->v8_mutex_);
      uv_mutex_lock(&rt->v8_mutex_);
    }

    uv_mutex_unlock(&rt->v8_mutex_);
  }

  v8_goroutine_thread = false;
  isolate->Exit();
}

// ---- Runtime ----

Runtime* Runtime::GetInstance() {
  static Runtime instance;
  return &instance;
}

void Runtime::Init(uint32_t gomaxprocs, uv_loop_t* loop,
                   v8::Isolate* isolate) {
  if (initialized_.exchange(true)) return;

  gomaxprocs_ = gomaxprocs;
  loop_ = loop;
  isolate_ = isolate;

  fprintf(stderr, "[goroutine] runtime: GOMAXPROCS=%u\n", gomaxprocs);

  scheduler_ = Scheduler::GetInstance();
  scheduler_->Init(gomaxprocs);

  // V8 execution token — M0 starts as the owner.
  uv_mutex_init(&v8_mutex_);
  v8_mutex_init_ = true;
  uv_mutex_lock(&v8_mutex_);

  // Goroutine notification semaphore.
  uv_sem_init(&goroutine_sem_, 0);
  sem_init_ = true;

  // M0 = main thread, bound to P0.
  m0_ = new M(0);
  m0_->set_p(scheduler_->procs_[0]);

  // uv_prepare: release V8 before the event loop enters I/O poll.
  uv_prepare_init(loop_, &prepare_handle_);
  prepare_handle_.data = this;
  uv_prepare_start(&prepare_handle_, OnPrepare);
  uv_unref(reinterpret_cast<uv_handle_t*>(&prepare_handle_));
  prepare_active_ = true;

  // uv_check: reacquire V8 after I/O poll + drain run queue on M0.
  uv_check_init(loop_, &check_handle_);
  check_handle_.data = this;
  uv_check_start(&check_handle_, OnCheck);
  uv_unref(reinterpret_cast<uv_handle_t*>(&check_handle_));
  check_active_ = true;

  // uv_idle: spins while goroutines are pending (prevents epoll blocking).
  uv_idle_init(loop_, &idle_handle_);
  idle_handle_.data = this;
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_handle_));

  // Worker M-threads (M1 … M_{GOMAXPROCS-1}).
  for (uint32_t i = 1; i < gomaxprocs; i++) {
    M* m = new M(i);
    m->set_p(scheduler_->procs_[i]);
    m->StartThread(this);
    workers_.push_back(m);
  }

  fprintf(stderr, "[goroutine] runtime: ready (%u workers)\n",
          static_cast<uint32_t>(workers_.size()));
}

void Runtime::Shutdown() {
  if (!initialized_.load() || shutdown_.exchange(true)) return;

  // Stop worker M-threads first (they need the semaphore alive).
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

  // Release V8 mutex held by M0.
  if (v8_mutex_init_) {
    uv_mutex_unlock(&v8_mutex_);
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

  fprintf(stderr, "[goroutine] runtime: shutdown\n");
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

  rt->DrainRunQueue();
}

void Runtime::OnIdle(uv_idle_t* handle) {
  Runtime* rt = static_cast<Runtime*>(handle->data);
  // M0 holds v8_mutex_ here (idle fires before prepare).
  rt->DrainRunQueue();
}

void Runtime::DrainRunQueue() {
  if (!m0_ || !isolate_) return;

  constexpr int kBudget = 128;
  bool found = false;
  for (int i = 0; i < kBudget; i++) {
    if (!m0_->ExecuteOne(isolate_)) break;
    found = true;
  }

  // If nothing was runnable, stop spinning.
  if (!found && idle_active_) {
    uv_idle_stop(&idle_handle_);
    idle_active_ = false;
  }
}

void Runtime::NotifyGoroutineAvailable() {
  if (sem_init_) {
    uv_sem_post(&goroutine_sem_);
  }
  // Start idle spinner so the event loop doesn't block in poll
  // while goroutines are pending.
  if (!idle_active_ && loop_) {
    uv_idle_start(&idle_handle_, OnIdle);
    idle_active_ = true;
  }
}

}  // namespace goroutine
}  // namespace node

