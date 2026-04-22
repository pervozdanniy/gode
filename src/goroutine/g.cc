#include "g.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "stack.h"
#include "context.h"


extern "C" void* v8_goroutine_gc_alloc();
extern "C" void  v8_goroutine_gc_free(void* state);
// Deep-compile all SFIs in the goroutine's script on the calling (main) thread
// so worker M threads never trigger Runtime_CompileLazy concurrently.
extern "C" void v8_goroutine_deep_compile_script(v8::Isolate* isolate,
                                                  v8::Local<v8::Function> fn);
// Set per-M StackGuard jslimit from the current stack pointer.
// Must be called from INSIDE the goroutine fiber so that &sp is on the fiber
// stack — analogous to OnCheck() resetting the main-thread stack limit.
extern "C" void v8_goroutine_set_stack_limit(uintptr_t limit);

namespace node {
namespace goroutine {

std::atomic<uint64_t> G::next_goid_{1};

G::G(v8::Isolate* isolate,
     v8::Local<v8::Function> entry_func,
     v8::Local<v8::Array> args)
    : goid_(next_goid_.fetch_add(1, std::memory_order_relaxed)),
      stack_(StackAllocator::GetInstance()->Alloc()),
      state_(GState::Gidle),
      entry_func_(),
      args_(),
      stack_context_(nullptr) {

  if (!entry_func.IsEmpty()) {
    // Deep pre-compile: compile ALL SharedFunctionInfos in the goroutine's
    // script on the calling (main) thread before the goroutine is dispatched
    // to a worker M thread. This guarantees Runtime_CompileLazy is never
    // triggered from worker threads, making lock-free concurrent interpretation
    // safe. The compilation is idempotent — already-compiled SFIs are skipped.
    v8_goroutine_deep_compile_script(isolate, entry_func);
    entry_func_.Reset(isolate, entry_func);
  }
  if (!args.IsEmpty()) {
    args_.Reset(isolate, args);
  }

  stack_context_ = InitContext(this, stack_->base());
  gc_state_ = v8_goroutine_gc_alloc();
}

G::~G() {
  v8_goroutine_gc_free(gc_state_);
  gc_state_ = nullptr;
  if (stack_) {
    StackAllocator::GetInstance()->Free(stack_);
    stack_ = nullptr;
  }

  entry_func_.Reset();
  args_.Reset();
}

void G::SetState(GState new_state) {
  state_.store(new_state, std::memory_order_release);
}

void G::Execute(v8::Isolate* isolate) {
  // Stack limit is already set correctly by RunG() before jumping here:
  //   v8_goroutine_set_stack_limit(g_stack_bottom + 8192)
  // Do NOT override it with sp - 900KB — the goroutine stack is only 64KB,
  // so sp - 900KB would disable V8's stack overflow detection and let the
  // goroutine silently overflow into adjacent heap memory.

  // No entry function → g0 (scheduler goroutine), nothing to run.
  if (entry_func_.IsEmpty()) {
    SetState(GState::Gdead);
    return;
  }

  v8::HandleScope handle_scope(isolate);

  // Get the current V8 context.  On the main thread this is always valid.
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (context.IsEmpty()) {
    context = isolate->GetEnteredOrMicrotaskContext();
  }
  if (context.IsEmpty()) {
    Panic("no V8 context available");
    return;
  }

  v8::Context::Scope context_scope(context);

  v8::Local<v8::Function> func = entry_func_.Get(isolate);

  v8::Local<v8::Array> args_array = args_.Get(isolate);


  uint32_t argc = args_array.IsEmpty() ? 0 : args_array->Length();
  std::vector<v8::Local<v8::Value>> argv(argc);
  for (uint32_t i = 0; i < argc; i++) {
    if (!args_array->Get(context, i).ToLocal(&argv[i])) {
      argv[i] = v8::Undefined(isolate);
    }
  }

  v8::TryCatch try_catch(isolate);

  v8::MaybeLocal<v8::Value> result = func->Call(
      context,
      v8::Undefined(isolate),
      argc,
      argc > 0 ? argv.data() : nullptr);


  (void)result;

  if (try_catch.HasCaught()) {
    v8::String::Utf8Value exception(isolate, try_catch.Exception());
    const char* msg = *exception ? *exception : "(unknown error)";

    fprintf(stderr, "goroutine %llu panic: %s\n",
            static_cast<unsigned long long>(goid_), msg);

    v8::Local<v8::Value> stack_val;
    if (try_catch.StackTrace(context).ToLocal(&stack_val)) {
      v8::String::Utf8Value stack(isolate, stack_val);
      if (*stack) fprintf(stderr, "%s\n", *stack);
    }
    // Like Go: unrecovered panic in a goroutine crashes the process.
    std::abort();
  }

  SetState(GState::Gdead);
}

void G::SaveContext(void* ctx) {
  stack_context_ = ctx;
}

void G::RestoreContext() {
  // Handled by Boost.Context in Phase 1.5.
}

[[noreturn]] void G::Panic(const char* msg) {
  fprintf(stderr, "\ngoroutine %llu panic: %s\n",
          static_cast<unsigned long long>(goid_), msg);
  std::abort();
}

}  // namespace goroutine
}  // namespace node


