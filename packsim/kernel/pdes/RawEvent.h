/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <folly/CPortability.h>
#include <folly/Function.h>
#include <folly/Likely.h>

namespace packsim::pdes {

struct SimulationContext;

struct RawEvent {
  RawEvent() = default;

  // Action to take when the time comes. Callables of up to 48 bytes that move
  // without throwing, such as a member function, its object and a PacketPtr,
  // are stored inline, so scheduling them does not allocate.
  // 64 bytes, 16-byte aligned: declared first so that its alignment adds no
  // padding and the event packs into 96 bytes.
  folly::Function<void()> func;

  // Event time in femtoseconds (1e-15 seconds).
  // 8 bytes
  uint64_t timeFs = 0;

  // Index of context that scheduled this event, for consistent sorting
  // 8 bytes
  uint64_t sortId = 0;

  // Context used to schedule this event. Context is a pointer to something in
  // memory. Its purpose is to just glue the events together for the sake of
  // event scheduling. All events with the same context run on the same thread.
  // 8 bytes
  SimulationContext* context{};

  // Used to track "versions" in case something else holds a reference to this
  // event past its lifetime
  // 4 bytes
  std::atomic_uint32_t version{0};

  // Position of this event in its context's queue (see EventQueue). Outside
  // the queue, while the event waits in a worker's backlog or runs, it is
  // kNotQueued, or kCancelled once the event is cancelled there.
  // 4 bytes
  uint32_t queueIndex = kNotQueued;

  // queueIndex of an active event outside its context's queue.
  static constexpr uint32_t kNotQueued = UINT32_MAX;

  // queueIndex of an event cancelled outside its context's queue: a worker
  // drops it from its backlog instead of queueing it, and one already running
  // finishes that run.
  static constexpr uint32_t kCancelled = kNotQueued - 1;

  // Number of low-order bits of sortId reserved for the per-context event id;
  // the scheduling context index is packed into the remaining high-order bits.
  static constexpr uint64_t kSortIdEventIdBits = 32;

  inline void setSortId(uint64_t ctxIdx, uint64_t eventId) noexcept {
    sortId = (ctxIdx << kSortIdEventIdBits) | eventId;
  }

  // Every access to `version` is relaxed. Only the thread that runs an event's
  // context queues, pops, runs, cancels and resets the event, and a handle may
  // read or cancel it only on that thread or between frames. Program order, or
  // the event queues and the frame barriers (which also order a context's move
  // to another thread), order each access against those it must see, and none
  // of them publishes other data, so they only need to be tear-free: the bump
  // needs no read-modify-write and no store a fence. `queueIndex` follows the
  // same rule and is never read from another thread, so it is a plain field. A
  // handle used from another thread within a frame races the event under any
  // memory order.
  void reset() {
    version.store(
        version.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    sortId = 0;
    timeFs = 0;
    func = nullptr;
    context = nullptr;
    queueIndex = kNotQueued;
  }
};

static_assert(
    sizeof(RawEvent) <= 96,
    "RawEvent outgrew 96 bytes; keep func first so that its 16-byte alignment "
    "adds no padding");

struct EventComparator {
  FOLLY_ALWAYS_INLINE bool operator()(
      RawEvent* const& lhs,
      RawEvent* const& rhs) const noexcept {
    if (LIKELY(lhs->timeFs != rhs->timeFs)) {
      return lhs->timeFs > rhs->timeFs;
    }
    return lhs->sortId > rhs->sortId;
  }
};

} // namespace packsim::pdes
