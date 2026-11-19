/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/kernel/pdes/Event.h"

#include <utility>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "packsim/kernel/pdes/EventPool.h"
#include "packsim/kernel/pdes/EventQueue.h"
#include "packsim/kernel/pdes/ObjectAllocator.h"
#include "packsim/kernel/pdes/RawEvent.h"
#include "packsim/kernel/pdes/RunningContext.h"
#include "packsim/kernel/pdes/SimulationContext.h"

namespace packsim::pdes {

namespace {
/**
 * Cancel `event` at once: remove it from its context's queue and recycle it,
 * or, while it is outside the queue, mark it cancelled. Call it only on the
 * thread that runs the event's context, or between frames. Recycling can run
 * out of memory, which terminates the process.
 *
 * @param event  Event to cancel, still under the version of the handle used.
 */
void cancelNow(RawEvent& event) noexcept {
  switch (event.queueIndex) {
    case RawEvent::kCancelled:
      return;
    case RawEvent::kNotQueued:
      // Waiting in a backlog, which PdesEngine::drainEventBacklog() then drops
      // it from, or running, after which the worker recycles it as usual.
      event.queueIndex = RawEvent::kCancelled;
      return;
    default:
      event.context->eventQ.erase(&event);
      RawEvent* cancelled = &event;
      EventPool::get().push(cancelled);
  }
}
} // namespace

void Event::cancel() const noexcept {
  // cancel()'s noexcept guarantee depends on isValid() being noexcept; pin
  // that at compile time so a future logging/dispatch addition to isValid()
  // that introduces a throwing path fails to build here rather than silently
  // terminating from inside a noexcept call.
  static_assert(noexcept(std::declval<const Event&>().isValid()));
  if (isValid()) {
    // From another context, removing the event would change a queue that
    // another worker may be using (see the NOTE on Event).
    XDCHECK(
        RunningContext::get() == nullptr ||
        RunningContext::get() == rawEvent->context)
        << fmt::format(
               "Context {} cancelled an event of context {}; cancel it from "
               "its own context",
               RunningContext::get()->nameWithPtr(),
               rawEvent->context->nameWithPtr());
    cancelNow(*rawEvent);
  }
}

} // namespace packsim::pdes
