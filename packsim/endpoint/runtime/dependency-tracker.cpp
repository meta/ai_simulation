/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/endpoint/runtime/dependency-tracker.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

namespace packsim {

namespace {
/**
 * The one rank that @p workloadId has on this node. A workload in a DAG places
 * at most one rank on a node.
 *
 * @param workloads   The ranks hosted on this node
 * @param workloadId  A workload with a rank on this node
 * @return            That rank
 * @throw std::invalid_argument if the workload has more than one rank here
 */
const WorkloadRank* FOLLY_NONNULL getOnlyRank(
    DependencyTracker::NodeWorkloadMap const& workloads,
    uint32_t const workloadId) {
  const auto& ranks = workloads.at(workloadId);
  if (ranks.size() != 1) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {} has {} ranks on one node, but a workload in a DAG may "
            "place only one",
            workloadId,
            ranks.size()));
  }
  return ranks.begin()->second.get();
}
} // namespace

DependencyTracker::DependencyTracker(
    NodeWorkloadMap const& workloads,
    DependencyInfos const& infos,
    DependencyTracker::TriggerWorkloadFn const& triggerWorkloadFn,
    DependencyTracker::BuildWorkloadFn const& buildWorkloadFn,
    GlobalDagCoordinator* FOLLY_NONNULL globalDagCoordinator,
    SimContext* FOLLY_NONNULL simCtx)
    : workloads_(workloads),
      infos_(infos),
      triggerWorkloadFn_(triggerWorkloadFn),
      buildWorkloadFn_(buildWorkloadFn),
      workloadIdDependencies_(filterDependencyMapByNodeWorkloads(
          infos_.workloadIdDependencies,
          workloads_)),
      dagToWorkloadIds_(
          filterDagWorkloadMapByNodeWorkloads(infos_, workloads_)),
      globalDagCoordinator_(globalDagCoordinator),
      simCtx_(simCtx) {}

void DependencyTracker::postWorkloadCompletion(uint32_t const workloadId) {
  if (infos_.minIterationsPerDag.has_value()) {
    XCHECK(simCtx_ != nullptr);
    XCHECK(globalDagCoordinator_ != nullptr);
    checkAndTriggerNextIteration(
        simCtx_,
        globalDagCoordinator_,
        workloadIdDependencies_,
        dagToWorkloadIds_,
        workloadId,
        infos_,
        workloads_,
        triggerWorkloadFn_,
        buildWorkloadFn_);
  }
  if (!infos_.workloadIdDependents.contains(workloadId)) {
    return;
  }
  for (auto const dependentWorkloadId :
       infos_.workloadIdDependents.at(workloadId)) {
    if (!workloadIdDependencies_.contains(dependentWorkloadId)) {
      continue;
    }
    auto& dependencies = workloadIdDependencies_.at(dependentWorkloadId);
    if (!dependencies.contains(workloadId)) {
      throw std::runtime_error(
          fmt::format(
              "Dependency relationship missing for workload {} "
              "(could be repeated completions or a missing dependency)",
              workloadId));
    }
    dependencies.erase(workloadId);
    if (dependencies.empty()) {
      triggerWorkloadFn_(dependentWorkloadId);
    }
  }
}

const DependencyInfos* DependencyTracker::getDependencyInfos() const {
  return &infos_;
}

/* static */
DependencyTracker::WorkloadDependencyMap
DependencyTracker::filterDependencyMapByNodeWorkloads(
    WorkloadDependencyMap const& originalDepMap,
    NodeWorkloadMap const& nodeWorkloads) {
  WorkloadDependencyMap depMap;
  for (auto const& [workloadId, dependencyWorkloadIds] : originalDepMap) {
    if (!nodeWorkloads.contains(workloadId)) {
      // Skip the workload if not placed on this node.
      continue;
    }
    for (auto const dependencyWorkloadId : dependencyWorkloadIds) {
      if (!nodeWorkloads.contains(dependencyWorkloadId)) {
        // Skip the dependency if it's not placed on this node.
        continue;
      }
      depMap[workloadId].emplace(dependencyWorkloadId);
    }
  }
  return depMap;
}

/* static */
DependencyTracker::DagWorkloadMap
DependencyTracker::filterDagWorkloadMapByNodeWorkloads(
    DependencyInfos const& infos,
    NodeWorkloadMap const& nodeWorkloads) {
  DagWorkloadMap dagMap;
  for (auto const& [workloadId, _] : nodeWorkloads) {
    if (!infos.workloadToDagId.contains(workloadId)) {
      continue;
    }
    auto const dagId = infos.workloadToDagId.at(workloadId);
    dagMap[dagId].emplace(workloadId);
  }
  return dagMap;
}

/* static */
void DependencyTracker::checkAndTriggerNextIteration(
    SimContext* FOLLY_NONNULL simCtx,
    GlobalDagCoordinator* FOLLY_NONNULL globalDagCoordinator,
    WorkloadDependencyMap& workloadIdDependencies,
    DagWorkloadMap& dagToWorkloadIds,
    uint32_t const workloadId,
    DependencyInfos const& infos,
    NodeWorkloadMap const& workloads,
    DependencyTracker::TriggerWorkloadFn const& triggerWorkloadFn,
    DependencyTracker::BuildWorkloadFn const& buildWorkloadFn) {
  const auto* workloadRank = getOnlyRank(workloads, workloadId);
  XLOGF(
      DBG, "Workload ID {} rank {} completed", workloadId, workloadRank->rank_);
  if (infos.workloadToDagId.contains(workloadId)) {
    auto const dagId = infos.workloadToDagId.at(workloadId);
    if (dagToWorkloadIds.contains(dagId)) {
      dagToWorkloadIds.at(dagId).erase(workloadId);
      XLOGF(
          DBG,
          "Number of remaining workloads for DAG ID {} is {}",
          dagId,
          dagToWorkloadIds.at(dagId).size());
      if (dagToWorkloadIds.at(dagId).empty()) {
        if (workloadRank->rank_ ==
            workloadRank->workCfg_->rankToId()->begin()->first) {
          XLOGF(INFO, "Current iteration completed for DAG ID {}", dagId);
        }
        globalDagCoordinator->arrive(
            dagId,
            simCtx,
            [&workloadIdDependencies,
             &infos,
             &dagToWorkloadIds,
             &workloads,
             &buildWorkloadFn,
             &triggerWorkloadFn,
             dagId](uint32_t numIter) {
              // Reset the tracker state for this DAG.
              // Create the workload again.
              folly::F14FastSet<uint32_t> dagWorkloadIds;
              for (auto const dagWorkloadId :
                   infos.dagList.at(dagId).allWorkloadIds) {
                if (workloads.contains(dagWorkloadId)) {
                  dagWorkloadIds.emplace(dagWorkloadId);
                }
              }
              dagToWorkloadIds[dagId] = dagWorkloadIds;
              for (auto const dagWorkloadId : dagWorkloadIds) {
                auto const workload = getOnlyRank(workloads, dagWorkloadId);
                buildWorkloadFn(
                    *workload->workCfg_,
                    workload->rank_,
                    numIter + 1 /* next iteration # */);
                if (infos.workloadIdDependencies.contains(dagWorkloadId)) {
                  for (auto const dagWorkloadIdDependency :
                       infos.workloadIdDependencies.at(dagWorkloadId)) {
                    if (workloads.contains(dagWorkloadIdDependency)) {
                      workloadIdDependencies[dagWorkloadId].emplace(
                          dagWorkloadIdDependency);
                    }
                  }
                }
              }
              // Kick off the DAG again.
              for (auto const dagWorkloadId : dagWorkloadIds) {
                // Only start workloads that have no dependencies.
                if (!infos.workloadIdDependencies.contains(dagWorkloadId)) {
                  triggerWorkloadFn(dagWorkloadId);
                }
              }
            });
      }
    }
  }
}

} // namespace packsim
