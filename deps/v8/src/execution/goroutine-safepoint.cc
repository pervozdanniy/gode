// Goroutine M-thread GC safepoint registry implementation.
// See goroutine-safepoint.h for design overview.

#include "src/execution/goroutine-safepoint.h"

#include <cassert>
#include <cstdio>

namespace v8 {
namespace internal {

// static
GoroutineSafepointRegistry& GoroutineSafepointRegistry::Get() {
  static GoroutineSafepointRegistry instance;
  return instance;
}

ExternalSafepointEntry* GoroutineSafepointRegistry::Register() {
  ExternalSafepointEntry* entry = new ExternalSafepointEntry();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(entry);
  }
  return entry;
}

void GoroutineSafepointRegistry::Unregister(ExternalSafepointEntry* entry) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // If safepoint is active, decrement stopped count for this entry
    // since it won't be waking up.
    if (safepoint_requested_) {
      auto state = entry->Load();
      if (state == ExternalThreadState::kInSafepoint) {
        // Thread was stopped for GC, now unregistering — count it as done.
        stopped_count_++;
        cv_stopped_.notify_all();
      }
    }
    entries_.erase(std::remove(entries_.begin(), entries_.end(), entry),
                   entries_.end());
  }
  delete entry;
}

void GoroutineSafepointRegistry::SetRunning(ExternalSafepointEntry* entry) {
  // If safepoint is already requested, go straight to kInSafepoint.
  std::unique_lock<std::mutex> lock(mutex_);
  if (safepoint_requested_) {
    // Don't enter running state — park immediately.
    entry->state.store(static_cast<int>(ExternalThreadState::kInSafepoint),
                       std::memory_order_release);
    stopped_count_++;
    cv_stopped_.notify_all();
    // Wait for GC to finish.
    cv_resumed_.wait(lock, [this] { return !safepoint_requested_; });
    entry->SetRunning();
  } else {
    entry->SetRunning();
  }
}

void GoroutineSafepointRegistry::SetParked(ExternalSafepointEntry* entry) {
  entry->SetParked();
}

void GoroutineSafepointRegistry::CheckSafepoint(
    ExternalSafepointEntry* entry) {
  // Fast path: no safepoint requested (common case).
  if (entry->Load() != ExternalThreadState::kRunning) return;

  std::unique_lock<std::mutex> lock(mutex_);
  if (!safepoint_requested_) return;

  // Transition to kInSafepoint and notify GC.
  entry->state.store(static_cast<int>(ExternalThreadState::kInSafepoint),
                     std::memory_order_release);
  stopped_count_++;
  cv_stopped_.notify_all();

  // Wait for GC to finish.
  cv_resumed_.wait(lock, [this] { return !safepoint_requested_; });

  // Back to running.
  entry->SetRunning();
}

int GoroutineSafepointRegistry::RequestSafepoint() {
  std::lock_guard<std::mutex> lock(mutex_);
  safepoint_requested_ = true;
  stopped_count_ = 0;

  // Count threads currently in kRunning state.
  // They will call CheckSafepoint() soon and stop.
  // Threads in kParked are already safe.
  int running_count = 0;
  for (ExternalSafepointEntry* entry : entries_) {
    if (entry->Load() == ExternalThreadState::kRunning) {
      running_count++;
    }
  }
  return running_count;
}

void GoroutineSafepointRegistry::WaitForAll(int running_count) {
  if (running_count == 0) return;

  std::unique_lock<std::mutex> lock(mutex_);
  cv_stopped_.wait(lock, [this, running_count] {
    return stopped_count_ >= running_count;
  });
}

void GoroutineSafepointRegistry::Resume() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    safepoint_requested_ = false;
    stopped_count_ = 0;
  }
  cv_resumed_.notify_all();
}

}  // namespace internal
}  // namespace v8

// ---- C API ----

extern "C" {

void* v8_goroutine_safepoint_register() {
  return v8::internal::GoroutineSafepointRegistry::Get().Register();
}

void v8_goroutine_safepoint_unregister(void* handle) {
  if (!handle) return;
  auto* entry = static_cast<v8::internal::ExternalSafepointEntry*>(handle);
  v8::internal::GoroutineSafepointRegistry::Get().Unregister(entry);
}

void v8_goroutine_safepoint_set_running(void* handle) {
  if (!handle) return;
  auto* entry = static_cast<v8::internal::ExternalSafepointEntry*>(handle);
  v8::internal::GoroutineSafepointRegistry::Get().SetRunning(entry);
}

void v8_goroutine_safepoint_set_parked(void* handle) {
  if (!handle) return;
  auto* entry = static_cast<v8::internal::ExternalSafepointEntry*>(handle);
  v8::internal::GoroutineSafepointRegistry::Get().SetParked(entry);
}

void v8_goroutine_safepoint_check(void* handle) {
  if (!handle) return;
  auto* entry = static_cast<v8::internal::ExternalSafepointEntry*>(handle);
  v8::internal::GoroutineSafepointRegistry::Get().CheckSafepoint(entry);
}

}  // extern "C"
