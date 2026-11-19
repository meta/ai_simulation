/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/Event.h"
#include "packsim/kernel/pdes/PdesEngine.h"
#include "packsim/kernel/pdes/SimulationContext.h"

namespace packsim::pdes {

TEST(EventDeathTest, CancelFromAnotherContextAbortsInDebugBuilds) {
  // The statement builds its own engine: in a debug build it runs in the death
  // test's forked child, and this binary starts no thread before the fork, so
  // the engine's worker can start there. In an opt build the check is compiled
  // out and the statement runs here, on one worker, where the cancel is safe.
  // NOLINTNEXTLINE(facebook-gtest-expect-death)
  EXPECT_DEBUG_DEATH(
      {
        auto pdes = PdesEngine(/* lookaheadTimeFs= */ 100, /* numThreads= */ 1);
        SimulationContext canceller;
        SimulationContext target;
        const Event event =
            pdes.scheduleAt(&target, /* timeFs= */ 150, []() {});
        pdes.scheduleAt(
            &canceller, /* timeFs= */ 10, [&event]() { event.cancel(); });
        pdes.run();
      },
      "cancelled an event of context");
}

} // namespace packsim::pdes
