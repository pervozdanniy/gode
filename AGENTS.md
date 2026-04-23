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

## Key Source Files

| File | Description |
|------|-------------|
| `src/goroutine/runtime.cc` | M threads, Runtime, Shutdown (two-phase: SignalStop all → JoinThread all) |
| `src/goroutine/runtime.h` | M + Runtime declarations |
| `src/goroutine/context.cc` | RunG / YieldG, fcontext switching, ASAN fiber annotations |
| `src/goroutine/g.cc` | G struct, Execute(), deep pre-compile |
| `src/goroutine/scheduler.cc` | Local + global queue, work-stealing |
| `src/goroutine/stack.{h,cc}` | Stack allocation/pooling (mmap stacks; 64KB default, 256KB ASAN) |
| `src/goroutine/channel.{h,cc}` | Channel for inter-goroutine communication |
| `deps/v8/src/execution/goroutine-flag.h` | Thin bridge header — the ONLY goroutine include allowed in V8 files |
| `deps/v8/src/execution/goroutine-thread-state.{h,cc}` | Per-M IsolateData, HSD save/restore, `tls_per_m_isolate_data` TLS |
| `deps/v8/src/execution/goroutine-local-heap.{h,cc}` | Per-M LocalHeap: register worker threads with V8 GC safepoint |
| `deps/v8/src/execution/goroutine-gc-roots.{h,cc}` | GC root scanning for yielded goroutine mmap stacks (Phase 1.2) |
| `deps/v8/src/execution/goroutine-shape-seqlock.{h,cc}` | SeqLock protecting shape transitions during MigrateToMap (Phase 1.3) |
| `deps/v8/src/heap/heap-allocator.{h,cc}` | Patched: `ReplaceOldSpaceLAB()` for shared LAB; slow-path redirect for M-threads |
| `deps/v8/src/heap/factory.cc` | Patched: goroutine allocation redirect (AllocateRaw, New, AllocateRawWithAllocationSite) |
| `deps/v8/src/heap/local-heap.cc` | Patched: goroutine safepoint park/unpark with GC registry hooks |
| `deps/v8/src/execution/isolate.h` | Patched `thread_local_top()` / `handle_scope_data()` / `handle_scope_implementer()` via `tls_per_m_isolate_data` |

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
- **Shared LAB (no sync)**: `ReplaceOldSpaceLAB()` at M-thread init makes LocalHeap's `old_space_allocator_` point at per-M `IsolateData::old_allocation_info_`. Ignition fast path (r13) and LocalHeap share the **same** `LinearAllocationArea` — no manual LAB sync needed. `LabSyncBeforeRun` only syncs `roots_table_` and `marking_flags` after GC (rare). `LabSyncAfterRun` is a no-op.
- **roots_table_ is a copy**: per-M IsolateData contains a **copy** of main roots_table (~4KB). GC updates only the main copy → must `memcpy` after every safepoint. This is cheap (nanoseconds vs GC milliseconds).
- **Lock-safepoint deadlock (FIXED)**: any mutex held during V8 allocations must NOT use blocking `mutex.lock()` on goroutine M-threads, because if GC fires the thread is stuck in the kernel and cannot park → GC waits forever. Fix: `v8_goroutine_map_transition_lock()` uses `try_lock()` + `LocalHeap::Safepoint()` spin loop so GC can always proceed. See `goroutine-shape-seqlock.cc`.
- **Shared FeedbackVectors / no per-M JIT (KI-6)**: goroutines sharing the same JS function share one FeedbackVector → IC pollution → megamorphic slowdown. Fix is Phase 2 (per-M FeedbackVectors + per-M JIT tier-up). Until then, goroutines with heavy allocations are ~5-8x slower than main thread.

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
