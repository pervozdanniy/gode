# GODE — Coroutines for Node.js (working name: goroutines → Phase 7 переименует в Co)

## Что это
Форк Node.js с настоящими корутинами. JS-функции запускаются через `go(fn)` и исполняются конкурентно на M тредах. Переключение — кооперативное через `yield()`.

## Следуем плану: plan.md

---

## Текущий статус

| Фаза | Статус | Описание |
|------|--------|----------|
| 1.1 GC Safepoints | ✅ DONE | LocalHeap per M thread (Park/Unpark/Safepoint) |
| 1.2 GC roots (yielded goroutines) | ✅ DONE | goroutine-gc-roots.cc, gc_state_ в G |
| **GOMAXPROCS > 1 дедлок** | ✅ FIXED | ThreadLoop: unpark внутри GVL |
| 1.3 SeqLock shape transitions | ❌ TODO | — |
| 2 Per-thread JIT cache | ❌ TODO | пока без --jitless (JIT работает, но через GVL) |
| 3 Goroutine runtime | ✅ mostly done | go/yield/goid/scheduler работают |
| 4 Unified thread pool | ❌ TODO | — |
| 5 Per-M uv_loop (netpoller) | ❌ TODO | — |
| 6 Channels + sync | ⚠️ stub | blocking send/recv, select — TODO |
| 7 Переименование в Co | ❌ TODO | в конце |

---

## Архитектура (актуальная)

```
Main Thread (M0):
  event loop → epoll_wait
             → uv_check: DrainRunQueue()
               M0::ExecuteOne(): lock GVL → RunG → unlock GVL
               (M0 — всегда Running, park/unpark не нужен)

Worker Thread (M1..Mn):
  LocalHeap starts Parked (GC-safe)
  loop:
    uv_sem_wait(goroutine_sem_)           ← ждём сигнала (Parked)
    inner loop:
      FindRunnable()                      ← поиск горутины (Parked, нет heap access)
      lock GVL (g_v8_lock)               ← ждём GVL пока Parked → GC может работать
      LocalHeap::Unpark()                ← становимся Running (внутри GVL)
      RunG(g, isolate)                   ← исполнение JS
      LocalHeap::Park()                  ← Parked (до unlock GVL)
      unlock GVL                         ← другой M или GC могут продолжить
      if g dead: delete g
    end inner loop
    uv_async_send(async_handle_)         ← будим event loop
  end loop

GC coordination:
  LocalHeap::Park()   → GC-safe (любой момент)
  LocalHeap::Unpark() → блокируется пока GC идёт
  GVL гарантирует: только один M в состоянии Running одновременно
  goroutine-gc-roots.cc → GC сканирует yielded горутины
```

**Ключевое решение GVL (g_v8_lock):**
- Один глобальный мьютекс сериализует выполнение V8 между M тредами
- Фикс дедлока: `unpark` вызывается **внутри** GVL, а не до захвата
  - Раньше: unpark вне GVL → поток Running, ждёт GVL → GC deadlock
  - Теперь: lock GVL (Parked) → unpark (Running) → RunG → park → unlock
- Phase 2 (per-thread JIT) позволит убрать GVL совсем

---

## Реализованные файлы

### Node.js runtime (src/goroutine/)
| Файл | Что делает |
|------|-----------|
| g.h / g.cc | G struct + gc_state_ (Park при yield, Unpark при resume) |
| runtime.h / runtime.cc | M + Runtime: GVL (g_v8_lock), LocalHeap, ThreadLoop, event loop hooks |
| scheduler.h / scheduler.cc | Local queue + global queue + work-stealing |
| stack.h / stack.cc | mmap стеки с guard pages (64KB + guard page per goroutine) |
| context.h / context.cc | RunG, YieldG с gc_park/gc_unpark (Boost.Context fcontext) |
| channel.h / channel.cc | Channel stub (TrySend/TryRecv работают, blocking — TODO) |

### V8 патчи (deps/v8/src/execution/)
| Файл | Что делает |
|------|-----------|
| goroutine-thread.h/cc | TLS флаг `v8_goroutine_thread` |
| goroutine-thread-state.h/cc | Per-M IsolateData copy (HSD, LABs, StackGuard) |
| goroutine-gc-roots.h/cc | GC root scanning для yielded горутин |
| goroutine-local-heap.h/cc | LocalHeap C API (create/park/unpark/safepoint/destroy) |

### V8 патчи (deps/v8/src/heap/)
| Файл | Изменение |
|------|----------|
| heap.cc | GoroutineGCRegistry::IterateRoots в IterateRoots() |
| safepoint.cc | без изменений (LocalHeap+IsolateSafepoint покрывают всё) |

### JS API
| Файл | Что делает |
|------|-----------|
| src/goroutine_wrap.cc | V8 binding: go(), yield(), goid() |
| lib/goroutine.js | Public API |
| lib/internal/goroutine.js | Internal binding wrapper |

---

## Сборка и тесты

```bash
cd /home/pervozdanniy/code/gode

# ./node — симлинк на out/Release/node
ninja -C out/Release node

# Тесты (все проходят с GOMAXPROCS=1 и GOMAXPROCS=4)
./node test_context_switch.js
GOMAXPROCS=4 ./node test_context_switch.js
GOMAXPROCS=4 ./node test/goroutine/test-goroutine-basic.js
GOMAXPROCS=4 ./node test/goroutine/test-goroutine-many.js
GOMAXPROCS=4 ./node test/goroutine/test-goroutine-goid.js
```

---

## Ключевые решения

| Решение | Обоснование |
|---------|------------|
| **GVL (g_v8_lock)** | Сериализует V8 между M тредами; убрать после Phase 2 (per-thread JIT) |
| **LocalHeap** | Регистрирует M треды в V8 GC safepoint без кастомного registry |
| **unpark внутри GVL** | Фикс дедлока GOMAXPROCS>1: GC не застревает на safepoint пока M ждёт GVL |
| **goroutine-gc-roots.cc** | GC обновляет указатели в стеках yielded горутин |
| **Нет isolate->Enter()** | LocalHeap делает Isolate::SetCurrent() в конструкторе |
| **Boost.Context (fcontext)** | Кооперативное переключение контекстов (CPU regs) |
| **mmap стеки** | 64KB + guard page per goroutine |

---

## Следующие шаги (по плану)

1. **Phase 1.3** — SeqLock на shape transitions (M тред читает map, main thread пишет)
2. **Phase 2** — Per-thread JIT cache → убрать GVL (настоящий параллелизм)
3. **Phase 5** — Per-M uv_loop (netpoller) → горутины делают IO
4. **Phase 6** — Lock-free channels + select + SharedMutex
5. **Phase 7** — Переименование: goroutine → coroutine, go() → Co.run()
