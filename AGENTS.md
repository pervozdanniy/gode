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
- **Build**: `ninja -C out/Release node -j8`
- **Test binary**: `./node` (symlink to `out/Release/node`)

## Key Source Files

| File | Description |
|------|-------------|
| `src/goroutine/runtime.cc` | M threads, Runtime, Shutdown (two-phase: SignalStop all → JoinThread all) |
| `src/goroutine/runtime.h` | M + Runtime declarations |
| `src/goroutine/context.cc` | RunG / YieldG, fcontext switching |
| `src/goroutine/g.cc` | G struct, Execute(), deep pre-compile |
| `src/goroutine/scheduler.cc` | Local + global queue, work-stealing |
| `deps/v8/src/execution/goroutine-thread-state.cc` | Per-M IsolateData, HSD save/restore |
| `deps/v8/src/execution/isolate.h` | Patched `isolate_data()` / `handle_scope_implementer()` |

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

