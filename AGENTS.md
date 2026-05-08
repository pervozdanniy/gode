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
| `src/goroutine_wrap.cc` | Node.js C++ binding exposing goroutine API to JavaScript (`go`, `yield`, `goid`, `threadid`, `getErrorStack`) |
| `lib/goroutine.js` | Public JavaScript API for goroutines; exports `go`, `goyield`, `goid`, `threadid` |
| `lib/internal/goroutine.js` | Internal implementation wrapping `internalBinding('goroutine')`; `execute()` wraps goroutine fn in try/catch and uses `getErrorStack(e)` (mutex-serialised `e.stack` read via C++) for safe error capture from M-threads |
| `src/goroutine/runtime.cc` | M threads, Runtime, Shutdown (two-phase: SignalStop all → JoinThread all) |
| `src/goroutine/runtime.h` | M + Runtime declarations |
| `src/goroutine/context.cc` | RunG / YieldG, fcontext switching, ASAN fiber annotations |
| `src/goroutine/g.cc` | G struct, Execute(), deep pre-compile |
| `src/goroutine/scheduler.cc` | Local + global queue, work-stealing |
| `src/goroutine/stack.{h,cc}` | Stack allocation/pooling (mmap stacks; 64KB default, 256KB ASAN) |
| `src/goroutine/channel.{h,cc}` | Channel for inter-goroutine communication |
| `docs/per-m-minor-gc-plan.md` | Per-M Minor GC implementation plan (page tracking, per-M free list, per-M collector) |
| `deps/v8/src/execution/goroutine-flag.h` | Thin bridge header — the ONLY goroutine include allowed in V8 files; declares `v8_goroutine_thread` and `v8_goroutine_real_isolate` TLS |
| `deps/v8/src/execution/goroutine-thread.{h,cc}` | Defines `v8_goroutine_thread` and `v8_goroutine_real_isolate` TLS vars; `goroutine-thread.h` re-exports `goroutine-flag.h` |
| `deps/v8/src/execution/goroutine-thread-state.{h,cc}` | Per-M IsolateData, HSD save/restore, `tls_per_m_isolate_data` TLS |
| `deps/v8/src/execution/goroutine-local-heap.{h,cc}` | Per-M LocalHeap: register worker threads with V8 GC safepoint |
| `deps/v8/src/execution/goroutine-gc-roots.{h,cc}` | GC root scanning for yielded goroutine mmap stacks (Phase 1.2) |
| `deps/v8/src/execution/goroutine-shape-lock.{h,cc}` | Map transition mutex (`v8_goroutine_map_transition_lock/unlock`); spinlock with `LocalHeap::Safepoint()` on M-threads to avoid GC deadlock; DCL fast path in `TransitionToDataProperty` skips mutex on cache-hit |
| `deps/v8/src/execution/goroutine-feedback.{h,cc}` | Phase 2 active: `GoroutineFeedbackState` maps unique_id → per-M FeedbackVector via PersistentHandles; `CreatePerMFeedbackVector` clones FV via `FeedbackVector::New`; `GetOrCreate` fast cache lookup from InterpreterEntryTrampoline; `v8_goroutine_create_per_m_feedback` called from BytecodeBudgetInterrupt |
| `deps/v8/src/heap/heap-allocator.{h,cc}` | Patched: `ReplaceOldSpaceLAB()` for shared LAB; slow-path redirect for M-threads |
| `deps/v8/src/heap/factory.cc` | Patched: goroutine allocation redirect (AllocateRaw, New, AllocateRawWithAllocationSite) |
| `deps/v8/src/heap/local-heap.cc` | Patched: goroutine safepoint park/unpark with GC registry hooks |
| `deps/v8/src/heap/heap.cc` | Patched: GC safepoint hooks (`v8_goroutine_safepoint_park/unpark`); GC root scanning for yielded stacks; Minor GC request from M-threads (`v8_goroutine_minor_gc_requested_`); NewSpace expansion cap 64MB for goroutines |
| `deps/v8/src/heap/minor-mark-sweep.cc` | Patched: `FilterNormalObject` validates Map addresses during conservative stack scanning via `LookupChunkContainingAddressInSafepoint` + meta-map ReadOnlySpace check — prevents SIGSEGV from false-positive stack values matching NewSpace page addresses |
| `deps/v8/src/heap/collection-barrier.cc` | Patched: M-threads skip `ExecuteWhileParked` callback (mmap stack frames not on system stack → SIGSEGV in conservative GC) |
| `deps/v8/src/heap/main-allocator.cc` | Patched: per-thread `tls_last_lab_page_` for PagedNewSpace; goroutine-aware page expansion |
| `deps/v8/src/heap/local-heap.cc` | Patched: goroutine safepoint park/unpark with GC registry hooks; `MakeLinearAllocationAreasIterable` + `FreeLinearAllocationAreas` before park |
| `deps/v8/src/heap/mark-compact.cc` | Patched: goroutine NewSpace page handling |
| `deps/v8/src/heap/paged-spaces.cc` | Patched: goroutine NewSpace page handling |
| `deps/v8/src/execution/isolate.h` | Patched `thread_local_top()` / `handle_scope_data()` / `handle_scope_implementer()` via `tls_per_m_isolate_data` |
| `deps/v8/src/execution/arguments.h` | Patched: `RUNTIME_FUNCTION` macro overrides computed `Isolate*` with `v8_goroutine_real_isolate` on M-threads |
| `deps/v8/src/execution/execution.cc` | Patched: stack-guard uses per-M IsolateData root on M-threads |
| `deps/v8/src/handles/handles{-inl}.h` | Patched: allows handle creation/usage on goroutine M-threads (bypasses DCHECK) |
| `deps/v8/src/logging/counters.cc` | Patched: `Histogram::AddSample` returns early on M-threads (`if (v8_goroutine_thread) return`) — prevents race on regexp telemetry (`V8.RegExpBacktracks`) when `IrregexpInterpreter` runs from goroutines |
| `deps/v8/src/objects/descriptor-array-inl.h` | Patched: `SearchWithCache` skips per-Isolate `DescriptorLookupCache` on M-threads (`if (v8_goroutine_thread) return Search(...)`) — the cache is not thread-safe; bypassing it fixes SIGSEGV in `LookupInRegularHolder` during cold property lookups |
| `deps/v8/src/objects/feedback-vector{-inl}.h` | Patched: IC slot writes (`ComputeHandler`, `SetOptimizedCode`) are skipped on M-threads to prevent shared FV mutation |
| `deps/v8/src/interpreter/interpreter-assembler.cc` | Patched: `UpdateInterruptBudget` skips shared `FeedbackCell::interrupt_budget` store on M-threads (goroutine flag check via `[r13 + tables_alignment_padding_offset]`) — eliminates MESI cache-line bouncing |
| `deps/v8/src/objects/map.cc` | Patched: map transitions guarded by `v8_goroutine_map_transition_lock()`; DCL fast path: speculative lock-free `SearchTransition` + `CanHoldValue` before taking mutex |
| `deps/v8/src/objects/js-objects.cc` | Patched: `MigrateToMap` — no extra locking (seqlock removed; map transition mutex protects transition creation, not MigrateToMap itself) |
| `deps/v8/src/objects/shared-function-info.cc` | Patched: tier-up skipped on M-threads (`v8_goroutine_thread` guard) |
| `deps/v8/src/runtime/runtime-internal.cc` | Patched: allocation lock + real Isolate fixup in `RUNTIME_FUNCTION` handlers on M-threads |
| `deps/v8/src/api/api{-inl}.h` | Patched: `HandleScope` skips microtask/exception callbacks on goroutine M-threads |

## JavaScript API

Goroutines are accessed via `require('goroutine')`:

```javascript
const { go, goyield, goid, threadid } = require('goroutine');

go(fn, ...args);        // Create and schedule a goroutine; returns goroutine ID
goyield();              // Cooperative yield (explicit scheduling point)
goid();                 // Get current goroutine ID (0 on main thread)
threadid();             // Get OS thread ID (Linux gettid) of the M-thread executing this goroutine
```

**Output from goroutines:**
- `console.log()` / `console.error()` are safe — `process.stdout.write` is patched via `fs.WriteSync` (synchronous write(2) syscall) so output from M-threads does not race through libuv
- `console.log` still must NOT be called from inside V8 allocation paths (it is safe at the JS level between goroutine steps)

**Error handling in goroutines:**
- Uncaught exceptions inside `go(fn)` are caught automatically
- `e.stack` access is serialised via a `static std::mutex` in `GetErrorStack` C++ binding — safe from concurrent M-threads
- Errors are forwarded to the main thread and emitted as `'uncaughtException'`

**Usage constraints:**
- Direct libuv `fs.*` / `net.*` / `http.*` I/O is **NOT** safe from goroutines (libuv is not thread-safe)
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

- **GVL removed**: workers run V8 concurrently; safe only for pre-compiled pure JS (no direct libuv I/O from goroutines — use `console.log` which is patched to use synchronous `write(2)` instead)
- **`console.log` from goroutines — objects print as `[object Object]` (KNOWN LIMITATION)**: `console.log` on M-threads bypasses `Console.prototype.log` entirely (patched in `lib/internal/goroutine.js`) and calls `_writeFdSync(write(2))` directly. String/number/boolean args print fine. Non-primitive args (objects, arrays, Maps, etc.) fall through `String(a)` → `[object Object]`. The correct fix would be to call `util.inspect(a)` for objects, but `util.inspect` internally uses regex (`keyStrRegExp.test(key)` etc.) and V8 C++ operations that trigger `v8::internal::Relocatable` registration in `real_isolate->relocatable_top_` — a per-Isolate non-thread-safe linked list. When multiple M-threads concurrently create Relocatable objects (e.g. via `CustomArgumentsBase` for property access interceptors, or implicitly via string replace builtins), GC on the main thread races on this list → SIGSEGV in `RootMarkingVisitor::VisitRootPointers`. **Root cause**: `Relocatable::relocatable_top_` is not dispatched via `tls_per_m_isolate_data` — it lives directly in the real `Isolate` struct without per-thread storage or locking. **TODO**: either protect `relocatable_top_` with a spinlock, or provide per-M `Relocatable` lists and make GC scan all of them.
- **`Buffer` from goroutines is UNSAFE (KNOWN LIMITATION)**: `Buffer.from()`, `Buffer.allocUnsafe()`, and `Buffer.alloc()` all ultimately go through `new Uint8Array(...)` / `Builtins_CreateTypedArray` which reads `array_buffer_allocator` and other fields from the per-M fake Isolate (not the real one) → SIGSEGV. The shared pool (`allocPool`, `poolOffset`) in `lib/buffer.js` is also a data race: multiple M-threads read/write `poolOffset` without atomics → corrupted pool pointer → `new Uint8Array(pool, garbage_offset, size)` → crash. **TODO**: (1) patch `AllocateExternalBackingStore`/`SetAllocatorFromIsolate` in `backing-store.cc` to use `v8_goroutine_real_isolate`; (2) use per-M buffer pool or always `createFromString` (pool bypass) for M-threads. Until fixed: do not use `Buffer` from goroutine code; raw number/string computation is safe.
- **Shutdown two-phase**: `SignalStop()` all workers first, then `JoinThread()` all — avoids shared-semaphore deadlock where wrong M steals sem_post
- **LocalHeap destroy**: must be called from the OWNER worker thread (uses thread-local write barriers); do NOT call from main thread
- **`isolate_data()` always returns main**: `Isolate::isolate_data()` returns `&isolate_data_` on ALL threads (no per-M dispatch). Only these accessors dispatch via `tls_per_m_isolate_data`: `thread_local_top()`, `handle_scope_data()`, `handle_scope_implementer()`. **NEVER use `isolate->isolate_data()->handle_scope_data_` or `isolate->isolate_data()->thread_local_top_` directly** — always go through the accessor methods. `stack_guard()` is accessed via `g_active_p_state->isolate_data->stack_guard()` in goroutine code.
- **r13 (kRootRegister)** is set to per-M IsolateData in `goroutine_entry()` and after `YieldG()` resume via inline asm. V8 builtins access IsolateData fields through r13, so this is consistent with `tls_per_m_isolate_data`.
- **Shared LAB (no sync)**: `ReplaceOldSpaceLAB()` at M-thread init makes LocalHeap's `old_space_allocator_` point at per-M `IsolateData::old_allocation_info_`. Also creates `new_space_allocator_` backed by old space using per-M `IsolateData::new_allocation_info_` (pointer arithmetic `lab - 1`). Ignition fast path (r13) and LocalHeap share the **same** `LinearAllocationArea` — no manual LAB sync needed. `LabSyncBeforeRun` only syncs `roots_table_` and `marking_flags` after GC (rare). `LabSyncAfterRun` is a no-op.
- **roots_table_ is a copy**: per-M IsolateData contains a **copy** of main roots_table (~4KB). GC updates only the main copy → must `memcpy` after every safepoint. This is cheap (nanoseconds vs GC milliseconds).
- **Lock-safepoint deadlock (FIXED)**: any mutex held during V8 allocations must NOT use blocking `mutex.lock()` on goroutine M-threads, because if GC fires the thread is stuck in the kernel and cannot park → GC waits forever. Fix: `v8_goroutine_map_transition_lock()` uses `try_lock()` + `LocalHeap::Safepoint()` spin loop so GC can always proceed. See `goroutine-shape-lock.cc`.
- **Shared FeedbackVectors / per-M FV (Phase 2 — ACTIVE)**: goroutines now get per-M FeedbackVector clones via `CreatePerMFeedbackVector` (called lazily from `BytecodeBudgetInterrupt`). `UpdateInterruptBudget` in `interpreter-assembler.cc` skips the shared `FeedbackCell::interrupt_budget` store on M-threads (returns `INT32_MAX/2`) to avoid MESI cache-line bouncing. IC slot writes (`ComputeHandler`, `SetOptimizedCode`) are still skipped on M-threads (`feedback-vector-inl.h` patches). Per-M JIT tier-up is still disabled (Phase 2 TODO).
- **`v8_goroutine_real_isolate` TLS**: `CEntryStub` computes `Isolate*` as `r13 - kRootRegisterBias`, which resolves to per-M IsolateData (not the real Isolate). `v8_goroutine_real_isolate` holds the correct value. Set in `goroutine-thread.cc`, consumed by the `RUNTIME_FUNCTION` macro in `arguments.h` and in `string-table.cc`. Always set this alongside `v8_goroutine_thread` when activating an M-thread.
- **Allocation performance (WIP: per-M NewSpace allocation)**: goroutines previously allocated everything into old space (no real young generation). **Active fix**: `ReplaceOldSpaceLAB()` now creates `new_space_allocator_` backed by real `PagedNewSpace` (with `--minor-ms`), enabling Minor Mark-Sweep for goroutine allocations instead of full Mark-Compact. GC safepoint hooks added to allocation failure path (`CollectGarbage`/`CollectAllAvailableGarbage`); per-thread `tls_last_lab_page_` eliminates data race; `ShouldExpandYoungGenerationOnSlowAllocation` allows up to 64MB for goroutine NewSpace; `CollectGarbageForBackground` triggers Minor GC when `v8_goroutine_minor_gc_requested_`. Full plan: `docs/per-m-minor-gc-plan.md`. **Remaining TODO**: per-M page tracking, per-M free list, per-M Minor GC collector, scheduler local-yield + work-steal page promotion.
- **M-threads don't respond to SafepointScope in tight loops (KNOWN LIMITATION)**: M-threads executing tight JS loops without allocations (e.g. `for (j=0; j<10M; j++) sum += j*10`) never check `SafepointRequested`. `UpdateInterruptBudget` in `interpreter-assembler.cc` returns `INT32_MAX/2` for M-threads (to avoid MESI cache-line bouncing on shared `FeedbackCell::interrupt_budget`), so the interrupt handler never fires and `Safepoint()` is never called. **Consequence**: if main thread creates a `SafepointScope` (e.g. via `StartMinorMSIncrementalMarkingIfNeeded`), it hangs in `WaitUntilRunningThreadsInSafepoint` waiting for M-threads that never park. **Current workaround**: removed `StartMinorMSIncrementalMarkingIfNeeded()` from the goroutine patch in `CollectGarbageForBackground` (heap.cc) — this was the only place where main thread independently initiated SafepointScope while M-threads could be running. GC is now triggered only by allocation failure paths, where M-threads park themselves before SafepointScope is created. **Proper fix (TODO)**: per-M interrupt budget counter stored in IsolateData (accessed via r13), so each M-thread decrements its own budget without MESI bouncing, and the interrupt handler fires periodically → calls `LocalHeap::Safepoint()`. Alternatively, inject safepoint checks at JumpLoop (backward branches) via codegen in `interpreter-assembler.cc`.
- **`FilterNormalObject` patch in Minor Mark-Sweep (INVESTIGATION TODO)**: conservative stack scanning during Minor GC can encounter false-positive pointers to NewSpace pages that contain garbage map words. The `FilterNormalObject` patch in `minor-mark-sweep.cc` validates Map addresses via `LookupChunkContainingAddressInSafepoint` + meta-map ReadOnlySpace check to avoid SIGSEGV in `SizeFromMap`. **Open question**: V8's standard worker threads (via `--parallel-marking`, background compilation, etc.) also use conservative stack scanning but do NOT need this filter. Why do goroutines specifically trigger this? Possible causes: (1) goroutine mmap stacks contain stale V8 pointers from previous goroutine executions (stack reuse without zeroing); (2) per-M LAB pages mix live objects with uninitialized memory differently from standard thread allocation; (3) the conservative stack walker sees more false positives on goroutine stacks because they are smaller (64KB) and reused more aggressively. Needs investigation to determine if the root cause can be fixed upstream instead of filtering in the scanner.

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
