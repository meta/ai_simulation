/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/RunningContext.h"
#include "packsim/kernel/pdes/SimulationContext.h"

namespace packsim::pdes {

TEST(RunningContext, IsNullOnAThreadThatTracksNoFieldWhileAnotherDoes) {
  SimulationContext context;
  const SimulationContext* runningContext = &context;

  std::thread([&context, &runningContext] {
    SimulationContext* currentContext = &context;
    RunningContext::trackThisThread(currentContext);
    std::thread([&runningContext] {
      runningContext = RunningContext::get();
    }).join();
  }).join();

  EXPECT_EQ(runningContext, nullptr);
}

TEST(RunningContext, FollowsTheFieldItsThreadTracks) {
  SimulationContext context;
  std::vector<const SimulationContext*> seenContexts;

  std::thread([&context, &seenContexts] {
    SimulationContext* currentContext = nullptr;
    RunningContext::trackThisThread(currentContext);
    seenContexts.emplace_back(RunningContext::get());
    currentContext = &context;
    seenContexts.emplace_back(RunningContext::get());
    currentContext = nullptr;
    seenContexts.emplace_back(RunningContext::get());
  }).join();

  const std::vector<const SimulationContext*> expected{
      nullptr, &context, nullptr};
  EXPECT_EQ(seenContexts, expected);
}

} // namespace packsim::pdes
