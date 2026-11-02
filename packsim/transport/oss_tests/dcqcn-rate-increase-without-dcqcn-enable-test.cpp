/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <memory>
#include <optional>

#include <gtest/gtest.h>
#include <thrift/lib/cpp2/FieldRef.h>

#include "packsim/kernel/data-rate.h"
#include "packsim/kernel/device-id.h"
#include "packsim/kernel/scheduler.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/kernel/time.h"
#include "packsim/transport/congestion-control.h"
#include "packsim/transport/oss_tests/dcqcn-test-fixture.h"

namespace packsim {

// Without dcqcnEnable, no rate-increase timer runs until a CNP starts it, and
// a later CNP restarts it. Each case records `numRateIncreaseEvents()` at
// chosen times.
class DcqcnRateIncreaseWithoutDcqcnEnableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg_.dcqcnEnable() = false;
    cc_ = std::make_unique<DcqcnCongestionControl>(
        socket_,
        simCtx_,
        cfg_,
        FlexDeviceId({1, 20}),
        FlexDeviceId({1, 10}),
        DataRate("200Gbps"),
        /*smtu=*/4096,
        /*encapBytes=*/58);
    periodUs_ = static_cast<uint64_t>(*cfg_.rpTimerReset());
  }

  // Send a CNP to the controller at `atUs`.
  void sendCnpAt(const uint64_t atUs) {
    simCtx_.schedule(Time::fromUs(atUs), [this] { cc_->onCongestionSignal(); });
  }

  // Record `numRateIncreaseEvents()` at `atUs` into `count`.
  void recordAt(const uint64_t atUs, std::optional<uint32_t>& count) {
    simCtx_.schedule(Time::fromUs(atUs), [this, &count] {
      count = cc_->numRateIncreaseEvents();
    });
  }

  Scheduler scheduler_;
  SimContext simCtx_{&scheduler_, 0};
  NoopCongestionControlSocket socket_;
  packsim::thrift::DcqcnConfig cfg_;
  std::unique_ptr<DcqcnCongestionControl> cc_;
  // rpTimerReset in whole microseconds (300 by default).
  uint64_t periodUs_{0};
};

TEST_F(DcqcnRateIncreaseWithoutDcqcnEnableTest, FirstCnpStartsTheTimer) {
  std::optional<uint32_t> beforeCnp;
  std::optional<uint32_t> onePeriodAfterCnp;
  recordAt(periodUs_ + 1, beforeCnp);
  sendCnpAt(periodUs_ + 2);
  recordAt(2 * periodUs_ + 3, onePeriodAfterCnp);

  scheduler_.run(Time::fromUs(2 * periodUs_ + 4));

  EXPECT_EQ(beforeCnp, 0);
  EXPECT_EQ(onePeriodAfterCnp, 1);
}

TEST_F(DcqcnRateIncreaseWithoutDcqcnEnableTest, LaterCnpRestartsTheTimer) {
  // The second CNP comes after rpRateReduceMonitorPeriod (4 us), so it cuts
  // the rate and moves the next tick to one period after itself.
  const uint64_t secondCnpUs = periodUs_ / 2;
  std::optional<uint32_t> onePeriodAfterFirstCnp;
  std::optional<uint32_t> onePeriodAfterSecondCnp;
  sendCnpAt(1);
  sendCnpAt(secondCnpUs);
  recordAt(periodUs_ + 2, onePeriodAfterFirstCnp);
  recordAt(secondCnpUs + periodUs_ + 1, onePeriodAfterSecondCnp);

  scheduler_.run(Time::fromUs(secondCnpUs + periodUs_ + 2));

  EXPECT_EQ(onePeriodAfterFirstCnp, 0);
  EXPECT_EQ(onePeriodAfterSecondCnp, 1);
}

} // namespace packsim
