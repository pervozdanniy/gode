// Goroutine Map-transition mutex.
//
// Protects TransitionToDataProperty / TransitionToAccessorProperty from
// goroutine-to-goroutine races: two workers simultaneously finding no existing
// transition, both creating a new Map, and both inserting into the parent
// Map's transition array — corrupting it.

#ifndef V8_EXECUTION_GOROUTINE_SHAPE_LOCK_H_
#define V8_EXECUTION_GOROUTINE_SHAPE_LOCK_H_

extern "C" {
  // Mutex-based lock for Map transition creation (TransitionToDataProperty).
  // Must be taken by ALL callers of TransitionToDataProperty (any thread).
  // Uses try_lock + LocalHeap::Safepoint() spin on M-threads so GC can
  // always proceed without deadlocking on this mutex.
  void v8_goroutine_map_transition_lock();
  void v8_goroutine_map_transition_unlock();
}

#endif  // V8_EXECUTION_GOROUTINE_SHAPE_LOCK_H_
