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
#include <folly/CppAttributes.h>
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

/**
 * Handle to a scheduled event, returned by the engine's schedule calls, to
 * cancel the event before it runs or ask whether it is still pending. It holds
 * the event's pointer and version, so it may outlive the event: once the event
 * has run or been cancelled, cancel() does nothing and isActive() returns
 * false, and once its slot is recycled under a new version, the handle reads
 * as stale.
 *
 * Example usage:
 *
 *      Event timeout = engine.schedule(delayFs, &Conn::onTimeout, this);
 *      ...
 *      timeout.cancel(); // no-op if onTimeout already ran
 *
 * NOTE: Use a handle only from its event's own context, or between runs; debug
 * builds check this in cancel(), except for a cancel made during a run from a
 * thread outside the worker pool or while a worker drains its backlog (for
 * example by the destructor of something a cancelled callback held), which is
 * just as unsafe. To cancel another context's event, schedule an event on that
 * context, at least a lookahead ahead, that cancels it there. Destructors of
 * what a cancelled callback holds run inside cancel() or while a worker drains
 * its backlog, possibly outside any context, so they must not schedule events
 * or read the engine's clock (PdesEngine::nowFs()).
 * Every handle packsim keeps comes from scheduling on its owner's own context
 * and stays there, so the event is cancelled through it, run and recycled on
 * one worker, in program order. The frame barriers order the rare accesses made
 * between runs, and a context's move to another worker, which rebalancing does
 * only between frames. The accessors rely on this rule and use relaxed atomics
 * (see RawEvent::reset()); a handle used from another worker during a frame
 * races the event under any memory order.
 */
struct Event {
  Event() = default;

  explicit Event(RawEvent* rawEventPtr)
      : rawEvent(rawEventPtr),
        rawEventVersion(rawEventPtr->version.load(std::memory_order_relaxed)) {}

  /**
   * Stop the event's callback from running: remove the event from its
   * context's queue and recycle it at once, which destroys the callback and
   * everything it holds. An event still in a worker's backlog is only marked,
   * and recycled when the backlog is drained. No-op on a default-constructed
   * handle, on a stale handle (the event already ran or was cancelled and its
   * slot was recycled), and on an already-cancelled event, so callers need not
   * check isActive() first. Called from the event's own callback, it does not
   * stop that run; it only makes isActive() return false. Defined in
   * PdesEngine.cpp, which owns the event pools.
   */
  void cancel() const noexcept;

  // True from scheduling until the event is cancelled or its callback has
  // returned; still true while the callback itself runs.
  bool isActive() const noexcept {
    return isValid() && rawEvent->queueIndex != RawEvent::kCancelled;
  }

  uint64_t getEventTimeFs() const noexcept {
    if (isValid()) {
      return rawEvent->timeFs;
    }
    return 0;
  }

  bool isValid() const noexcept {
    return rawEvent != nullptr &&
        rawEvent->version.load(std::memory_order_relaxed) == rawEventVersion;
  }

  RawEvent* rawEvent{nullptr};
  uint32_t rawEventVersion{};
};

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
