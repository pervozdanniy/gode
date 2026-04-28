#include "stack.h"
#include "util.h"  // For CHECK macros
#include <cstring>
#include <cstdlib>  // For abort()

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace node {
namespace goroutine {

// Stack implementation
Stack::Stack() : base_(nullptr), top_(nullptr), memory_(nullptr) {
#ifdef _WIN32
  // Windows: VirtualAlloc with guard page
  memory_ = VirtualAlloc(nullptr,
                         kTotalStackSize,
                         MEM_COMMIT | MEM_RESERVE,
                         PAGE_READWRITE);
  CHECK_NOT_NULL(memory_);  // Abort if allocation fails

  // Set guard page
  DWORD old_protect;
  VirtualProtect(memory_, kStackGuardSize, PAGE_NOACCESS, &old_protect);

  top_ = memory_;
  base_ = static_cast<char*>(memory_) + kTotalStackSize;
#else
  // Unix: mmap with guard page
  memory_ = mmap(nullptr,
                 kTotalStackSize,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 -1,
                 0);

  CHECK_NE(memory_, MAP_FAILED);  // Abort if allocation fails

  // Set guard page (no access)
  mprotect(memory_, kStackGuardSize, PROT_NONE);

  top_ = static_cast<char*>(memory_) + kStackGuardSize;
  base_ = static_cast<char*>(memory_) + kTotalStackSize;
#endif
}

Stack::~Stack() {
  if (memory_) {
#ifdef _WIN32
    VirtualFree(memory_, 0, MEM_RELEASE);
#else
    munmap(memory_, kTotalStackSize);
#endif
    memory_ = nullptr;
  }
}

bool Stack::Contains(void* ptr) const {
  return ptr >= top_ && ptr <= base_;
}

// StackAllocator implementation
StackAllocator* StackAllocator::GetInstance() {
  static StackAllocator instance;
  return &instance;
}

// Per-thread stack pool — zero-contention fast path for Alloc/Free.
static thread_local std::vector<Stack*> tls_stack_pool;

Stack* StackAllocator::Alloc() {
  // Fast path: TLS pool (no mutex, no contention).
  if (!tls_stack_pool.empty()) {
    Stack* stack = tls_stack_pool.back();
    tls_stack_pool.pop_back();
    return stack;
  }

  // Slow path: global pool under mutex.
  {
    Mutex::ScopedLock lock(mutex_);
    if (!pool_.empty()) {
      Stack* stack = pool_.back();
      pool_.pop_back();
      return stack;
    }
  }

  // Allocate new stack (no lock held — mmap is thread-safe).
  allocated_count_++;
  return new Stack();
}

void StackAllocator::Free(Stack* stack) {
  if (!stack) return;

  // Fast path: TLS pool.
  if (tls_stack_pool.size() < kMaxPoolSize) {
    tls_stack_pool.push_back(stack);
    return;
  }

  // TLS pool full — try global pool.
  Mutex::ScopedLock lock(mutex_);
  if (pool_.size() < kMaxPoolSize) {
    pool_.push_back(stack);
  } else {
    delete stack;
  }
}

size_t StackAllocator::pool_size() const {
  Mutex::ScopedLock lock(const_cast<Mutex&>(mutex_));
  return pool_.size();
}

}  // namespace goroutine
}  // namespace node


