## Финальный план реализации

---

### Фаза 1: Патч V8 — общий heap

**1.1 GC Safepoints для внешних тредов**

```
Файлы: src/heap/safepoint.cc, safepoint.h

Изменения:
  - ExternalThreadRegistry — список всех M тредов
  - При старте M треда → регистрация в registry
  - IsolateSafepoint::StopTheWorld() → останавливает все M
  - M треды проверяют safepoint request в scheduler loop
    между горутинами (не на каждой инструкции)

Тест: 4 M треда читают heap пока GC работает → нет крашей
~130 строк
```

**1.2 GC не эвакуирует shared объекты**

```
Файлы: src/heap/mark-compact.cc, heap-object.h

Изменения:
  - Флаг is_shared в Map (Shape) объекта
  - Все новые объекты → is_shared = true по умолчанию
  - Scavenger: shared объекты → сразу old generation
  - Mark-Compact: не эвакуировать shared объекты

Тест: M тред держит pointer → GC не ломает pointer
~80 строк
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

**1.4 Shared String Table**

```
Файлы: src/strings/string-table.cc

Изменения:
  - Мьютекс на string internment
  - String objects → always old generation

Тест: M тред создаёт строку → main тред читает → корректно
~80 строк
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

### Итоговый объём

```
Фаза 1: Патч V8 (общий heap)
  GC safepoints                  ~130 строк
  GC не эвакуирует shared        ~80 строк
  SeqLock shape transitions      ~150 строк
  Shared string table            ~80 строк
  Итого V8:                      ~440 строк

Фаза 2: Per-thread JIT
  PerThreadJitCache              ~200 строк
  Per-thread FeedbackVector      ~150 строк
  Ignition call path             ~80 строк
  Итого V8:                      ~430 строк

Фаза 3: Goroutine runtime
  Goroutine struct               ~200 строк
  Stack в shared heap            ~200 строк
  Планировщик + work stealing    ~300 строк
  JS API                         ~150 строк
  Итого:                         ~850 строк

Фаза 4: Unified thread pool
  UnifiedThreadPool              ~200 строк
  Динамический размер            ~100 строк
  libuv интеграция               ~150 строк
  Итого:                         ~450 строк

Фаза 5: Per-M uv_loop
  Инициализация loops            ~80 строк
  Регистрация fd / парковка      ~100 строк
  IO ready callback + read       ~120 строк
  Перерегистрация (select)       ~80 строк
  Итого:                         ~380 строк

Фаза 6: Channels + примитивы
  Channel                        ~250 строк
  SharedMutex / RWMutex          ~150 строк
  select                         ~150 строк
  Итого:                         ~550 строк

─────────────────────────────────────────────
Патч V8:                         ~870 строк
Новый рантайм:                   ~2230 строк
Итого:                           ~3100 строк
```

---

### Порядок разработки

```
Недели 1-3:   Фаза 1 — патч V8
  Тест: M треды читают heap во время GC → нет крашей
  Тест: shape transition во время чтения → корректно

Недели 4-6:   Фаза 2 — per-thread JIT
  Тест: функция компилируется независимо per-thread
  Тест: deopt в M1 не влияет на M2

Недели 7-9:   Фаза 3 — горутины + планировщик
  Тест: горутина мигрирует между тредами
  Тест: 10k горутин на 4 тредах, work stealing
  Тест: замыкания доступны после миграции

Недели 10-11: Фаза 4 — unified thread pool
  Тест: fs.readFile в горутине не блокирует пул
  Тест: динамическое создание тредов при syscall burst

Недели 12-13: Фаза 5 — per-M uv_loop
  Тест: net.read() паркует горутину корректно
  Тест: данные читаются прямо в shared heap
  Тест: 10k одновременных соединений

Недели 14-15: Фаза 6 — channels
  Тест: producer/consumer между горутинами
  Тест: select с таймаутом
  Тест: закрытый канал

Недели 16-17: интеграция + бенчмарки
  Тест: реальный HTTP сервер
  Бенчмарк: сравнение с Node.js на data-heavy задачах
  Бенчмарк: передача больших объектов vs worker_threads
```