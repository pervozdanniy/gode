#include "runtime.h"
#include "scheduler.h"
#include "context.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <time.h>

// Uncomment to enable per-goroutine execution tracing:
#define GOROUTINE_TRACE 1

// Global V8 Lock — serialises concurrent goroutine execution until
// per-thread JIT cache (Phase 2) makes true parallelism safe.
// GVL removed: workers run V8 concurrently via per-M IsolateData + LocalHeap
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>


// Per-M V8 IsolateData state (goroutine-thread-state.cc)
extern "C" void* v8_goroutine_p_state_create(v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_activate_with_isolate(void* state, v8::Isolate* isolate);
extern "C" void  v8_goroutine_p_state_deactivate();
extern "C" void  v8_goroutine_p_state_destroy(void* state);
// Set per-M StackGuard limit (does NOT touch shared Isolate::stack_size_).
extern "C" void  v8_goroutine_set_stack_limit(uintptr_t limit);

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

  // Create LocalHeap — starts Parked (GC-safe).
  local_heap_ = v8_goroutine_local_heap_create(isolate);
  v8_goroutine_p_state_activate_with_isolate(v8_state_, isolate);
  // ActivatePState already set the per-M StackGuard limit from the current
  // stack pointer. No need to call SetStackLimit again here.

  while (running_.load()) {
    // ---- Parked: waiting for work signal (GC-safe) ----
    rt->sleeping_workers_.fetch_add(1, std::memory_order_relaxed);
    uv_sem_wait(&rt->goroutine_sem_);
    rt->sleeping_workers_.fetch_sub(1, std::memory_order_relaxed);
    if (!running_.load()) break;

    // Inner loop: drain goroutines while available.
    int count = 0;
    while (running_.load()) {
      // FindRunnable while Parked — no V8 heap access, GC-safe.
      Scheduler* sched = Scheduler::GetInstance();
      G* g = sched->FindRunnable(id_);
      if (!g) break;
      count++;

      current_g_ = g;
      g->SetState(GState::Grunning);

      // Unpark: signals V8 heap access begins; GC must wait for safepoint.
      v8_goroutine_local_heap_unpark(local_heap_);

#if defined(GOROUTINE_TRACE)
      struct timespec ts_wall_start, ts_wall_end, ts_cpu_start, ts_cpu_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_wall_start);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_cpu_start);
      pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
#endif

      RunG(g, isolate);

#if defined(GOROUTINE_TRACE)
      clock_gettime(CLOCK_MONOTONIC, &ts_wall_end);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_cpu_end);
      double wall_ms = (ts_wall_end.tv_sec - ts_wall_start.tv_sec) * 1000.0 +
                       (ts_wall_end.tv_nsec - ts_wall_start.tv_nsec) / 1e6;
      double cpu_ms  = (ts_cpu_end.tv_sec  - ts_cpu_start.tv_sec)  * 1000.0 +
                       (ts_cpu_end.tv_nsec  - ts_cpu_start.tv_nsec)  / 1e6;
      double abs_ms  = ts_wall_start.tv_sec * 1000.0 + ts_wall_start.tv_nsec / 1e6;
      double cpu_util = (wall_ms > 0) ? (cpu_ms / wall_ms * 100.0) : 0.0;
      fprintf(stderr, "[M%u tid=%d] g=%p start=%.1f wall=%.1fms cpu=%.1fms util=%.0f%%\n",
              id_, (int)tid, static_cast<void*>(g), abs_ms,
              wall_ms, cpu_ms, cpu_util);
#endif

      v8_goroutine_local_heap_park(local_heap_);

      current_g_ = nullptr;
      if (g->state() == GState::Gdead) {
        // v8::Global::~G() calls Reset() which is NOT thread-safe.
        // Enqueue for deletion on main thread (drained in OnAsync).
        std::lock_guard<std::mutex> lock(rt->dead_mutex_);
        rt->dead_queue_.push_back(g);
      }

      // Send uv_async after each goroutine so the main thread can drain the
      // dead-G queue and check the Go-style GC trigger mid-batch.
      // uv_async_send is coalescent: N sends → at most 1 OnAsync per event-loop
      // iteration, so there is no amplification. Cost is one eventfd write (~ns).
      uv_async_send(&rt->async_handle_);
    }  // end inner while
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

  // uv_check: resets main-thread stack limit after each I/O poll.
  uv_check_init(loop_, &check_handle_);
  check_handle_.data = this;
  uv_check_start(&check_handle_, OnCheck);
  uv_unref(reinterpret_cast<uv_handle_t*>(&check_handle_));
  check_active_ = true;

  // uv_async: workers use this to wake the event loop (dead-G cleanup, print).
  uv_async_init(loop_, &async_handle_, OnAsync);
  async_handle_.data = this;
  uv_unref(reinterpret_cast<uv_handle_t*>(&async_handle_));
  async_init_ = true;

  // Initialise the GC trigger with a sensible default (64 MB) so the first
  // OnAsync call never fires LowMemoryNotification unconditionally.
  // The trigger is recalculated after every GC based on actual live heap size.
  gc_trigger_heap_.store(64 * 1024 * 1024, std::memory_order_relaxed);

  // GOMAXPROCS = number of ADDITIONAL worker M-threads.
  // Main thread (M0) NEVER executes goroutines — always uses worker threads.
  // Minimum is always 1 worker even if GOMAXPROCS=1.
  uint32_t num_workers = (num_threads > 0) ? num_threads : 1;
  for (uint32_t i = 0; i < num_workers; i++) {
    M* m = new M(i);  // 0-based: matches local_queues_[i]
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

  // Drain any remaining dead goroutines.
  {
    std::lock_guard<std::mutex> lock(dead_mutex_);
    for (G* g : dead_queue_) delete g;
    dead_queue_.clear();
  }

  if (check_active_) {
    uv_check_stop(&check_handle_);
    check_active_ = false;
  }
  if (async_init_) {
    uv_close(reinterpret_cast<uv_handle_t*>(&async_handle_), nullptr);
    async_init_ = false;
  }

  if (sem_init_) {
    uv_sem_destroy(&goroutine_sem_);
    sem_init_ = false;
  }


  if (scheduler_) scheduler_->Shutdown();
}

void Runtime::OnCheck(uv_check_t* handle) {
  // Main thread does NOT execute goroutines — workers do.
  // OnCheck is kept to reset the stack limit after I/O poll.
  Runtime* rt = static_cast<Runtime*>(handle->data);
  uintptr_t sp = reinterpret_cast<uintptr_t>(&sp);
  rt->isolate_->SetStackLimit(sp - (900 * 1024));
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

  // Go-style adaptive GC: trigger when heap has grown by kGCGrowthFactor
  // since the last GC, then update the trigger for the next cycle.
  // This mirrors Go's nextGC = live_after_gc * (1 + GOGC/100):
  //   - heavy allocators → trigger fires sooner (heap grows fast)
  //   - light workloads  → trigger fires rarely (heap stays small)
  if (!dead.empty()) {
    v8::HeapStatistics hs;
    rt->isolate_->GetHeapStatistics(&hs);
    size_t used = hs.used_heap_size();
    size_t trigger = rt->gc_trigger_heap_.load(std::memory_order_relaxed);
    if (used >= trigger) {
      // MemoryPressureNotification(kModerate) starts incremental marking —
      // safe to call while goroutine workers are running JS (no STW required).
      // LowMemoryNotification() is intentionally NOT used here: it calls
      // CollectAllAvailableGarbage (multiple synchronous full GCs) which can
      // trigger OOM exceptions in running goroutines and crash UnwindAndFindHandler
      // on fiber stacks.
      rt->isolate_->MemoryPressureNotification(v8::MemoryPressureLevel::kModerate);
      // Recalculate trigger after notifying based on current live heap size.
      rt->isolate_->GetHeapStatistics(&hs);
      size_t live = hs.used_heap_size();
      rt->gc_trigger_heap_.store(
          static_cast<size_t>(live * kGCGrowthFactor),
          std::memory_order_relaxed);
    }
  }

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

void Runtime::NotifyGoroutineAvailable() {
  if (!sem_init_) return;
  // Always post — avoid startup race where workers haven't reached
  // uv_sem_wait yet (sleeping_workers_ == 0) but goroutines are already
  // queued. Spurious extra wakeups are harmless: workers drain the queue
  // in the inner loop then go back to sleep immediately.
  uv_sem_post(&goroutine_sem_);
}

}  // namespace goroutine
}  // namespace node
