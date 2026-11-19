/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/Event.h"
#include "packsim/kernel/pdes/RawEvent.h"

// Tests that need the engine go in EventDeathTests.cpp or PdesEngineTests.cpp.

namespace packsim::pdes {

TEST(Event, DefaultConstructedIsInvalidAndInactive) {
  Event event;
  EXPECT_FALSE(event.isValid());
  EXPECT_FALSE(event.isActive());
  EXPECT_EQ(event.getEventTimeFs(), 0u);
}

TEST(Event, ValidHandleReflectsRawEventState) {
  RawEvent raw;
  raw.timeFs = 42;
  Event event(&raw);

  EXPECT_TRUE(event.isValid());
  EXPECT_TRUE(event.isActive());
  EXPECT_EQ(event.getEventTimeFs(), 42u);
}

TEST(Event, CancelOutsideAQueueMarksTheEventCancelled) {
  RawEvent raw;
  raw.timeFs = 10;
  Event event(&raw);

  event.cancel();

  EXPECT_EQ(raw.queueIndex, RawEvent::kCancelled);
  EXPECT_FALSE(event.isActive());
}

TEST(Event, StaleHandleAfterResetIsInvalidAndCancelIsNoOp) {
  RawEvent raw;
  raw.timeFs = 99;
  Event event(&raw);
  ASSERT_TRUE(event.isValid());

  // reset() bumps the version, so the previously-cached handle is now stale.
  raw.reset();

  EXPECT_FALSE(event.isValid());
  EXPECT_EQ(event.getEventTimeFs(), 0u);
  EXPECT_FALSE(event.isActive());

  // Cancelling through a stale handle must not touch the recycled RawEvent.
  event.cancel();
  EXPECT_EQ(raw.queueIndex, RawEvent::kNotQueued);
}

} // namespace packsim::pdes
