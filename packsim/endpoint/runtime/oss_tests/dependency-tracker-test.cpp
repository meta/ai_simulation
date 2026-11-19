/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <folly/container/F14Map.h>
#include <gmock/gmock-function-mocker.h>
#include <gtest/gtest.h>

#include "packsim/endpoint/runtime/dependency-tracker.h"
#include "packsim/endpoint/runtime/global-dag-coordinator.h"

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Key;
using ::testing::MockFunction;
using ::testing::Ne;
using ::testing::Pair;
using ::testing::ThrowsMessage;
using ::testing::UnorderedElementsAre;

namespace packsim {

namespace {
class MockWorkloadRank : public WorkloadRank {
 public:
  explicit MockWorkloadRank(WorkloadRankParams const& params)
      : WorkloadRank(params) {}

  MOCK_METHOD(void, run, (), (override));
  MOCK_METHOD(uint64_t, getExpectedBytes, (), (override));
  MOCK_METHOD(bool, complete, (), (override));
};

class MockGlobalDagCoordinator : public GlobalDagCoordinator {
 public:
  explicit MockGlobalDagCoordinator(
      Scheduler* scheduler,
      size_t const numDags,
      uint32_t const minIters)
      : GlobalDagCoordinator(scheduler, numDags, minIters) {}

  MOCK_METHOD(
      void,
      addDagNode,
      (uint32_t const, SimContext* const),
      (override));
  MOCK_METHOD(
      void,
      arrive,
      (uint32_t const,
       SimContext* const,
       std::function<void(uint32_t /* iter # */)>),
      (override));
};
} // namespace

TEST(DependencyTracker, TestTriggerDependents) {
  // Workloads 0 and 7 are not placed on our test node here (it only has
  // workloads 1 to 6), so they should be filtered automatically inside the
  // dependency tracker.
  DependencyInfos infos{
      .workloadIdDependents =
          {{0, {4}},
           {1, {4, 5}},
           {2, {4, 5}},
           {3, {4, 5}},
           {4, {6, 7}},
           {5, {6, 7}}},
      .workloadIdDependencies =
          {{4, {0, 1, 2, 3}}, {5, {1, 2, 3}}, {6, {4, 5}}, {7, {4, 5}}},
  };

  DependencyTracker::NodeWorkloadMap workloads;

  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};

  packsim::workload::thrift::Workload workCfg;
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;
  WorkloadRankParams params{
      .simCtx = &simCtx,
      .workCfg = workCfg,
      .smcb = smcb,
      .rmcb = rmcb,
      .rccb = rccb};

  for (uint32_t i = 1; i <= 6; ++i) {
    workloads[i].emplace(0, std::make_unique<MockWorkloadRank>(params));
  }

  MockFunction<void(uint32_t /* workload ID */)> triggerWorkloadCb;
  MockFunction<void(
      const packsim::workload::thrift::Workload& /* workload config */,
      const uint32_t /* rank */,
      const uint32_t /* iteration */)>
      buildWorkloadCb;

  auto mockGlobalDagCoordinator =
      std::make_unique<MockGlobalDagCoordinator>(&scheduler, 0, 0);

  DependencyTracker dt(
      workloads,
      infos,
      triggerWorkloadCb.AsStdFunction(),
      buildWorkloadCb.AsStdFunction(),
      mockGlobalDagCoordinator.get(),
      &simCtx);

  EXPECT_CALL(triggerWorkloadCb, Call(6)).Times(1).WillRepeatedly([&dt]() {
    dt.postWorkloadCompletion(6);
  });
  EXPECT_CALL(triggerWorkloadCb, Call(5)).Times(1).WillRepeatedly([&dt]() {
    dt.postWorkloadCompletion(5);
  });
  EXPECT_CALL(triggerWorkloadCb, Call(4)).Times(1).WillRepeatedly([&dt]() {
    dt.postWorkloadCompletion(4);
  });

  {
    dt.postWorkloadCompletion(1);
    dt.postWorkloadCompletion(2);
    dt.postWorkloadCompletion(3);

    // If a workload that has dependents is completed more than once we throw.
    for (uint32_t i = 1; i <= 5; ++i) {
      EXPECT_THAT(
          ([&dt, i]() { dt.postWorkloadCompletion(i); }),
          ThrowsMessage<std::runtime_error>(HasSubstr(
              fmt::format(
                  "Dependency relationship missing for workload {}", i))));
    }

    // Posting completion for workload 6 has no effect since it has no
    // dependents. It also must not throw.
    dt.postWorkloadCompletion(6);
  }
}

TEST(DependencyTracker, TestFilterDagWorkloadMapByNodeWorkloads) {
  DependencyTracker::NodeWorkloadMap workloads;

  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};

  packsim::workload::thrift::Workload workCfg;
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;
  WorkloadRankParams params{
      .simCtx = &simCtx,
      .workCfg = workCfg,
      .smcb = smcb,
      .rmcb = rmcb,
      .rccb = rccb};

  for (uint32_t i = 1; i <= 10; ++i) {
    workloads[i].emplace(0, std::make_unique<MockWorkloadRank>(params));
  }

  DependencyInfos infos{
      .dagList =
          {
              DependencyDagInfos{
                  .allWorkloadIds = {1, 2},
                  .startingWorkloadIds = {1},
                  .finishingWorkloadIds = {2},
              },
              DependencyDagInfos{
                  .allWorkloadIds = {3, 4, 5, 6, 7, 8, 9},
                  .startingWorkloadIds = {3, 4},
                  .finishingWorkloadIds = {8, 9},
              },
          },
      .workloadToDagId =
          {
              {1, 0},
              {2, 0},
              {3, 1},
              {4, 1},
              {5, 1},
              {6, 1},
              {7, 1},
              {8, 1},
              {9, 1},
          },
  };

  auto dagToWorkloadIds =
      DependencyTracker::filterDagWorkloadMapByNodeWorkloads(infos, workloads);

  ASSERT_THAT(dagToWorkloadIds, UnorderedElementsAre(Key(0), Key(1)));

  EXPECT_THAT(dagToWorkloadIds.at(0), UnorderedElementsAre(1, 2));
  EXPECT_THAT(
      dagToWorkloadIds.at(1), UnorderedElementsAre(3, 4, 5, 6, 7, 8, 9));
}

TEST(DependencyTracker, TestCheckAndTriggerNextIteration) {
  DependencyTracker::NodeWorkloadMap workloads;

  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};

  std::vector<packsim::workload::thrift::Workload> workCfgs(11);
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;

  for (uint32_t i = 1; i <= 10; ++i) {
    workCfgs.at(i).workloadId() = i;
    WorkloadRankParams params{
        .simCtx = &simCtx,
        .workCfg = workCfgs.at(i),
        .rank = i,
        .smcb = smcb,
        .rmcb = rmcb,
        .rccb = rccb};
    workloads[i].emplace(i, std::make_unique<MockWorkloadRank>(params));
  }

  DependencyInfos infos{
      .workloadIdDependencies =
          {
              {2, {1}},
              {8, {3, 4}},
              {7, {3, 4, 5, 6}},
              {9, {7, 8}},
          },
      .dagList =
          {
              DependencyDagInfos{
                  .allWorkloadIds = {1, 2},
                  .startingWorkloadIds = {1},
                  .finishingWorkloadIds = {2},
              },
              DependencyDagInfos{
                  .allWorkloadIds = {3, 4, 5, 6, 7, 8, 9},
                  .startingWorkloadIds = {3, 4, 5, 6},
                  .finishingWorkloadIds = {9},
              },
          },
      .workloadToDagId =
          {
              {1, 0},
              {2, 0},
              {3, 1},
              {4, 1},
              {5, 1},
              {6, 1},
              {7, 1},
              {8, 1},
              {9, 1},
          },
  };

  folly::F14FastMap<uint32_t /* DAG ID */, uint32_t /* # iterations */>
      dagIterations = {
          {0, 1},
          {1, 2},
      };
  DependencyTracker::WorkloadDependencyMap workloadIdDependencies = {
      {2, {/* 1, */}},
      {8, {/* 3, */ 4}},
      {7, {/* 3, 4, */ 5, 6}},
      {9, {7, 8}},
  };
  DependencyTracker::DagWorkloadMap dagToWorkloadIds = {
      {0, {/* 1, */ 2}},
      {1, {/* 3, 4, */ 5, 6, 7, 8, 9}},
  };

  MockFunction<void(uint32_t /* workload ID */)> triggerWorkloadCb;
  MockFunction<void(
      const packsim::workload::thrift::Workload& /* workload config */,
      const uint32_t /* rank */,
      const uint32_t /* iteration */)>
      buildWorkloadCb;

  auto mockGlobalDagCoordinator =
      std::make_unique<MockGlobalDagCoordinator>(&scheduler, 0, 0);

  constexpr uint32_t minIterationsPerDag = 4;

  ON_CALL(*mockGlobalDagCoordinator, arrive(_, _, _))
      .WillByDefault(
          [&dagIterations](
              uint32_t const dagId,
              SimContext* const /* context */,
              std::function<void(uint32_t /* iter # */)> const& callback) {
            auto& numIters = dagIterations.at(dagId);
            numIters++;
            if (std::all_of(
                    dagIterations.begin(),
                    dagIterations.end(),
                    [](auto const& dagIdIterationsPair) {
                      return dagIdIterationsPair.second >= minIterationsPerDag;
                    })) {
              // All DAGs have completed the required # iterations, the
              // simulation can now end. We will not kick off any new
              // iterations.
              return;
            }
            callback(numIters);
          });

  auto checkAndTriggerNextIteration = [&](uint32_t const workloadId) {
    DependencyTracker::checkAndTriggerNextIteration(
        &simCtx,
        mockGlobalDagCoordinator.get(),
        workloadIdDependencies,
        dagToWorkloadIds,
        workloadId,
        infos,
        workloads,
        triggerWorkloadCb.AsStdFunction(),
        buildWorkloadCb.AsStdFunction());
  };

  // Calls for DAG 0
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(1), 1, 3)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(2), 2, 3)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(1), 1, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(2), 2, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(1), 1, 5)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(2), 2, 5)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(1)).Times(3);

  // Calls for DAG 1
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(3), 3, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(4), 4, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(5), 5, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(6), 6, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(7), 7, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(8), 8, 4)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(9), 9, 4)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(3)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(4)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(5)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(6)).Times(1);

  checkAndTriggerNextIteration(/* workloadId= */ 2);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 2), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 1);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 2), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 2);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 4);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 5);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 6);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 7);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 8);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 1);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 3), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 2);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 2)));

  checkAndTriggerNextIteration(/* workloadId= */ 9);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 3);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 4);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 5);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 6);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 7);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 8);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 3)));

  checkAndTriggerNextIteration(/* workloadId= */ 9);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 4)));

  checkAndTriggerNextIteration(/* workloadId= */ 1);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 4), Pair(1, 4)));

  checkAndTriggerNextIteration(/* workloadId= */ 2);
  EXPECT_THAT(dagIterations, UnorderedElementsAre(Pair(0, 5), Pair(1, 4)));
}

TEST(
    DependencyTracker,
    CheckAndTriggerNextIterationThrowsWhenAWorkloadHasTwoRanksOnTheNode) {
  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};
  packsim::workload::thrift::Workload workCfg;
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;
  DependencyTracker::NodeWorkloadMap workloads;
  for (uint32_t rank = 0; rank < 2; ++rank) {
    WorkloadRankParams params{
        .simCtx = &simCtx,
        .workCfg = workCfg,
        .rank = rank,
        .smcb = smcb,
        .rmcb = rmcb,
        .rccb = rccb};
    workloads[1].emplace(rank, std::make_unique<MockWorkloadRank>(params));
  }
  DependencyInfos infos;
  infos.workloadToDagId.emplace(1, 0);
  DependencyTracker::WorkloadDependencyMap workloadIdDependencies;
  DependencyTracker::DagWorkloadMap dagToWorkloadIds;
  auto mockGlobalDagCoordinator =
      std::make_unique<MockGlobalDagCoordinator>(&scheduler, 0, 0);

  EXPECT_THAT(
      [&]() {
        DependencyTracker::checkAndTriggerNextIteration(
            &simCtx,
            mockGlobalDagCoordinator.get(),
            workloadIdDependencies,
            dagToWorkloadIds,
            /*workloadId=*/1,
            infos,
            workloads,
            /*triggerWorkloadFn=*/{},
            /*buildWorkloadFn=*/{});
      },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("Workload 1 has 2 ranks on one node")));
}

TEST(DependencyTracker, TriggersOnlyWorkloadsWhoseDependenciesAllRunElsewhere) {
  // Workload 0 runs on another node. On this node, workload 1 has no
  // dependencies, 2 waits for 1, 3 waits only for 0, and 4 waits for 0 and 1.
  DependencyInfos infos{
      .workloadIdDependents = {{0, {3, 4}}, {1, {2, 4}}},
      .workloadIdDependencies = {{2, {1}}, {3, {0}}, {4, {0, 1}}},
  };

  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};
  packsim::workload::thrift::Workload workCfg;
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;
  WorkloadRankParams params{
      .simCtx = &simCtx,
      .workCfg = workCfg,
      .smcb = smcb,
      .rmcb = rmcb,
      .rccb = rccb};
  DependencyTracker::NodeWorkloadMap workloads;
  for (uint32_t i = 1; i <= 4; ++i) {
    workloads[i].emplace(0, std::make_unique<MockWorkloadRank>(params));
  }

  MockFunction<void(uint32_t /* workload ID */)> triggerWorkloadCb;
  auto mockGlobalDagCoordinator =
      std::make_unique<MockGlobalDagCoordinator>(&scheduler, 0, 0);
  DependencyTracker dt(
      workloads,
      infos,
      triggerWorkloadCb.AsStdFunction(),
      /*buildWorkloadFn=*/{},
      mockGlobalDagCoordinator.get(),
      &simCtx);

  EXPECT_CALL(triggerWorkloadCb, Call(3)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(Ne(3))).Times(0);
  dt.triggerWorkloadsWithOnlyRemoteDependencies();
}

TEST(
    DependencyTracker,
    NextIterationTriggersAWorkloadWhoseDependenciesAllRunElsewhere) {
  // DAG 0 holds workloads 0, 1 and 2, and 2 waits for 0. Workload 0 runs on
  // another node, so on this node 2 has nothing to wait for in any iteration.
  DependencyInfos infos{
      .workloadIdDependents = {{0, {2}}},
      .workloadIdDependencies = {{2, {0}}},
      .dagList =
          {
              DependencyDagInfos{
                  .allWorkloadIds = {0, 1, 2},
                  .startingWorkloadIds = {0, 1},
                  .finishingWorkloadIds = {1, 2},
              },
          },
      .workloadToDagId = {{0, 0}, {1, 0}, {2, 0}},
  };

  Scheduler scheduler;
  SimContext simCtx{&scheduler, 0};
  std::vector<packsim::workload::thrift::Workload> workCfgs(3);
  WorkloadRankParams::SubmitMessageFn smcb;
  WorkloadRankParams::SubmitMessageFn rmcb;
  WorkloadRankParams::OnRankCompleteCallback rccb;
  DependencyTracker::NodeWorkloadMap workloads;
  for (uint32_t i = 1; i <= 2; ++i) {
    workCfgs.at(i).workloadId() = i;
    workCfgs.at(i).rankToId() = {{i, {i}}};
    WorkloadRankParams params{
        .simCtx = &simCtx,
        .workCfg = workCfgs.at(i),
        .rank = i,
        .smcb = smcb,
        .rmcb = rmcb,
        .rccb = rccb};
    workloads[i].emplace(i, std::make_unique<MockWorkloadRank>(params));
  }
  DependencyTracker::WorkloadDependencyMap workloadIdDependencies;
  DependencyTracker::DagWorkloadMap dagToWorkloadIds = {{0, {1, 2}}};

  MockFunction<void(uint32_t /* workload ID */)> triggerWorkloadCb;
  MockFunction<void(
      const packsim::workload::thrift::Workload& /* workload config */,
      const uint32_t /* rank */,
      const uint32_t /* iteration */)>
      buildWorkloadCb;
  auto mockGlobalDagCoordinator =
      std::make_unique<MockGlobalDagCoordinator>(&scheduler, 0, 0);

  EXPECT_CALL(*mockGlobalDagCoordinator, arrive(0, &simCtx, _))
      .WillOnce([](uint32_t const /* dagId */,
                   SimContext* const /* context */,
                   std::function<void(uint32_t /* iter # */)> const& callback) {
        callback(/* numIter= */ 1);
      });
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(1), 1, 2)).Times(1);
  EXPECT_CALL(buildWorkloadCb, Call(workCfgs.at(2), 2, 2)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(1)).Times(1);
  EXPECT_CALL(triggerWorkloadCb, Call(2)).Times(1);

  for (uint32_t const workloadId : {1u, 2u}) {
    DependencyTracker::checkAndTriggerNextIteration(
        &simCtx,
        mockGlobalDagCoordinator.get(),
        workloadIdDependencies,
        dagToWorkloadIds,
        workloadId,
        infos,
        workloads,
        triggerWorkloadCb.AsStdFunction(),
        buildWorkloadCb.AsStdFunction());
  }
}

} // namespace packsim
