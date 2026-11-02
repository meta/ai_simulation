/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/kernel/ticker.h"

#include <stdexcept>
#include <utility>

#include <folly/logging/xlog.h>

#include "packsim/kernel/sim-context.h"

namespace packsim {

namespace {

Duration requireNonZeroPeriod(const Duration period) {
  if (period.isZero()) {
    throw std::invalid_argument(
        "Ticker period is zero; it must be at least 1 fs");
  }
  return period;
}

Ticker::OnTickCallback requireSubscriber(Ticker::OnTickCallback onTick) {
  if (!onTick) {
    throw std::invalid_argument("Ticker has no tick subscriber");
  }
  return onTick;
}

} // namespace

Ticker::Ticker(SimContext& simCtx, Options options)
    : simCtx_(simCtx),
      period_(requireNonZeroPeriod(options.period)),
      onTick_(requireSubscriber(std::move(options.onTick))) {
  restart();
}

Ticker::~Ticker() {
  stop();
}

void Ticker::restart() {
  nextTick_.cancel();
  nextTick_ = simCtx_.schedule(period_, &Ticker::tick, this);
}

void Ticker::stop() noexcept {
  nextTick_.cancel();
}

void Ticker::tick() {
  nextTick_ = simCtx_.schedule(period_, &Ticker::tick, this);
  onTick();
}

void Ticker::onTick() {
  XCHECK(onTick_) << "Ticker has no tick subscriber";
  onTick_();
}

} // namespace packsim
