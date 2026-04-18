// SeqLock for shape transitions (Phase 1.3).
//
// Problem: a goroutine may be migrated from M1 to M2 while the main thread
// performs a JSObject::MigrateToMap on an object that the goroutine holds a
// reference to.  MigrateToMap is a compound operation:
//
//   1. Allocate new backing store (PropertyArray)
//   2. Copy old fields into new backing store
//   3. object->SetProperties(new_store)   <- backing store pointer changes
//   4. object->set_map(new_map, kRelease) <- map pointer changes (LAST)
//
// If a goroutine resumes between steps 3 and 4 it sees a new backing store
// under an old map -> wrong field indices -> crash.
//
// Solution: a single global SeqLock (one per process; we have one Isolate).
//
//   Writer (main thread, wrapping MigrateToMap):
//     v8_goroutine_shape_seqlock_begin()  -> seq becomes odd  (write in-flight)
//     ... MigrateToMap body ...
//     v8_goroutine_shape_seqlock_end()    -> seq becomes even (write done)
//
//   Reader (M thread, in YieldG right after jump_fcontext returns):
//     v8_goroutine_shape_seqlock_wait()
//       spins while seq is odd -> goroutine stays suspended until transition
//       completes, then resumes on a fully consistent heap view.
//
// Overhead on fast path: 1 atomic load per goroutine resume (~1 ns).
// Spin happens only during an actual shape transition (rare, us-range).

#ifndef V8_EXECUTION_GOROUTINE_SHAPE_SEQLOCK_H_
#define V8_EXECUTION_GOROUTINE_SHAPE_SEQLOCK_H_

extern "C" {
  // Called by main thread BEFORE any shape transition (MigrateToMap et al).
  void v8_goroutine_shape_seqlock_begin();

  // Called by main thread AFTER the shape transition completes.
  void v8_goroutine_shape_seqlock_end();

  // Called by M thread before resuming a goroutine.
  // Spins with cpu_relax until no write is in flight (seq is even).
  void v8_goroutine_shape_seqlock_wait();

  // Mutex-based lock for Map transition creation (TransitionToDataProperty).
  // Prevents goroutine-to-goroutine races where two workers simultaneously
  // find no existing transition, both create a new Map, and both insert into
  // the parent Map's transition array — corrupting it.
  // Must be called by ALL callers of TransitionToDataProperty (any thread).
  void v8_goroutine_map_transition_lock();
  void v8_goroutine_map_transition_unlock();
}

#endif  // V8_EXECUTION_GOROUTINE_SHAPE_SEQLOCK_H_
