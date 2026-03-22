#ifndef SRC_GOROUTINE_G_H_
#define SRC_GOROUTINE_G_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
#include <cstdint>
#include "v8.h"
#include "stack.h"

namespace node {
namespace goroutine {

// Goroutine states (matching Go runtime)
enum class GState : int32_t {
  Gidle = 0,      // Just allocated
  Grunnable = 1,  // Ready to run
  Grunning = 2,   // Currently executing
  Gwaiting = 3,   // Blocked (IO, channel, sync)
  Gdead = 4,      // Execution finished
};

// Represents a single goroutine (G in Go terminology)
class G {
 public:
  G(v8::Isolate* isolate,
    v8::Local<v8::Function> entry_func,
    v8::Local<v8::Array> args);
  ~G();

  // Getters
  inline uint64_t goid() const { return goid_; }
  inline GState state() const { return state_.load(std::memory_order_acquire); }
  inline Stack* stack() { return stack_; }
  inline void* stack_context() const { return stack_context_; }

  // State transitions
  void SetState(GState new_state);

  // Execution
  void Execute(v8::Isolate* isolate);
  void SaveContext(void* ctx);
  void RestoreContext();

  // Panic handling
  [[noreturn]] void Panic(const char* msg);

  // Linking for queues
  G* waitlink = nullptr;

  // Delete copy/move constructors
  G(const G&) = delete;
  G& operator=(const G&) = delete;

 private:
  static std::atomic<uint64_t> next_goid_;

  uint64_t goid_;
  Stack* stack_;
  std::atomic<GState> state_;

  // V8 function and arguments
  v8::Global<v8::Function> entry_func_;
  v8::Global<v8::Array> args_;

  // Context for stack switching
  void* stack_context_;

  // Saved V8 HandleScopeData for context-switch isolation.
  alignas(8) char saved_hsd_[64];
  bool has_saved_hsd_ = false;

 public:
  void* saved_hsd_buf() { return saved_hsd_; }
  const void* saved_hsd_buf() const { return saved_hsd_; }
  bool has_saved_hsd() const { return has_saved_hsd_; }
  void mark_saved_hsd() { has_saved_hsd_ = true; }
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_G_H_

