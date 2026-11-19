/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/container/F14Map.h>

#include "packsim/endpoint/runtime/global-dag-coordinator.h"
#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {

/**
 * Information about a single dependency DAG within the workload graph.
 */
struct DependencyDagInfos {
  // All workload IDs in this DAG.
  folly::F14FastSet<uint32_t /* workload ID */> allWorkloadIds;
  // Workload IDs in this DAG with no dependencies.
  folly::F14FastSet<uint32_t /* workload ID */> startingWorkloadIds;
  // Workload IDs in this DAG with no dependents.
  folly::F14FastSet<uint32_t /* workload ID */> finishingWorkloadIds;
};

/**
 * Complete dependency information for all workloads in a simulation.
 */
struct DependencyInfos {
  // For each workload, the set of workloads that depend on it (its successors).
  folly::F14FastMap<
      uint32_t /* workload ID */,
      folly::F14FastSet<uint32_t /* succeeding workload ID */>>
      workloadIdDependents{};
  // For each workload, the set of workloads it depends on (its predecessors).
  folly::F14FastMap<
      uint32_t /* workload ID */,
      folly::F14FastSet<uint32_t /* preceding workload ID */>>
      workloadIdDependencies{};
  // One entry per independent dependency DAG in the workload graph.
  std::vector<DependencyDagInfos> dagList{};
  // Maps each workload ID to the index of its DAG in dagList.
  folly::F14FastMap<
      uint32_t /* workload ID */,
      uint32_t /* DAG ID (index in dagList) */>
      workloadToDagId{};
  // Minimum iteration count required across all DAGs, if constrained.
  std::optional<uint32_t> minIterationsPerDag{};
};

/**
 * Tracks workload dependencies and coordinates execution of dependent
 * workloads.
 */
class DependencyTracker {
 public:
  // The node asks the tracker to watch its DAG; these are how the tracker asks
  // back. Both are commands the tracker issues, not notifications it sends, so
  // they are `Fn` rather than callbacks.

  // Asks the node to start a workload that has nothing left to wait for on
  // this node: each of its dependencies with a rank here has completed, or
  // none has one. The workload starts once that holds on every node it uses.
  using TriggerWorkloadFn = std::function<void(uint32_t /* workload ID */)>;

  // Rebuilds a workload rank for the next iteration of its DAG.
  using BuildWorkloadFn = std::function<void(
      const packsim::workload::thrift::Workload& /* workload config */,
      const uint32_t /* rank */,
      const uint32_t /* iteration */)>;

  using WorkloadDependencyMap = folly::F14FastMap<
      uint32_t /* workload ID */,
      folly::F14FastSet<uint32_t /* preceding workload ID */>>;
  using DagWorkloadMap = folly::F14FastMap<
      uint32_t /* DAG ID */,
      folly::F14FastSet<uint32_t /* workload ID */>>;

  // The ranks a node hosts. Ordered by rank so that a workload's ranks start
  // in the same order on every run. A workload in a DAG places at most one
  // rank on a node. A value map rather than F14FastMap, which is a vector map
  // for a value this size: that visits a node's workloads in another order once
  // it holds 15 or more, and the order breaks ties between workloads that start
  // at the same time.
  using NodeWorkloadMap = folly::F14ValueMap<
      uint32_t /* workload ID */,
      std::map<uint32_t /* rank */, std::unique_ptr<packsim::WorkloadRank>>>;

  /**
   * Constructs a DependencyTracker for managing workload dependencies.
   *
   * @param workloads                Ranks hosted on this node
   * @param infos                    Dependency information for all workloads
   * @param triggerWorkloadFn        Asks the node to start a workload that
   * has nothing left to wait for on this node
   * @param buildWorkloadFn          Rebuilds a workload rank for the next
   * iteration
   * @param globalDagCoordinator     Global DAG coordinator for cross-node
   * synchronization
   * @param simCtx                   Simulation context
   */
  DependencyTracker(
      NodeWorkloadMap const& workloads,
      DependencyInfos const& infos,
      TriggerWorkloadFn const& triggerWorkloadFn,
      BuildWorkloadFn const& buildWorkloadFn,
      GlobalDagCoordinator* FOLLY_NONNULL globalDagCoordinator,
      SimContext* FOLLY_NONNULL simCtx);

  /**
   * Notifies the tracker that a workload has completed.
   *
   * @param workloadId The ID of the completed workload
   * @throw std::invalid_argument if minIterationsPerDag is set and a workload
   * in a DAG has more than one rank on this node
   */
  void postWorkloadCompletion(uint32_t const workloadId);

  /**
   * Triggers every workload on this node whose dependencies all run on other
   * nodes, since it has nothing to wait for here. A workload without
   * dependencies is not triggered: the node starts it at its start time.
   *
   * NOTE: Call it once, when the simulation starts.
   */
  void triggerWorkloadsWithOnlyRemoteDependencies();

  /**
   * Gets the dependency information.
   *
   * @return Pointer to the DependencyInfos structure
   */
  const DependencyInfos* getDependencyInfos() const;

  /**
   * Filters a dependency map to include only dependencies for node-local
   * workloads.
   *
   * @param originalDepMap Original dependency map
   * @param nodeWorkloads  Workloads local to this node
   * @return               Filtered dependency map
   */
  static WorkloadDependencyMap filterDependencyMapByNodeWorkloads(
      WorkloadDependencyMap const& originalDepMap,
      NodeWorkloadMap const& nodeWorkloads);

  /**
   * Filters a DAG-to-workload map to include only node-local workloads.
   *
   * @param infos         Dependency information
   * @param nodeWorkloads Workloads local to this node
   * @return              Filtered DAG-to-workload map
   */
  static DagWorkloadMap filterDagWorkloadMapByNodeWorkloads(
      DependencyInfos const& infos,
      NodeWorkloadMap const& nodeWorkloads);

  /**
   * Checks if a DAG iteration is complete and triggers the next iteration if
   * needed.
   *
   * @param simCtx                    Simulation context
   * @param globalDagCoordinator      Global DAG coordinator
   * @param workloadIdDependencies    Map of workload dependencies
   * @param dagToWorkloadIds          Map of DAG IDs to workload IDs
   * @param workloadId                The workload ID that just completed
   * @param infos                     Dependency information
   * @param workloads                 Ranks hosted on this node, one per
   * workload
   * @param triggerWorkloadFn         Asks the node to start a workload that
   * has nothing left to wait for on this node
   * @param buildWorkloadFn           Rebuilds a workload rank for the next
   * iteration
   * @throw std::invalid_argument if a workload in a DAG has more than one rank
   * on this node
   */
  static void checkAndTriggerNextIteration(
      SimContext* FOLLY_NONNULL simCtx,
      GlobalDagCoordinator* FOLLY_NONNULL globalDagCoordinator,
      WorkloadDependencyMap& workloadIdDependencies,
      DagWorkloadMap& dagToWorkloadIds,
      uint32_t const workloadId,
      DependencyInfos const& infos,
      NodeWorkloadMap const& workloads,
      TriggerWorkloadFn const& triggerWorkloadFn,
      BuildWorkloadFn const& buildWorkloadFn);

 private:
  // Reference to the ranks hosted on this node
  NodeWorkloadMap const& workloads_;
  // Reference to dependency information
  DependencyInfos const& infos_;
  // Asks the node to start a workload that has nothing left to wait for on
  // this node.
  TriggerWorkloadFn const triggerWorkloadFn_;
  // Rebuilds a workload rank for the next iteration.
  BuildWorkloadFn const buildWorkloadFn_;

  // Filtered dependency map for node-local workloads
  WorkloadDependencyMap workloadIdDependencies_;
  // Filtered DAG-to-workload map for node-local workloads
  DagWorkloadMap dagToWorkloadIds_;
  // Global DAG coordinator for cross-node synchronization
  GlobalDagCoordinator* globalDagCoordinator_;
  // Simulation context
  SimContext* simCtx_;
};

} // namespace packsim
