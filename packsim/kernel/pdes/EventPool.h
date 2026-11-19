/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/SingletonThreadLocal.h>

#include "packsim/kernel/pdes/ObjectAllocator.h"
#include "packsim/kernel/pdes/RawEvent.h"

namespace packsim::pdes {

/**
 * Per-thread pools of reusable events. Scheduling takes an event from the
 * scheduling thread's pool, and an event that has run or been cancelled goes
 * back to the pool of the thread that recycles it, which need not be the same
 * thread.
 *
 * Example usage:
 *
 *      RawEvent* event = EventPool::get().pop();
 *      ...
 *      EventPool::get().push(event); // resets the event and clears `event`
 *
 * NOTE: get() returns the calling thread's pool, which is not thread-safe: use
 * it only on that thread.
 */
class EventPool {
 public:
  /**
   * Returns the calling thread's pool, created on the thread's first call.
   *
   * @return  The pool, which lives until the thread exits.
   */
  static detail::ObjectAllocator<RawEvent>& get() {
    return folly::SingletonThreadLocal<
        detail::ObjectAllocator<RawEvent>>::get();
  }
};

} // namespace packsim::pdes
