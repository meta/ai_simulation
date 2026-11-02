/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/kernel/ticker.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/kernel/duration.h"
#include "packsim/kernel/scheduler.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/kernel/time.h"

namespace packsim {

using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

// A Ticker with a 10 ns period on one context, recording the time of every
// tick in ns.
class TickerTest : public ::testing::Test {
 protected:
  static constexpr Duration kPeriod = Duration::fromNs(10);

  void SetUp() override {
    sched_.setLookaheadTimeFs(Time::fromNs(1).getFs());
  }

  // Options for a ticker with kPeriod that appends the time of each tick to
  // tickTimesNs_.
  Ticker::Options recordTicks() {
    return Ticker::Options{.period = kPeriod, .onTick = [this] {
                             tickTimesNs_.emplace_back(ctx_.nowNs());
                           }};
  }

  // Run `action` on ctx_ at `atNs`.
  template <typename TFunc>
  void scheduleAt(const uint64_t atNs, TFunc&& action) {
    ctx_.schedule(Duration::fromNs(atNs), std::forward<TFunc>(action));
  }

  // Scheduler driving ctx_.
  Scheduler sched_;
  // Context the ticks run on.
  SimContext ctx_{&sched_, 0};
  // Time of every tick, in ns, in order.
  std::vector<uint64_t> tickTimesNs_;
};

TEST_F(TickerTest, TicksEveryPeriodStartingOnePeriodAfterConstruction) {
  const Ticker ticker(ctx_, recordTicks());

  sched_.runUntil(Time::fromNs(35));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10, 20, 30}));
}

TEST_F(TickerTest, ZeroPeriodThrows) {
  EXPECT_THAT(
      [this] {
        const Ticker ticker(
            ctx_, Ticker::Options{.period = Duration(), .onTick = [] {}});
      },
      ThrowsMessage<std::invalid_argument>(HasSubstr("period is zero")));
}

TEST_F(TickerTest, EmptyOnTickThrows) {
  EXPECT_THAT(
      [this] { const Ticker ticker(ctx_, Ticker::Options{.period = kPeriod}); },
      ThrowsMessage<std::invalid_argument>(HasSubstr("no tick subscriber")));
}

TEST_F(TickerTest, IsRunningOnceBuilt) {
  const Ticker ticker(ctx_, recordTicks());

  EXPECT_TRUE(ticker.isRunning());
}

TEST_F(TickerTest, StopEndsTicking) {
  Ticker ticker(ctx_, recordTicks());
  scheduleAt(15, [&ticker] { ticker.stop(); });

  sched_.runUntil(Time::fromNs(45));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10}));
  EXPECT_FALSE(ticker.isRunning());
}

TEST_F(TickerTest, StopFromOnTickEndsTicking) {
  std::optional<Ticker> ticker;
  ticker.emplace(
      ctx_, Ticker::Options{.period = kPeriod, .onTick = [this, &ticker] {
                              tickTimesNs_.emplace_back(ctx_.nowNs());
                              if (tickTimesNs_.size() == 2) {
                                ticker->stop();
                              }
                            }});

  sched_.runUntil(Time::fromNs(45));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10, 20}));
}

TEST_F(TickerTest, RestartFromOnTickDoesNotAddATick) {
  std::optional<Ticker> ticker;
  ticker.emplace(
      ctx_, Ticker::Options{.period = kPeriod, .onTick = [this, &ticker] {
                              tickTimesNs_.emplace_back(ctx_.nowNs());
                              ticker->restart();
                            }});

  sched_.runUntil(Time::fromNs(35));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10, 20, 30}));
}

TEST_F(TickerTest, RestartMovesTheNextTickOnePeriodFromNow) {
  Ticker ticker(ctx_, recordTicks());
  scheduleAt(15, [&ticker] { ticker.restart(); });

  sched_.runUntil(Time::fromNs(40));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10, 25, 35}));
}

TEST_F(TickerTest, RestartAfterStopResumesTicking) {
  Ticker ticker(ctx_, recordTicks());
  scheduleAt(15, [&ticker] { ticker.stop(); });
  scheduleAt(22, [&ticker] { ticker.restart(); });

  sched_.runUntil(Time::fromNs(40));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10, 32}));
  EXPECT_TRUE(ticker.isRunning());
}

TEST_F(TickerTest, DestroyingTheTickerCancelsItsPendingTick) {
  std::optional<Ticker> ticker;
  ticker.emplace(ctx_, recordTicks());
  scheduleAt(15, [&ticker] { ticker.reset(); });

  sched_.runUntil(Time::fromNs(45));

  EXPECT_EQ(tickTimesNs_, (std::vector<uint64_t>{10}));
}

TEST_F(TickerTest, NextTickRunsBeforeAnEventOnTickSchedulesForTheSameTime) {
  std::vector<std::string> log;
  const Ticker ticker(
      ctx_,
      Ticker::Options{
          .period = kPeriod, .onTick = [this, &log] {
            log.emplace_back(fmt::format("tick at {}", ctx_.nowNs()));
            if (log.size() == 1) {
              ctx_.schedule(kPeriod, [this, &log] {
                log.emplace_back(fmt::format("event at {}", ctx_.nowNs()));
              });
            }
          }});

  sched_.runUntil(Time::fromNs(25));

  EXPECT_EQ(
      log,
      (std::vector<std::string>{"tick at 10", "tick at 20", "event at 20"}));
}

} // namespace packsim
