/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/endpoint/runtime/dependency-info-builder.h"
#include "packsim/if/gen-cpp2/config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"

namespace packsim {
namespace {

// Registers an internal workload `internalName` (an expansion of user-facing
// `originalName`) with internal id `id`.
void addInternalWorkload(
    packsim::thrift::WorkloadNewSchedule& schedule,
    const std::string& internalName,
    const std::string& originalName,
    uint32_t id) {
  auto& info = (*schedule.workloadNameInfos())[internalName];
  info.originalWorkloadName() = originalName;
  info.internalWorkloadId() = id;
}

// Registers workload `name` with internal id `id` (internal name == original
// name; each original expands to a single internal workload).
void addWorkload(
    packsim::thrift::WorkloadNewSchedule& schedule,
    const std::string& name,
    uint32_t id) {
  addInternalWorkload(schedule, name, name, id);
}

// Registers internal workload `internalName`, an expansion of `originalName`,
// with internal id `id`, placing rank r on the node with ID nodeIds[r].
void addScheduledInternalWorkload(
    packsim::thrift::WorkloadNewSchedule& schedule,
    const std::string& internalName,
    const std::string& originalName,
    uint32_t id,
    const std::vector<uint32_t>& nodeIds) {
  addInternalWorkload(schedule, internalName, originalName, id);
  auto& workload = schedule.workloads()->emplace_back();
  workload.name() = internalName;
  workload.workloadId() = id;
  for (uint32_t rank = 0; rank < nodeIds.size(); ++rank) {
    (*workload.rankToId())[rank] = {nodeIds.at(rank)};
  }
}

// Registers workload `name` with internal id `id`, placing rank r on the node
// with ID nodeIds[r].
void addScheduledWorkload(
    packsim::thrift::WorkloadNewSchedule& schedule,
    const std::string& name,
    uint32_t id,
    const std::vector<uint32_t>& nodeIds) {
  addScheduledInternalWorkload(schedule, name, name, id, nodeIds);
}

// Builds a WorkloadDag from a {workloadName -> dependencyNames} mapping.
packsim::workload::thrift::WorkloadDag makeDag(
    const std::map<std::string, std::vector<std::string>>& deps) {
  packsim::workload::thrift::WorkloadDag dag;
  auto& dagDeps = *dag.workloadDependencies();
  for (const auto& [name, dependencyNames] : deps) {
    auto& depSet = dagDeps[name];
    for (const auto& dependencyName : dependencyNames) {
      depSet.insert(dependencyName);
    }
  }
  return dag;
}

TEST(DependencyInfoBuilderTest, ReturnsNullWhenNoWorkloadDags) {
  packsim::thrift::Simulation sim;
  // The schedule is present by default but declares no workload DAGs.
  EXPECT_EQ(DependencyInfoBuilder::build(sim), nullptr);
}

TEST(DependencyInfoBuilderTest, BuildsDependencyGraphForSimpleDag) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  addWorkload(schedule, "B", 1);
  // B depends on A.
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  ASSERT_EQ(infos->dagList.size(), 1u);
  EXPECT_TRUE(infos->workloadIdDependencies.at(1).contains(0));
  EXPECT_TRUE(infos->workloadIdDependents.at(0).contains(1));
  EXPECT_EQ(infos->workloadToDagId.at(0), 0);
  EXPECT_EQ(infos->workloadToDagId.at(1), 0);
  // A has no dependencies -> a starting node; B has no dependents -> finishing.
  EXPECT_TRUE(infos->dagList.at(0).startingWorkloadIds.contains(0));
  EXPECT_TRUE(infos->dagList.at(0).finishingWorkloadIds.contains(1));
}

TEST(DependencyInfoBuilderTest, ThrowsWhenDagReferencesUnknownWorkload) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  // The DAG references "B", which was never registered as a workload.
  schedule.workloadDags().ensure().emplace_back(makeDag({{"B", {}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::HasSubstr("missing from input workloads list")));
}

TEST(DependencyInfoBuilderTest, ThrowsWhenWorkloadAppearsInMultipleDags) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  // "A" is placed in two separate DAGs, which is disallowed.
  auto& dags = schedule.workloadDags().ensure();
  dags.emplace_back(makeDag({{"A", {}}}));
  dags.emplace_back(makeDag({{"A", {}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::HasSubstr("already used by another DAG")));
}

TEST(DependencyInfoBuilderTest, ExpandsOriginalNameToAllInternalWorkloads) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  // Original "A" expands to two internal workloads (ids 0 and 1); "B" is id 2.
  addInternalWorkload(schedule, "A_r0", "A", 0);
  addInternalWorkload(schedule, "A_r1", "A", 1);
  addWorkload(schedule, "B", 2);
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  // B depends on both internal expansions of A.
  EXPECT_TRUE(infos->workloadIdDependencies.at(2).contains(0));
  EXPECT_TRUE(infos->workloadIdDependencies.at(2).contains(1));
  EXPECT_TRUE(infos->workloadIdDependents.at(0).contains(2));
  EXPECT_TRUE(infos->workloadIdDependents.at(1).contains(2));
}

TEST(
    DependencyInfoBuilderTest,
    ClassifiesMiddleNodeAsNeitherStartingNorFinishing) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  addWorkload(schedule, "B", 1);
  addWorkload(schedule, "C", 2);
  // Chain A -> B -> C.
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}, {"C", {"B"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  ASSERT_EQ(infos->dagList.size(), 1u);
  const auto& dag = infos->dagList.at(0);
  EXPECT_TRUE(dag.startingWorkloadIds.contains(0));
  EXPECT_TRUE(dag.finishingWorkloadIds.contains(2));
  // The middle node B has both a dependency and a dependent.
  EXPECT_FALSE(dag.startingWorkloadIds.contains(1));
  EXPECT_FALSE(dag.finishingWorkloadIds.contains(1));
}

TEST(DependencyInfoBuilderTest, BuildsMultipleIndependentDags) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  addWorkload(schedule, "B", 1);
  auto& dags = schedule.workloadDags().ensure();
  dags.emplace_back(makeDag({{"A", {}}}));
  dags.emplace_back(makeDag({{"B", {}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  EXPECT_EQ(infos->dagList.size(), 2u);
  EXPECT_EQ(infos->workloadToDagId.at(0), 0);
  EXPECT_EQ(infos->workloadToDagId.at(1), 1);
}

TEST(DependencyInfoBuilderTest, ThrowsWhenDependencyNameIsUnknown) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  // "A" depends on "Z", which was never registered.
  schedule.workloadDags().ensure().emplace_back(makeDag({{"A", {"Z"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::HasSubstr("missing from input workloads list")));
}

TEST(DependencyInfoBuilderTest, ThrowsWhenDagHasACycle) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  addWorkload(schedule, "B", 1);
  addWorkload(schedule, "C", 2);
  // A -> B -> C -> A: no workload is free to start.
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {"C"}}, {"B", {"A"}}, {"C", {"B"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(::testing::StrEq(
          "Workload DAG 0 has a dependency cycle, so these workloads can "
          "never start: A, B, C")));
}

TEST(DependencyInfoBuilderTest, ThrowsWhenWorkloadDependsOnItself) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  schedule.workloadDags().ensure().emplace_back(makeDag({{"A", {"A"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::EndsWith("can never start: A")));
}

TEST(DependencyInfoBuilderTest, NamesOnlyTheWorkloadsACycleBlocks) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "Start", 0);
  addWorkload(schedule, "A", 1);
  addWorkload(schedule, "B", 2);
  addWorkload(schedule, "Downstream", 3);
  addWorkload(schedule, "Side", 4);
  // Start can run, and so can Side after it. A and B wait on each other, and
  // Downstream waits behind A.
  schedule.workloadDags().ensure().emplace_back(makeDag(
      {{"Start", {}},
       {"A", {"Start", "B"}},
       {"B", {"A"}},
       {"Downstream", {"A"}},
       {"Side", {"Start"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::EndsWith("can never start: A, B, Downstream")));
}

TEST(DependencyInfoBuilderTest, NamesTheOriginalWorkloadOfACycleOnce) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  // Original "A" expands to two internal workloads; the cycle is reported by
  // the name the DAG was written with, once.
  addInternalWorkload(schedule, "A_r0", "A", 0);
  addInternalWorkload(schedule, "A_r1", "A", 1);
  addWorkload(schedule, "B", 2);
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {"B"}}, {"B", {"A"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(
          ::testing::EndsWith("can never start: A, B")));
}

TEST(DependencyInfoBuilderTest, NamesTheDagThatHasTheCycle) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addWorkload(schedule, "A", 0);
  addWorkload(schedule, "B", 1);
  addWorkload(schedule, "C", 2);
  // DAG 0 is fine; DAG 1 has B and C waiting on each other.
  auto& dags = schedule.workloadDags().ensure();
  dags.emplace_back(makeDag({{"A", {}}}));
  dags.emplace_back(makeDag({{"B", {"C"}}, {"C", {"B"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(::testing::StrEq(
          "Workload DAG 1 has a dependency cycle, so these workloads can "
          "never start: B, C")));
}

TEST(
    DependencyInfoBuilderTest,
    ThrowsWhenAWorkloadSharesNoNodeWithAnyDependency) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addScheduledWorkload(schedule, "A", 0, {0, 1});
  addScheduledWorkload(schedule, "B", 1, {2, 3});
  addScheduledWorkload(schedule, "C", 2, {1, 2});
  // B runs on other nodes than A, its only dependency. C shares node 1 with A
  // and node 2 with B.
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}, {"C", {"A", "B"}}}));

  EXPECT_THAT(
      [&] { DependencyInfoBuilder::build(sim); },
      ::testing::ThrowsMessage<std::invalid_argument>(::testing::StrEq(
          "Workload DAG 0 has workloads that share no node with any workload "
          "they depend on: B. A workload waits for a dependency only on the "
          "nodes the two share, so these would wait for none.")));
}

// Each rail of a two-rail workload depends on both rails of the workload before
// it, as a DAG of rail-split workloads does, but shares nodes only with its own
// rail.
TEST(
    DependencyInfoBuilderTest,
    AcceptsAWorkloadThatSharesANodeWithOnlySomeOfItsDependencies) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addScheduledInternalWorkload(schedule, "A_r0", "A", 0, {0, 2});
  addScheduledInternalWorkload(schedule, "A_r1", "A", 1, {1, 3});
  addScheduledInternalWorkload(schedule, "B_r0", "B", 2, {0, 2});
  addScheduledInternalWorkload(schedule, "B_r1", "B", 3, {1, 3});
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  EXPECT_THAT(
      infos->workloadIdDependencies.at(2),
      ::testing::UnorderedElementsAre(0, 1));
  EXPECT_THAT(
      infos->workloadIdDependencies.at(3),
      ::testing::UnorderedElementsAre(0, 1));
}

// The simulation pads a node ID with zeros, so [1] and [1, 0] name one node.
TEST(DependencyInfoBuilderTest, NodeIdsThatDifferByTrailingZerosNameOneNode) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addScheduledWorkload(schedule, "A", 0, {1});
  addScheduledWorkload(schedule, "B", 1, {1});
  (*schedule.workloads()->at(1).rankToId())[0] = {1, 0};
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  EXPECT_THAT(
      infos->workloadIdDependencies.at(1), ::testing::UnorderedElementsAre(0));
}

TEST(DependencyInfoBuilderTest, AcceptsAWorkloadOnMoreNodesThanItsDependency) {
  packsim::thrift::Simulation sim;
  auto& schedule = *sim.newWorkloadSchedule();
  addScheduledWorkload(schedule, "A", 0, {0, 1});
  addScheduledWorkload(schedule, "B", 1, {0, 1, 2, 3});
  schedule.workloadDags().ensure().emplace_back(
      makeDag({{"A", {}}, {"B", {"A"}}}));

  const auto infos = DependencyInfoBuilder::build(sim);
  ASSERT_NE(infos, nullptr);
  EXPECT_THAT(
      infos->workloadIdDependencies.at(1), ::testing::UnorderedElementsAre(0));
}

} // namespace
} // namespace packsim
