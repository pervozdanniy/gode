// Goroutine thread flag — minimal V8-internal header.
// This file intentionally has no dependency on goroutine runtime headers.
// Defined in goroutine-thread.cc (src/goroutine/).
#ifndef V8_EXECUTION_GOROUTINE_FLAG_H_
#define V8_EXECUTION_GOROUTINE_FLAG_H_

// Set to true on M-threads (goroutine worker threads), false on main thread.
extern thread_local __attribute__((tls_model("initial-exec"))) bool v8_goroutine_thread;

// Real Isolate* for the current M-thread. CEntryStub computes Isolate* as
// r13 - kRootRegisterBias which points to per-M IsolateData clone, not the
// real Isolate. This TLS holds the correct pointer for RUNTIME_FUNCTION fixup.
extern thread_local __attribute__((tls_model("initial-exec"))) void* v8_goroutine_real_isolate;

#endif  // V8_EXECUTION_GOROUTINE_FLAG_H_

