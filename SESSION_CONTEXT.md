# GODE — Go-style Goroutines for Node.js

## Что это
Форк Node.js с настоящими горутинами à la Go.  JS-функции запускаются через
`go(fn, ...args)` и исполняются конкурентно по модели GMP (Goroutine / Machine / Processor).

## Текущий статус — ✅ Phase 2 работает
Worker M-threads запускают горутины на отдельных OS-потоках.
V8 mutex сериализует доступ к V8: M0 отпускает в `uv_prepare` (перед epoll),
worker Ms подхватывают, M0 забирает обратно в `uv_check`.
Per-G HandleScopeData save/restore изолирует handle scopes между горутинами.
Все тесты проходят с GOMAXPROCS=1 и GOMAXPROCS=2.

---

## Архитектура (GMP — точная копия Go)

### Структуры
| Go     | Наш аналог | Где                           |
|--------|-----------|-------------------------------|
| G      | `G`       | `src/goroutine/g.h/.cc`       |
| M      | `M`       | `src/goroutine/runtime.h/.cc` |
| P      | `P`       | `src/goroutine/scheduler.h/.cc` |
| sched  | `Scheduler` | `src/goroutine/scheduler.h/.cc` |
| mcache | per-P IsolateData | `deps/v8/src/execution/goroutine-thread-state.h/.cc` |

### G (Goroutine)
- Собственный стек 64KB (`stack.h`, mmap + guard page)
- `v8::Global<Function>` entry + `v8::Global<Array>` args
- fcontext_t для Boost.Context (Phase 1.5)
- Состояния: Gidle → Grunnable → Grunning → Gdead/Gwaiting
- `Execute(isolate)` — вызывает JS-функцию, ловит исключения

### M (Machine = OS thread)
- Phase 1: M0 = main thread, нет отдельных потоков
- Phase 2: дополнительные M — отдельные OS-потоки
- `ExecuteOne(isolate)` — берёт G из очереди P, исполняет

### P (Processor)
- Lock-free ring buffer на 256 G (`runq_head_/tail_`, atomics)
- Per-P V8 state (Phase 2: saved IsolateData для in-place swap)
- Phase 2: `InitV8State / Activate / Deactivate` — swap per-P IsolateData ↔ isolate->isolate_data_

### Scheduler
- `Schedule(g)` → global queue (Mutex)
- `FindRunnable(p)` → local queue → global steal → work-stealing от random P
- `PushGlobal/PopGlobal/StealFromGlobal`

### Runtime
- Singleton. Lazy-init при первом `go()`.
- `Init(gomaxprocs, loop, isolate)` — создаёт Scheduler, P's, M0, uv_check
- `uv_check_t` → `DrainRunQueue()` → `M0.ExecuteOne()` в цикле (до 128 за тик)
- `uv_unref` чтобы check не мешал event loop завершиться

---

## Roadmap (фазы)

### Phase 1 ✅ DONE — Single-threaded, main thread
- Всё на main thread, без контекст-свитча
- `go()` → создаёт G → scheduler queue → uv_check → Execute() напрямую
- kRootRegister не меняется, V8 патчи не активны (`v8_goroutine_thread = false`)
- JIT работает нормально, --jitless не нужен

### Phase 1.5 ✅ DONE — Boost.Context (cooperative yield)
- `G::Execute` запускается через `jump_fcontext(g0 → G)` на собственном 64KB стеке
- `goroutine_entry(transfer_t)` получает g0 ctx, вызывает Execute, потом jump обратно
- `yield()` из JS → `YieldG()` → re-schedule + `jump_fcontext(G → g0)`
- `goid()` → `CurrentG()->goid()` через `thread_local G* tls_current_g`
- V8 stack limit обновляется при входе/выходе из горутины (`isolate->SetStackLimit`)
- Горутины чередуются на одном потоке (как Go GOMAXPROCS=1)

### Phase 2 ✅ DONE — Multi-threaded (V8 execution token)
- Worker M-threads (OS threads): `M::StartThread` → `ThreadLoop`
- `ThreadLoop`: `uv_sem_wait` → `uv_mutex_lock(v8_mutex)` → `RunG` → `uv_mutex_unlock`
- V8 mutex: M0 holds by default, releases in `uv_prepare` (before epoll), reacquires in `uv_check`
- Worker gets V8 during epoll_wait, runs goroutines, yields V8 between each G
- `v8_goroutine_thread = true` на worker потоках → V8 патчи активны
- `isolate->Enter()/Exit()` на worker потоках → V8 PerIsolateThreadData
- `NotifyGoroutineAvailable()` → `uv_sem_post` → будит worker
- Stack limit: каждый M устанавливает свой при входе в V8
- Per-P IsolateData swap НЕ нужен (V8 mutex сериализует — один M в V8 за раз)

### Phase 3 — True parallel V8
- Патч JSEntry: kRootRegister из TLS (per-M → per-P IsolateData)
- Per-P IsolateData по отдельным адресам
- Isolate::FromRootAddress через TLS
- Per-P LABs + locked slow path (как Go: mcache lock-free, mcentral/mheap с локом)
- GC safe points (stop-the-world)
- Убрать V8 execution token → настоящий параллелизм

---

## V8 патчи (для Phase 2+, сейчас неактивны)

Все патчи проверяют `extern thread_local bool v8_goroutine_thread`.
На main thread (Phase 1) флаг = false → патчи = no-op.

| Файл | Что патчим |
|------|-----------|
| `src/execution/goroutine-thread.h/.cc` | TLS флаг `v8_goroutine_thread` |
| `src/execution/goroutine-thread-state.h/.cc` | Per-P V8 state: create/activate/deactivate/destroy (extern "C") |
| `src/execution/isolate.h` | `isolate_data()` → per-thread redirect |
| `src/execution/isolate-data.h` | `friend class GoroutineThreadState` |
| `src/execution/stack-guard.h` | `friend class GoroutineThreadState` |
| `src/api/api.cc` | Skip Locker check, skip thread assertions |
| `src/handles/handles-inl.h` | Skip DCHECK for goroutine threads |
| `src/execution/execution.cc` | Skip AllowJavascriptExecution check |

---

## Файлы проекта

```
src/goroutine/
  g.h / g.cc           — Goroutine (стек, функция, Execute)
  runtime.h / runtime.cc — M + Runtime (uv_check, DrainRunQueue)
  scheduler.h / scheduler.cc — P + Scheduler (runqueues, work-stealing)
  stack.h / stack.cc    — Stack allocator (mmap, guard pages, pool)
  context.h / context.cc — Boost.Context wrapper (fcontext)
  channel.h / channel.cc — Channels (TODO)

src/goroutine_wrap.cc   — Node.js binding: go(), yield(), goid()
lib/goroutine.js        — Public JS API
lib/internal/goroutine.js — Internal binding wrapper

deps/v8/src/execution/goroutine-thread.h/.cc     — TLS flag
deps/v8/src/execution/goroutine-thread-state.h/.cc — Per-P V8 state
deps/boost/                                        — Boost.Context (fcontext asm)
```

## Сборка и тест
```bash
# Сборка (из out/Release)
ninja -j$(nproc)

# Тест
./out/Release/node test_ultra_minimal.js          # с debug stderr
./out/Release/node test_ultra_minimal.js 2>/dev/null  # чисто

# С переменной окружения
NODE_GOMAXPROCS=1 ./out/Release/node test_ultra_minimal.js
```

## Ключевые решения
- **No user-visible locks**: data races — ответственность программиста (как в Go)
- **Internal locks OK**: scheduler global queue, stack allocator pool (как в Go runtime)
- **Per-P memory**: IsolateData (HandleScope, LABs, ThreadLocalTop) привязан к P, не к M
- **kRootRegister проблема**: builtins читают IsolateData через r13 (фиксированный адрес).
  Phase 1: не проблема (main thread). Phase 2: in-place swap. Phase 3: TLS-based r13.
