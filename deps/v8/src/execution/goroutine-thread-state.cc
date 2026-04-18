// Goroutine per-M V8 state implementation.
// See goroutine-thread-state.h for architecture overview.
// GM model: V8 state belongs to M (thread) directly, no P abstraction.

#include "src/execution/goroutine-thread-state.h"

#include "src/execution/goroutine-flag.h"
#include "src/execution/isolate.h"
#include "src/execution/isolate-data.h"
#include "src/execution/stack-guard.h"
#include "src/heap/linear-allocation-area.h"
#include "src/heap/local-heap.h"
#include "src/heap/local-heap-inl.h"
#include "src/heap/main-allocator.h"
#include "src/heap/main-allocator-inl.h"
#include "src/objects/contexts-inl.h"
#include "src/api/api.h"
#include "src/api/api-inl.h"
#include "src/codegen/compiler.h"
#include "src/objects/js-function-inl.h"
#include "src/objects/shared-function-info-inl.h"
#include "src/objects/script.h"
#include <cstdlib>
#include <cstring>

namespace v8 {
namespace internal {

// Thread-local: pointer to the ACTIVE M's V8 state on this thread.
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

// ---- Per-M state lifecycle ----

GoroutinePState* GoroutineThreadState::CreatePState(Isolate* isolate) {
  // Called on main thread. isolate->isolate_data() returns the real main data.
  IsolateData* main_data = isolate->isolate_data();
  Tagged<Context> main_ctx = main_data->thread_local_top().context_;

  // Allocate per-M IsolateData (aligned).
  constexpr size_t kAlign = alignof(IsolateData);
  constexpr size_t kSize = (sizeof(IsolateData) + kAlign - 1) & ~(kAlign - 1);
  void* raw = std::aligned_alloc(kAlign, kSize);
  if (!raw) {
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
  // TODO: Per-M LAB chunks from Heap.
  p_data->new_allocation_info_.Reset(kNullAddress, kNullAddress);
  p_data->old_allocation_info_.Reset(kNullAddress, kNullAddress);

  // HandleScopeImplementer: per-M instance.
  HandleScopeImplementer* hsi = new HandleScopeImplementer(isolate);

  if (!main_ctx.is_null() && main_ctx.ptr() != kNullAddress) {
    Tagged<NativeContext> native_ctx = main_ctx->native_context();
    hsi->EnterContext(native_ctx);
  }

  return new GoroutinePState{p_data, hsi};
}

void GoroutineThreadState::DestroyPState(GoroutinePState* state) {
  if (!state) return;
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
}

void GoroutineThreadState::DeactivatePState() {
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

void GoroutineThreadState::LabSyncBeforeRun() {
  if (!g_active_p_state) return;
  LocalHeap* lh = LocalHeap::Current();
  if (!lh) return;
  // Steal LocalHeap's Old Space LAB into per-M IsolateData so JIT
  // bump-pointer fast path (r13-based) works without hitting slow path.
  MainAllocator* old_alloc = lh->allocator()->old_space_allocator();
  Address top   = *old_alloc->allocation_top_address();
  Address limit = *old_alloc->allocation_limit_address();
  g_active_p_state->isolate_data->old_allocation_info_.Reset(top, limit);
}

void GoroutineThreadState::LabSyncAfterRun() {
  if (!g_active_p_state) return;
  LocalHeap* lh = LocalHeap::Current();
  if (!lh) return;
  // Flush the JIT-updated top back to LocalHeap so it tracks consumption.
  // IMPORTANT: only write back if per-M IsolateData top is within the LAB
  // range AND is greater than LocalHeap's current top. If Ignition never
  // used the LAB fast path (top == 0 because LAB was empty), writing 0
  // would corrupt LocalHeap's allocator (top=0, limit=valid → next alloc
  // succeeds with address 0 → SIGSEGV).
  Address pstate_top = g_active_p_state->isolate_data->old_allocation_info_.top();
  MainAllocator* old_alloc = lh->allocator()->old_space_allocator();
  Address lh_top   = *old_alloc->allocation_top_address();
  Address lh_limit = *old_alloc->allocation_limit_address();
  if (pstate_top > lh_top && pstate_top <= lh_limit) {
    *old_alloc->allocation_top_address() = pstate_top;
  }
  // Zero out IsolateData LAB so a stale limit can't be used after resume.
  g_active_p_state->isolate_data->old_allocation_info_.Reset(
      kNullAddress, kNullAddress);
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

void v8_goroutine_p_state_activate_with_isolate(void* p_state, v8::Isolate* isolate) {
  v8_goroutine_real_isolate = static_cast<void*>(isolate);
  v8::internal::GoroutineThreadState::ActivatePState(
      static_cast<v8::internal::GoroutinePState*>(p_state));
}

void v8_goroutine_p_state_deactivate() {
  v8::internal::GoroutineThreadState::DeactivatePState();
  v8_goroutine_real_isolate = nullptr;
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

void v8_goroutine_lab_sync_before_run() {
  v8::internal::GoroutineThreadState::LabSyncBeforeRun();
}

void v8_goroutine_lab_sync_after_run() {
  v8::internal::GoroutineThreadState::LabSyncAfterRun();
}

// Deep-compile all SharedFunctionInfos in the same script as |fn|.
// Must be called on the main thread before dispatching any goroutine worker.
// After this call, Runtime_CompileLazy will never be triggered from goroutines.
void v8_goroutine_deep_compile_script(v8::Isolate* isolate,
                                      v8::Local<v8::Function> fn) {
  using namespace v8::internal;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);

  Handle<JSFunction> i_func =
      Cast<JSFunction>(v8::Utils::OpenHandle(*fn));

  // Step 1: compile the entry function itself.
  {
    IsCompiledScope scope;
    Compiler::Compile(i_isolate, i_func,
                      Compiler::CLEAR_EXCEPTION, &scope);
  }

  // Step 2: iterate every SFI in the same script and compile lazily.
  Tagged<HeapObject> script_obj = i_func->shared()->script();
  if (!IsScript(script_obj)) return;

  Tagged<Script> script = Cast<Script>(script_obj);
  SharedFunctionInfo::ScriptIterator iter(i_isolate, script);
  for (Tagged<SharedFunctionInfo> sfi = iter.Next();
       !sfi.is_null(); sfi = iter.Next()) {
    if (!sfi->is_compiled()) {
      Handle<SharedFunctionInfo> h(sfi, i_isolate);
      IsCompiledScope scope;
      Compiler::Compile(i_isolate, h,
                        Compiler::CLEAR_EXCEPTION, &scope);
    }
  }
  // Step 3 (heap JSFunction scan) removed: now that HeapAllocator slow path
  // routes through LocalHeap for goroutine threads, EnsureFeedbackVector and
  // Runtime_InstallSFICode are safe to run on worker threads.
}

// Returns per-M IsolateData pointer (for setting r13 / kRootRegister).
void* v8_goroutine_get_isolate_data() {
  return v8::internal::GoroutineThreadState::GetIsolateData();
}

}  // extern "C"

