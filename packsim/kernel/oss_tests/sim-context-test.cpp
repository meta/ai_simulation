/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/kernel/duration.h"
#include "packsim/kernel/scheduler.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/kernel/time.h"

namespace packsim {
namespace {

// Appends `value` to `out`; scheduled as a plain function.
void appendValue(std::vector<int>& out, int value) {
  out.emplace_back(value);
}

// Same, as a callable object that adds `bump`.
struct AppendBumped {
  std::vector<int>& out;
  int bump;

  void operator()(int value) const {
    out.emplace_back(value + bump);
  }
};

} // namespace

class SimContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scheduler_.setLookaheadTimeFs(kLookaheadFs);
  }

  // 10ns. Large enough that events at 5ns and 15ns land in distinct frames.
  static constexpr int64_t kLookaheadFs = 10'000'000;

  Scheduler scheduler_;
};

TEST_F(SimContextTest, NameOnlyConstructorDelegatesWithSeedZero) {
  // Arrange / Act.
  SimContext ctx(&scheduler_, "unseeded");

  // Assert.
  EXPECT_EQ(ctx.random.getSeed(), 0u);
}

TEST_F(SimContextTest, GetLookaheadTimeReportsSchedulerLookahead) {
  // Arrange / Act.
  SimContext ctx(&scheduler_, "lookahead-ctx");

  // Assert.
  EXPECT_EQ(ctx.getLookaheadTime(), Time(kLookaheadFs));
}

TEST_F(SimContextTest, NowIsZeroBeforeRunStarts) {
  // Arrange / Act.
  SimContext ctx(&scheduler_, "fresh-ctx");

  // Assert.
  EXPECT_EQ(ctx.now(), Time(0));
}

TEST_F(SimContextTest, NowTracksEventTimeInFemtoseconds) {
  // Arrange.
  SimContext ctx(&scheduler_, "now-ctx");
  std::vector<uint64_t> observedFs;

  ctx.schedule(Time::fromNs(5), [&ctx, &observedFs]() {
    observedFs.emplace_back(ctx.now().getFs());
  });
  ctx.schedule(Time::fromNs(15), [&ctx, &observedFs]() {
    observedFs.emplace_back(ctx.now().getFs());
  });

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(observedFs, (std::vector<uint64_t>{5'000'000, 15'000'000}));
}

TEST_F(SimContextTest, ScheduleAddsDelayToCurrentSimTime) {
  // Arrange: the inner event is scheduled while the context is at 5ns, so its
  // delay is relative to that, not to the start of the simulation.
  SimContext ctx(&scheduler_, "delay-ctx");
  uint64_t innerFs = 0;

  ctx.schedule(Time::fromNs(5), [&ctx, &innerFs]() {
    ctx.schedule(
        Time::fromNs(30), [&ctx, &innerFs]() { innerFs = ctx.now().getFs(); });
  });

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(innerFs, 35'000'000u);
}

TEST_F(SimContextTest, ScheduleDurationAddsDelayToCurrentSimTime) {
  // Arrange: as above, through the Duration overload.
  SimContext ctx(&scheduler_, "duration-delay-ctx");
  uint64_t innerFs = 0;

  ctx.schedule(Duration::fromNs(5), [&ctx, &innerFs]() {
    ctx.schedule(Duration::fromNs(30), [&ctx, &innerFs]() {
      innerFs = ctx.now().getFs();
    });
  });

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(innerFs, 35'000'000u);
}

TEST_F(SimContextTest, ScheduleFreeFunctionForwardsArguments) {
  // Arrange.
  SimContext ctx(&scheduler_, "fn-ctx");
  std::vector<int> collected;
  // std::ref: schedule decay-copies its arguments into the event, which would
  // otherwise copy the vector instead of referring to it.
  ctx.schedule(Time::fromNs(5), &appendValue, std::ref(collected), 7);
  ctx.schedule(Time::fromNs(15), &appendValue, std::ref(collected), 9);

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(collected, (std::vector<int>{7, 9}));
}

TEST_F(SimContextTest, ScheduleFunctionReferenceForwardsArguments) {
  // Arrange.
  SimContext ctx(&scheduler_, "fn-ref-ctx");
  std::vector<int> collected;
  // Passing `appendValue` without `&`: the event must store it as a pointer.
  ctx.schedule(Time::fromNs(5), appendValue, std::ref(collected), 11);

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(collected, (std::vector<int>{11}));
}

TEST_F(SimContextTest, ScheduleFunctorForwardsArguments) {
  // Arrange.
  SimContext ctx(&scheduler_, "functor-ctx");
  std::vector<int> collected;
  ctx.schedule(Time::fromNs(5), AppendBumped{collected, /*bump=*/100}, 7);

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(collected, (std::vector<int>{107}));
}

TEST_F(SimContextTest, ScheduleNowAddsNoDelay) {
  // Arrange: scheduleNow from inside an event at 5ns must land at 5ns.
  SimContext ctx(&scheduler_, "now-delay-ctx");
  uint64_t innerFs = 0;

  ctx.schedule(Time::fromNs(5), [&]() {
    ctx.scheduleNow([&]() { innerFs = ctx.now().getFs(); });
  });

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(innerFs, 5'000'000u);
}

TEST_F(SimContextTest, ScheduleRemoteDelaysByExactlyLookahead) {
  // Arrange: the causality guarantee. An event pushed onto another context
  // must be at least one lookahead period in the future, and scheduleRemote
  // must supply exactly that much.
  SimContext sender(&scheduler_, "sender");
  SimContext receiver(&scheduler_, "receiver");
  uint64_t receivedFs = 0;

  sender.schedule(Time::fromNs(5), [&]() {
    receiver.scheduleRemote([&]() { receivedFs = receiver.now().getFs(); });
  });

  // Act.
  scheduler_.run();

  // Assert.
  EXPECT_EQ(receivedFs, 5'000'000u + kLookaheadFs);
}

} // namespace packsim
