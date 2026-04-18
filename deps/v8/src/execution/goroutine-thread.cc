// Goroutine thread detection implementation
#include "src/execution/goroutine-thread.h"

// False by default (main thread)
thread_local __attribute__((tls_model("initial-exec"))) bool v8_goroutine_thread = false;

// Real Isolate* for this M-thread (nullptr on main thread).
thread_local __attribute__((tls_model("initial-exec"))) void* v8_goroutine_real_isolate = nullptr;

