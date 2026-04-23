// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/goroutine-shape-seqlock.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

// Access to LocalHeap::Current() and LocalHeap::Safepoint() so we can
// respond to GC safepoint requests while waiting for the transition mutex.
// Without this, a goroutine M-thread blocked in pthread_mutex_lock() keeps
// its LocalHeap in Running state and cannot park → GC deadlock:
//   M1 holds mutex → parks for GC → GC waits for M2 → M2 blocked in
//   pthread_mutex_lock() → can't park → deadlock.
#include "src/heap/local-heap.h"
#include "src/heap/local-heap-inl.h"

// Global sequence counter.
//   even  = no transition in flight (stable)
//   odd   = transition in progress  (writer holds)
//
// Stored in its own cache line to avoid false sharing with other hot data.
alignas(64) static std::atomic<uint32_t> g_shape_seq{0};

// Global mutex for Map::TransitionToDataProperty.
// Prevents concurrent goroutine workers from simultaneously creating duplicate
// Map transitions and inserting them into the parent Map's transition array.
// Both goroutine threads and the main thread take this mutex — so all Map
// transition creations are serialized globally. Cost is negligible: transitions
// only happen once per new shape, after which the cached transition is followed
// (read-only, no mutex needed).
static std::mutex g_map_transition_mutex;

// Pause instruction — avoids saturating the store port during spin.
// Defined for x86/x64; on ARM64 we use yield.
static inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ volatile("yield" ::: "memory");
#else
  // Fallback: compiler barrier is enough to prevent loop hoisting.
  __asm__ volatile("" ::: "memory");
#endif
}

extern "C" {

void v8_goroutine_shape_seqlock_begin() {
  // Increment to odd: signals "write in flight".
  // release: all preceding stores (SetProperties, field copies) are visible
  // to any reader that sees this increment.
  g_shape_seq.fetch_add(1, std::memory_order_release);
}

void v8_goroutine_shape_seqlock_end() {
  // Increment to even: signals "write done".
  // release: set_map(kReleaseStore) already happened; this is the final
  // notification that the entire transition (including map update) is done.
  g_shape_seq.fetch_add(1, std::memory_order_release);
}

void v8_goroutine_shape_seqlock_wait() {
  // Fast path: seq is even -> no transition in flight, return immediately.
  // acquire: pairs with the release in seqlock_end, ensuring the goroutine
  // sees all stores made during the transition (SetProperties + set_map).
  uint32_t seq = g_shape_seq.load(std::memory_order_acquire);
  if ((seq & 1u) == 0) return;

  // Slow path: spin until transition completes.
  do {
    cpu_relax();
    seq = g_shape_seq.load(std::memory_order_acquire);
  } while (seq & 1u);
}

void v8_goroutine_map_transition_lock() {
  // Fast path: no contention — grab immediately.
  if (g_map_transition_mutex.try_lock()) return;

  // Slow path: need to wait.
  //
  // DEADLOCK HAZARD: if we call the blocking mutex.lock() while our
  // LocalHeap is in Running state, a GC safepoint can fire and wait for
  // us to park — but we're stuck in the kernel and can never respond.
  //   Fix: spin with try_lock() + Safepoint() so GC can always proceed.
  //
  // For the main thread (LocalHeap::Current() == nullptr) there is no
  // goroutine safepoint mechanism; use a plain blocking lock instead.
  v8::internal::LocalHeap* lh = v8::internal::LocalHeap::Current();
  if (lh != nullptr) {
    // Goroutine M-thread path: spin, yielding safepoints on each iteration.
    while (!g_map_transition_mutex.try_lock()) {
      // If GC has requested a safepoint, park immediately and resume after.
      // If no GC is pending this is a cheap atomic load + return.
      lh->Safepoint();
      // Brief yield so the thread holding the mutex can make progress.
      std::this_thread::yield();
    }
  } else {
    // Main thread: no LocalHeap → plain blocking lock is safe.
    g_map_transition_mutex.lock();
  }
}

void v8_goroutine_map_transition_unlock() {
  g_map_transition_mutex.unlock();
}

}  // extern "C"
