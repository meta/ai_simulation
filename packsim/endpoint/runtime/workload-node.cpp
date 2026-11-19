/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/endpoint/runtime/workload-node.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "packsim/if/gen-cpp2/topology_constants.h"
#include "packsim/kernel/device-id.h"
#include "packsim/workloads/builder.h"

namespace packsim {

namespace {

/**
 * Returns the address of an endpoint's NIC. Builders repeat its id once per
 * link, but an endpoint has one address, so ids naming different addresses are
 * rejected. Ids that differ only in trailing zero segments name one address.
 *
 * @param nodeName     Name of the node, for errors.
 * @param endpointIdx  Index of the endpoint in the node, for errors.
 * @param nicIds       The endpoint's EndpointConfig.nicIds.
 *
 * @return             The NIC's address.
 * @throw              std::invalid_argument if nicIds is empty or names
 *                     different addresses.
 */
FlexDeviceId getEndpointNicId(
    const std::string& nodeName,
    const uint32_t endpointIdx,
    const std::vector<packsim::thrift::PrefixId>& nicIds) {
  if (nicIds.empty()) {
    throw std::invalid_argument(
        fmt::format("{}: endpoint {} lists no NIC id", nodeName, endpointIdx));
  }
  const FlexDeviceId nicId(nicIds.front());
  if (std::any_of(nicIds.begin() + 1, nicIds.end(), [&](const auto& other) {
        return FlexDeviceId(other) != nicId;
      })) {
    throw std::invalid_argument(
        fmt::format(
            "{}: endpoint {} lists different NIC ids, but an endpoint has one "
            "address; give each NIC its own endpoint",
            nodeName,
            endpointIdx));
  }
  return nicId;
}

/**
 * Count `msg` in the context's messagesCompletedInError if a transport
 * completed it in error.
 *
 * @param simCtx  The context of the workload node `msg` reached.
 * @param msg     A message a transport completed.
 */
void countIfCompletedInError(SimContext& simCtx, const Message& msg) noexcept {
  if (msg.completedInError()) {
    ++simCtx.messagesCompletedInError;
  }
}

} // namespace

WorkloadNode::WorkloadNode(
    const FlexNodeParams& params,
    const packsim::thrift::Node& nodeCfg,
    const std::map<std::string, packsim::thrift::NicConfig>& nicMap,
    const packsim::thrift::NetworkLevelFeatureConfig* networkLevelFeatureConfig)
    : FlexNode(params), networkLevelFeatureConfig_(networkLevelFeatureConfig) {
  if (networkLevelFeatureConfig_ != nullptr) {
    enablePerMessageTracking_ =
        *networkLevelFeatureConfig_->enablePerMessageTracking();
  }
  if (enablePerMessageTracking_) {
    messageTracker_ = std::make_unique<MessageTracker>();
  }
  if (nodeCfg.endpointCfgs()->size() > 0) {
    for (uint32_t i = 0; i < nodeCfg.endpointCfgs()->size(); ++i) {
      for (const auto& linkId : *nodeCfg.endpointCfgs()->at(i).linkIds()) {
        linkToEndpoint_[linkId] = i;
      }
      const std::string epName = fmt::format("{}.ep{}", name_, i);
      auto& cfgName = *nodeCfg.endpointCfgs()->at(i).nicConfig();
      const FlexNodeParams epParams{
          .simCtx = simCtx_,
          .name = epName,
          .flexId = getEndpointNicId(
              name_, i, *nodeCfg.endpointCfgs()->at(i).nicIds()),
          .lpmTable = &nodeCfg.endpointCfgs()->at(i).lpmSpec().value(),
          .topo = topo_,
          .portReachability = params.portReachability,
      };
      // A port index of 0 means the endpoint has no port field.
      const uint8_t portIndex = *nodeCfg.endpointCfgs()->at(i).portIdx();
      if (portIndex >= FlexDeviceId::kNumFlexDeviceIdFields) {
        throw std::invalid_argument(
            fmt::format(
                "{}: endpoint {} has portIdx {}, but a FlexDeviceId has only {} "
                "fields",
                name_,
                i,
                portIndex,
                FlexDeviceId::kNumFlexDeviceIdFields));
      }
      const std::optional<uint8_t> maybePortIndex =
          portIndex != 0 ? std::optional<uint8_t>(portIndex) : std::nullopt;
      const auto it = nicMap.find(cfgName);
      if (it == nicMap.end()) {
        throw std::runtime_error(
            fmt::format("NIC config {} not found in nicMap", cfgName));
      }
      endpoints_.emplace_back(
          std::make_unique<Endpoint>(epParams, &it->second, maybePortIndex));
    }
  } else {
    const std::string epName = fmt::format("{}.ep{}", name_, 0);
    const FlexNodeParams epParams{
        .simCtx = simCtx_,
        .name = epName,
        .flexId = flexId_,
        .lpmTable = lpmTable_,
        .topo = topo_,
    };
    endpoints_.emplace_back(
        std::make_unique<Endpoint>(
            epParams,
            &nicMap.at(thrift::topology_constants::kDefaultNicConfig()),
            std::nullopt));
  }

  for (const auto& ep : endpoints_) {
    ep->setOnMessageReceivedCallback(
        [this](Message* msg) { onMessageReceived(msg); });
    ep->setOnMessageAckedCallback(
        [this](Message* msg) { onMessageAcked(msg); });
  }
}

Socket* WorkloadNode::eagerCreateSocket(
    const uint32_t endpointId,
    const FlexDeviceId& dstId,
    const uint32_t localQPN,
    const uint32_t remoteQPN,
    const packsim::workload::thrift::Workload* workload) {
  return endpoints_.at(endpointId)
      ->getTransport()
      ->findOrMakeSocket(dstId, localQPN, remoteQPN, workload);
}

void WorkloadNode::setDependencyInfos(
    DependencyInfos* FOLLY_NONNULL depInfos,
    GlobalDagCoordinator* FOLLY_NONNULL globalDagCoordinator) {
  const auto triggerWorkloadFn = [this](const uint32_t workloadId) {
    workloadBarrier_.at(workloadId)->arrive([this, workloadId]() {
      runWorkload(workloadId, /* delayNs= */ 0);
    });
  };
  const auto buildWorkloadFn =
      [this](
          const packsim::workload::thrift::Workload& wCfg,
          const uint32_t rank,
          const uint32_t iteration) {
        buildWorkloadRank(wCfg, rank, iteration);
      };
  dt_ = std::make_unique<DependencyTracker>(
      workloads_,
      *depInfos,
      triggerWorkloadFn,
      buildWorkloadFn,
      globalDagCoordinator,
      simCtx_);
}

void WorkloadNode::setCollectiveBarrier(
    const uint32_t workloadId,
    std::unique_ptr<CollectiveBarrier> barrier) {
  // A barrier stands for one rank and the leader waits for every rank's, so
  // replacing one would hang the workload, or free the leader's barrier while
  // the other ranks' barriers still point at it.
  if (workloadBarrier_.contains(workloadId)) {
    throw std::invalid_argument(
        fmt::format(
            "{}: workload {} already has a barrier on this node; a workload in "
            "a DAG may place only one rank on a node",
            name_,
            workloadId));
  }
  workloadBarrier_.emplace(workloadId, std::move(barrier));
}

void WorkloadNode::startWorkloads() {
  for (const auto& [workloadId, ranks] : workloads_) {
    if (dt_ &&
        dt_->getDependencyInfos()->workloadIdDependencies.contains(
            workloadId)) {
      continue;
    }
    runWorkload(workloadId, *ranks.begin()->second->workCfg_->startTimeNs());
  }
}

void WorkloadNode::runWorkload(
    const uint32_t workloadId,
    const uint64_t delayNs) {
  const auto it = workloads_.find(workloadId);
  if (it == workloads_.end()) {
    throw std::runtime_error(
        fmt::format(
            "Workload ID {} does not exist on workload node {} and "
            "cannot be scheduled to run.",
            workloadId,
            name_));
  }
  for (const auto& [_, workloadRank] : it->second) {
    simCtx_->schedule(
        Time::fromNs(delayNs), &WorkloadRank::run, workloadRank.get());
  }
}

void WorkloadNode::addPort(
    EthernetPort* ethPort,
    const FlexDeviceId& partnerId,
    const packsim::thrift::LinkId linkId,
    const uint16_t portNum) {
  // A node configured without any linkIds must have a single endpoint, which
  // takes every port (e.g. the default endpoint of a node without
  // endpointCfgs).
  uint32_t epId = 0;
  if (const auto it = linkToEndpoint_.find(linkId);
      it != linkToEndpoint_.end()) {
    epId = it->second;
  } else if (!linkToEndpoint_.empty() || endpoints_.size() != 1) {
    throw std::invalid_argument(
        fmt::format(
            "{}: link {} is not in the linkIds of any of its {} endpoints",
            name_,
            linkId,
            endpoints_.size()));
  }
  endpoints_.at(epId)->addPort(ethPort, partnerId, linkId, portNum);
}

std::tuple<
    WorkloadNode::SubmitMessageFn,
    WorkloadNode::SubmitMessageFn,
    WorkloadNode::OnRankCompleteCallback>
WorkloadNode::genCallbacks() {
  // Braced return rather than make_tuple: the declared return type converts
  // each lambda, so the element types are not spelled out here.
  return {
      [this](Message* msg) { makeSendRequest(msg); },
      [this](Message* msg) { makeReceiveRequest(msg); },
      [this](uint32_t workloadId, uint32_t rank) {
        onWorkloadComplete(workloadId, rank);
      }};
}

void WorkloadNode::buildWorkloadRank(
    const packsim::workload::thrift::Workload& wCfg,
    const uint32_t rank,
    const uint32_t iteration) {
  const uint32_t workloadId = *wCfg.workloadId();
  if (const auto it = pendingRanks_.find(workloadId);
      it != pendingRanks_.end() && it->second.contains(rank)) {
    throw std::runtime_error(
        fmt::format(
            "Workload {} rank {} is currently active and cannot be overwritten/re-started.",
            workloadId,
            rank));
  }
  pendingRanks_[workloadId].emplace(rank);
  const auto [smcb, rmcb, rccb] = genCallbacks();
  auto w = packsim::buildWorkloadRank(
      simCtx_, workloadId, wCfg, rank, smcb, rmcb, rccb);
  w->setIteration(iteration);
  onRankBuilt(*w, wCfg);
  simCtx_->dataRcvdExpected += w->getExpectedBytes();
  workloads_[workloadId][rank] = std::move(w);
}

void WorkloadNode::makeSendRequest(packsim::Message* FOLLY_NONNULL msg) {
  XLOGF(
      DBG,
      "WorkloadNode::makeSendRequest: {} endpoints={} msgSize={}",
      getName(),
      endpoints_.size(),
      msg->sizeBytes);

  if (enablePerMessageTracking_) {
    messageTracker_->recordSend(msg, simCtx_->now().getNs());
  }

  submitSend(msg);
}

void WorkloadNode::makeReceiveRequest(packsim::Message* FOLLY_NONNULL msg) {
  XLOGF(
      DBG,
      "WorkloadNode::makeReceiveRequest: {} endpoints={} msgSize={}",
      getName(),
      endpoints_.size(),
      msg->sizeBytes);

  submitReceive(msg);
}

void WorkloadNode::onMessageReceived(packsim::Message* msg) {
  // Counted before resolveReceived, which can map the message to another one
  // or hold it back, so that the run check sees every message a transport
  // completed in error.
  countIfCompletedInError(*simCtx_, *msg);
  msg = resolveReceived(msg);
  if (msg == nullptr) {
    return;
  }

  if (enablePerMessageTracking_) { // @oss-enable
  // @oss-disable
    messageTracker_->recordRecv(msg, simCtx_->now().getNs());
  }

  if (auto* const workloadRank =
          findRankForMessage(msg->workloadId, msg->dstRank)) {
    workloadRank->recordCompletionStatus(*msg);
    workloadRank->requestReceived(msg);
  } else if (onMessageReceived_) {
    // Optional because nothing subscribes at all: no caller outside tests
    // invokes setOnMessageReceivedCallback on a node, so in production this
    // branch is unreachable and an unknown workloadId always falls to the
    // warning below. Unused surface rather than an optional event.
    onMessageReceived_(msg);
  } else {
    XLOGF(
        WARN,
        "Workload ID {} not found and no callback registered; dropping receive on the floor",
        msg->workloadId);
  }
}

void WorkloadNode::onMessageAcked(packsim::Message* msg) {
  // Counted before resolveAcked, for the reason onMessageReceived gives.
  countIfCompletedInError(*simCtx_, *msg);
  msg = resolveAcked(msg);
  if (msg == nullptr) {
    return;
  }

  // A READ completes at the rank that posted it, which is its dstRank; any
  // other message is acknowledged to its sender.
  const uint32_t ackedRank = msg->isRead ? msg->dstRank : msg->srcRank;
  if (auto* const workloadRank =
          findRankForMessage(msg->workloadId, ackedRank)) {
    workloadRank->recordCompletionStatus(*msg);
    if (workloadRank->tryHandleTransportAck(msg)) {
      return;
    }
    workloadRank->requestAcked(msg);
  } else if (onMessageAcked_) {
    // Optional for the same reason as onMessageReceived: no production
    // subscriber, so this branch is unreachable outside tests.
    onMessageAcked_(msg);
  } else {
    XLOGF(
        WARN,
        "Workload ID {} not found and no callback registered; dropping ACK on the floor",
        msg->workloadId);
  }
}

WorkloadRank* FOLLY_NULLABLE WorkloadNode::findRankForMessage(
    const uint32_t workloadId,
    const uint32_t rank) const {
  const auto it = workloads_.find(workloadId);
  if (it == workloads_.end()) {
    return nullptr;
  }
  const auto& ranks = it->second;
  const auto rankIt = ranks.find(rank);
  if (rankIt == ranks.end()) {
    throw std::runtime_error(
        fmt::format(
            "{}: workload {} has no rank {} on this node",
            name_,
            workloadId,
            rank));
  }
  return rankIt->second.get();
}

void WorkloadNode::onWorkloadComplete(
    const uint32_t workloadId,
    const uint32_t rank) {
  const auto pendingIt = pendingRanks_.find(workloadId);
  if (pendingIt == pendingRanks_.end() || !pendingIt->second.contains(rank)) {
    throw std::runtime_error(
        fmt::format(
            "Workload {} rank {} was already completed and not pending.",
            workloadId,
            rank));
  }
  // Only the pending set forgets a finished rank. It stays in workloads_, so a
  // message that still names it, such as an ACK that arrives after it
  // finished, reaches it and not another rank of its workload.
  pendingIt->second.erase(rank);
  workloadRankResults_.emplace_back(
      workloads_.at(workloadId).at(rank)->getThriftResult());
  if (!pendingIt->second.empty()) {
    return;
  }
  pendingRanks_.erase(pendingIt);

  if (dt_) {
    if (workloadSocketInfos_.contains(workloadId)) {
      for (const auto& [endpointId, dstIdx, localQPN] :
           workloadSocketInfos_.at(workloadId)) {
        endpoints_.at(endpointId)
            ->getTransport()
            ->unregisterSocket(dstIdx, localQPN);
      }
      workloadSocketInfos_.erase(workloadId);
    }
    dt_->postWorkloadCompletion(workloadId);
  }
  if (pendingRanks_.empty()) {
    onAllWorkloadsComplete();
  }
}

void WorkloadNode::onAllWorkloadsComplete() {
  XCHECK(onAllWorkloadsComplete_)
      << "no all-workloads-complete subscriber registered on node "
      << getName();
  onAllWorkloadsComplete_(flexId_);
}

void WorkloadNode::submitSend(Message* FOLLY_NONNULL msg) {
  endpoints_.at(msg->endpointId)->makeSendRequest(msg);
  // @oss-disable
  // @oss-disable
    // @oss-disable
  // @oss-disable
  trackWorkloadSocket(
      msg->workloadId, msg->endpointId, msg->dstIdx, msg->localQPN);
}

void WorkloadNode::submitReceive(Message* FOLLY_NONNULL msg) {
  endpoints_.at(msg->endpointId)->makeReceiveRequest(msg);
  // @oss-disable
    // @oss-disable
  // @oss-disable
  trackWorkloadSocket(
      msg->workloadId, msg->endpointId, msg->srcIdx, msg->localQPN);
}

void WorkloadNode::trackWorkloadSocket(
    const uint32_t workloadId,
    const uint32_t endpointId,
    const FlexDeviceId& peer,
    const uint32_t localQPN) {
  workloadSocketInfos_[workloadId].emplace(endpointId, peer, localQPN);
}

} // namespace packsim
