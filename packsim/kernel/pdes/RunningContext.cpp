/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/kernel/pdes/RunningContext.h"

namespace packsim::pdes {

namespace {
// The field the calling thread tracks with RunningContext::trackThisThread();
// null on a thread that tracks none.
thread_local SimulationContext* const* tlsCurrentContext{nullptr};
} // namespace

void RunningContext::trackThisThread(
    SimulationContext* const& currentContext) noexcept {
  tlsCurrentContext = &currentContext;
}

const SimulationContext* RunningContext::get() noexcept {
  return tlsCurrentContext != nullptr ? *tlsCurrentContext : nullptr;
}

} // namespace packsim::pdes
