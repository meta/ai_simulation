/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/EventPool.h"
#include "packsim/kernel/pdes/ObjectAllocator.h"
#include "packsim/kernel/pdes/RawEvent.h"

namespace packsim::pdes {

TEST(EventPool, HandsOutAgainAnEventReturnedOnTheSameThread) {
  RawEvent* event = EventPool::get().pop();
  const RawEvent* const pushedEvent = event;
  const uint32_t pushedVersion = event->version.load();

  EventPool::get().push(event);
  RawEvent* reissuedEvent = EventPool::get().pop();

  EXPECT_EQ(reissuedEvent, pushedEvent);
  // push() bumps the version; an event freed and allocated again at the same
  // address would start over at 0.
  EXPECT_EQ(reissuedEvent->version.load(), pushedVersion + 1);
  EventPool::get().push(reissuedEvent);
}

TEST(EventPool, GivesEachThreadItsOwnPool) {
  const detail::ObjectAllocator<RawEvent>* const thisThreadsPool =
      &EventPool::get();
  bool sharesThisThreadsPool = true;

  std::thread([thisThreadsPool, &sharesThisThreadsPool] {
    sharesThisThreadsPool = &EventPool::get() == thisThreadsPool;
  }).join();

  EXPECT_FALSE(sharesThisThreadsPool);
}

} // namespace packsim::pdes
