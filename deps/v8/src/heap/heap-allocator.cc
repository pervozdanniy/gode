// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/heap-allocator.h"

#include "src/base/logging.h"
#include "src/common/globals.h"
#include "src/execution/isolate.h"
#include "src/execution/goroutine-flag.h"
#include "src/heap/heap-allocator-inl.h"

// GOROUTINE PATCH: GC safepoint hooks for M-thread allocation failure path.
extern "C" void v8_goroutine_safepoint_park(v8::internal::Isolate* isolate);
extern "C" void v8_goroutine_safepoint_unpark();
extern "C" void v8_goroutine_lab_sync_before_run();
extern "C" void v8_goroutine_lab_sync_after_run();

namespace v8 { namespace internal {
// GOROUTINE PATCH: persistent flag — set when ANY M-thread has kYoung
// allocator backed by PagedNewSpace. Used by Mark-Compact to force in-place
// page promotion (no object movement).
std::atomic<bool> v8_goroutine_uses_newspace_{false};
} }  // namespace v8::internal
#include "src/heap/heap-inl.h"
#include "src/heap/local-heap.h"
#include "src/heap/local-heap-inl.h"
#include "src/logging/counters.h"

namespace v8 {
namespace internal {

class Heap;

HeapAllocator::HeapAllocator(LocalHeap* local_heap)
    : local_heap_(local_heap), heap_(local_heap->heap()) {}

void HeapAllocator::Setup(LinearAllocationArea* new_allocation_info,
                          LinearAllocationArea* old_allocation_info) {
  for (int i = FIRST_SPACE; i <= LAST_SPACE; ++i) {
    spaces_[i] = heap_->space(i);
  }

  if ((heap_->new_space() || v8_flags.sticky_mark_bits) &&
      local_heap_->is_main_thread()) {
    new_space_allocator_.emplace(
        local_heap_,
        v8_flags.sticky_mark_bits
            ? static_cast<SpaceWithLinearArea*>(heap_->sticky_space())
            : static_cast<SpaceWithLinearArea*>(heap_->new_space()),
        MainAllocator::IsNewGeneration::kYes, new_allocation_info);
  }

  old_space_allocator_.emplace(local_heap_, heap_->old_space(),
                               MainAllocator::IsNewGeneration::kNo,
                               old_allocation_info);

  trusted_space_allocator_.emplace(local_heap_, heap_->trusted_space(),
                                   MainAllocator::IsNewGeneration::kNo);
  code_space_allocator_.emplace(local_heap_, heap_->code_space(),
                                MainAllocator::IsNewGeneration::kNo);

  if (heap_->isolate()->has_shared_space()) {
    shared_space_allocator_.emplace(local_heap_,
                                    heap_->shared_allocation_space(),
                                    MainAllocator::IsNewGeneration::kNo);
    shared_lo_space_ = heap_->shared_lo_allocation_space();

    shared_trusted_space_allocator_.emplace(
        local_heap_, heap_->shared_trusted_allocation_space(),
        MainAllocator::IsNewGeneration::kNo);
    shared_trusted_lo_space_ = heap_->shared_trusted_lo_allocation_space();
  }
}

// GOROUTINE PATCH: Recreate old_space_allocator_ with a custom LAB pointer
// so that it shares top/limit with per-M IsolateData::old_allocation_info_
// (accessed by Ignition via r13). Eliminates LabSync overhead.
// Also creates new_space_allocator_ backed by old space, using per-M
// IsolateData::new_allocation_info_ (immediately before old_allocation_info_
// in IsolateData layout). This gives CSA's kYoung fast-path a working
// bump-pointer on M-threads, avoiding ~980K Runtime calls per goroutine.
void HeapAllocator::ReplaceOldSpaceLAB(LinearAllocationArea* lab) {
  // Free existing LAB to avoid leaking pages.
  if (old_space_allocator_.has_value()) {
    old_space_allocator_->FreeLinearAllocationArea();
    old_space_allocator_.reset();
  }
  old_space_allocator_.emplace(local_heap_, heap_->old_space(),
                               MainAllocator::IsNewGeneration::kNo, lab);

  // new_allocation_info_ is immediately before old_allocation_info_ in
  // IsolateData (see ISOLATE_DATA_FIELDS macro in isolate-data.h).
  // Both are LinearAllocationArea structs, so (lab - 1) points to
  // new_allocation_info_.  Create a separate allocator backed by old space
  // so CSA kYoung bump-pointer works via [r13 + new_allocation_info_offset].
  LinearAllocationArea* new_lab = lab - 1;
  if (v8_flags.minor_ms && heap_->new_space()) {
    // Real PagedNewSpace (--minor-ms): M-thread kYoung allocations go into
    // shared PagedNewSpace. SemiSpaceNewSpace is NOT thread-safe.
    new_space_allocator_.emplace(
        local_heap_,
        static_cast<SpaceWithLinearArea*>(heap_->new_space()),
        MainAllocator::IsNewGeneration::kYes, new_lab);
    // Signal Mark-Compact to force in-place page promotion.
    v8_goroutine_uses_newspace_.store(true, std::memory_order_relaxed);
  } else {
    // Fallback: backed by old space for CSA kYoung bump-pointer fast path.
    new_space_allocator_.emplace(local_heap_, heap_->old_space(),
                                 MainAllocator::IsNewGeneration::kNo, new_lab);
  }
}

void HeapAllocator::SetReadOnlySpace(ReadOnlySpace* read_only_space) {
  read_only_space_ = read_only_space;
}

AllocationResult HeapAllocator::AllocateRawLargeInternal(
    int size_in_bytes, AllocationType allocation, AllocationOrigin origin,
    AllocationAlignment alignment) {
  DCHECK_GT(size_in_bytes, heap_->MaxRegularHeapObjectSize(allocation));
  // GOROUTINE PATCH: on M-threads, redirect kYoung large objects to old LO
  // space. NewLargeObjectSpace is not thread-safe and is managed by Scavenger.
  if (v8_goroutine_thread && allocation == AllocationType::kYoung) {
    allocation = AllocationType::kOld;
  }
  switch (allocation) {
    case AllocationType::kYoung:
      return new_lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kOld:
      return lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kCode:
      return code_lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kSharedOld:
      return shared_lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kTrusted:
      return trusted_lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kSharedTrusted:
      return shared_trusted_lo_space()->AllocateRaw(local_heap_, size_in_bytes);
    case AllocationType::kMap:
    case AllocationType::kReadOnly:
    case AllocationType::kSharedMap:
      UNREACHABLE();
  }
}

namespace {

constexpr AllocationSpace AllocationTypeToGCSpace(AllocationType type) {
  switch (type) {
    case AllocationType::kYoung:
      return NEW_SPACE;
    case AllocationType::kOld:
    case AllocationType::kCode:
    case AllocationType::kMap:
    case AllocationType::kTrusted:
      // OLD_SPACE indicates full GC.
      return OLD_SPACE;
    case AllocationType::kReadOnly:
    case AllocationType::kSharedMap:
    case AllocationType::kSharedOld:
    case AllocationType::kSharedTrusted:
      UNREACHABLE();
  }
}

}  // namespace

AllocationResult HeapAllocator::AllocateRawWithLightRetrySlowPath(
    int size, AllocationType allocation, AllocationOrigin origin,
    AllocationAlignment alignment) {
  auto Allocate = [&](AllocationType allocation) {
    return AllocateRaw(size, allocation, origin, alignment);
  };
  auto RetryAllocate = [&](AllocationType allocation) {
    return RetryAllocateRaw(size, allocation, origin, alignment);
  };

  return AllocateRawWithLightRetrySlowPath(Allocate, RetryAllocate, allocation);
}

void HeapAllocator::CollectGarbage(AllocationType allocation) {
  if (IsSharedAllocationType(allocation)) {
    heap_->CollectGarbageShared(local_heap_,
                                GarbageCollectionReason::kAllocationFailure);
  } else if (local_heap_->is_main_thread()) {
    // On the main thread we can directly start the GC.
    AllocationSpace space_to_gc = AllocationTypeToGCSpace(allocation);
    heap_->CollectGarbage(space_to_gc,
                          GarbageCollectionReason::kAllocationFailure);
  } else {
    // GOROUTINE PATCH: If M-thread's kYoung allocation failed (NewSpace),
    // signal that we need Minor GC (not full Mark-Compact which evacuates
    // NewSpace pages and breaks compressed pointers in M-thread registers).
    if (v8_goroutine_thread && allocation == AllocationType::kYoung) {
      extern std::atomic<bool> v8_goroutine_minor_gc_requested_;  // heap.cc
      v8_goroutine_minor_gc_requested_.store(true, std::memory_order_relaxed);
    }
    // GOROUTINE PATCH: Register goroutine mmap stack as GC root before
    // requesting GC. Without this, Minor Mark-Sweep sweeps objects reachable
    // only from goroutine interpreter frames → stale pointers → SIGSEGV.
    if (v8_goroutine_thread) {
      v8_goroutine_lab_sync_after_run();
      MakeLinearAllocationAreasIterable();
      v8_goroutine_safepoint_park(heap_->isolate());
    }
    // Request GC from main thread.
    heap_->CollectGarbageFromAnyThread(local_heap_);
    if (v8_goroutine_thread) {
      v8_goroutine_safepoint_unpark();
      v8_goroutine_lab_sync_before_run();
    }
  }
}

AllocationResult HeapAllocator::AllocateRawWithRetryOrFailSlowPath(
    int size, AllocationType allocation, AllocationOrigin origin,
    AllocationAlignment alignment) {
  // GOROUTINE PATCH: goroutine M-threads must not use the main thread's
  // HeapAllocator (local_heap_->is_main_thread() == true). Route ALL
  // allocations through the goroutine's background LocalHeap instead.
  //
  // Only intercept when THIS allocator IS the main thread's (local_heap_->
  // is_main_thread()). When goroutine's own LH allocator calls us,
  // local_heap_->is_main_thread() == false → fall through to normal slow-path
  // (CollectGarbageFromAnyThread on goroutine's LH which is correct).
  //
  // Use AllocateRawOrFail so GC/retry is handled inside goroutine's LH
  // (not the main thread's CollectAllAvailableGarbage which requires main thread).
  if (v8_goroutine_thread && local_heap_->is_main_thread()) {
    LocalHeap* lh = LocalHeap::Current();
    if (lh && !lh->is_main_thread()) {
      // kYoung now goes through per-M new_space_allocator_ (backed by old
      // space) — no need to redirect to kOld.
      if (allocation == AllocationType::kYoung ||
          allocation == AllocationType::kOld ||
          allocation == AllocationType::kTrusted) {
        Address addr =
            lh->AllocateRawOrFail(size, allocation, origin, alignment);
        return AllocationResult::FromObject(HeapObject::FromAddress(addr));
      }
    }
  }
  auto Allocate = [&](AllocationType allocation) {
    return AllocateRaw(size, allocation, origin, alignment);
  };
  auto RetryAllocate = [&](AllocationType allocation) {
    return RetryAllocateRaw(size, allocation, origin, alignment);
  };
  return AllocateRawWithRetryOrFailSlowPath(Allocate, RetryAllocate,
                                            allocation);
}

void HeapAllocator::CollectAllAvailableGarbage(AllocationType allocation) {
  if (IsSharedAllocationType(allocation)) {
    heap_->CollectGarbageShared(heap_->main_thread_local_heap(),
                                GarbageCollectionReason::kLastResort);
  } else if (local_heap_->is_main_thread()) {
    // On the main thread we can directly start the GC.
    heap_->CollectAllAvailableGarbage(GarbageCollectionReason::kLastResort);
  } else {
    // GOROUTINE PATCH: register goroutine stack for GC root scanning.
    if (v8_goroutine_thread) {
      v8_goroutine_lab_sync_after_run();
      MakeLinearAllocationAreasIterable();
      v8_goroutine_safepoint_park(heap_->isolate());
    }
    // Request GC from main thread.
    heap_->CollectGarbageFromAnyThread(local_heap_);
    if (v8_goroutine_thread) {
      v8_goroutine_safepoint_unpark();
      v8_goroutine_lab_sync_before_run();
    }
  }
}

AllocationResult HeapAllocator::RetryAllocateRaw(
    int size_in_bytes, AllocationType allocation, AllocationOrigin origin,
    AllocationAlignment alignment) {
  // Initially flags on the LocalHeap are always disabled. They are only
  // active while this method is running.
  DCHECK(!local_heap_->IsRetryOfFailedAllocation());
  local_heap_->SetRetryOfFailedAllocation(true);
  AllocationResult result =
      AllocateRaw(size_in_bytes, allocation, origin, alignment);
  local_heap_->SetRetryOfFailedAllocation(false);
  return result;
}

void HeapAllocator::MakeLinearAllocationAreasIterable() {
  if (new_space_allocator_) {
    new_space_allocator_->MakeLinearAllocationAreaIterable();
  }
  old_space_allocator_->MakeLinearAllocationAreaIterable();
  trusted_space_allocator_->MakeLinearAllocationAreaIterable();
  code_space_allocator_->MakeLinearAllocationAreaIterable();

  if (shared_space_allocator_) {
    shared_space_allocator_->MakeLinearAllocationAreaIterable();
  }

  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->MakeLinearAllocationAreaIterable();
  }
}

#if DEBUG
void HeapAllocator::VerifyLinearAllocationAreas() const {
  if (new_space_allocator_) {
    new_space_allocator_->Verify();
  }
  old_space_allocator_->Verify();
  trusted_space_allocator_->Verify();
  code_space_allocator_->Verify();

  if (shared_space_allocator_) {
    shared_space_allocator_->Verify();
  }

  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->Verify();
  }
}
#endif  // DEBUG

void HeapAllocator::MarkLinearAllocationAreasBlack() {
  DCHECK(!v8_flags.black_allocated_pages);
  old_space_allocator_->MarkLinearAllocationAreaBlack();
  trusted_space_allocator_->MarkLinearAllocationAreaBlack();
  code_space_allocator_->MarkLinearAllocationAreaBlack();
}

void HeapAllocator::UnmarkLinearAllocationsArea() {
  DCHECK(!v8_flags.black_allocated_pages);
  old_space_allocator_->UnmarkLinearAllocationArea();
  trusted_space_allocator_->UnmarkLinearAllocationArea();
  code_space_allocator_->UnmarkLinearAllocationArea();
}

void HeapAllocator::MarkSharedLinearAllocationAreasBlack() {
  DCHECK(!v8_flags.black_allocated_pages);
  if (shared_space_allocator_) {
    shared_space_allocator_->MarkLinearAllocationAreaBlack();
  }
  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->MarkLinearAllocationAreaBlack();
  }
}

void HeapAllocator::UnmarkSharedLinearAllocationAreas() {
  DCHECK(!v8_flags.black_allocated_pages);
  if (shared_space_allocator_) {
    shared_space_allocator_->UnmarkLinearAllocationArea();
  }
  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->UnmarkLinearAllocationArea();
  }
}

void HeapAllocator::FreeLinearAllocationAreasAndResetFreeLists() {
  DCHECK(v8_flags.black_allocated_pages);
  old_space_allocator_->FreeLinearAllocationAreaAndResetFreeList();
  trusted_space_allocator_->FreeLinearAllocationAreaAndResetFreeList();
  code_space_allocator_->FreeLinearAllocationAreaAndResetFreeList();
}

void HeapAllocator::FreeSharedLinearAllocationAreasAndResetFreeLists() {
  DCHECK(v8_flags.black_allocated_pages);
  if (shared_space_allocator_) {
    shared_space_allocator_->FreeLinearAllocationAreaAndResetFreeList();
  }
  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->FreeLinearAllocationAreaAndResetFreeList();
  }
}

void HeapAllocator::FreeLinearAllocationAreas() {
  if (new_space_allocator_) {
    new_space_allocator_->FreeLinearAllocationArea();
  }
  old_space_allocator_->FreeLinearAllocationArea();
  trusted_space_allocator_->FreeLinearAllocationArea();
  code_space_allocator_->FreeLinearAllocationArea();

  if (shared_space_allocator_) {
    shared_space_allocator_->FreeLinearAllocationArea();
  }

  if (shared_trusted_space_allocator_) {
    shared_trusted_space_allocator_->FreeLinearAllocationArea();
  }
}

void HeapAllocator::PublishPendingAllocations() {
  if (new_space_allocator_) {
    new_space_allocator_->MoveOriginalTopForward();
  }

  old_space_allocator_->MoveOriginalTopForward();
  trusted_space_allocator_->MoveOriginalTopForward();
  code_space_allocator_->MoveOriginalTopForward();

  lo_space()->ResetPendingObject();
  if (new_lo_space()) new_lo_space()->ResetPendingObject();
  code_lo_space()->ResetPendingObject();
  trusted_lo_space()->ResetPendingObject();
}

void HeapAllocator::AddAllocationObserver(
    AllocationObserver* observer, AllocationObserver* new_space_observer) {
  if (new_space_allocator_) {
    new_space_allocator_->AddAllocationObserver(new_space_observer);
  }
  if (new_lo_space()) {
    new_lo_space()->AddAllocationObserver(new_space_observer);
  }
  old_space_allocator_->AddAllocationObserver(observer);
  lo_space()->AddAllocationObserver(observer);
  trusted_space_allocator_->AddAllocationObserver(observer);
  trusted_lo_space()->AddAllocationObserver(observer);
  code_space_allocator_->AddAllocationObserver(observer);
  code_lo_space()->AddAllocationObserver(observer);
}

void HeapAllocator::RemoveAllocationObserver(
    AllocationObserver* observer, AllocationObserver* new_space_observer) {
  if (new_space_allocator_) {
    new_space_allocator_->RemoveAllocationObserver(new_space_observer);
  }
  if (new_lo_space()) {
    new_lo_space()->RemoveAllocationObserver(new_space_observer);
  }
  old_space_allocator_->RemoveAllocationObserver(observer);
  lo_space()->RemoveAllocationObserver(observer);
  trusted_space_allocator_->RemoveAllocationObserver(observer);
  trusted_lo_space()->RemoveAllocationObserver(observer);
  code_space_allocator_->RemoveAllocationObserver(observer);
  code_lo_space()->RemoveAllocationObserver(observer);
}

void HeapAllocator::PauseAllocationObservers() {
  if (new_space_allocator_) {
    new_space_allocator_->PauseAllocationObservers();
  }
  old_space_allocator_->PauseAllocationObservers();
  trusted_space_allocator_->PauseAllocationObservers();
  code_space_allocator_->PauseAllocationObservers();
}

void HeapAllocator::ResumeAllocationObservers() {
  if (new_space_allocator_) {
    new_space_allocator_->ResumeAllocationObservers();
  }
  old_space_allocator_->ResumeAllocationObservers();
  trusted_space_allocator_->ResumeAllocationObservers();
  code_space_allocator_->ResumeAllocationObservers();
}

#ifdef DEBUG

void HeapAllocator::IncrementObjectCounters() {
  heap_->isolate()->counters()->objs_since_last_full()->Increment();
  heap_->isolate()->counters()->objs_since_last_young()->Increment();
}

#endif  // DEBUG

#ifdef V8_ENABLE_ALLOCATION_TIMEOUT
// static
void HeapAllocator::InitializeOncePerProcess() {
  SetAllocationGcInterval(v8_flags.gc_interval);
}

// static
void HeapAllocator::SetAllocationGcInterval(int allocation_gc_interval) {
  allocation_gc_interval_.store(allocation_gc_interval,
                                std::memory_order_relaxed);
}

// static
std::atomic<int> HeapAllocator::allocation_gc_interval_{-1};

void HeapAllocator::SetAllocationTimeout(int allocation_timeout) {
  if (allocation_timeout > 0) {
    allocation_timeout_ = allocation_timeout;
  } else {
    allocation_timeout_.reset();
  }
}

void HeapAllocator::UpdateAllocationTimeout() {
  if (v8_flags.random_gc_interval > 0) {
    const int new_timeout = heap_->isolate()->fuzzer_rng()->NextInt(
        v8_flags.random_gc_interval + 1);
    // Reset the allocation timeout, but make sure to allow at least a few
    // allocations after a collection. The reason for this is that we have a lot
    // of allocation sequences and we assume that a garbage collection will
    // allow the subsequent allocation attempts to go through.
    constexpr int kFewAllocationsHeadroom = 6;
    int timeout = std::max(kFewAllocationsHeadroom, new_timeout);
    SetAllocationTimeout(timeout);
    DCHECK(allocation_timeout_.has_value());
    return;
  }

  int timeout = allocation_gc_interval_.load(std::memory_order_relaxed);
  SetAllocationTimeout(timeout);
}

bool HeapAllocator::ReachedAllocationTimeout() {
  DCHECK(allocation_timeout_.has_value());

  if (heap_->always_allocate() || local_heap_->IsRetryOfFailedAllocation()) {
    return false;
  }

  allocation_timeout_ = std::max(0, allocation_timeout_.value() - 1);
  return allocation_timeout_.value() <= 0;
}

#endif  // V8_ENABLE_ALLOCATION_TIMEOUT

}  // namespace internal
}  // namespace v8
