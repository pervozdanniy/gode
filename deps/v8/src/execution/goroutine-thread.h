// Goroutine thread detection
#ifndef V8_GOROUTINE_THREAD_H_
#define V8_GOROUTINE_THREAD_H_

// Set this to true in M-threads (goroutine worker threads)
extern thread_local bool v8_goroutine_thread;

#endif  // V8_GOROUTINE_THREAD_H_

