// Goroutine thread detection implementation
#include "src/execution/goroutine-thread.h"

// False by default (main thread)
thread_local bool v8_goroutine_thread = false;

