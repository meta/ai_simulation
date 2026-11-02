/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/kernel/duration.h"

#include <cstdint>
#include <limits>

#include <folly/logging/xlog.h>

namespace packsim {

void Duration::reportOutOfRange(const double units, const uint64_t fsPerUnit) {
  // Only debug builds check, so the floating-point factories cost nothing in
  // other builds; callers validate a length where it enters, e.g. at config
  // load. This catches a caller that skipped that.
  XLOGF(
      FATAL,
      "Duration of {} units at {} fs per unit is outside [0, {})",
      units,
      fsPerUnit,
      std::numeric_limits<uint64_t>::max() / fsPerUnit);
}

} // namespace packsim
