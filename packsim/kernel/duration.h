/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <fmt/format.h>
#include <folly/Portability.h>

#include "packsim/kernel/time.h"

namespace packsim {

/**
 * How `Duration` turns a floating-point length into whole femtoseconds. Each
 * rule is an empty tag passed as the second argument, e.g.
 * `Duration::fromUs(1.7, Rounding::NearestFs{})`; the explicit default
 * constructors reject a bare `{}`, so every call site names its rule.
 *
 * The femtosecond rules see the length after it is scaled to femtoseconds in
 * double precision, so a decimal that binary cannot hold can land a hair off a
 * whole femtosecond: 1.001 us scales to 1000999999.9999999 fs.
 */
struct Rounding {
  // Drops everything below the source unit, as an implicit conversion to an
  // integer count does: 1.7 us becomes 1 us, 0.5 us becomes 0. Keeps the
  // results of code that converted that way.
  struct TruncateToUnit {
    explicit TruncateToUnit() = default;
  };
  // Rounds to the nearest femtosecond, halfway cases up: 1.7 us stays 1.7 us,
  // and 1.001 us is 1001000000 fs.
  struct NearestFs {
    explicit NearestFs() = default;
  };
  // Rounds up to the next whole femtosecond, so the result is never shorter
  // than the scaled length, e.g. for a timeout that must not fire early.
  struct CeilFs {
    explicit CeilFs() = default;
  };
};

// The rounding rules `Duration`'s floating-point factories accept.
template <typename T>
concept RoundingMode = std::same_as<T, Rounding::TruncateToUnit> ||
    std::same_as<T, Rounding::NearestFs> || std::same_as<T, Rounding::CeilFs>;

/**
 * A span of simulated time in femtoseconds, never negative. `Time` is a point
 * on the simulation clock and `Duration` the distance between two points.
 * Neither converts to the other, implicitly or explicitly, so passing one
 * where the other is meant does not compile. They meet only through
 * `Time + Duration`, `Time - Duration` and `Duration::between`.
 *
 * The integer factories and addition wrap modulo 2^64 fs (about 5.1 hours), as
 * `Time`'s do. A subtraction that would go below zero throws. A floating-point
 * length must name how it rounds (see `Rounding`); without a rule it does not
 * compile, and it must lie in the range its factory documents.
 *
 * Example usage:
 *
 *      const Duration delay = Duration::fromNs(100);
 *      const Time drainAt = simCtx.now() + delay;
 *      simCtx.schedule(delay, &Port::drain, port);
 *      // Later, from another event on simCtx:
 *      const Duration left = Duration::between(simCtx.now(), drainAt);
 */
class Duration {
 public:
  // Zero length.
  constexpr Duration() noexcept = default;

  /**
   * Duration of `fs` femtoseconds.
   *
   * @param fs  Length in femtoseconds.
   */
  explicit constexpr Duration(const uint64_t fs) noexcept : durationFs_(fs) {}

  /**
   * Duration of `ps` picoseconds. The femtosecond product wraps modulo 2^64
   * fs, as in `Time::fromPs`.
   *
   * @param ps  Length in picoseconds.
   *
   * @return    The same length as a Duration.
   */
  static constexpr Duration fromPs(const uint64_t ps) noexcept {
    return Duration(ps * kFsPerPs);
  }

  /**
   * Duration of `ns` nanoseconds. The femtosecond product wraps modulo 2^64
   * fs, as in `Time::fromNs`.
   *
   * @param ns  Length in nanoseconds.
   *
   * @return    The same length as a Duration.
   */
  static constexpr Duration fromNs(const uint64_t ns) noexcept {
    return Duration(ns * kFsPerNs);
  }

  /**
   * Duration of `us` microseconds. The femtosecond product wraps modulo 2^64
   * fs, as in `Time::fromUs`.
   *
   * @param us  Length in microseconds.
   *
   * @return    The same length as a Duration.
   */
  static constexpr Duration fromUs(const uint64_t us) noexcept {
    return Duration(us * kFsPerUs);
  }

  /**
   * Duration of `us` microseconds, rounded by `mode`.
   *
   * @param us    Length in microseconds, in [0, getUsUpperBound()), e.g. 300.0.
   *              Checked in debug builds, and in a constant expression a
   *              negative or NaN value fails to compile. Other builds do not
   *              check; a value outside the range is undefined behaviour, so
   *              validate it where it enters, e.g. at config load.
   * @param mode  How to round, e.g. `Rounding::NearestFs{}`.
   *
   * @return      The rounded length as a Duration.
   */
  template <std::floating_point TFloat, RoundingMode TMode>
  static constexpr Duration fromUs(const TFloat us, const TMode mode) noexcept {
    return fromUnits<kFsPerUs>(us, mode);
  }

  // Exclusive upper bound of the microseconds `fromUs(double, mode)` accepts:
  // 18446744073, the whole microseconds in 2^64 fs.
  static constexpr uint64_t getUsUpperBound() noexcept {
    return std::numeric_limits<uint64_t>::max() / kFsPerUs;
  }

  /**
   * Duration of `ms` milliseconds. The femtosecond product wraps modulo 2^64
   * fs, as in `Time::fromMs`.
   *
   * @param ms  Length in milliseconds.
   *
   * @return    The same length as a Duration.
   */
  static constexpr Duration fromMs(const uint64_t ms) noexcept {
    return Duration(ms * kFsPerMs);
  }

  /**
   * Duration of `sec` seconds. The femtosecond product wraps modulo 2^64 fs,
   * as in `Time::fromSec`.
   *
   * @param sec  Length in seconds.
   *
   * @return     The same length as a Duration.
   */
  static constexpr Duration fromSec(const uint64_t sec) noexcept {
    return Duration(sec * kFsPerSec);
  }

  /**
   * The span from `start` to `end`, the typed form of `end - start` (which
   * yields a `Time`).
   *
   * @param start  Earlier point.
   * @param end    Later point.
   *
   * @return       Length of the span.
   *
   * @throw std::runtime_error if `end` is before `start`.
   */
  static Duration between(const Time start, const Time end) {
    return Duration((end - start).getFs());
  }

  // Length in femtoseconds.
  constexpr uint64_t getFs() const noexcept {
    return durationFs_;
  }

  // Whole picoseconds, truncated toward zero.
  constexpr uint64_t getPs() const noexcept {
    return durationFs_ / kFsPerPs;
  }

  // Whole nanoseconds, truncated toward zero.
  constexpr uint64_t getNs() const noexcept {
    return durationFs_ / kFsPerNs;
  }

  // Whole microseconds, truncated toward zero.
  constexpr uint64_t getUs() const noexcept {
    return durationFs_ / kFsPerUs;
  }

  // Whether the length is zero.
  constexpr bool isZero() const noexcept {
    return durationFs_ == 0;
  }

  // Ordered by length; also declares `==`.
  constexpr auto operator<=>(const Duration&) const noexcept = default;

  /**
   * Sum of two durations; wraps modulo 2^64 fs, as `Time` addition does.
   *
   * @param lhs  First duration.
   * @param rhs  Second duration.
   *
   * @return     The combined length.
   */
  friend constexpr Duration operator+(
      const Duration lhs,
      const Duration rhs) noexcept {
    return Duration(lhs.durationFs_ + rhs.durationFs_);
  }

  /**
   * `lhs` shortened by `rhs`.
   *
   * @param lhs  Duration to shorten.
   * @param rhs  Length to take off.
   *
   * @return     The remaining length.
   *
   * @throw std::runtime_error if `rhs` is longer than `lhs`.
   */
  friend constexpr Duration operator-(const Duration lhs, const Duration rhs) {
    if (rhs.durationFs_ > lhs.durationFs_) {
      throw std::runtime_error("Duration difference is negative");
    }
    return Duration(lhs.durationFs_ - rhs.durationFs_);
  }

  /**
   * Add `rhs` in place; wraps like `operator+`.
   *
   * @param rhs  Duration to add.
   *
   * @return     This duration.
   */
  constexpr Duration& operator+=(const Duration rhs) noexcept {
    durationFs_ += rhs.durationFs_;
    return *this;
  }

  /**
   * Take `rhs` off in place.
   *
   * @param rhs  Length to take off.
   *
   * @return     This duration.
   *
   * @throw std::runtime_error if `rhs` is longer than this duration, which is
   *        then left unchanged.
   */
  constexpr Duration& operator-=(const Duration rhs) {
    *this = *this - rhs;
    return *this;
  }

  /**
   * The point `rhs` after `lhs`, computed with `Time` addition, so it wraps
   * as `Time` does.
   *
   * @param lhs  Starting point.
   * @param rhs  Length to move forward.
   *
   * @return     The later point.
   */
  friend Time operator+(const Time lhs, const Duration rhs) noexcept {
    return lhs + Time(rhs.durationFs_);
  }

  /**
   * The point `rhs` before `lhs`, computed with `Time` subtraction.
   *
   * @param lhs  Starting point.
   * @param rhs  Length to move back.
   *
   * @return     The earlier point.
   *
   * @throw std::runtime_error if `rhs` reaches back before time zero.
   */
  friend Time operator-(const Time lhs, const Duration rhs) {
    return lhs - Time(rhs.durationFs_);
  }

  // A floating-point length must name its rounding rule; these would truncate
  // it silently.
  template <std::floating_point TFloat>
  static Duration fromPs(TFloat) = delete;
  template <std::floating_point TFloat>
  static Duration fromNs(TFloat) = delete;
  template <std::floating_point TFloat>
  static Duration fromUs(TFloat) = delete;
  template <std::floating_point TFloat>
  static Duration fromMs(TFloat) = delete;
  template <std::floating_point TFloat>
  static Duration fromSec(TFloat) = delete;

 private:
  // Femtoseconds per picosecond, nanosecond, microsecond, millisecond and
  // second.
  static constexpr uint64_t kFsPerPs = 1'000ULL;
  static constexpr uint64_t kFsPerNs = 1'000'000ULL;
  static constexpr uint64_t kFsPerUs = 1'000'000'000ULL;
  static constexpr uint64_t kFsPerMs = 1'000'000'000'000ULL;
  static constexpr uint64_t kFsPerSec = 1'000'000'000'000'000ULL;

  // Length in femtoseconds.
  uint64_t durationFs_{0};

  // Exclusive upper bound of a floating-point length in units of
  // `kFsPerUnit` femtoseconds: the whole units in 2^64 fs.
  template <uint64_t kFsPerUnit>
  static constexpr uint64_t getUnitsUpperBound() noexcept {
    return std::numeric_limits<uint64_t>::max() / kFsPerUnit;
  }

  /**
   * `units` source units of `kFsPerUnit` femtoseconds each, with everything
   * below the source unit dropped.
   *
   * @param units  Length in source units, in [0, getUnitsUpperBound()).
   *
   * @return       The truncated length as a Duration.
   */
  template <uint64_t kFsPerUnit, std::floating_point TFloat>
  static constexpr Duration fromUnits(
      const TFloat units,
      Rounding::TruncateToUnit) noexcept {
    checkRange<kFsPerUnit>(units);
    return Duration(static_cast<uint64_t>(units) * kFsPerUnit);
  }

  /**
   * `units` source units of `kFsPerUnit` femtoseconds each, rounded to the
   * nearest femtosecond, halfway cases up.
   *
   * @param units  Length in source units, in [0, getUnitsUpperBound()).
   *
   * @return       The rounded length as a Duration.
   */
  template <uint64_t kFsPerUnit, std::floating_point TFloat>
  static constexpr Duration fromUnits(
      const TFloat units,
      Rounding::NearestFs) noexcept {
    checkRange<kFsPerUnit>(units);
    const double fs =
        static_cast<double>(units) * static_cast<double>(kFsPerUnit);
    const auto wholeFs = static_cast<uint64_t>(fs);
    // Not `fs + 0.5`, which rounds 0.49999999999999994 and odd counts above
    // 2^52 up. `fs - wholeFs` is exact below 2^53 fs, and above it every
    // double is already whole.
    return Duration(
        wholeFs + (fs - static_cast<double>(wholeFs) >= 0.5 ? 1 : 0));
  }

  /**
   * `units` source units of `kFsPerUnit` femtoseconds each, rounded up to the
   * next whole femtosecond.
   *
   * @param units  Length in source units, in [0, getUnitsUpperBound()).
   *
   * @return       The rounded-up length as a Duration.
   */
  template <uint64_t kFsPerUnit, std::floating_point TFloat>
  static constexpr Duration fromUnits(
      const TFloat units,
      Rounding::CeilFs) noexcept {
    checkRange<kFsPerUnit>(units);
    const double fs =
        static_cast<double>(units) * static_cast<double>(kFsPerUnit);
    const auto wholeFs = static_cast<uint64_t>(fs);
    return Duration(wholeFs + (fs > static_cast<double>(wholeFs) ? 1 : 0));
  }

  /**
   * In debug builds, abort unless `units` is in [0, getUnitsUpperBound()):
   * converting a negative, NaN or larger value to an integer is undefined
   * behaviour. Other builds do not check.
   *
   * @param units  Length in source units of `kFsPerUnit` femtoseconds.
   */
  template <uint64_t kFsPerUnit, std::floating_point TFloat>
  static constexpr void checkRange(const TFloat units) noexcept {
    if constexpr (folly::kIsDebug) {
      // In double, where the bound is exact; a float bound would round it.
      constexpr auto kUpperBound =
          static_cast<double>(getUnitsUpperBound<kFsPerUnit>());
      const auto value = static_cast<double>(units);
      if (!std::is_constant_evaluated() &&
          !(value >= 0 && value < kUpperBound)) {
        reportOutOfRange(value, kFsPerUnit);
      }
    }
  }

  /**
   * Abort, naming a floating-point length outside its factory's range.
   *
   * @param units      The length, in source units.
   * @param fsPerUnit  Femtoseconds per source unit, e.g. 1000000000 for us.
   */
  [[noreturn]] static void reportOutOfRange(double units, uint64_t fsPerUnit);
};

} // namespace packsim

/**
 * Prints a `Duration` like a `Time`, in the largest unit it fills: 1500000000
 * fs prints as "1.5us".
 */
template <>
struct fmt::formatter<packsim::Duration> : fmt::formatter<packsim::Time> {
  template <typename FormatContext>
  auto format(const packsim::Duration duration, FormatContext& ctx) const {
    return fmt::formatter<packsim::Time>::formatFs(duration.getFs(), ctx);
  }
};
