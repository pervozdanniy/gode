// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/goroutine-shape-seqlock.h"

#include <atomic>
#include <cstdint>

// Global sequence counter.
//   even  = no transition in flight (stable)
//   odd   = transition in progress  (writer holds)
//
// Stored in its own cache line to avoid false sharing with other hot data.
alignas(64) static std::atomic<uint32_t> g_shape_seq{0};

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

}  // extern "C"
