// Goroutine per-P V8 state implementation.
// See goroutine-thread-state.h for architecture overview.

#include "src/execution/goroutine-thread-state.h"

#include "src/execution/isolate.h"
#include "src/execution/isolate-data.h"
#include "src/execution/stack-guard.h"
#include "src/heap/linear-allocation-area.h"
#include "src/objects/contexts-inl.h"
#include "src/api/api.h"
#include "src/api/api-inl.h"
#include "src/codegen/compiler.h"
#include "src/objects/js-function-inl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace v8 {
namespace internal {

// Thread-local: pointer to the ACTIVE P's state on this M-thread.
static thread_local GoroutinePState* g_active_p_state = nullptr;

// ---- Accessors (called from patched V8 code) ----

IsolateData* GoroutineThreadState::GetIsolateData() {
  return g_active_p_state ? g_active_p_state->isolate_data : nullptr;
}

HandleScopeImplementer* GoroutineThreadState::GetHandleScopeImplementer() {
  return g_active_p_state ? g_active_p_state->handle_scope_impl : nullptr;
}

bool GoroutineThreadState::IsActive() {
  return g_active_p_state != nullptr;
}

// ---- P-state lifecycle ----

GoroutinePState* GoroutineThreadState::CreatePState(Isolate* isolate) {
  // Called on main thread. isolate->isolate_data() returns the real main data.
  IsolateData* main_data = isolate->isolate_data();
  Tagged<Context> main_ctx = main_data->thread_local_top().context_;

  fprintf(stderr, "[PState] Creating per-P V8 state (IsolateData size=%zu)\n",
          sizeof(IsolateData));
  fflush(stderr);

  // Allocate per-P IsolateData (aligned).
  constexpr size_t kAlign = alignof(IsolateData);
  constexpr size_t kSize = (sizeof(IsolateData) + kAlign - 1) & ~(kAlign - 1);
  void* raw = std::aligned_alloc(kAlign, kSize);
  if (!raw) {
    fprintf(stderr, "[PState] FATAL: aligned_alloc failed\n");
    std::abort();
  }

  std::memcpy(raw, main_data, sizeof(IsolateData));
  IsolateData* p_data = static_cast<IsolateData*>(raw);

  // Fix up thread-specific fields.
  p_data->thread_local_top().Clear();
  p_data->thread_local_top().Initialize(isolate);
  p_data->thread_local_top().context_ = main_ctx;

  p_data->handle_scope_data_.Initialize();

  // Allocation LABs: reset to force slow-path for now.
  // TODO: Per-P LAB chunks from Heap (Go mcache equivalent).
  p_data->new_allocation_info_.Reset(kNullAddress, kNullAddress);
  p_data->old_allocation_info_.Reset(kNullAddress, kNullAddress);

  // HandleScopeImplementer: per-P instance.
  HandleScopeImplementer* hsi = new HandleScopeImplementer(isolate);

  if (!main_ctx.is_null() && main_ctx.ptr() != kNullAddress) {
    Tagged<NativeContext> native_ctx = main_ctx->native_context();
    hsi->EnterContext(native_ctx);
  }

  GoroutinePState* state = new GoroutinePState{p_data, hsi};

  fprintf(stderr, "[PState] Created: IsolateData=%p, main=%p\n",
          static_cast<void*>(p_data), static_cast<void*>(main_data));
  fflush(stderr);

  return state;
}

void GoroutineThreadState::DestroyPState(GoroutinePState* state) {
  if (!state) return;
  fprintf(stderr, "[PState] Destroying per-P state (IsolateData=%p)\n",
          static_cast<void*>(state->isolate_data));
  fflush(stderr);

  std::free(state->isolate_data);
  delete state->handle_scope_impl;
  delete state;
}

void GoroutineThreadState::ActivatePState(GoroutinePState* state) {
  g_active_p_state = state;
  v8_goroutine_thread = true;

  // Update StackGuard for THIS M-thread's stack.
  uintptr_t stack_here = reinterpret_cast<uintptr_t>(&stack_here);
  uintptr_t stack_limit = stack_here - (2 * 1024 * 1024) + (64 * 1024);
  StackGuard* sg = state->isolate_data->stack_guard();
  sg->thread_local_.real_jslimit_ = stack_limit;
  sg->thread_local_.set_jslimit(stack_limit);
#ifdef USE_SIMULATOR
  sg->thread_local_.real_climit_ = stack_limit;
  sg->thread_local_.set_climit(stack_limit);
#endif

  fprintf(stderr, "[PState] Activated on M-thread (IsolateData=%p)\n",
          static_cast<void*>(state->isolate_data));
  fflush(stderr);
}

void GoroutineThreadState::DeactivatePState() {
  if (g_active_p_state) {
    fprintf(stderr, "[PState] Deactivated on M-thread\n");
    fflush(stderr);
  }
  g_active_p_state = nullptr;
  v8_goroutine_thread = false;
}

// ---- Per-G HandleScopeData helpers ----

void GoroutineThreadState::SaveHSD(Isolate* isolate, void* buf) {
  std::memcpy(buf, &isolate->isolate_data()->handle_scope_data_,
              sizeof(HandleScopeData));
}

void GoroutineThreadState::RestoreHSD(Isolate* isolate, const void* buf) {
  std::memcpy(&isolate->isolate_data()->handle_scope_data_, buf,
              sizeof(HandleScopeData));
}

void GoroutineThreadState::ForceNewHandleBlock(Isolate* isolate) {
  auto& hsd = isolate->isolate_data()->handle_scope_data_;
  hsd.next = hsd.limit;
}

}  // namespace internal
}  // namespace v8

// ---- C-linkage wrappers (thin shims for Node.js code) ----

extern "C" {

void* v8_goroutine_p_state_create(v8::Isolate* isolate) {
  auto* i_isolate = reinterpret_cast<v8::internal::Isolate*>(isolate);
  return static_cast<void*>(
      v8::internal::GoroutineThreadState::CreatePState(i_isolate));
}

void v8_goroutine_p_state_activate(void* p_state) {
  v8::internal::GoroutineThreadState::ActivatePState(
      static_cast<v8::internal::GoroutinePState*>(p_state));
}

void v8_goroutine_p_state_deactivate() {
  v8::internal::GoroutineThreadState::DeactivatePState();
}

void v8_goroutine_p_state_destroy(void* p_state) {
  v8::internal::GoroutineThreadState::DestroyPState(
      static_cast<v8::internal::GoroutinePState*>(p_state));
}

bool v8_goroutine_ensure_compiled(v8::Isolate* isolate,
                                  v8::Local<v8::Function> function) {
  using namespace v8::internal;

  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);

  Handle<JSReceiver> receiver = v8::Utils::OpenHandle(*function);
  if (!IsJSFunction(*receiver)) return true;

  Handle<JSFunction> js_func = Cast<JSFunction>(receiver);
  if (js_func->is_compiled(i_isolate)) return true;

  IsCompiledScope is_compiled_scope;
  return Compiler::Compile(i_isolate, js_func,
                           Compiler::CLEAR_EXCEPTION,
                           &is_compiled_scope);
}

void v8_goroutine_save_hsd(v8::Isolate* isolate, void* buf) {
  v8::internal::GoroutineThreadState::SaveHSD(
      reinterpret_cast<v8::internal::Isolate*>(isolate), buf);
}

void v8_goroutine_restore_hsd(v8::Isolate* isolate, const void* buf) {
  v8::internal::GoroutineThreadState::RestoreHSD(
      reinterpret_cast<v8::internal::Isolate*>(isolate), buf);
}

void v8_goroutine_force_new_handle_block(v8::Isolate* isolate) {
  v8::internal::GoroutineThreadState::ForceNewHandleBlock(
      reinterpret_cast<v8::internal::Isolate*>(isolate));
}

}  // extern "C"

