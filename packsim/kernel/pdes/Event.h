/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "packsim/kernel/pdes/RawEvent.h"

namespace packsim::pdes {

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
   * stop that run; it only makes isActive() return false.
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

} // namespace packsim::pdes
