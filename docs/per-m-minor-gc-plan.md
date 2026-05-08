# Per-M Minor GC — Implementation Plan

## Problem

M-threads can't trigger Minor GC. When NewSpace is full, objects promote to OldSpace → excessive Mark-Compact (~10 per test), zero Minor MS.

## Solution

Each M-thread owns NewSpace pages exclusively and can collect them independently without stopping other M-threads.

## Key Invariants

1. Goroutine yields → goes to **same M's local queue** (not global)
2. Each M tracks its own NewSpace pages via per-M bitmap (no V8 header changes)
3. Cross-M young refs only on work-steal → promote **stolen G's pages** to OldSpace
4. Per-M Minor GC: mark/sweep only own pages, filter remembered set by target page

## Phase 1: Per-M Page Tracking

**Goal**: M knows its pages; pages identified by bitmap lookup.

| Where | What |
|-------|------|
| `src/goroutine/runtime.h` | Add `PerMState::page_bitmap` — `std::bitset` or `uint64_t[]` indexed by `PageIndex(addr)` |
| `src/goroutine/g.h` | Add `G::alloc_pages_` — `SmallVector<PageMetadata*, 4>` of pages G got LABs on |
| `main-allocator.cc` EnsureAllocation | On new LAB page: `current_g->alloc_pages_.push_back(page)` + set bit in M's bitmap |
| `main-allocator.cc` FreeLinearAllocationArea | Clear page from bitmap on release |

**PageIndex**: `(page_address >> kPageSizeBits) & kBitmapMask` — O(1) lookup.

**No V8 header changes.** Bitmap lives in goroutine runtime code.

## Phase 2: Scheduler — Local Yield + Work-Steal Promotion  

**Goal**: Yield → local queue. Steal → promote only stolen G's pages.

| Where | What |
|-------|------|
| `src/goroutine/context.cc` YieldG | Push to `M::local_queue` instead of global |
| `src/goroutine/scheduler.cc` StealHalf | After steal: iterate `stolen_g->alloc_pages_`, flip each to OldSpace |
| Page flip | `page->owner = OLD_SPACE` (metadata); remove from M's bitmap |
| G cleanup | `stolen_g->alloc_pages_.clear()` — starts fresh on new M |

**Shared pages**: If page has allocations from multiple Gs on same M, and only one G is stolen — page gets promoted. Other G's objects on that page are now in OldSpace (valid, collected by Major GC later). Acceptable for v1.

## Phase 3: Per-M Free List

**Goal**: Each M allocates from own free list — zero mutex contention on fast path.

| Where | What |
|-------|------|
| `src/goroutine/runtime.h` | `PerMState::free_list` — V8 `FreeList` instance per M |
| `main-allocator.cc` TryAllocationFromFreeList | Goroutines: use per-M free list (no global mutex!) |
| `main-allocator.cc` TryAllocatePage | Goroutines: page budget = `MaxCapacity / num_threads` per M |
| Trigger | When per-M pages full + free list empty → per-M Minor GC (Phase 4) |

**LAB bump-pointer (fast path)**: unchanged, still zero-overhead via r13.

## Phase 4: Per-M Minor GC Collector

**Goal**: Lightweight mark-sweep on M's pages only. Other M-threads unaffected.

New file: `deps/v8/src/heap/goroutine-minor-gc.{h,cc}`

### Algorithm

```
PerMMinorGC::Collect(m_id):
  1. ROOTS:
     - Scan stacks of Gs in M's local_queue + current G
     - (Goroutine stacks on mmap — iterate via saved TLT c_entry_fp)
  
  2. REMEMBERED SET (old→young filter):
     for each old_page with slot_set[OLD_TO_NEW]:
       for each slot in slot_set:
         target = *slot
         if M.page_bitmap.Test(PageIndex(target)):
           Mark(target)  // our object — mark live
  
  3. MARK (BFS from roots):
     while worklist not empty:
       obj = worklist.pop()
       for each field in obj:
         if field points to M's page → mark + push
         if field points elsewhere → ignore (not our scope)
  
  4. SWEEP:
     for each page in M's pages:
       iterate objects:
         if unmarked → free (add to M's free_list)
         if marked → clear mark bit
  
  5. RESUME allocation from M's free_list
```

### Concurrency Safety

- **Other M-threads**: continue allocating on THEIR pages — zero contention
- **Write barriers from others**: may add new RS entries pointing to our pages during GC — conservative (extra roots = safe, we never miss live objects)
- **Global IncrementalMarking**: completely untouched — per-M GC is independent
- **Main thread**: may be running JS — doesn't touch M's pages

### Trigger

In `EnsureAllocation` when `TryAllocatePage` fails for this M:
```cpp
if (v8_goroutine_thread) {
  PerMMinorGC::Collect(tls_m_id, isolate);
  // Retry after GC freed space
  return TryAllocationFromPerMFreeList(size_in_bytes, origin);
}
```

## Phase 5: Integration

- Flag: `--goroutine-per-m-gc` (default on)
- Trace: `--trace-goroutine-gc` — per-M GC events
- Fallback: if per-M GC doesn't free enough → promote all M's pages to OldSpace (current behavior)
- Global Mark-Compact: still works — sees all pages (both M-owned young + global old)

## Dependency Order

```
Phase 1 (page tracking)
  → Phase 3 (per-M free list)  
  → Phase 2 (scheduler: local yield + steal promotion)
  → Phase 4 (per-M Minor GC)
  → Phase 5 (integration)
```

## Risks & Mitigations

| Risk | Mitigation |
|------|-----------|
| Shared pages on steal | Promote whole page — objects in OldSpace are valid |
| RS scan is global | Filter is O(1) per entry; optimize with per-M RS later if needed |
| Per-M GC during global GC | Check `heap->IsInGC()` — skip per-M GC, participate in safepoint |
| Page fragmentation (1 G = 1 page min) | Pages are 256KB; typical goroutine allocates 64KB+ → <4× waste |

