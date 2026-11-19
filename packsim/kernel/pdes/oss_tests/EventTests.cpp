/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <queue>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/Event.h"
#include "packsim/kernel/pdes/PdesEngine.h"
#include "packsim/kernel/pdes/SimulationContext.h"

namespace packsim::pdes {

TEST(RawEvent, SetSortIdPacksContextIndexAndEventId) {
  RawEvent event;
  event.setSortId(/*ctxIdx=*/3, /*eventId=*/7);
  EXPECT_EQ(event.sortId, (uint64_t{3} << RawEvent::kSortIdEventIdBits) | 7u);
}

TEST(RawEvent, ResetClearsFieldsAndBumpsVersion) {
  int sentinel = 0;
  RawEvent event;
  event.timeFs = 123;
  event.sortId = 456;
  event.queueIndex = RawEvent::kCancelled;
  event.func = [] {
  }; // set non-empty so the post-reset func check is meaningful
  event.context = reinterpret_cast<SimulationContext*>(&sentinel); // non-null
  const uint32_t versionBefore = event.version.load();

  event.reset();

  EXPECT_EQ(event.timeFs, 0);
  EXPECT_EQ(event.sortId, 0u);
  EXPECT_EQ(event.queueIndex, RawEvent::kNotQueued);
  EXPECT_FALSE(static_cast<bool>(event.func));
  EXPECT_EQ(event.context, nullptr);
  EXPECT_EQ(event.version.load(), versionBefore + 1);
}

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

TEST(Event, CancelFromAnotherContextAbortsInDebugBuilds) {
  // The statement builds its own engine: in a debug build it runs in the death
  // test's forked child, and this binary starts no thread before the fork, so
  // the engine's worker can start there. In an opt build the check is compiled
  // out and the statement runs here, on one worker, where the cancel is safe.
  // NOLINTNEXTLINE(facebook-gtest-expect-death)
  EXPECT_DEBUG_DEATH(
      {
        auto pdes = PdesEngine(/* lookaheadTimeFs= */ 100, /* numThreads= */ 1);
        SimulationContext canceller;
        SimulationContext target;
        const Event event =
            pdes.scheduleAt(&target, /* timeFs= */ 150, []() {});
        pdes.scheduleAt(
            &canceller, /* timeFs= */ 10, [&event]() { event.cancel(); });
        pdes.run();
      },
      "cancelled an event of context");
}

TEST(EventComparator, OrdersByAscendingTime) {
  RawEvent late;
  late.timeFs = 30;
  RawEvent early;
  early.timeFs = 10;
  RawEvent mid;
  mid.timeFs = 20;

  std::priority_queue<RawEvent*, std::vector<RawEvent*>, EventComparator> pq;
  pq.push(&late);
  pq.push(&early);
  pq.push(&mid);

  EXPECT_EQ(pq.top(), &early);
  pq.pop();
  EXPECT_EQ(pq.top(), &mid);
  pq.pop();
  EXPECT_EQ(pq.top(), &late);
}

TEST(EventComparator, BreaksTiesByAscendingSortId) {
  RawEvent higherSortId;
  higherSortId.timeFs = 5;
  higherSortId.sortId = 100;
  RawEvent lowerSortId;
  lowerSortId.timeFs = 5;
  lowerSortId.sortId = 50;

  std::priority_queue<RawEvent*, std::vector<RawEvent*>, EventComparator> pq;
  pq.push(&higherSortId);
  pq.push(&lowerSortId);

  EXPECT_EQ(pq.top(), &lowerSortId);
}

} // namespace packsim::pdes
