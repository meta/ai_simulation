/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/kernel/duration.h"
#include "packsim/kernel/time.h"

namespace packsim {

using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

// A point and a span never convert into each other, so mixing them does not
// compile.
static_assert(!std::is_convertible_v<Time, Duration>);
static_assert(!std::is_convertible_v<Duration, Time>);
static_assert(!std::is_constructible_v<Duration, Time>);
static_assert(!std::is_constructible_v<Time, Duration>);
static_assert(!std::is_convertible_v<uint64_t, Duration>);

// Passed by value like the uint64_t it wraps.
static_assert(std::is_trivially_copyable_v<Duration>);
static_assert(sizeof(Duration) == sizeof(uint64_t));

// Arithmetic between durations works in constant expressions.
static_assert(
    Duration::fromNs(1) + Duration::fromPs(500) == Duration(1'500'000));
static_assert(
    Duration::fromUs(1) - Duration::fromNs(1) == Duration::fromNs(999));
static_assert(Duration::fromPs(999) < Duration::fromNs(1));

// A floating-point length compiles only with a rounding rule.
template <typename... TArgs>
concept CanMakeFromUs = requires(TArgs... args) { Duration::fromUs(args...); };
struct NotARoundingRule {};
static_assert(CanMakeFromUs<uint64_t>);
static_assert(!CanMakeFromUs<double>);
static_assert(!CanMakeFromUs<float>);
static_assert(CanMakeFromUs<double, Rounding::TruncateToUnit>);
static_assert(CanMakeFromUs<float, Rounding::TruncateToUnit>);
static_assert(CanMakeFromUs<double, Rounding::NearestFs>);
static_assert(CanMakeFromUs<double, Rounding::CeilFs>);
static_assert(!CanMakeFromUs<double, NotARoundingRule>);
// The rule is named at the call: a bare `{}` does not make one.
template <typename T>
concept CopyListInitializableFromEmptyBraces =
    requires(void (*takesT)(T)) { takesT({}); };
static_assert(!CopyListInitializableFromEmptyBraces<Rounding::TruncateToUnit>);
static_assert(!CopyListInitializableFromEmptyBraces<Rounding::NearestFs>);
static_assert(!CopyListInitializableFromEmptyBraces<Rounding::CeilFs>);

// The rounding works in constant expressions.
static_assert(
    Duration::fromUs(1.7, Rounding::TruncateToUnit{}) == Duration::fromUs(1));
static_assert(
    Duration::fromUs(1.7, Rounding::NearestFs{}) == Duration(1'700'000'000));
static_assert(
    Duration::fromUs(1.7, Rounding::CeilFs{}) == Duration(1'700'000'000));

// Lengths whose scaling to femtoseconds is exact, so each rule's result is
// known without floating-point slack.
// 2^-31 us: 0.4656612873077393 fs.
constexpr double kUnderHalfFsInUs = 0x1p-31;
// 1.5 * 2^-9 us: 2929687.5 fs.
constexpr double kHalfwayFsInUs = 0x1.8p-9;
// 2305843011 * 2^-9 us: 4503599630859375 fs, odd and above 2^52.
constexpr double kOddAbove2To52FsInUs = 2'305'843'011.0 / 512;
constexpr uint64_t kOddAbove2To52Fs = 4'503'599'630'859'375;
// 1e7 us: 1e16 fs, above 2^53, where every double is whole.
constexpr double kAbove2To53FsInUs = 1e7;
constexpr uint64_t kAbove2To53Fs = 10'000'000'000'000'000;

// The exclusive bound of `fromUs(double, mode)`: whole microseconds in 2^64 fs.
static_assert(Duration::getUsUpperBound() == 18'446'744'073);

TEST(DurationTest, DefaultIsZero) {
  EXPECT_TRUE(Duration().isZero());
  EXPECT_EQ(Duration(), Duration(0));
}

TEST(DurationTest, NonZeroLengthIsNotZero) {
  EXPECT_FALSE(Duration(1).isZero());
}

TEST(DurationTest, NamedConstructorsScaleToFemtoseconds) {
  EXPECT_EQ(Duration::fromPs(1).getFs(), 1'000u);
  EXPECT_EQ(Duration::fromNs(1).getFs(), 1'000'000u);
  EXPECT_EQ(Duration::fromUs(1).getFs(), 1'000'000'000u);
  EXPECT_EQ(Duration::fromMs(1).getFs(), 1'000'000'000'000u);
  EXPECT_EQ(Duration::fromSec(1).getFs(), 1'000'000'000'000'000u);
}

TEST(DurationTest, GettersTruncateToCoarserUnit) {
  // Just under 2 us, so rounding would give the next unit at every scale.
  const Duration d(1'999'999'999);
  EXPECT_EQ(d.getPs(), 1'999'999u);
  EXPECT_EQ(d.getNs(), 1'999u);
  EXPECT_EQ(d.getUs(), 1u);
}

TEST(DurationTest, Add) {
  EXPECT_EQ(Duration::fromNs(5) + Duration::fromNs(10), Duration::fromNs(15));
}

TEST(DurationTest, PlusAssign) {
  Duration d = Duration::fromNs(10);
  d += Duration::fromNs(5);
  EXPECT_EQ(d, Duration::fromNs(15));
}

TEST(DurationTest, Subtract) {
  EXPECT_EQ(Duration::fromNs(11) - Duration::fromNs(5), Duration::fromNs(6));
}

TEST(DurationTest, SubtractEqualLengthsIsZero) {
  EXPECT_EQ(Duration::fromNs(5) - Duration::fromNs(5), Duration());
}

TEST(DurationTest, MinusAssign) {
  Duration d = Duration::fromNs(10);
  d -= Duration::fromNs(3);
  EXPECT_EQ(d, Duration::fromNs(7));
}

TEST(DurationTest, SubtractUnderflowThrows) {
  EXPECT_THAT(
      [] { Duration::fromNs(5) - Duration::fromNs(10); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Duration difference is negative")));
}

TEST(DurationTest, MinusAssignUnderflowThrowsAndKeepsValue) {
  Duration d = Duration::fromNs(5);
  EXPECT_THAT(
      [&d] { d -= Duration::fromNs(6); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Duration difference is negative")));
  EXPECT_EQ(d, Duration::fromNs(5));
}

TEST(DurationTest, OrdersByLength) {
  EXPECT_LT(Duration::fromPs(999), Duration::fromNs(1));
  EXPECT_LE(Duration::fromNs(1), Duration::fromPs(1'000));
  EXPECT_GT(Duration::fromNs(2), Duration::fromNs(1));
  EXPECT_NE(Duration::fromNs(1), Duration::fromNs(2));
}

TEST(DurationTest, TimePlusDurationIsLaterTime) {
  EXPECT_EQ(Time::fromNs(10) + Duration::fromNs(5), Time::fromNs(15));
}

TEST(DurationTest, TimeMinusDurationIsEarlierTime) {
  EXPECT_EQ(Time::fromNs(10) - Duration::fromNs(4), Time::fromNs(6));
}

TEST(DurationTest, TimeMinusLongerDurationThrows) {
  EXPECT_THAT(
      [] { Time::fromNs(5) - Duration::fromNs(6); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Time difference is negative")));
}

TEST(DurationTest, BetweenIsTheSpanFromStartToEnd) {
  EXPECT_EQ(
      Duration::between(Time::fromNs(5), Time::fromNs(12)),
      Duration::fromNs(7));
}

TEST(DurationTest, BetweenThrowsWhenEndIsBeforeStart) {
  EXPECT_THAT(
      [] { Duration::between(Time::fromNs(12), Time::fromNs(5)); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Time difference is negative")));
}

TEST(DurationTest, TruncateToUnitDropsTheFractionOfAMicrosecond) {
  EXPECT_EQ(
      Duration::fromUs(1.7, Rounding::TruncateToUnit{}), Duration::fromUs(1));
}

TEST(DurationTest, TruncateToUnitOfLessThanAMicrosecondIsZero) {
  EXPECT_TRUE(Duration::fromUs(0.5, Rounding::TruncateToUnit{}).isZero());
}

TEST(DurationTest, TruncateToUnitAcceptsAFloat) {
  EXPECT_EQ(
      Duration::fromUs(2.5F, Rounding::TruncateToUnit{}), Duration::fromUs(2));
}

TEST(
    DurationTest,
    TruncateToUnitDropsTheFractionOfALengthAbove2To52Femtoseconds) {
  EXPECT_EQ(
      Duration::fromUs(kOddAbove2To52FsInUs, Rounding::TruncateToUnit{}),
      Duration::fromUs(4'503'599));
}

TEST(DurationTest, TruncateToUnitKeepsTheLargestWholeMicrosecondInRange) {
  // 2^64 fs is 18446744073.7 us.
  constexpr uint64_t kMaxWholeUs = 18'446'744'072;
  EXPECT_EQ(
      Duration::fromUs(
          static_cast<double>(kMaxWholeUs) + 0.9, Rounding::TruncateToUnit{}),
      Duration::fromUs(kMaxWholeUs));
}

TEST(DurationTest, NearestFsKeepsTheFractionOfAMicrosecond) {
  EXPECT_EQ(
      Duration::fromUs(1.7, Rounding::NearestFs{}), Duration(1'700'000'000));
}

TEST(DurationTest, NearestFsRoundsAScaledLengthJustBelowAWholeFsUp) {
  // 1.001 us scales to 1000999999.9999999 fs.
  EXPECT_EQ(
      Duration::fromUs(1.001, Rounding::NearestFs{}), Duration(1'001'000'000));
}

TEST(DurationTest, NearestFsRoundsHalfAFemtosecondUp) {
  EXPECT_EQ(
      Duration::fromUs(kHalfwayFsInUs, Rounding::NearestFs{}),
      Duration(2'929'688));
}

TEST(DurationTest, NearestFsRoundsLessThanHalfAFemtosecondDown) {
  EXPECT_TRUE(
      Duration::fromUs(kUnderHalfFsInUs, Rounding::NearestFs{}).isZero());
}

TEST(DurationTest, NearestFsKeepsAnOddCountAbove2To52Femtoseconds) {
  // Adding 0.5 and truncating would give the next even count.
  EXPECT_EQ(
      Duration::fromUs(kOddAbove2To52FsInUs, Rounding::NearestFs{}),
      Duration(kOddAbove2To52Fs));
}

TEST(DurationTest, NearestFsKeepsAWholeCountAbove2To53Femtoseconds) {
  EXPECT_EQ(
      Duration::fromUs(kAbove2To53FsInUs, Rounding::NearestFs{}),
      Duration(kAbove2To53Fs));
}

TEST(DurationTest, NearestFsAcceptsAFloat) {
  EXPECT_EQ(
      Duration::fromUs(2.5F, Rounding::NearestFs{}), Duration(2'500'000'000));
}

TEST(DurationTest, CeilFsRoundsAFractionOfAFemtosecondUp) {
  EXPECT_EQ(
      Duration::fromUs(kUnderHalfFsInUs, Rounding::CeilFs{}), Duration(1));
}

TEST(DurationTest, CeilFsRoundsHalfAFemtosecondUp) {
  EXPECT_EQ(
      Duration::fromUs(kHalfwayFsInUs, Rounding::CeilFs{}),
      Duration(2'929'688));
}

TEST(DurationTest, CeilFsKeepsAWholeFemtosecondCount) {
  EXPECT_EQ(Duration::fromUs(1.0, Rounding::CeilFs{}), Duration::fromUs(1));
}

TEST(DurationTest, CeilFsKeepsAnOddCountAbove2To52Femtoseconds) {
  EXPECT_EQ(
      Duration::fromUs(kOddAbove2To52FsInUs, Rounding::CeilFs{}),
      Duration(kOddAbove2To52Fs));
}

TEST(DurationTest, CeilFsKeepsAWholeCountAbove2To53Femtoseconds) {
  EXPECT_EQ(
      Duration::fromUs(kAbove2To53FsInUs, Rounding::CeilFs{}),
      Duration(kAbove2To53Fs));
}

// The range check exists only in debug builds, so these are registered only
// there; other builds report nothing skipped.
#ifndef NDEBUG
TEST(DurationDeathTest, NegativeMicrosecondsAbortInDebugBuilds) {
  EXPECT_DEATH(
      (void)Duration::fromUs(-1.0, Rounding::TruncateToUnit{}),
      "outside \\[0, 18446744073\\)");
}

TEST(DurationDeathTest, NanMicrosecondsAbortInDebugBuilds) {
  EXPECT_DEATH(
      (void)Duration::fromUs(
          std::numeric_limits<double>::quiet_NaN(), Rounding::TruncateToUnit{}),
      "outside");
}

TEST(DurationDeathTest, NegativeMicrosecondsAbortWithNearestFsInDebugBuilds) {
  EXPECT_DEATH((void)Duration::fromUs(-1.0, Rounding::NearestFs{}), "outside");
}

TEST(DurationDeathTest, NegativeMicrosecondsAbortWithCeilFsInDebugBuilds) {
  EXPECT_DEATH((void)Duration::fromUs(-1.0, Rounding::CeilFs{}), "outside");
}

TEST(DurationDeathTest, MicrosecondsAtTheUpperBoundAbortInDebugBuilds) {
  EXPECT_DEATH(
      (void)Duration::fromUs(
          static_cast<double>(Duration::getUsUpperBound()),
          Rounding::TruncateToUnit{}),
      "outside \\[0, 18446744073\\)");
}
#endif

} // namespace packsim
