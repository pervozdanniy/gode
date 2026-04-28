// Goroutine per-M V8 state implementation.
// See goroutine-thread-state.h for architecture overview.
// GM model: V8 state belongs to M (thread) directly, no P abstraction.

#include "src/execution/goroutine-thread-state.h"

#include <atomic>
#include <unistd.h>
#include <sys/syscall.h>
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

// Fast TLS pointer for hot-path dispatch in isolate.h (thread_local_top,
// handle_scope_data). Defined here, declared in goroutine-thread-state.h.
thread_local __attribute__((tls_model("initial-exec")))
    IsolateData* tls_per_m_isolate_data = nullptr;

// Pointer to the REAL main-thread IsolateData (the original, not any per-M copy).
// Set once in CreatePState() when v8_goroutine_thread is still false, so
// isolate->isolate_data() returns the genuine main IsolateData.
// Used in LabSyncBeforeRun() to refresh per-M roots_table_ after GC.
static IsolateData* g_main_isolate_data = nullptr;

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

  // Capture the real main IsolateData pointer ONCE (before any M-thread activates
  // its per-M IsolateData via the isolate_data() patch).  Used by LabSyncBeforeRun
  // to refresh per-M roots_table_ after GC without going through the patch.
  if (!g_main_isolate_data) {
    g_main_isolate_data = main_data;
  }
  Tagged<Context> main_ctx = main_data->thread_local_top().context_;

  // Allocate per-M IsolateData (aligned).
  // Add 8KB padding: V8 signal handlers (TrapWebAssemblyOrContinue) and some
  // builtins access fields at offsets beyond sizeof(IsolateData) assuming it's
  // embedded in the full Isolate struct. The padding prevents ASAN OOB reports.
  constexpr size_t kAlign = alignof(IsolateData);
#ifdef __SANITIZE_ADDRESS__
  constexpr size_t kPadding = 8192;  // V8 signal handlers read past IsolateData
#else
  constexpr size_t kPadding = 0;
#endif
  constexpr size_t kSize = (sizeof(IsolateData) + kPadding + kAlign - 1) & ~(kAlign - 1);
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

  // Repurpose tables_alignment_padding_[0] as is_goroutine_thread flag.
  // Will be set to 1 in ActivatePState(). Main IsolateData has it as 0.
  p_data->tables_alignment_padding_[0] = 0;

  // Per-M interrupt budget: initialise to a large positive value so the
  // budget never reaches zero and BytecodeBudgetInterrupt is never triggered
  // from goroutine M-threads. Since this field lives in the per-M IsolateData
  // (accessed via [r13 + offset]), each M-thread has its own private copy —
  // no cache-line sharing between M-threads even for hot JumpLoop bytecodes.

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
  // GOROUTINE: Set is_goroutine_thread flag in per-M IsolateData.
  // This byte is checked inline in InterpreterEntryTrampoline for fast-path.
  state->isolate_data->tables_alignment_padding_[0] = 1;

  g_active_p_state = state;
  v8_goroutine_thread = true;
  tls_per_m_isolate_data = state->isolate_data;

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
  if (g_active_p_state) {
    g_active_p_state->isolate_data->tables_alignment_padding_[0] = 0;
  }
  g_active_p_state = nullptr;
  v8_goroutine_thread = false;
  tls_per_m_isolate_data = nullptr;
}


// ---- Per-G HandleScopeData helpers ----

void GoroutineThreadState::SaveHSD(Isolate* isolate, void* buf) {
  // Use handle_scope_data() which returns per-M data on M-threads
  // (via tls_per_m_isolate_data), not isolate_data()->handle_scope_data_
  // which always returns MAIN data and would race with the main thread.
  std::memcpy(buf, isolate->handle_scope_data(), sizeof(HandleScopeData));
}

void GoroutineThreadState::RestoreHSD(Isolate* isolate, const void* buf) {
  std::memcpy(isolate->handle_scope_data(), buf, sizeof(HandleScopeData));
}

void GoroutineThreadState::ForceNewHandleBlock(Isolate* isolate) {
  HandleScopeData* hsd = isolate->handle_scope_data();
  hsd->next = hsd->limit;
}

void GoroutineThreadState::LabSyncBeforeRun() {
  if (!g_active_p_state) return;
  LocalHeap* lh = LocalHeap::Current();
  if (!lh) return;

  // Refresh per-M IsolateData from the real main IsolateData.
  // This is critical after MarkCompact GC: GC updates main IsolateData's
  // roots_table_ in-place (objects moved to new addresses), but per-M
  // IsolateData is a malloc'd copy that GC does not visit. Without this sync,
  // goroutines read stale root pointers from their per-M IsolateData (via r13),
  // leading to "Check failed: instance_type() >= FIRST_JS_RECEIVER_TYPE" and
  // similar crashes when they access moved objects through stale root slots.
  //
  // Called while LocalHeap is still Running (we just Unparked), so GC cannot
  // start — no concurrent modification of g_main_isolate_data->roots().
  if (g_main_isolate_data) {
    // Copy the full roots table (~4 KB, ~500 pointers). Fast memcpy.
    std::memcpy(&g_active_p_state->isolate_data->roots(),
                &g_main_isolate_data->roots(),
                sizeof(RootsTable));

    // Sync write-barrier marking flags. These flags are set when concurrent
    // marking starts (at arbitrary times after M-thread creation). The per-M
    // copy was initialised from main at CreatePState time; if marking started
    // later, the per-M flag stays 0 and goroutine write-barriers silently skip
    // recording → objects not traced → freed prematurely → heap corruption.
    // Previously this sync was a no-op because isolate->isolate_data() returned
    // per-M data through the patch. g_main_isolate_data bypasses the patch.
    g_active_p_state->isolate_data->is_marking_flag_ =
        g_main_isolate_data->is_marking_flag_;
    g_active_p_state->isolate_data->is_minor_marking_flag_ =
        g_main_isolate_data->is_minor_marking_flag_;
  }

  // LAB steal removed: ReplaceOldSpaceLAB (called at M-thread init) makes
  // LocalHeap's old_space_allocator share the same LinearAllocationArea as
  // per-M IsolateData::old_allocation_info_. No manual copy needed.
}

void GoroutineThreadState::LabSyncAfterRun() {
  // No-op: ReplaceOldSpaceLAB makes LocalHeap's allocator share the same
  // LinearAllocationArea as per-M IsolateData. No manual flush needed.
}

// Set per-M StackGuard stack limit without touching the shared
// Isolate::stack_size_ field. Called via v8_goroutine_set_stack_limit().
static void GoSetStackLimit(uintptr_t limit) {
  if (!g_active_p_state) return;
  StackGuard* sg = g_active_p_state->isolate_data->stack_guard();
  sg->SetStackLimit(limit);
}

// Returns the current M-thread's HandleScopeImplementer (per-M HSI).
// Called from goroutine-gc-roots.cc during safepoint park to register HSI
// for GC root scanning. nullptr if no goroutine M-thread is active.
static HandleScopeImplementer* GetCurrentHSI() {
  return g_active_p_state ? g_active_p_state->handle_scope_impl : nullptr;
}

LinearAllocationArea* GoroutineThreadState::GetOldAllocationInfo(
    IsolateData* data) {
  return &data->old_allocation_info_;
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

void* v8_goroutine_p_state_get_isolate_data(void* p_state) {
  if (!p_state) return nullptr;
  return static_cast<void*>(
      static_cast<v8::internal::GoroutinePState*>(p_state)->isolate_data);
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
//
// NOTE: We do NOT call Compiler::Compile(JSFunction) here (the old "Step 1").
// That call allocates a FeedbackVector on the main heap using DirectHandles,
// which is NOT safe while goroutine M-threads are Unparked (they can trigger
// a minor GC via LocalHeap that moves new-space objects, leaving the main
// thread's DirectHandle stale → SIGSEGV).
// FeedbackVector initialisation is deferred to the first M-thread call of the
// function; since M-threads use LocalHeap for allocation, that path is
// properly GC-coordinated and safe.
void v8_goroutine_deep_compile_script(v8::Isolate* isolate,
                                      v8::Local<v8::Function> fn) {
  using namespace v8::internal;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);

  // HandleScope is required: Compiler::Compile allocates, which can trigger
  // GC (Mark-Compact when goroutine M-threads fill old space via LocalHeap).
  // All raw tagged pointers must be in Handles before any allocation point.
  HandleScope scope(i_isolate);

  Handle<JSFunction> i_func =
      Cast<JSFunction>(v8::Utils::OpenHandle(*fn));

  Tagged<HeapObject> script_raw = i_func->shared()->script();
  if (!IsScript(script_raw)) return;

  // Hold Handle<Script> so GC keeps our reference current.
  Handle<Script> script(Cast<Script>(script_raw), i_isolate);

  // Use the Handle<WeakFixedArray> constructor of ScriptIterator —
  // it stores the handle internally, so the infos array reference survives
  // any GC triggered by Compiler::Compile allocations.
  Handle<WeakFixedArray> infos(script->infos(), i_isolate);
  SharedFunctionInfo::ScriptIterator iter(infos);

  for (Tagged<SharedFunctionInfo> sfi = iter.Next();
       !sfi.is_null(); sfi = iter.Next()) {
    if (!sfi->is_compiled()) {
      Handle<SharedFunctionInfo> h(sfi, i_isolate);
      IsCompiledScope is_compiled_scope;
      Compiler::Compile(i_isolate, h,
                        Compiler::CLEAR_EXCEPTION, &is_compiled_scope);
    }
  }
}

// Returns per-M IsolateData pointer (for setting r13 / kRootRegister).
void* v8_goroutine_get_isolate_data() {
  return v8::internal::GoroutineThreadState::GetIsolateData();
}

// Set the per-M StackGuard stack limit WITHOUT touching the shared
// Isolate::stack_size_ field. Use instead of v8::Isolate::SetStackLimit()
// from goroutine M-threads.
void v8_goroutine_set_stack_limit(uintptr_t limit) {
  v8::internal::GoSetStackLimit(limit);
}

// Returns the current M-thread's HandleScopeImplementer (per-M HSI).
// Used by GoroutineGCRegistry::Park to capture the HSI for GC root visiting.
// Must be called from the M-thread that is about to park (safepoint_park).
void* v8_goroutine_get_current_hsi() {
  return static_cast<void*>(v8::internal::GetCurrentHSI());
}

// Returns opaque StackGuard* for the current M-thread.
// Call once at RunG start, then use v8_goroutine_set_stack_limit_direct()
// to avoid repeated TLS lookups of g_active_p_state.
void* v8_goroutine_get_stack_guard() {
  if (!v8::internal::GoroutineThreadState::IsActive()) return nullptr;
  return static_cast<void*>(
      v8::internal::GoroutineThreadState::GetIsolateData()->stack_guard());
}

// Set stack limit on a previously obtained StackGuard* (no TLS lookup).
void v8_goroutine_set_stack_limit_direct(void* sg_ptr, uintptr_t limit) {
  if (!sg_ptr) return;
  static_cast<v8::internal::StackGuard*>(sg_ptr)->SetStackLimit(limit);
}

// ---- Combined RunG ceremony functions ----
// Reduces 5+ extern C calls to 2, cutting per-goroutine overhead.

// Called before jump_fcontext in RunG.
// Saves g0 HSD → g0_hsd_buf, restores G's HSD (or forces new block),
// sets goroutine stack limit, sets GC state TLS.
// Returns opaque StackGuard* for use in run_exit.
void* v8_goroutine_run_enter(v8::Isolate* isolate,
                              void* g0_hsd_buf,
                              const void* g_hsd_buf,
                              bool has_saved_hsd,
                              uintptr_t g_stack_bottom,
                              void* gc_state) {
  using namespace v8::internal;
  auto* i_isolate = reinterpret_cast<Isolate*>(isolate);

  // Save g0's HandleScopeData.
  GoroutineThreadState::SaveHSD(i_isolate, g0_hsd_buf);

  // Restore G's HSD (resuming) or force a fresh handle block (new G).
  if (has_saved_hsd) {
    GoroutineThreadState::RestoreHSD(i_isolate, g_hsd_buf);
  } else {
    GoroutineThreadState::ForceNewHandleBlock(i_isolate);
  }

  // Get StackGuard once (single TLS lookup).
  StackGuard* sg = GoroutineThreadState::IsActive()
      ? GoroutineThreadState::GetIsolateData()->stack_guard()
      : nullptr;

  // Set goroutine mmap stack limit.
  if (sg) sg->SetStackLimit(g_stack_bottom + 8192);

  // Set GC state TLS for safepoint hooks.
  // (v8_goroutine_set_current_gc_state is called from runtime.cc now,
  //  but we also accept it here for the grouped API.)
  extern void v8_goroutine_set_current_gc_state(void*);
  v8_goroutine_set_current_gc_state(gc_state);

  return static_cast<void*>(sg);
}

// Called after jump_fcontext returns in RunG.
// Saves G's HSD (if alive), restores g0 HSD, restores M-thread stack limit.
void v8_goroutine_run_exit(v8::Isolate* isolate,
                            void* sg_ptr,
                            const void* g0_hsd_buf,
                            void* g_hsd_buf,
                            bool g_is_dead,
                            uintptr_t sp) {
  using namespace v8::internal;
  auto* i_isolate = reinterpret_cast<Isolate*>(isolate);

  // Save G's HSD only if goroutine will be resumed.
  if (!g_is_dead) {
    GoroutineThreadState::SaveHSD(i_isolate, g_hsd_buf);
  }

  // Restore g0's HSD.
  GoroutineThreadState::RestoreHSD(i_isolate, g0_hsd_buf);

  // Restore M-thread OS stack limit.
  if (sg_ptr) {
    static_cast<StackGuard*>(sg_ptr)->SetStackLimit(sp - (900 * 1024));
  }
}

}  // extern "C"


