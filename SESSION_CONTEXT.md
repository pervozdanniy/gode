# GODE — Go-style Goroutines for Node.js

## Что это
Форк Node.js с настоящими горутинами à la Go. JS-функции запускаются через
`go(fn, ...args)` и исполняются конкурентно по модели GMP (Goroutine / Machine / Processor).

## Текущий статус — ✅ Phase 2.5: P-based scheduling (Release + Debug)

### Что реализовано и работает

#### Phase 1 ✅ — Single-threaded, main thread
- `go()` → создаёт G → scheduler queue → uv_check/uv_idle → Execute()
- `v8_goroutine_thread = false` на main thread

#### Phase 1.5 ✅ — Boost.Context (cooperative yield)
- `G::Execute` через `jump_fcontext(g0 → G)` на 64KB стеке
- `goroutine_entry(transfer_t)` → Execute → jump обратно
- `yield()` → `YieldG()` → re-schedule + jump_fcontext(G → g0)
- `goid()` → `CurrentG()->goid()` через `thread_local G* tls_current_g`
- V8 stack limit обновляется при входе/выходе из горутины
- Per-G HandleScopeData save/restore (предотвращает handle overlap)

#### Phase 2 ✅ — Multi-threaded (V8 execution token)
- Worker M-threads: `M::StartThread` → `ThreadLoop`
- V8 mutex: один M в V8 за раз
- M0 releases mutex в `uv_prepare` (before epoll), reacquires в `uv_check`
- Worker gets V8 during epoll_wait
- `v8_goroutine_thread = true` на worker потоках
- `isolate->Enter()/Exit()` на workers

#### Phase 2.5 ✅ — P-based scheduling
- P с lock-free ring buffer (256 slots)
- Per-P V8 state: IsolateData + HandleScopeImplementer
- Activate/Deactivate при acquirep/releasep
- V8 mutex сохраняется (Phase 2 serialization)
- Все тесты проходят с GOMAXPROCS=1 и 4

---

## Реализованные файлы

### Node.js runtime (src/goroutine/)
| Файл | Строк | Что делает |
|------|-------|-----------|
| g.h/g.cc | 86+127 | G struct: стек, функция, Execute, состояния, saved HSD |
| runtime.h/runtime.cc | 129+287 | M struct + Runtime singleton, ThreadLoop, event loop hooks |
| scheduler.h/scheduler.cc | 127+281 | P struct + Scheduler: local queue, global queue, work-stealing |
| stack.h/stack.cc | 73+? | Stack allocator: mmap, guard pages, pool |
| context.h/context.cc | 34+118 | Boost.Context wrapper: RunG, YieldG, HSD save/restore |
| channel.h/channel.cc | 64+128 | Channel stub (mutex-based, блокировка TODO) |

### V8 патчи (deps/v8/src/execution/)
| Файл | Что делает |
|------|-----------|
| goroutine-thread.h/cc | TLS флаг `v8_goroutine_thread` |
| goroutine-thread-state.h/cc | Per-P V8 state: create/activate/deactivate/destroy, HSD save/restore |

### JS API
| Файл | Что делает |
|------|-----------|
| src/goroutine_wrap.cc | V8 binding: go(), yield(), goid() |
| lib/goroutine.js | Public API |
| lib/internal/goroutine.js | Internal binding wrapper |

### Другие V8 патчи (проверки отключены для goroutine threads)
- `src/api/api.cc` — Skip Locker check
- `src/handles/handles-inl.h` — Skip DCHECK
- `src/execution/execution.cc` — Skip AllowJavascriptExecution check
- `src/execution/isolate-data.h` — friend class GoroutineThreadState
- `src/execution/stack-guard.h` — friend class GoroutineThreadState

---

## Архитектура (текущая)

```
Main Thread (M0):
  event loop → uv_prepare (release v8_mutex)
                 → epoll_wait (workers run goroutines)
               uv_check (reacquire v8_mutex)
                 → GOMAXPROCS=1: DrainRunQueue
                 → GOMAXPROCS>1: workers handle all

Worker Thread (M1..Mn):
  sem_wait → mutex_lock(v8_mutex) → ExecuteOne → mutex_unlock → sem_wait
  ExecuteOne: FindRunnable(P) → RunG(g, isolate)
  RunG: save g0 HSD → restore G HSD → SetStackLimit → jump_fcontext → back → save G HSD → restore g0 HSD

Schedule(g): push to global queue → NotifyGoroutineAvailable
FindRunnable(p): local queue → global steal → work-stealing from random P
```

---

## Сборка и тесты

```bash
# Release
cd out/Release && ninja -j$(nproc)
./out/Release/node test_stress.js

# Debug
cd out/Debug && ninja -j$(nproc)
./out/Debug/node test_stress.js

# С несколькими P
NODE_GOMAXPROCS=4 ./out/Release/node test_stress.js
```

## Ключевые решения
- **V8 mutex** сериализует доступ к V8 (Phase 2, пока не убран)
- **Per-P IsolateData**: каждый P имеет свою копию IsolateData (LABs, HandleScope, ThreadLocalTop)
- **Per-G HSD save/restore**: HandleScopeData сохраняется/восстанавливается при context switch
- **No user-visible locks**: data races — ответственность программиста (как в Go)
- **Unrecovered panic = abort**: как в Go

---

## Новая концепция (plan.md) — АНАЛИЗ И СРАВНЕНИЕ

### ═══════════════════════════════════════════
### Фаза 1 плана: Патч V8 — общий heap
### ═══════════════════════════════════════════

**Что предлагает план:**
1. GC Safepoints для внешних тредов (ExternalThreadRegistry, ~130 строк)
2. GC не эвакуирует shared объекты (is_shared флаг, ~80 строк)
3. SeqLock на shape transitions (version counter, ~150 строк)
4. Shared String Table (мьютекс на internment, ~80 строк)

**Текущее состояние:**
- ❌ Ничего из этого НЕ реализовано
- V8 mutex сериализует всё → GC проблемы пока не возникают
- Для убирания mutex (true parallel) ВСЁ из Фазы 1 будет необходимо

**Оценка:**
- Это самая критичная и сложная часть — глубокие изменения V8 GC
- Без этого невозможен настоящий параллелизм
- ~440 строк нового кода в V8 heap/objects

### ═══════════════════════════════════════════
### Фаза 2 плана: Per-thread JIT cache
### ═══════════════════════════════════════════

**Что предлагает план:**
1. PerThreadJitCache (per-thread Code objects, ~200 строк)
2. Per-thread Feedback Vectors (~150 строк)
3. Ignition per-thread call path (~80 строк)

**Текущее состояние:**
- ❌ НЕ реализовано
- JIT/компиляция общая для всех тредов
- V8 mutex делает это безопасным сейчас

**Оценка:**
- Нужно для true parallel — иначе deopt в одном треде крашит другой
- ~430 строк нового кода в V8 compiler/interpreter

### ═══════════════════════════════════════════
### Фаза 3 плана: Goroutine runtime
### ═══════════════════════════════════════════

**Что предлагает план:**
1. Goroutine struct (~200 строк)
2. Горутинный стек в shared heap (~200 строк)
3. Планировщик + work-stealing (~300 строк)
4. JS API go() + Promise (~150 строк)

**Что уже реализовано (совпадения):**
- ✅ G struct с состояниями (Gidle/Grunnable/Grunning/Gwaiting/Gdead)
- ✅ Stack allocator (64KB + guard page) — но через mmap, не shared heap
- ✅ Планировщик с local queue + global queue + work-stealing
- ✅ JS API go(), yield(), goid()
- ✅ Boost.Context для context switching (fcontext)
- ✅ Per-G HandleScopeData save/restore

**Различия:**
| Аспект | План | Текущее |
|--------|------|---------|
| Стеки | shared heap (GC-visible) | mmap (не GC-visible) |
| Возврат значения | Promise | fire-and-forget (goid) |
| Сохранение состояния | InterpreterRegisters | CPU regs (fcontext) |
| Парковка | mcall-аналог | YieldG → jump_fcontext |
| IO waiting | PendingIO + IOResult | нет |
| Context | v8::Global<Context> per G | берём isolate->GetCurrentContext() |

**Оценка:**
- ~60% функциональности Фазы 3 уже реализовано
- Основные gap: Promise API, IO integration, shared heap стеки

### ═══════════════════════════════════════════
### Фаза 4 плана: Unified Thread Pool
### ═══════════════════════════════════════════

**Что предлагает план:**
1. Объединённый пул (goroutine + libuv tasks, ~200 строк)
2. Динамический размер пула (~100 строк)
3. libuv интеграция (~150 строк)

**Текущее состояние:**
- ⚠️ Частично: есть worker threads, но фиксированное число
- M↔P привязка статична при создании (plan: динамическая)
- libuv thread pool и goroutine workers — отдельные пулы
- `entersyscall()`/`exitsyscall()` подготовлены, но не подключены

**Оценка:**
- Нужно объединить пулы и сделать динамический grow/shrink
- entersyscall/exitsyscall нужно прокинуть в реальные syscall пути

### ═══════════════════════════════════════════
### Фаза 5 плана: Per-M uv_loop (netpoller)
### ═══════════════════════════════════════════

**Что предлагает план:**
1. Per-M uv_loop инициализация (~80 строк)
2. Регистрация fd горутины + парковка (~100 строк)
3. IO ready callback + zero-copy read (~120 строк)
4. Перерегистрация при миграции (~80 строк)

**Текущее состояние:**
- ❌ НЕ реализовано
- Один uv_loop (main thread)
- Workers не имеют своих event loops
- Горутины не могут делать IO → нет автоматического yield на IO

**Оценка:**
- Критично для реальных приложений (net, fs, etc.)
- Аналог Go netpoller

### ═══════════════════════════════════════════
### Фаза 6 плана: Channels и примитивы синхронизации
### ═══════════════════════════════════════════

**Что предлагает план:**
1. Channel (lock-free ring buffer, SharedValue, ~250 строк)
2. SharedMutex / RWMutex (~150 строк)
3. select (~150 строк)

**Текущее состояние:**
- ⚠️ Channel stub (channel.h/cc): есть структура, TrySend/TryRecv работают
- Blocking send/recv = TODO (printf "TODO: Block sender")
- select = не реализован
- SharedMutex / RWMutex = не реализованы

**Оценка:**
- Нужно переписать Channel на lock-free
- Добавить блокировку горутины (не треда!) при send/recv
- select — ключевая Go-фича

---

## ИТОГО: Что переиспользуется из текущего кода

### ✅ Полностью переиспользуется:
1. **G struct** (g.h/g.cc) — ядро горутины
2. **Boost.Context** — context switching (fcontext)
3. **Stack allocator** (stack.h/cc) — mmap стеки с guard pages
4. **Scheduler** (scheduler.h/cc) — local queue + global + work-stealing
5. **JS binding** (goroutine_wrap.cc, lib/goroutine.js)
6. **V8 TLS flag** (goroutine-thread.h/cc)
7. **V8 per-P state** (goroutine-thread-state.h/cc) — IsolateData клонирование

### ⚠️ Нужна доработка:
1. **Runtime** (runtime.h/cc) — добавить per-M uv_loop, динамический пул
2. **Channel** (channel.h/cc) — переписать на lock-free
3. **M struct** — добавить entersyscall/exitsyscall wiring

### ❌ Нужно написать с нуля (из плана):
1. **GC safepoints** (V8 heap) — ~130 строк
2. **GC shared object pinning** — ~80 строк
3. **SeqLock shape transitions** — ~150 строк
4. **Shared string table** — ~80 строк
5. **Per-thread JIT cache** — ~430 строк
6. **Per-M uv_loop (netpoller)** — ~380 строк
7. **SharedMutex / RWMutex** — ~150 строк
8. **select** — ~150 строк
9. **Promise API для go()** — ~50 строк

---

## Рекомендуемый порядок следующих шагов

1. **Phase 3 → True Parallel V8** (убрать v8_mutex):
   - GC safepoints
   - Shared object pinning
   - SeqLock
   - Per-thread JIT
   ⟹ после этого: убрать v8_mutex, N тредов в V8 одновременно

2. **Netpoller (per-M uv_loop)**:
   - Горутины могут делать IO
   - Автоматический yield на IO
   ⟹ после этого: горутины полезны для реальных задач

3. **Channels + sync primitives**:
   - Lock-free channels
   - select
   ⟹ после этого: Go-style concurrency patterns

4. **Unified thread pool + dynamic sizing**:
   - Объединить libuv и goroutine workers
   - entersyscall/exitsyscall
