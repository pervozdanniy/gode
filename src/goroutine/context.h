#ifndef SRC_GOROUTINE_CONTEXT_H_
#define SRC_GOROUTINE_CONTEXT_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "v8.h"

namespace node {
namespace goroutine {

class G;

// Initialize a goroutine's fcontext (called from G constructor).
// Returns opaque fcontext_t stored as void*.
void* InitContext(G* g, void* stack_top);

// Run goroutine G on its own stack via Boost.Context jump.
// Returns when G yields or finishes.
void RunG(G* g, v8::Isolate* isolate);

// Yield current goroutine back to the scheduler.
// Called from within a running goroutine (JS yield()).
void YieldG();

// Get the currently executing goroutine on this thread, or nullptr.
G* CurrentG();

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_CONTEXT_H_

