/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>
#include <folly/CPortability.h>

namespace packsim {

class Time {
 public:
  Time() = default;

  /**
   * Time of `fs` femtoseconds.
   *
   * @param fs  Duration in femtoseconds.
   */
  explicit Time(const uint64_t fs) noexcept : timeFs_(fs) {}

  /**
   * Time of `ps` picoseconds. The femtosecond product wraps modulo 2^64 fs
   * (about 5.1 hours).
   *
   * @param ps  Duration in picoseconds.
   *
   * @return    The same duration as a Time.
   */
  FOLLY_ALWAYS_INLINE static Time fromPs(const uint64_t ps) noexcept {
    return Time(ps * 1'000ULL);
  }

  /**
   * Time of `ns` nanoseconds. The femtosecond product wraps modulo 2^64 fs
   * (about 5.1 hours).
   *
   * @param ns  Duration in nanoseconds.
   *
   * @return    The same duration as a Time.
   */
  FOLLY_ALWAYS_INLINE static Time fromNs(const uint64_t ns) noexcept {
    return Time(ns * 1'000'000ULL);
  }

  /**
   * Time of `us` microseconds. The femtosecond product wraps modulo 2^64 fs
   * (about 5.1 hours).
   *
   * @param us  Duration in microseconds.
   *
   * @return    The same duration as a Time.
   */
  FOLLY_ALWAYS_INLINE static Time fromUs(const uint64_t us) noexcept {
    return Time(us * 1'000'000'000ULL);
  }

  /**
   * Time of `ms` milliseconds. The femtosecond product wraps modulo 2^64 fs
   * (about 5.1 hours).
   *
   * @param ms  Duration in milliseconds.
   *
   * @return    The same duration as a Time.
   */
  FOLLY_ALWAYS_INLINE static Time fromMs(const uint64_t ms) noexcept {
    return Time(ms * 1'000'000'000'000ULL);
  }

  /**
   * Time of `sec` seconds. The femtosecond product wraps modulo 2^64 fs
   * (about 5.1 hours).
   *
   * @param sec  Duration in seconds.
   *
   * @return     The same duration as a Time.
   */
  FOLLY_ALWAYS_INLINE static Time fromSec(const uint64_t sec) noexcept {
    return Time(sec * 1'000'000'000'000'000ULL);
  }

  friend Time operator+(const Time lhs, const Time rhs) {
    return Time(lhs.timeFs_ + rhs.timeFs_);
  }

  friend Time operator-(const Time lhs, const Time rhs) {
    if (rhs.timeFs_ > lhs.timeFs_) {
      throw std::runtime_error("Time difference is negative");
    }
    return Time(lhs.timeFs_ - rhs.timeFs_);
  }

  Time& operator+=(const Time& rhs) {
    timeFs_ += rhs.timeFs_;
    return *this;
  }

  Time& operator-=(const Time& rhs) {
    if (rhs.timeFs_ > timeFs_) {
      throw std::runtime_error("Time difference is negative");
    }
    timeFs_ -= rhs.timeFs_;
    return *this;
  }

  template <class T>
    requires std::is_arithmetic_v<T>
  friend Time operator*(const Time lhs, const T& rhs) {
    return Time(lhs.timeFs_ * rhs);
  }

  template <class T>
    requires std::is_arithmetic_v<T>
  friend Time operator*(const T& lhs, const Time rhs) {
    return Time(lhs * rhs.timeFs_);
  }

  template <class T>
    requires std::is_arithmetic_v<T>
  friend Time operator/(const Time lhs, const T& rhs) {
    return Time(lhs.timeFs_ / rhs);
  }

  double operator/(const Time& rhs) const {
    return static_cast<double>(timeFs_) / rhs.timeFs_;
  }

  template <class T>
  Time& operator*=(const T& rhs) {
    timeFs_ *= rhs;
    return *this;
  }

  template <class T>
  Time& operator/=(const T& rhs) {
    timeFs_ /= rhs;
    return *this;
  }

  // Times compare by their single member, so the defaults order them by
  // duration and supply every comparison operator.
  friend bool operator==(const Time& lhs, const Time& rhs) noexcept = default;
  friend auto operator<=>(const Time& lhs, const Time& rhs) noexcept = default;

  bool isZero() const noexcept {
    return timeFs_ == 0;
  }

  // Duration in femtoseconds.
  FOLLY_ALWAYS_INLINE uint64_t getFs() const noexcept {
    return timeFs_;
  }

  // Whole picoseconds, truncated toward zero.
  FOLLY_ALWAYS_INLINE uint64_t getPs() const noexcept {
    return timeFs_ / 1'000ULL;
  }

  // Whole nanoseconds, truncated toward zero.
  FOLLY_ALWAYS_INLINE uint64_t getNs() const noexcept {
    return timeFs_ / 1'000'000ULL;
  }

  // Whole microseconds, truncated toward zero.
  FOLLY_ALWAYS_INLINE uint64_t getUs() const noexcept {
    return timeFs_ / 1'000'000'000ULL;
  }

 private:
  uint64_t timeFs_{0};
};

} // namespace packsim

/**
 * Prints a `Time` as its distance from time zero, in the largest unit it fills
 * and without losing a femtosecond: 1500000000 fs prints as "1.5us", 300 ns as
 * "300ns", 1 fs as "1fs" and zero as "0s".
 *
 * Example usage:
 *
 *      // Logs "Sending NACK at 2.5us".
 *      XLOGF(DBG2, "Sending NACK at {}", simCtx.now());
 */
template <>
struct fmt::formatter<packsim::Time> {
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const packsim::Time time, FormatContext& ctx) const {
    return formatFs(time.getFs(), ctx);
  }

  /**
   * Print `fs` femtoseconds the way a `Time` prints, for a type that counts
   * them too, such as `Duration`.
   *
   * @param fs   Femtoseconds to print.
   * @param ctx  Context to print into.
   *
   * @return     The iterator past the printed text.
   */
  template <typename FormatContext>
  static auto formatFs(const uint64_t fs, FormatContext& ctx) {
    if (fs == 0) {
      return fmt::format_to(ctx.out(), "0s");
    }
    // A nonzero time fills at least the last unit, a femtosecond.
    return formatIn(
        fs,
        *std::ranges::find_if(
            kUnits, [fs](const Unit& unit) { return fs >= unit.fsPerUnit; }),
        ctx);
  }

 private:
  // A unit to print in.
  struct Unit {
    // Size of the unit in femtoseconds, e.g. 1000 for "ps".
    uint64_t fsPerUnit;
    // Decimal digits a fraction of the unit needs, e.g. 3 for "ps".
    int fractionDigits;
    // Printed after the number, e.g. "ps".
    std::string_view suffix;
  };

  // Largest first.
  static constexpr std::array<Unit, 6> kUnits{{
      {1'000'000'000'000'000ULL, 15, "s"},
      {1'000'000'000'000ULL, 12, "ms"},
      {1'000'000'000ULL, 9, "us"},
      {1'000'000ULL, 6, "ns"},
      {1'000ULL, 3, "ps"},
      {1ULL, 0, "fs"},
  }};

  /**
   * Print `fs` femtoseconds in `unit`, with the fraction's trailing zeros
   * dropped: 1500000000 fs in "us" prints as "1.5us".
   *
   * @param fs    Femtoseconds to print.
   * @param unit  Unit to print them in.
   * @param ctx   Context to print into.
   *
   * @return      The iterator past the printed text.
   */
  template <typename FormatContext>
  static auto
  formatIn(const uint64_t fs, const Unit& unit, FormatContext& ctx) {
    const uint64_t whole = fs / unit.fsPerUnit;
    uint64_t fraction = fs % unit.fsPerUnit;
    if (fraction == 0) {
      return fmt::format_to(ctx.out(), "{}{}", whole, unit.suffix);
    }
    int digits = unit.fractionDigits;
    while (fraction % 10 == 0) {
      fraction /= 10;
      --digits;
    }
    return fmt::format_to(
        ctx.out(), "{}.{:0{}}{}", whole, fraction, digits, unit.suffix);
  }
};
