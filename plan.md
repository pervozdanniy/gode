## Финальный план реализации

> Последнее обновление: 2026-04-18 (factory.cc allocation fix)
> Статусы: ✅ DONE | 🔨 IN PROGRESS (собрано, не протестировано) | ❌ KNOWN ISSUE | 📋 TODO

---

## Текущая реализация (фундамент)

### per-M IsolateData ✅ DONE

```
Каждый M-тред имеет клон IsolateData (memcpy + fix-up полей).
Регистр r13 указывает на per-M копию → JIT fast path (LAB, roots,
StackGuard, handle_scope_data) изолирован между тредами.

Файл: deps/v8/src/execution/goroutine-thread-state.cc
      → GoroutineThreadState::CreatePState / ActivatePState
```

### LocalHeap per-M ✅ DONE

```
Каждый M-тред создаёт LocalHeap(kBackground):
  - Регистрируется в V8 IsolateSafepoint автоматически
  - LocalHeap::Park()      → M тред GC-safe (между горутинами)
  - LocalHeap::Unpark()    → M тред начинает исполнение
  - LocalHeap::Safepoint() → кооперативная точка (~1ns fast path)
  - Вызывает Isolate::SetCurrent() → заменяет isolate->Enter()

Файл: deps/v8/src/execution/goroutine-local-heap.cc
```

### GVL ✅ УДАЛЁН (фаза 2 — параллельный V8 через per-M IsolateData)

```
GVL (g_v8_lock) был временным костылём для сериализации V8 между M тредами.
Удалён: M треды работают параллельно через per-M IsolateData + LocalHeap.
Параллельность безопасна для pre-compiled pure JS без мутации общих объектов.
```

### Isolate* fix (CEntryStub) ✅ DONE

```
yield() → gc_park(): сохраняет ThreadLocalTop (c_entry_fp цепочка V8 фреймов)
                     регистрирует горутину в GoroutineGCRegistry
GC IterateRoots() → обходит mmap-стек каждой yielded горутины,
                    обновляет tagged-указатели in-place (эвакуация ОК)
resume() → gc_unpark(): снимает регистрацию

Файл: deps/v8/src/execution/goroutine-gc-roots.h/cc
```

### G struct, Scheduler, Runtime ✅ DONE

```
src/goroutine/g.h/cc        — G struct, Execute, HSD save/restore, gc_state
src/goroutine/scheduler.cc  — local queues (256 слотов), global queue,
                               work-stealing, schedtick (fairness каждые 61)
src/goroutine/runtime.cc    — M threads, uv handles, two-phase shutdown
src/goroutine/context.cc    — RunG / YieldG, fcontext switching
src/goroutine/stack.h       — mmap стек 64 КБ (не shared heap)
```

### deep_compile_script ✅ DONE

```
Все SFI скрипта компилируются на main thread перед dispatch горутины.
Runtime_CompileLazy никогда не вызывается из worker треда.

Файл: deps/v8/src/execution/goroutine-thread-state.cc
      → v8_goroutine_deep_compile_script
```

### Heap allocation routing через LocalHeap ✅ DONE

```
Точка перехвата — factory.cc (НЕ V8 хедеры):
  Factory::AllocateRaw / AllocateRawWithAllocationSite:
    if (v8_goroutine_thread) → LocalHeap::Current()->AllocateRawWith
    LAB sync: after_run перед вызовом, before_run после.
    kYoung → kOld (background LocalHeap не имеет Young Space LAB).

  heap-allocator.cc slow-path:
    Если local_heap_->is_main_thread() И v8_goroutine_thread
    → перехватываем и роутим в LocalHeap::Current()->AllocateRawOrFail.
    Это отличает «main heap's allocator вызван из M-треда» от
    «goroutine's LH allocator дошёл до slow-path».

  runtime-internal.cc → Runtime_Allocate* → LocalHeap::AllocateRawWith

Горутины никогда не трогают main thread LAB без синхронизации.

Файлы: deps/v8/src/heap/factory.cc
        deps/v8/src/heap/heap-allocator.cc

Тест: 5000 горутин × new Array(40000) под GOMAXPROCS=1 --jitless
  → множество Mark-Compact циклов → завершилось за ~8.6s без краша ✅
```

### Isolate* fix (CEntryStub) 🔨 IN PROGRESS

```
Проблема: CEntryStub вычисляет Isolate* как r13 - kRootRegisterBias.
  На worker треде r13 = per-M IsolateData → неверный указатель.

```
TLS v8_goroutine_real_isolate (goroutine-flag.h) хранит реальный Isolate*.
RUNTIME_FUNCTION_RETURNS_TYPE (arguments.h) исправляет isolate в начале
каждого runtime-вызова через: if (v8_goroutine_thread && v8_goroutine_real_isolate)
  isolate = static_cast<Isolate*>(v8_goroutine_real_isolate);

Файлы: deps/v8/src/execution/goroutine-flag.h
        deps/v8/src/execution/goroutine-thread-state.cc
        deps/v8/src/execution/arguments.h
Статус: реализовано и активно (без GVL — критически важно).
```

### Old Space LAB sync ✅ DONE

```
Перед RunG:   LocalHeap Old Space LAB → per-M IsolateData
              JIT fast path (bump pointer через r13) работает без slow path.
После RunG / при YieldG: обновлённый top → обратно в LocalHeap.

Файлы: deps/v8/src/execution/goroutine-thread-state.cc
        src/goroutine/context.cc
Статус: реализовано.
```

---

## ❌ Known Issues (обязательно устранить)

### KI-1: GC safepoint не доходит до worker тредов

```
Проблема: GC запрашивает safepoint, записывая jslimit = 0 в StackGuard
  реального IsolateData. Worker треды читают StackGuard из per-M клона
  → сигнал не доходит → долго работающая горутина блокирует GC.

Решение: добавить LocalHeap::Safepoint() в M::ThreadLoop между горутинами.
  Это встроенный механизм LocalHeap (~1ns если GC не ждёт):

    // src/goroutine/runtime.cc → M::ThreadLoop, после RunG:
    LocalHeap::Current()->Safepoint();

Файл: src/goroutine/runtime.cc
```

### KI-2: Goroutine аллокации идут только в Old Space

```
Статус: KNOWN, не критично для корректности.

Причина: LocalHeap background тредов не имеет Young Space LAB.
  Все горутинные аллокации → Old Space → повышенное давление на Major GC,
  Minor GC не очищает short-lived горутинные объекты.

Текущее поведение: factory.cc роутит kYoung → kOld для M-тредов.
  5000 горутин × 40K array → множество Mark-Compact → работает стабильно.

Варианты решения:
  A) Исследовать Young Space поддержку в LocalHeap (есть в новых V8).
  B) Per-goroutine bump-pointer arena, сбрасываемая при GState::Gdead.
     (безопаснее, не требует V8 изменений)
```

### KI-3: libuv I/O не thread-safe из горутин

```
Проблема: console.log, fs.*, net.* из worker тредов вызывают libuv
  напрямую → data race, возможный crash.

Текущий workaround: Runtime::EnqueuePrint + print_queue → main thread
  разгребает в OnAsync.

Полное решение: Фаза 4 (per-M uv_loop) + маршрутизация всех I/O запросов
  через main loop (промежуточно) или per-M loops (финально).
```

### KI-4: GC не может прервать CPU-bound горутину изнутри

```
Проблема: GC пишет jslimit = kInterruptLimit в реальный IsolateData.
  Worker треды читают jslimit из per-M клона → сигнал не доходит
  → CPU-bound горутина (без yield) блокирует GC на произвольное время.
  Особенно критично т.к. горутины используются именно для CPU-bound задач.

Решение (три шага):

  1. Глобальный список активных GoroutinePState* в GoroutineThreadState.
     При ActivatePState → добавить в список (под мьютексом).
     При DeactivatePState → удалить из списка.

  2. Хук в IsolateSafepoint::RequestSafepointOperation (или Notify):
     После записи в реальный IsolateData → вызвать
     GoroutineThreadState::SignalAllMThreads() которая пишет
     RequestInterrupt(kGCInterrupt) в per-M StackGuard каждого M.

  3. Патч Runtime_StackGuard для goroutine тредов:
     if (v8_goroutine_thread && LocalHeap::Current()->IsRunningWithSafepoint()) {
       // GC ждёт — паркуем текущую горутину
       v8_goroutine_gc_park(isolate, CurrentG()->gc_state());
       LocalHeap::Current()->Park();
       LocalHeap::Current()->Unpark();
       v8_goroutine_gc_unpark(CurrentG()->gc_state());
     }

Затраты: ~1 атомарная проверка на каждый вызов функции в горутине.
  (только при активном GC — иначе kInterruptLimit не выставлен)

Файлы:
  goroutine-thread-state.h/cc  — список активных p_states + SignalAllMThreads
  deps/v8/src/execution/isolate-safepoint.cc  — хук (минимальное изменение)
  deps/v8/src/runtime/runtime-stack-check.cc  — Runtime_StackGuard patch
```

### KI-5: Data race при небезопасном доступе к общим объектам

```
Проблема: если пользователь без явного мьютекса мутирует объект из горутины
  одновременно с мэйн тредом (или другой горутиной), возможны:
  - SIGSEGV (чтение битого указателя при одновременном shape transition)
  - тихая корrupция данных (race на значение поля)

  Пример:
    // мэйн тред:
    obj.b = 2;       // shape transition → новый Map объекта

    // горутина (одновременно):
    obj.a            // читает map pointer посередине записи → SIGSEGV

Решение: перехват SIGSEGV в обработчике сигнала worker M-треда.

  1. При старте каждого M-треда устанавливаем альтернативный стек (sigaltstack)
     и обработчик SIGSEGV/SIGBUS.

  2. В обработчике:
     - Убеждаемся, что адрес падения лежит в куче V8 (heap bounds check).
     - Если да → это скорее всего data race горутины.
     - Вызываем uv_async_send в main loop с ошибкой типа
       "DataRaceError: unsafe concurrent access to shared object in goroutine".

  3. Main loop получает async событие и вызывает
     process.emit('uncaughtException', err).
     Это даёт пользователю стандартный хук Node.js для graceful shutdown
     (логирование, flush, exit).

  4. Горутина/M-тред завершается (longjmp из обработчика или pthread_exit).

Ограничения:
  - SIGSEGV из JIT-кода не всегда указывает на data race (может быть баг рантайма).
  - После обработки состояние кучи может быть испорчено → process.exit() обязателен.
  - Как и в оригинальном Node.js: uncaughtException — это "last resort",
    после которого нужно перезапускать процесс.

Файлы:
  src/goroutine/runtime.cc  — установка sigaltstack + SA_ONSTACK при старте M
  src/goroutine/runtime.cc  — SIGSEGV handler → uv_async_send → main loop
  lib/goroutine.js          — emit('uncaughtException') или 'unhandledRejection'
```


---

## Фаза 1: Патч V8 (продолжение)

### 1.1 GC Safepoints ✅ DONE
### 1.2 GC roots для mmap-стеков ✅ DONE

### 1.3 SeqLock на shape transitions 📋 TODO

**1.1 GC Safepoints для M тредов** ✅ DONE

```
Реализовано через: deps/v8/src/execution/goroutine-local-heap.h/cc

Каждый M worker тред создаёт LocalHeap (kBackground):
  - Регистрируется в V8's IsolateSafepoint автоматически
  - Участвует в StopTheWorld без кастомного registry
  - LocalHeap::Park()      → M тред GC-safe (sem_wait, между горутинами)
  - LocalHeap::Unpark()    → M тред начинает исполнение (блокирует если GC идёт)
  - LocalHeap::Safepoint() → кооперативная проверка между горутинами (~1ns fast path)
  - LocalHeap также вызывает Isolate::SetCurrent() — заменяет isolate->Enter()

Старый GoroutineSafepointRegistry — удалён.
goroutine-safepoint.h/cc — удалены.

Тест: 4 M треда читают heap пока GC работает → нет крашей ✅
```

**1.2 GC не ломает указатели горутин** ✅ DONE

```
Реализовано через: deps/v8/src/execution/goroutine-gc-roots.h/cc
                   + gc_state_ в G struct (src/goroutine/g.h)
                   + gc_park/gc_unpark в YieldG (src/goroutine/context.cc)

Механизм:
  yield() → gc_park():
    Сохраняет ThreadLocalTop горутины (содержит c_entry_fp_ — начало
    цепочки V8 фреймов на mmap-стеке горутины).
    Регистрирует горутину в GoroutineGCRegistry.

  GC IterateRoots() → GoroutineGCRegistry::IterateRoots():
    Для каждой yielded горутины:
      StackFrameIterator(isolate, saved_tlt) → обходит mmap-стек
      frame->Iterate(visitor) → обновляет все tagged-указатели in-place
    Таким образом GC МОЖЕТ эвакуировать объекты (двигать их),
    и указатели в стеке горутины корректно обновляются.

  resume() → gc_unpark():
    Снимает регистрацию. Горутина продолжает с актуальными указателями.

  Пока горутина ВЫПОЛНЯЕТСЯ (не yielded):
    M тред в состоянии Unparked → GC ждёт Safepoint()
    Горутина рано или поздно вызывает yield() → gc_park → GC обновляет
    Гарантия: no running goroutine during evacuation.

Подход отличается от исходного плана:
  Исходный план: запрет эвакуации (is_shared flag, NEVER_EVACUATE)
  Реализация:    разрешить эвакуацию + обновлять указатели через GC roots

Тест: M тред держит pointer → GC не ломает pointer ✅
```

**GVL (Global V8 Lock) + ThreadLoop дедлок-фикс** ✅ УДАЛЁН

```
GVL удалён в фазе 2. M-треды выполняют V8 параллельно через per-M IsolateData.
Deadlock был: unpark вне GVL → Running, ждёт GVL → GC deadlock.
Больше не актуально — GVL нет.
```

**1.3 SeqLock на shape transitions**

```
Файлы: src/objects/heap-object.h, objects.cc

Изменения:
  - version counter (uint32) в object header
  - При shape transition (только main тред):
      version++ → write → version++
  - При чтении map pointer из M треда:
      читаем version до и после
      если изменилась → читаем снова

Тест: main тред добавляет свойства → M тред читает → нет UB
~150 строк
```

---

### Фаза 2: Per-thread JIT cache

**2.1 PerThreadJitCache**

```
Новые файлы: src/execution/per-thread-jit-cache.h/.cc

struct PerThreadJitCache {
    // ключ: адрес SharedFunctionInfo (стабильный, не перемещается)
    unordered_map<Address, LocalCode> cache
    PerThreadCodeHeap code_heap  // mmap: PROT_READ|PROT_WRITE|PROT_EXEC
    unordered_map<Address, FeedbackVector*> feedback
    unordered_map<Address, uint32_t> call_counts
}

thread_local PerThreadJitCache* jit_cache

~200 строк
```

**2.2 Per-thread Feedback Vectors**

```
Файлы: src/objects/feedback-vector.cc

Изменения:
  - При первом вызове функции в M треде:
      создать локальный FeedbackVector
      скопировать глобальный как стартовую точку
  - Обновляем только локальный feedback
  - Деоптимизация → только локальный Code object

Тест: deopt в M1 не влияет на M2
~150 строк
```

**2.3 Ignition: per-thread call path**

```
Файлы: src/interpreter/interpreter-assembler.cc

Изменения:
  CallFunction опкод:
    1. lookup в local jit cache по SFI адресу
    2. есть локальный Code → выполняем
    3. нет → инкремент per-thread call counter
    4. counter > threshold → компилируем локально
    5. иначе → интерпретируем bytecode напрямую

  Bytecode читается из SharedFunctionInfo.bytecode_array
  никогда не из JSFunction.code (там JIT main треда)

Тест: горячая функция компилируется независимо per-thread
~80 строк
```

---

### Фаза 3: Goroutine runtime

**3.1 Goroutine struct**

```
Новые файлы: src/runtime/goroutine.h/.cc

struct Goroutine {
    // Стек в shared heap (любой M может продолжить)
    SharedStack stack
    Address stack_top
    Address stack_bottom

    // Состояние Ignition (сохраняется при парковке)
    InterpreterRegisters regs  // register file
    Address resume_pc          // куда вернуться

    // Pending IO (обычно 0-1 элементов)
    SmallVector<PendingIO, 2> pending_ios

    // Результат последней IO операции
    IOResult io_result

    // Планировщик
    GoroutineStatus status     // RUNNABLE/RUNNING/WAITING/DEAD
    ThreadId current_thread    // какой M выполняет сейчас

    // GC
    v8::Global<v8::Context> context
}

struct PendingIO {
    int fd
    int events                 // UV_READABLE / UV_WRITABLE
    uv_poll_t* handle
    LoopId loop_id
}

struct IOResult {
    bool fd_ready
    int events
    int error
}

~200 строк
```

**3.2 Горутинный стек в shared heap**

```
Файлы: src/runtime/goroutine.cc

При создании горутины:
  stack = shared_heap->alloc(INITIAL_STACK_SIZE)  // 8KB

При парковке (mcall аналог):
  копируем Ignition register file → goroutine.regs
  сохраняем resume_pc
  M тред освобождается

При пробуждении:
  восстанавливаем regs на стек M треда
  прыгаем на resume_pc

Stack growth:
  при переполнении → alloc новый стек × 2
  копируем содержимое
  обновляем все указатели на стек

~200 строк
```

**3.3 Планировщик**

```
Новые файлы: src/runtime/scheduler.h/.cc

struct Scheduler {
    // Per-thread локальные очереди (lock-free, single consumer)
    PerThread<LockFreeDeque<Goroutine*>> local_queues

    // Глобальная очередь (mutex)
    Mutex global_mu
    Deque<Goroutine*> global_queue

    // Динамический пул тредов
    DynamicThreadPool thread_pool
}

Goroutine* FindNext(ThreadId tid):
    // 1. каждые 61 итерацию → глобальная (не голодает)
    if schedtick % 61 == 0:
        return global_queue.pop()

    // 2. своя локальная очередь
    if g = local_queues[tid].pop():
        return g

    // 3. work stealing — берём половину у случайного M
    if g = StealFrom(random_tid):
        return g

    // 4. глобальная очередь
    if g = global_queue.pop():
        return g

    // 5. нет работы → опрашиваем uv_loop
    uv_run(m_loops[tid], UV_RUN_NOWAIT)
    return null

~300 строк
```

**3.4 JS API горутин**

```javascript
// go() — запустить горутину
function go(fn) {
    const g = runtime.newGoroutine(fn)
    scheduler.enqueue(g)
    return g.promise  // Promise с результатом
}

// Использование
const result = await go(async () => {
    return compute()
})
```

```
~150 строк
```

---

### Фаза 4: Unified Thread Pool

**4.1 Объединённый пул**

```
Новые файлы: src/runtime/unified-thread-pool.h/.cc

struct UnifiedTask {
    enum Type { GOROUTINE, LIBUV_WORK } type
    union {
        Goroutine* goroutine
        uv_work_t* uv_work
    }
}

void WorkerLoop(ThreadId tid):
    while true:
        // обрабатываем отложенные uv_poll_stop запросы
        while h = stop_requests[tid].pop():
            uv_poll_stop(h)
            delete h

        // проверяем safepoint (GC)
        if safepoint_requested:
            HandleSafepoint()

        // берём задачу
        g = scheduler.FindNext(tid)
        if g:
            ExecuteGoroutine(g)
        else:
            // нет горутин → опрашиваем свой uv_loop
            uv_run(m_loops[tid], UV_RUN_NOWAIT)
            // если совсем нечего делать → спим
            WaitForWork()

~200 строк
```

**4.2 Динамический размер пула**

```
Мониторинг в scheduler loop:

  active_count     — всего тредов в пуле
  syscall_count    — сколько в blocking syscall
  available = active - syscall_count

  если available < MIN_JS_WORKERS (обычно = кол-во ядер):
      SpawnWorker()  // новый M тред

  если active > MAX_THREADS (default 10000):
      лишние треды → idle pool

  SpawnWorker():
      создать тред
      инициализировать uv_loop для него
      зарегистрировать в ExternalThreadRegistry (GC)
      запустить WorkerLoop()

~100 строк
```

**4.3 libuv интеграция**

```
Файлы: src/runtime/libuv-integration.cc

- libuv fs/dns/crypto операции → постят результат
  в global goroutine queue вместо main тред callback
- uv_work_t: work_cb выполняется в M треде
  after_work_cb будит горутину через scheduler

~150 строк
```

---

### Фаза 5: Per-M uv_loop (netpoller)

**5.1 Инициализация per-M loops**

```
Файлы: src/runtime/m-event-loop.h/.cc

При старте каждого M треда:
  uv_loop_t* m_loop = new uv_loop_t
  uv_loop_init(m_loop)
  uv_async_init(m_loop, &stop_async, on_stop_requests)

При остановке M треда:
  uv_loop_close(m_loop)
  delete m_loop

~80 строк
```

**5.2 Регистрация fd горутины**

```cpp
// Горутина делает net.read(conn):

void ParkOnIO(Goroutine* g, int fd, int events) {
    // 1. пробуем read() сразу — может данные уже есть
    int n = read(fd, buf, size)
    if n > 0:
        // данные есть → не паркуемся
        g->io_result = { data: buf, size: n }
        return

    // 2. EAGAIN → данных нет → паркуемся
    auto* handle = new uv_poll_t
    handle->data = g
    uv_poll_init(m_loops[current_tid], handle, fd)
    uv_poll_start(handle, events, on_io_ready)

    // сохраняем в горутине
    g->pending_ios.push({ fd, events, handle, current_tid })
    g->status = WAITING

    // M берёт следующую горутину
}

~100 строк
```

**5.3 IO ready callback**

```cpp
void on_io_ready(uv_poll_t* handle, int status, int events) {
    Goroutine* g = (Goroutine*)handle->data

    // помечаем результат
    g->io_result.fd_ready = true
    g->io_result.events = events
    g->io_result.error = status < 0 ? status : 0

    // останавливаем наблюдение
    uv_poll_stop(handle)
    delete handle
    g->pending_ios.clear()

    // горутина runnable
    scheduler->enqueue(g)
}

// Горутина просыпается и сама читает:
void ResumeAfterIO(Goroutine* g, int fd) {
    // fd гарантированно готов (epoll сказал)
    // читаем прямо в shared heap — zero copy
    auto* buf = shared_heap->alloc(READ_SIZE)
    int n = read(fd, buf, READ_SIZE)

    // результат → в register file горутины
    // JS код получает Buffer без копирования
}

~120 строк
```

**5.4 Перерегистрация при select (edge case)**

```cpp
// Только для случая когда горутина ждёт несколько fd
// (select с несколькими каналами)

void ReregisterPendingIO(Goroutine* g, ThreadId from, ThreadId to) {
    // горутина мигрирует пока ждёт несколько fd
    if g->pending_ios.empty(): return

    for io in g->pending_ios:
        // просим старый M остановить handle
        stop_requests[from].push(io.handle)
        uv_async_send(&stop_async_handles[from])

        // регистрируем на новом loop
        auto* new_handle = new uv_poll_t
        new_handle->data = g
        uv_poll_init(m_loops[to], new_handle, io.fd)
        uv_poll_start(new_handle, io.events, on_io_ready)

        io.handle = new_handle
        io.loop_id = to

~80 строк
```

---

### Фаза 6: Channels и примитивы синхронизации

**6.1 Channel**

```cpp
// src/runtime/channel.h

struct Channel {
    // Ring buffer — SharedValue в shared heap
    SharedValue* buffer        // аллоцирован в shared heap
    uint32_t capacity

    std::atomic<uint32_t> head
    std::atomic<uint32_t> tail
    std::atomic<uint32_t> count
    std::atomic<bool> closed

    // Waiters — горутины заблокированные на send/receive
    // futex для пробуждения
    std::atomic<uint32_t> send_futex
    std::atomic<uint32_t> recv_futex
    LockFreeQueue<Goroutine*> send_waiters
    LockFreeQueue<Goroutine*> recv_waiters
}

// SharedValue — 8 байт tagged pointer
// примитивы inline, объекты — pointer в shared heap
union SharedValue {
    uint64_t raw
    struct { uint64_t tag:3, int_val:61   } as_int
    struct { uint64_t tag:3, offset:61    } as_object
    struct { uint64_t tag:3, str_offset:61} as_string
}

~250 строк
```

```javascript
// JS API
const ch = new Channel(16)       // буфер 16 элементов
await ch.send(value)
const value = await ch.receive()
ch.close()
for await (const v of ch) { ... }
```

**6.2 SharedMutex / SharedRWMutex**

```cpp
struct SharedMutex {
    // 0 = unlocked, 1 = locked, 2 = locked + waiters
    std::atomic<uint32_t> state
    LockFreeQueue<Goroutine*> waiters

    // быстрый путь: CAS без syscall
    // медленный путь: парковка горутины (не треда!)
}

struct SharedRWMutex {
    // > 0: число читателей
    // -1:  писатель
    std::atomic<int32_t> state
    LockFreeQueue<Goroutine*> read_waiters
    LockFreeQueue<Goroutine*> write_waiters
}

// Важно: lock() паркует ГОРУТИНУ, не тред
// M тред свободен пока горутина ждёт мьютекс

~150 строк
```

**6.3 select**

```javascript
const { value, from } = await select(
    ch1.receive(),
    ch2.receive(),
    timeout(1000)
)
```

```cpp
// Регистрируем waiter на все каналы одновременно
// std::atomic<bool> resolved — только первый побеждает

void Select(vector<ChannelOp> ops, Goroutine* g) {
    auto resolved = make_shared<atomic<bool>>(false)

    for op in ops:
        RegisterWaiter(op.channel, [g, resolved, op](SharedValue val) {
            bool expected = false
            if resolved->compare_exchange_strong(expected, true):
                g->select_result = { value: val, from: op.channel }
                // отменяем остальные waiters
                CancelOtherWaiters(ops, op)
                scheduler->enqueue(g)
        })
}

~150 строк
```

---

### Фаза 7: Переименование — Coroutines / Co

**Цель:** убрать все отсылки к Go и горутинам. Это самостоятельная концепция.

**7.1 Переименование рантайма**

```
Внутренние имена (C++):
  G (Goroutine)          → Co (Coroutine)
  M (Machine)            → M (оставить — нейтральное)
  GState::Grunnable      → CoState::Runnable
  GState::Grunning       → CoState::Running
  GState::Gwaiting       → CoState::Waiting
  GState::Gdead          → CoState::Dead
  GOMAXPROCS             → CO_MAXPROCS (или просто MAXPROCS)

Файлы:
  src/goroutine/         → src/coroutine/
  goroutine_wrap.cc      → coroutine_wrap.cc
  lib/goroutine.js       → lib/coroutine.js
  lib/internal/goroutine.js → lib/internal/coroutine.js
```

**7.2 JS Public API**

```javascript
// Было:
const { go, yield, goid } = require('goroutines')
go(fn, ...args)
yield()
goid()

// Станет:
const Co = require('coroutines')
Co.run(fn, ...args)   // запустить корутину
Co.yield()            // уступить управление
Co.id()               // id текущей корутины
Co.exit()             // завершить текущую корутину

// Каналы:
const ch = new Co.Channel(16)
await ch.send(value)
const value = await ch.recv()
ch.close()
for await (const v of ch) { ... }

// Sync:
const mu = new Co.Mutex()
const rwmu = new Co.RWMutex()

// Select:
const { value, from } = await Co.select(
    ch1.recv(),
    ch2.recv(),
    Co.timeout(1000)
)
```

**7.3 V8 патчи**

```
Переименовать без изменения логики:
  v8_goroutine_thread        → v8_coroutine_thread
  v8_goroutine_p_state_*     → v8_coroutine_m_state_*
  v8_goroutine_gc_*          → v8_coroutine_gc_*
  v8_goroutine_local_heap_*  → v8_coroutine_local_heap_*
  goroutine-thread.h/cc      → coroutine-thread.h/cc
  goroutine-thread-state.h/cc → coroutine-thread-state.h/cc
  goroutine-gc-roots.h/cc    → coroutine-gc-roots.h/cc
  goroutine-local-heap.h/cc  → coroutine-local-heap.h/cc
  goroutine-safepoint.h/cc   → coroutine-safepoint.h/cc
```

**7.4 Env переменная**

```
NODE_GOMAXPROCS → CO_MAXPROCS
```

**Порядок:** делается в самом конце одним большим rename-рефакторингом.
Все семантические изменения к этому моменту уже завершены.
Это чисто механическая операция — grep/sed + проверка компиляции.
```