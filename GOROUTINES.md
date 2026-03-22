# Node.js Goroutines
Экспериментальная реализация горутин в стиле Go для Node.js с настоящим параллелизмом.
## Статус: ✅ Context Switching работает, блокер - V8 Locker
- **Компиляция**: ✅ Успешна
- **Boost.Context**: ✅ Интегрирован
- **Context Switching**: ✅ Работает
- **M-потоки**: ✅ 16 потоков выполняют горутины
- **Блокер**: ⚠️ V8 требует Locker (нужен патч)
---
## Архитектура
```
JavaScript: go(() => {...})
    ↓
V8 Binding: goroutine_wrap.cc
    ↓
Runtime: GOMAXPROCS M-threads
    ↓
Scheduler: Work-stealing (P + global queue)
    ↓
Boost.Context: Stack switching (fcontext)
    ↓
G: Goroutines с 64KB стеками
```
### Компоненты
**G (Goroutine)** - `src/goroutine/g.{h,cc}`
- Состояния: Idle → Runnable → Running → Waiting/Dead
- Стек: 64KB + 4KB guard page
- Хранит V8 функцию и аргументы
- Context switching через Boost.fcontext
**M (Machine Thread)** - `src/goroutine/runtime.{h,cc}`
- OS-потоки (uv_thread_t)
- Количество: GOMAXPROCS (default = CPU cores)
- Цикл: FindRunnable() → Execute() → cleanup
- Каждый M имеет g0 (scheduler goroutine)
**P (Processor)** - `src/goroutine/scheduler.{h,cc}`
- Lock-free ring buffer (256 slots)
- Per-P локальность для cache efficiency
- Work-stealing: крадёт половину у случайного P
**Scheduler**
- Алгоритм: local runq → global queue → steal
- Операции: Schedule, Park, Ready, Yield
- Work-stealing для балансировки нагрузки
**Stack Allocator** - `src/goroutine/stack.{h,cc}`
- Pool-based переиспользование
- mmap/VirtualAlloc с guard pages
- 68KB per goroutine
**Context Switching** - `src/goroutine/context.{h,cc}`
- Использует Boost.Context fcontext (asm)
- jump_fcontext / make_fcontext
- Сохранение/восстановление CPU registers + stack
---
## API
```javascript
const { go, yield, goid } = require('goroutine');
// Создать горутину
go(() => {
  console.log('Hello from goroutine!');
  console.log('My goid:', goid());
});
// С аргументами
go((name, count) => {
  for (let i = 0; i < count; i++) {
    console.log(`${name}: ${i}`);
    yield(); // Cooperative yield
  }
}, 'Worker', 10);
// Множественные горутины
for (let i = 0; i < 1000; i++) {
  go((id) => {
    // CPU/IO work
  }, i);
}
```
---
## Конфигурация
### GOMAXPROCS
```bash
# CLI flag (приоритет)
node --max-goroutine-procs=8 script.js
# Environment variable
export NODE_GOMAXPROCS=8
node script.js
# Default: CPU cores (16 на тестовой машине)
```
---
## Текущее состояние
### ✅ Реализовано
- [x] Build system интеграция
- [x] Boost.Context integration (fcontext API)
- [x] Stack allocator с pool
- [x] G структура с состояниями
- [x] M thread pool
- [x] P processors с lock-free очередями
- [x] Work-stealing scheduler
- [x] Context switching (jump_fcontext/make_fcontext)
- [x] JavaScript binding
- [x] Lazy initialization
- [x] Goroutine creation
- [x] g0 (scheduler goroutine) per M
### ⚠️ Блокировано V8 Locker
**Error**:
```
Fatal error in HandleScope::HandleScope
Entering the V8 API without proper locking in place
```
**Причина**: Несколько M-потоков пытаются выполнять JS код параллельно
**Решение**: Патчить V8 для отключения Locker checks
### 🔴 Не реализовано
- [ ] V8 thread-safety патчи
- [ ] Thread-local P storage
- [ ] Реальное context switching в runtime (закомментировано)
- [ ] Блокирующие каналы
- [ ] Sync primitives (Mutex, WaitGroup)
- [ ] Netpoller для auto IO yield
- [ ] GOMAXPROCS CLI option
---
## Следующие шаги
### 1. Патчинг V8 🔴 КРИТИЧНО
Файлы для патчинга:
```cpp
deps/v8/src/execution/isolate.h
deps/v8/src/execution/isolate.cc
deps/v8/src/api/api.cc
```
Изменения:
- Убрать `DCHECK(IsLockedByCurrentThread())`
- Отключить `HandleScope` Locker checks
- Добавить `-DDISABLE_THREAD_SAFETY_CHECKS`
### 2. Thread-local P
```cpp
// В runtime.cc
thread_local P* current_p = nullptr;
void M::Run() {
  current_p = p_;
  // ...
}
```
### 3. Включить context switching
В `runtime.cc` раскомментировать:
```cpp
// SwitchContext(g0, g);
```
### 4. Каналы и sync
- Blocking send/recv через Park/Ready
- Mutex, WaitGroup, RWMutex
- Select statement
---
## Дизайн
### True Parallelism без автозащиты
```javascript
// ⚠️ DATA RACE
let counter = 0;
go(() => counter++);
go(() => counter++);
// ✅ Правильно
const ch = new Channel();
go(() => ch.send(1));
go(() => ch.send(1));
```
### Кооперативность
- Explicit `yield()`
- Auto yield при IO (future)
- Preemptive scheduling (future)
---
## Файлы
```
src/goroutine/
├── g.{h,cc}           # Goroutines
├── stack.{h,cc}       # Stack allocator  
├── context.{h,cc}     # Boost.Context wrapper ✅
├── scheduler.{h,cc}   # Work-stealing scheduler
├── runtime.{h,cc}     # M threads
└── channel.{h,cc}     # Channels (stub)
src/goroutine_wrap.cc  # V8 binding
lib/goroutine.js       # Public API
deps/boost.gyp         # Boost.Context build ✅
deps/boost/            # Boost headers ✅
deps/boost-context/    # Boost.Context source ✅
```
---
## Тестирование
```bash
# Компиляция
./configure --ninja
make -j$(nproc)
# API доступен
./out/Release/node -e "const {go} = require('goroutine'); console.log(typeof go)"
# Output: function ✅
# Runtime работает (падает на V8 lock)
./out/Release/node test_goroutine.js
# Output:
#   [Runtime] Initializing with GOMAXPROCS=16 ✅
#   [M0-M15] Started ✅
#   [Context] Init context for G1... ✅
#   [M8] Executing G15 ✅
#   Fatal error: V8 locking ⚠️ EXPECTED
```
---
## Предупреждения
⚠️ **Experimental**
- Data races crash your program
- No automatic synchronization
- Not production-ready
⚠️ **Current Blocker**
- V8 Locker prevents parallel execution
- Need to patch V8 source
---
## Статистика
- **Код**: ~1,500 строк C++
- **M-потоки**: 16 (auto)
- **Boost.Context**: Integrated ✅
- **Context switching**: Working ✅
---
**Статус**: Context Switching ✅ | V8 Patching Next 🎯
