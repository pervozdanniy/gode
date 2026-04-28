# AI Agent Notes

## File System (WSL)

This project runs inside **WSL (Ubuntu)** accessed from Windows.

**CRITICAL**: When using file editing tools (`insert_edit_into_file`, `replace_string_in_file`, `create_file`, `read_file`), always use the **full Windows UNC path**:

```
\\wsl.localhost\Ubuntu\home\pervozdanniy\code\gode\<path>
```

**NOT** the Linux path:
```
/home/pervozdanniy/code/gode/<path>   ← WRONG for file tools
```

**JetBrains MCP tools** (`mcp_jetbrains_*`) use **project-relative paths** (e.g. `src/goroutine/runtime.cc`) with `projectPath=/home/pervozdanniy/code/gode` — this is correct and does NOT need the UNC prefix.

**Terminal commands** (`run_in_terminal`) use Linux paths as usual:
```bash
cd /home/pervozdanniy/code/gode && ...
```

## Project Overview

- **Project**: GODE — Node.js fork with real coroutines (goroutines)
- **Location**: `/home/pervozdanniy/code/gode` (WSL Ubuntu)
- **Build (Release)**: `ninja -C out/Release node -j8`
- **Build (ASAN)**: `ninja -C out/Asan node -j8`
- **Test binary**: `./node` (symlink to `out/Release/node`)
- **Worker threads**: Set via `GOMAXPROCS` environment variable (default 1, max 256)

## Key Source Files

| File | Description |
|------|-------------|
| `src/goroutine_wrap.cc` | Node.js C++ binding exposing goroutine API to JavaScript (`go`, `yield`, `goid`, `threadid`, `goprint`) |
| `lib/goroutine.js` | Public JavaScript API for goroutines; exports `go`, `goyield`, `goid`, `threadid`, `goprint` |
| `lib/internal/goroutine.js` | Internal implementation wrapping `internalBinding('goroutine')` |
| `src/goroutine/runtime.cc` | M threads, Runtime, Shutdown (two-phase: SignalStop all → JoinThread all) |
| `src/goroutine/runtime.h` | M + Runtime declarations |
| `src/goroutine/context.cc` | RunG / YieldG, fcontext switching, ASAN fiber annotations |
| `src/goroutine/g.cc` | G struct, Execute(), deep pre-compile |
| `src/goroutine/scheduler.cc` | Local + global queue, work-stealing |
| `src/goroutine/stack.{h,cc}` | Stack allocation/pooling (mmap stacks; 64KB default, 256KB ASAN) |
| `src/goroutine/channel.{h,cc}` | Channel for inter-goroutine communication |
| `deps/v8/src/execution/goroutine-flag.h` | Thin bridge header — the ONLY goroutine include allowed in V8 files; declares `v8_goroutine_thread` and `v8_goroutine_real_isolate` TLS |
| `deps/v8/src/execution/goroutine-thread.{h,cc}` | Defines `v8_goroutine_thread` and `v8_goroutine_real_isolate` TLS vars; `goroutine-thread.h` re-exports `goroutine-flag.h` |
| `deps/v8/src/execution/goroutine-thread-state.{h,cc}` | Per-M IsolateData, HSD save/restore, `tls_per_m_isolate_data` TLS |
| `deps/v8/src/execution/goroutine-local-heap.{h,cc}` | Per-M LocalHeap: register worker threads with V8 GC safepoint |
| `deps/v8/src/execution/goroutine-gc-roots.{h,cc}` | GC root scanning for yielded goroutine mmap stacks (Phase 1.2) |
| `deps/v8/src/execution/goroutine-shape-seqlock.{h,cc}` | SeqLock protecting shape transitions during MigrateToMap (Phase 1.3) |
| `deps/v8/src/execution/goroutine-feedback.{h,cc}` | Phase 2 active: `GoroutineFeedbackState` maps unique_id → per-M FeedbackVector via PersistentHandles; `CreatePerMFeedbackVector` clones FV via `FeedbackVector::New`; `GetOrCreate` fast cache lookup from InterpreterEntryTrampoline; `v8_goroutine_create_per_m_feedback` called from BytecodeBudgetInterrupt |
| `deps/v8/src/heap/heap-allocator.{h,cc}` | Patched: `ReplaceOldSpaceLAB()` for shared LAB; slow-path redirect for M-threads |
| `deps/v8/src/heap/factory.cc` | Patched: goroutine allocation redirect (AllocateRaw, New, AllocateRawWithAllocationSite) |
| `deps/v8/src/heap/local-heap.cc` | Patched: goroutine safepoint park/unpark with GC registry hooks |
| `deps/v8/src/heap/heap.cc` | Patched: GC safepoint hooks (`v8_goroutine_safepoint_park/unpark`); GC root scanning for yielded stacks |
| `deps/v8/src/heap/collection-barrier.cc` | Patched: M-threads skip `ExecuteWhileParked` callback (mmap stack frames not on system stack → SIGSEGV in conservative GC) |
| `deps/v8/src/execution/isolate.h` | Patched `thread_local_top()` / `handle_scope_data()` / `handle_scope_implementer()` via `tls_per_m_isolate_data` |
| `deps/v8/src/execution/arguments.h` | Patched: `RUNTIME_FUNCTION` macro overrides computed `Isolate*` with `v8_goroutine_real_isolate` on M-threads |
| `deps/v8/src/execution/execution.cc` | Patched: stack-guard uses per-M IsolateData root on M-threads |
| `deps/v8/src/handles/handles{-inl}.h` | Patched: allows handle creation/usage on goroutine M-threads (bypasses DCHECK) |
| `deps/v8/src/objects/feedback-vector{-inl}.h` | Patched: IC slot writes (`ComputeHandler`, `SetOptimizedCode`) are skipped on M-threads to prevent shared FV mutation |
| `deps/v8/src/interpreter/interpreter-assembler.cc` | Patched: `UpdateInterruptBudget` skips shared `FeedbackCell::interrupt_budget` store on M-threads (goroutine flag check via `[r13 + tables_alignment_padding_offset]`) — eliminates MESI cache-line bouncing |
| `deps/v8/src/objects/map.cc` | Patched: map transitions guarded by `v8_goroutine_map_transition_lock()` |
| `deps/v8/src/objects/js-objects.cc` | Patched: `MigrateToMap` wrapped in `v8_goroutine_shape_seqlock_begin/end()` |
| `deps/v8/src/objects/shared-function-info.cc` | Patched: tier-up skipped on M-threads (`v8_goroutine_thread` guard) |
| `deps/v8/src/runtime/runtime-internal.cc` | Patched: allocation lock + real Isolate fixup in `RUNTIME_FUNCTION` handlers on M-threads |
| `deps/v8/src/api/api{-inl}.h` | Patched: `HandleScope` skips microtask/exception callbacks on goroutine M-threads |

## JavaScript API

Goroutines are accessed via `require('goroutine')`:

```javascript
const { go, goyield, goid, threadid, goprint } = require('goroutine');

go(fn, ...args);        // Create and schedule a goroutine; returns goroutine ID
goyield();              // Cooperative yield (explicit scheduling point)
goid();                 // Get current goroutine ID (0 on main thread)
threadid();             // Get OS thread ID (Linux gettid) of the M-thread executing this goroutine
goprint(...args);       // Goroutine-safe print (enqueues output to main thread; use instead of console.log)
```

**Usage constraints:**
- `console.log()` / `console.error()` / libuv I/O calls are **NOT** safe from goroutines (libuv is not thread-safe)
- Use `goprint()` for output from goroutines
- Only pure JS computation is safe inside goroutines
- Runtime lazy-inits on first `go()` call; worker threads determined by `GOMAXPROCS` env var

**Test examples:** `test_go.js`, `test_context_switch.js`, `test_thread_ids.js`, `test_memory.js` in project root demonstrate usage patterns.

## Agent Behaviour Rules

- **ALWAYS explain the plan first** before writing any code. Describe what files will be changed, what approach will be used, and why. Ask clarifying questions if anything is unclear. Only start implementing after the user confirms.
- **NEVER pipe build commands through `tail`, `head`, or `grep`** — run `ninja` (and similar build tools) without any output filtering so the user sees full real-time progress.

## V8 Modification Rules

- **Minimize changes in V8 source** — touch only files that absolutely require it
- **Avoid changes to V8 headers** — prefer adding code in `.cc` files or in our own `goroutine-*.cc` files
- New V8-side logic goes into `deps/v8/src/execution/goroutine-*.cc` files (already our territory)
- Never add `#include "src/goroutine/..."` from V8 files — use only `goroutine-flag.h` (the thin bridge)

## Known Issues / Decisions

- **GVL removed**: workers run V8 concurrently; safe only for pre-compiled pure JS (no `console.log` from goroutines — libuv I/O is not thread-safe)
- **Shutdown two-phase**: `SignalStop()` all workers first, then `JoinThread()` all — avoids shared-semaphore deadlock where wrong M steals sem_post
- **LocalHeap destroy**: must be called from the OWNER worker thread (uses thread-local write barriers); do NOT call from main thread
- **`isolate_data()` always returns main**: `Isolate::isolate_data()` returns `&isolate_data_` on ALL threads (no per-M dispatch). Only these accessors dispatch via `tls_per_m_isolate_data`: `thread_local_top()`, `handle_scope_data()`, `handle_scope_implementer()`. **NEVER use `isolate->isolate_data()->handle_scope_data_` or `isolate->isolate_data()->thread_local_top_` directly** — always go through the accessor methods. `stack_guard()` is accessed via `g_active_p_state->isolate_data->stack_guard()` in goroutine code.
- **r13 (kRootRegister)** is set to per-M IsolateData in `goroutine_entry()` and after `YieldG()` resume via inline asm. V8 builtins access IsolateData fields through r13, so this is consistent with `tls_per_m_isolate_data`.
- **Shared LAB (no sync)**: `ReplaceOldSpaceLAB()` at M-thread init makes LocalHeap's `old_space_allocator_` point at per-M `IsolateData::old_allocation_info_`. Also creates `new_space_allocator_` backed by old space using per-M `IsolateData::new_allocation_info_` (pointer arithmetic `lab - 1`). Ignition fast path (r13) and LocalHeap share the **same** `LinearAllocationArea` — no manual LAB sync needed. `LabSyncBeforeRun` only syncs `roots_table_` and `marking_flags` after GC (rare). `LabSyncAfterRun` is a no-op.
- **roots_table_ is a copy**: per-M IsolateData contains a **copy** of main roots_table (~4KB). GC updates only the main copy → must `memcpy` after every safepoint. This is cheap (nanoseconds vs GC milliseconds).
- **Lock-safepoint deadlock (FIXED)**: any mutex held during V8 allocations must NOT use blocking `mutex.lock()` on goroutine M-threads, because if GC fires the thread is stuck in the kernel and cannot park → GC waits forever. Fix: `v8_goroutine_map_transition_lock()` uses `try_lock()` + `LocalHeap::Safepoint()` spin loop so GC can always proceed. See `goroutine-shape-seqlock.cc`.
- **Shared FeedbackVectors / per-M FV (Phase 2 — ACTIVE)**: goroutines now get per-M FeedbackVector clones via `CreatePerMFeedbackVector` (called lazily from `BytecodeBudgetInterrupt`). `UpdateInterruptBudget` in `interpreter-assembler.cc` skips the shared `FeedbackCell::interrupt_budget` store on M-threads (returns `INT32_MAX/2`) to avoid MESI cache-line bouncing. IC slot writes (`ComputeHandler`, `SetOptimizedCode`) are still skipped on M-threads (`feedback-vector-inl.h` patches). Per-M JIT tier-up is still disabled (Phase 2 TODO).
- **`v8_goroutine_real_isolate` TLS**: `CEntryStub` computes `Isolate*` as `r13 - kRootRegisterBias`, which resolves to per-M IsolateData (not the real Isolate). `v8_goroutine_real_isolate` holds the correct value. Set in `goroutine-thread.cc`, consumed by the `RUNTIME_FUNCTION` macro in `arguments.h` and in `string-table.cc`. Always set this alongside `v8_goroutine_thread` when activating an M-thread.
- **Allocation performance (inherent ~35% gap on alloc-heavy workloads)**: goroutines allocate everything into old space (no real young generation). This causes: (1) write/marking barriers on every old-space store during incremental marking; (2) heavier Mark-Compact GC instead of cheap Scavenge. Compute-only goroutines have **zero overhead** vs main thread. Current mitigations: `BoundAllocationLimit` floor 64MB in `heap-controller.cc` (reduces GC from 90→45); page-expansion-first LAB refill in `main-allocator.cc` (reduces slow-path from 137K→14K calls); CSA fast-path via per-M `new_allocation_info_` works (99.86% of allocations inline). Object literal allocation (`{a:1, b:2}`) has separate ~22× overhead from shape/map-transition SeqLock contention — separate issue. **TODO**: investigate per-M young generation or write-barrier skip for goroutine-allocated pages to close the remaining gap.

## ASAN Build

Separate ASAN build lives in `out/Asan/`. To create it:
```bash
python3 configure.py --ninja --enable-asan
mv out/Release out/Asan
```

Key ASAN adaptations:
- **Fiber annotations** (`context.cc`): `__sanitizer_start/finish_switch_fiber` around every `jump_fcontext` call, guarded by `#ifdef __SANITIZE_ADDRESS__`
- **Larger stacks** (`stack.h`): 256KB instead of 64KB — ASAN red zones bloat stack frames
- **IsolateData padding** (`goroutine-thread-state.cc`): 8KB padding after `aligned_alloc(sizeof(IsolateData))` — V8 signal handlers (`TrapWebAssemblyOrContinue`) read past IsolateData assuming it's embedded in the full Isolate struct
- **100K+ goroutines may OOM** under ASAN due to shadow memory overhead (256KB × N stacks)
