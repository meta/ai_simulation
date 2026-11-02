/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>

#include "packsim/kernel/duration.h"
#include "packsim/kernel/scheduler.h"

namespace packsim {

struct SimContext;

/**
 * Repeating work on the simulation clock. A `Ticker` raises its `onTick`
 * every `period`, starting one period after it is built, until it is stopped
 * or destroyed. Its period cannot be zero, so a periodic loop cannot become an
 * endless chain of zero-delay events. "Off" is a ticker that was never built:
 * hold it in a `std::optional`.
 *
 * Each tick arms the next one before raising `onTick`, so an event that
 * `onTick` schedules for the same time as the next tick runs after it.
 * `onTick` may stop or restart its ticker, but must not destroy it.
 *
 * Example usage:
 *
 *      std::optional<Ticker> statsTicker_;
 *      statsTicker_.emplace(
 *          *simCtx_,
 *          Ticker::Options{
 *              .period = Duration::fromUs(1),
 *              .onTick = [this] { sampleStats(); }});
 *
 * NOTE: Build, restart, stop and destroy a `Ticker` only from events of its own
 * `SimContext`, or while the simulation is not running, as for
 * `SimContext::schedule`.
 */
class Ticker {
 public:
  using OnTickCallback = std::function<void()>;

  struct Options {
    // Time between ticks. Must not be zero.
    Duration period;
    // Raised at every tick.
    OnTickCallback onTick;
  };

  /**
   * Start ticking: the first tick is one period from now.
   *
   * @param simCtx   Context whose events run the ticks. Must outlive the
   *                 ticker.
   * @param options  Period and tick subscriber.
   *
   * @throw std::invalid_argument if `options.period` is zero or
   *        `options.onTick` is empty.
   * @throw std::runtime_error as `SimContext::schedule` does.
   */
  Ticker(SimContext& simCtx, Options options);

  // Cancels the pending tick.
  ~Ticker();

  // The pending tick holds `this`, so a ticker never moves.
  Ticker(const Ticker&) = delete;
  Ticker& operator=(const Ticker&) = delete;
  Ticker(Ticker&&) = delete;
  Ticker& operator=(Ticker&&) = delete;

  /**
   * Make the next tick one period from now, whether the ticker was running or
   * stopped.
   *
   * @throw std::runtime_error as `SimContext::schedule` does.
   */
  void restart();

  /**
   * Cancel the pending tick. Called from `onTick`, it cancels the tick that
   * this one has just armed. No-op if already stopped.
   */
  void stop() noexcept;

  // Whether a tick is pending.
  bool isRunning() const noexcept {
    return nextTick_.isActive();
  }

  // Time between ticks.
  Duration getPeriod() const noexcept {
    return period_;
  }

 private:
  // Arm the next tick, then raise onTick.
  void tick();

  // Raise the tick subscriber; aborts if there is none.
  void onTick();

  // Context whose events run the ticks.
  SimContext& simCtx_;
  // Time between ticks.
  const Duration period_;
  // Tick subscriber.
  const OnTickCallback onTick_;
  // The next tick; inactive once stopped.
  Scheduler::EventHandle nextTick_;
};

} // namespace packsim
