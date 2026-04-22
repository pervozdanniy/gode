#ifndef SRC_GOROUTINE_STACK_H_
#define SRC_GOROUTINE_STACK_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "node_mutex.h"

namespace node {
namespace goroutine {

// Stack configuration (matching Go's defaults)
constexpr size_t kStackGuardSize = 4096;      // 4KB guard page
#ifdef __SANITIZE_ADDRESS__
constexpr size_t kStackSize = 262144;        // 256KB with ASAN (red zones bloat frames)
#else
constexpr size_t kStackSize = 65536;          // 64KB usable stack
#endif
constexpr size_t kTotalStackSize = kStackGuardSize + kStackSize;

// Represents a goroutine stack
class Stack {
 public:
  Stack();
  ~Stack();

  // Getters
  inline void* base() const { return base_; }
  inline void* top() const { return top_; }
  inline size_t size() const { return kStackSize; }

  // Check if pointer is within stack bounds
  bool Contains(void* ptr) const;

  Stack(const Stack&) = delete;
  Stack& operator=(const Stack&) = delete;

 private:
  void* base_;  // Bottom of stack (high address)
  void* top_;   // Top of stack (low address)
  void* memory_; // Actual allocated memory for cleanup
};

// Pool-based stack allocator for reuse
class StackAllocator {
 public:
  static StackAllocator* GetInstance();

  Stack* Alloc();
  void Free(Stack* stack);

  // Stats
  size_t allocated_count() const { return allocated_count_; }
  size_t pool_size() const;

 private:
  StackAllocator() = default;
  ~StackAllocator() = default;

  StackAllocator(const StackAllocator&) = delete;
  StackAllocator& operator=(const StackAllocator&) = delete;

  Mutex mutex_;
  std::vector<Stack*> pool_;
  size_t allocated_count_ = 0;
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_STACK_H_

