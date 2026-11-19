/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/endpoint/runtime/workload-node.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/endpoint/runtime/collective-barrier.h"
#include "packsim/endpoint/runtime/message-tracker.h"
#include "packsim/endpoint/runtime/oss_tests/workload-node-test-fixture.h"
#include "packsim/if/gen-cpp2/config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/kernel/device-id.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/link-layer/eth-port.h"
#include "packsim/node/flex-node.h"
#include "packsim/packet/ptr/PacketPtr.h"
#include "packsim/transport/transport.h"
#include "packsim/workloads/rank/workload-rank.h"

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::ThrowsMessage;

namespace packsim {

class WorkloadNodePortIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scheduler_.buildNewEngine();

    nicConfig_ = packsim::thrift::NicConfig();
    RoceV2TestConfig::configureRoceV2Nic(nicConfig_);

    nicMap_["default"] = nicConfig_;
  }

  packsim::thrift::Node createNodeConfig(uint8_t portIdx) {
    packsim::thrift::Node nodeCfg;
    nodeCfg.id() = {1, 0, 1, 1, 0, 0, 0, 0};
    nodeCfg.type() = packsim::thrift::NodeType::ACCEL;

    packsim::thrift::EndpointConfig epCfg;
    epCfg.nicIds()->push_back({1, 0, 1, 1, 1, 0, 0, 0});
    epCfg.portIdx() = portIdx;
    epCfg.lpmSpec() = packsim::thrift::LpmTableSpec{};

    nodeCfg.endpointCfgs()->emplace_back(epCfg);

    return nodeCfg;
  }

  std::unique_ptr<WorkloadNode> createWorkloadNode(
      const packsim::thrift::Node& nodeCfg) {
    FlexNodeParams params{
        .simCtx = &simCtx_,
        .name = "workload_node_test",
        .flexId = FlexDeviceId("1:0:1:1:0:0:0:0"),
        .lpmTable = nullptr,
        .topo = nullptr,
    };
    return std::make_unique<WorkloadNode>(params, nodeCfg, nicMap_);
  }

 protected:
  Scheduler scheduler_;
  SimContext simCtx_{&scheduler_, 0};
  packsim::thrift::NicConfig nicConfig_;
  std::map<std::string, packsim::thrift::NicConfig> nicMap_;
};

// The NIC-id checks reuse the port-index fixture's one-endpoint node.
using WorkloadNodeNicIdsTest = WorkloadNodePortIndexTest;

TEST_F(WorkloadNodeNicIdsTest, EndpointListingTwoDifferentNicIdsThrows) {
  auto nodeCfg = createNodeConfig(/*portIdx=*/0);
  nodeCfg.endpointCfgs()->front().nicIds()->emplace_back(
      packsim::thrift::PrefixId{1, 0, 1, 1, 2, 0, 0, 0});

  EXPECT_THAT(
      [&]() { createWorkloadNode(nodeCfg); },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("workload_node_test: endpoint 0 lists different NIC ids")));
}

TEST_F(WorkloadNodeNicIdsTest, EndpointListingNoNicIdThrows) {
  auto nodeCfg = createNodeConfig(/*portIdx=*/0);
  nodeCfg.endpointCfgs()->front().nicIds()->clear();

  EXPECT_THAT(
      [&]() { createWorkloadNode(nodeCfg); },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("workload_node_test: endpoint 0 lists no NIC id")));
}

TEST_F(WorkloadNodeNicIdsTest, EndpointRepeatingItsNicIdPerLinkIsAccepted) {
  auto nodeCfg = createNodeConfig(/*portIdx=*/0);
  auto& nicIds = *nodeCfg.endpointCfgs()->front().nicIds();
  nicIds.emplace_back(nicIds.front());

  const auto workloadNode = createWorkloadNode(nodeCfg);

  EXPECT_EQ(
      workloadNode->getEndpoint(0)->getFlexId(),
      FlexDeviceId("1:0:1:1:1:0:0:0"));
}

TEST_F(
    WorkloadNodeNicIdsTest,
    EndpointSpellingItsNicIdWithoutTrailingZerosIsAccepted) {
  auto nodeCfg = createNodeConfig(/*portIdx=*/0);
  nodeCfg.endpointCfgs()->front().nicIds()->emplace_back(
      packsim::thrift::PrefixId{1, 0, 1, 1, 1});

  const auto workloadNode = createWorkloadNode(nodeCfg);

  EXPECT_EQ(
      workloadNode->getEndpoint(0)->getFlexId(),
      FlexDeviceId("1:0:1:1:1:0:0:0"));
}

TEST_F(WorkloadNodePortIndexTest, PositivePortIdxCreatesEndpointWithPortIndex) {
  constexpr uint8_t portIdx = 5;
  auto nodeCfg = createNodeConfig(portIdx);
  auto workloadNode = createWorkloadNode(nodeCfg);
  ASSERT_NE(workloadNode, nullptr);

  auto* endpoint = workloadNode->getEndpoint(0);
  auto dataPkt = createPacket<PacketType::ROCE>(100);
  EXPECT_TRUE(endpoint->usingPortLevelAddresses(dataPkt));
}

TEST_F(WorkloadNodePortIndexTest, ZeroPortIdxCreatesEndpointWithoutPortIndex) {
  constexpr uint8_t portIdx = 0;
  auto nodeCfg = createNodeConfig(portIdx);
  auto workloadNode = createWorkloadNode(nodeCfg);
  ASSERT_NE(workloadNode, nullptr);

  Endpoint* endpoint = workloadNode->getEndpoint(0);
  auto dataPkt = createPacket<PacketType::ROCE>(100);
  EXPECT_FALSE(endpoint->usingPortLevelAddresses(dataPkt));
}

// A port index selects one of the 8 fields of a FlexDeviceId. A stored -1 reads
// as 255, which is rejected rather than used to index past the id.
TEST_F(WorkloadNodePortIndexTest, PortIdxBeyondTheFlexDeviceIdFieldsThrows) {
  auto nodeCfg = createNodeConfig(255);

  EXPECT_THAT(
      [&]() { createWorkloadNode(nodeCfg); },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("has portIdx 255, but a FlexDeviceId has only 8 fields")));
}

TEST_F(WorkloadNodePortIndexTest, MultipleEndpointsWithDifferentPortIdxValues) {
  packsim::thrift::Node nodeCfg;
  nodeCfg.name() = "test_node_multi";
  nodeCfg.id() = {1, 0, 0, 0, 0, 0, 0, 0};

  packsim::thrift::EndpointConfig epCfg1;
  epCfg1.nicIds()->push_back({1, 0, 1, 1, 1});
  epCfg1.portIdx() = 5;

  packsim::thrift::EndpointConfig epCfg2;
  epCfg2.nicIds()->push_back({1, 0, 1, 1, 1});
  epCfg2.portIdx() = 0;

  nodeCfg.endpointCfgs()->emplace_back(epCfg1);
  nodeCfg.endpointCfgs()->emplace_back(epCfg2);

  auto workloadNode = createWorkloadNode(nodeCfg);
  ASSERT_NE(workloadNode, nullptr);
  auto rocePkt = createPacket<PacketType::ROCE>(100);

  Endpoint* endpoint1 = workloadNode->getEndpoint(0);
  EXPECT_TRUE(endpoint1->usingPortLevelAddresses(rocePkt));

  Endpoint* endpoint2 = workloadNode->getEndpoint(1);
  EXPECT_FALSE(endpoint2->usingPortLevelAddresses(rocePkt));
}

// Two endpoints with a port field share a node whose links alternate between
// them, so each endpoint's node port indices ({0, 2} and {1, 3}) are neither
// contiguous nor in the order of its NIC egress ports.
TEST_F(WorkloadNodePortIndexTest, MacStatsListsEachEndpointsPortsByNicPort) {
  constexpr uint8_t kPortIdx = 5;
  packsim::thrift::Node nodeCfg = createNodeConfig(kPortIdx);
  nodeCfg.endpointCfgs()->emplace_back(nodeCfg.endpointCfgs()->front());
  nodeCfg.endpointCfgs()->at(0).linkIds() = {10, 12};
  nodeCfg.endpointCfgs()->at(1).linkIds() = {11, 13};

  struct PortSpec {
    packsim::thrift::LinkId linkId;
    uint16_t nodePortNum;
    // 1-based port field of the port's address: NIC egress port + 1.
    uint16_t portField;
  };
  const std::vector<PortSpec> specs{
      {.linkId = 10, .nodePortNum = 0, .portField = 2},
      {.linkId = 11, .nodePortNum = 1, .portField = 2},
      {.linkId = 12, .nodePortNum = 2, .portField = 1},
      {.linkId = 13, .nodePortNum = 3, .portField = 1},
  };
  // Declared before the node, which holds pointers to them.
  std::vector<std::unique_ptr<EthernetPort>> ports;
  auto workloadNode = createWorkloadNode(nodeCfg);
  for (const auto& spec : specs) {
    FlexDeviceId portId("1:0:1:1:1:0:0:0");
    portId.values[kPortIdx] = spec.portField;
    auto& port = ports.emplace_back(
        std::make_unique<EthernetPort>(&simCtx_, "100Gbps", portId));
    workloadNode->addPort(
        port.get(),
        FlexDeviceId("2:0:1:0:0:0:0:0"),
        spec.linkId,
        spec.nodePortNum);
    // Node port n gets n + 1 CoS queues, so its MAC stats identify it. Set
    // after addPort, which applies the NIC's queue config.
    port->setPortQueueConfig(
        std::vector<packsim::thrift::PortQueueConfig>(spec.nodePortNum + 1));
  }

  const auto cosCountsByEntry = [&](const uint32_t endpointIdx) {
    const auto summary =
        workloadNode->getEndpoint(endpointIdx)->getNicStatsSummaryResults();
    std::vector<size_t> cosCounts;
    for (const auto& portMacStats : *summary.macStats()) {
      cosCounts.emplace_back(portMacStats.cosStats()->size());
    }
    return cosCounts;
  };
  // Entry k is NIC egress port k: endpoint 0's egress port 0 is node port 2
  // and its egress port 1 is node port 0; endpoint 1's are node ports 3 and 1.
  EXPECT_EQ(cosCountsByEntry(0), (std::vector<size_t>{3, 1}));
  EXPECT_EQ(cosCountsByEntry(1), (std::vector<size_t>{4, 2}));
}

TEST_F(
    WorkloadNodePortIndexTest,
    PortOnALinkNoEndpointListsThrowsOnAMultiEndpointNode) {
  auto nodeCfg = createNodeConfig(/*portIdx=*/0);
  // A second endpoint like the first; neither lists any link.
  auto const secondEndpoint = nodeCfg.endpointCfgs()->front();
  nodeCfg.endpointCfgs()->emplace_back(secondEndpoint);
  auto workloadNode = createWorkloadNode(nodeCfg);
  EthernetPort port(&simCtx_, "100Gbps");

  EXPECT_THAT(
      [&]() {
        workloadNode->addPort(
            &port,
            FlexDeviceId("1:0:1:1:1:0:0:0"),
            /*linkId=*/7,
            /*portNum=*/0);
      },
      ThrowsMessage<std::invalid_argument>(HasSubstr(
          "workload_node_test: link 7 is not in the linkIds of any "
          "of its 2 endpoints")));
}

class WorkloadNodeRankRoutingTest : public WorkloadNodeMessageTrackerTest {
 protected:
  enum class AckDisposition {
    CONSUME,
    FALL_THROUGH,
  };

  class RoutingRank final : public WorkloadRank {
   public:
    RoutingRank(
        const WorkloadRankParams& params,
        const AckDisposition disposition)
        : WorkloadRank(params), disposition_(disposition) {
      setOnRequestAckedCallback([this](Message* msg) {
        ++fallbackCount;
        fallbackMessage = msg;
        fallbackResultInError = resultInError();
      });
      setOnRequestReceivedCallback([this](Message* msg) {
        ++receivedCount;
        receivedMessage = msg;
        receivedResultInError = resultInError();
      });
    }

    void run() override {
      if (runLog != nullptr) {
        runLog->emplace_back(rank_);
      }
    }

    bool tryHandleTransportAck(Message* msg) override {
      ++tryHandleCount;
      tryHandleMessage = msg;
      tryHandleResultInError = resultInError();
      return disposition_ == AckDisposition::CONSUME;
    }

    uint64_t getExpectedBytes() override {
      return 0;
    }

    bool complete() override {
      return false;
    }

    // Calls to the rank-specific ACK handler.
    uint32_t tryHandleCount{0};

    // Calls to the generic WorkloadRank ACK handler.
    uint32_t fallbackCount{0};

    // Last Message offered to the rank-specific handler.
    Message* tryHandleMessage{nullptr};

    // Last Message delivered to the generic handler.
    Message* fallbackMessage{nullptr};

    // Calls to the generic WorkloadRank receive handler.
    uint32_t receivedCount{0};

    // Last Message delivered to the generic receive handler.
    Message* receivedMessage{nullptr};

    // The rank's messagesCompletedInError result when the rank-specific ACK
    // handler last ran: what the rank reports if that handler completes it.
    uint64_t tryHandleResultInError{0};

    // The same when the generic ACK handler last ran.
    uint64_t fallbackResultInError{0};

    // The same when the generic receive handler last ran.
    uint64_t receivedResultInError{0};

    // When set, run() appends this rank's number to it.
    std::vector<uint32_t>* runLog{nullptr};

   private:
    // The count in the result that onWorkloadComplete would take if the rank
    // completed at this point.
    uint64_t resultInError() const {
      return *getThriftResult().messagesCompletedInError();
    }

    const AckDisposition disposition_;
  };

  RoutingRank* installRank(
      WorkloadNode& node,
      const AckDisposition disposition,
      const uint32_t rank = 0) {
    workload_.name() = "rank_routing";
    workload_.workloadId() = kWorkloadId;
    workload_.numEndpoints() = 1;
    const WorkloadRankParams::SubmitMessageFn messageCb = [](Message*) {};
    const WorkloadRankParams::OnRankCompleteCallback completeCb =
        [](uint32_t, uint32_t) {};
    const WorkloadRankParams params{
        .simCtx = &simCtx_,
        .workloadId = kWorkloadId,
        .workCfg = workload_,
        .rank = rank,
        .smcb = messageCb,
        .rmcb = messageCb,
        .rccb = completeCb,
        .parent = nullptr};
    auto routingRank = std::make_unique<RoutingRank>(params, disposition);
    auto* rankPtr = routingRank.get();
    node.workloads_[kWorkloadId].emplace(rank, std::move(routingRank));
    return rankPtr;
  }

  // Marks `rank` as still running, as buildWorkloadRank does for a rank it
  // builds.
  static void markRunning(WorkloadNode& node, const uint32_t rank) {
    node.pendingRanks_[kWorkloadId].emplace(rank);
  }

  static constexpr uint32_t kWorkloadId = 17;

  packsim::workload::thrift::Workload workload_;
};

TEST_F(WorkloadNodeRankRoutingTest, ClaimedAckSkipsGenericHandler) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::CONSUME);
  Message msg;
  msg.workloadId = kWorkloadId;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank->tryHandleCount, 1u);
  EXPECT_EQ(rank->tryHandleMessage, &msg);
  EXPECT_EQ(rank->fallbackCount, 0u);
}

TEST_F(WorkloadNodeRankRoutingTest, UnclaimedAckFallsThroughToGenericHandler) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::FALL_THROUGH);
  Message msg;
  msg.workloadId = kWorkloadId;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank->tryHandleCount, 1u);
  EXPECT_EQ(rank->tryHandleMessage, &msg);
  EXPECT_EQ(rank->fallbackCount, 1u);
  EXPECT_EQ(rank->fallbackMessage, &msg);
}

// The counts of messages completed in error reuse the rank-routing fixture's
// rank, which records what reaches each of its handlers.
using WorkloadNodeCompletionStatusTest = WorkloadNodeRankRoutingTest;

// A handler can complete the rank, which takes the rank's result at once, so
// an acknowledgment completed in error is counted before any handler sees it.
TEST_F(
    WorkloadNodeCompletionStatusTest,
    AnAckCompletedInErrorIsCountedAndStillDelivered) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::FALL_THROUGH);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.completionStatus = Message::CompletionStatus::FLUSHED_IN_ERROR;

  node->onMessageAcked(&msg);

  EXPECT_EQ(simCtx_.messagesCompletedInError, 1u);
  EXPECT_EQ(*rank->getThriftResult().messagesCompletedInError(), 1u);
  EXPECT_EQ(rank->tryHandleResultInError, 1u);
  EXPECT_EQ(rank->fallbackCount, 1u);
  EXPECT_EQ(rank->fallbackMessage, &msg);
  EXPECT_EQ(rank->fallbackResultInError, 1u);
}

// The same for a receive, which is counted before the receive handler sees it.
TEST_F(
    WorkloadNodeCompletionStatusTest,
    AReceiveCompletedInErrorIsCountedAndStillDelivered) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::FALL_THROUGH);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.completionStatus = Message::CompletionStatus::FLUSHED_IN_ERROR;

  node->onMessageReceived(&msg);

  EXPECT_EQ(simCtx_.messagesCompletedInError, 1u);
  EXPECT_EQ(*rank->getThriftResult().messagesCompletedInError(), 1u);
  EXPECT_EQ(rank->receivedCount, 1u);
  EXPECT_EQ(rank->receivedMessage, &msg);
  EXPECT_EQ(rank->receivedResultInError, 1u);
}

// A COMPLETE_FLUSHED message executed in full, so it is not an error.
TEST_F(WorkloadNodeCompletionStatusTest, ACompleteFlushedAckIsNotCounted) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::FALL_THROUGH);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.completionStatus = Message::CompletionStatus::COMPLETE_FLUSHED;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank->fallbackCount, 1u);
  EXPECT_EQ(simCtx_.messagesCompletedInError, 0u);
  EXPECT_EQ(*rank->getThriftResult().messagesCompletedInError(), 0u);
}

// The rank counts the message before its own handler can consume it, so the
// count does not depend on which handler takes the message.
TEST_F(
    WorkloadNodeCompletionStatusTest,
    AConsumedAckCompletedInErrorIsStillCounted) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank = installRank(*node, AckDisposition::CONSUME);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.completionStatus = Message::CompletionStatus::FLUSHED_IN_ERROR;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank->tryHandleCount, 1u);
  EXPECT_EQ(rank->tryHandleResultInError, 1u);
  EXPECT_EQ(rank->fallbackCount, 0u);
  EXPECT_EQ(simCtx_.messagesCompletedInError, 1u);
  EXPECT_EQ(*rank->getThriftResult().messagesCompletedInError(), 1u);
}

// A node whose resolve hooks hold back every completion, as a hook that waits
// for the rest of a larger message does.
class HoldingWorkloadNode final : public WorkloadNode {
 public:
  using WorkloadNode::WorkloadNode;

 protected:
  Message* FOLLY_NULLABLE
  resolveReceived(Message* FOLLY_NONNULL /*msg*/) override {
    return nullptr;
  }

  Message* FOLLY_NULLABLE
  resolveAcked(Message* FOLLY_NONNULL /*msg*/) override {
    return nullptr;
  }
};

// The run check counts a completion before the resolve hooks, so it sees a
// message a hook holds back, which the rank never sees.
TEST_F(
    WorkloadNodeCompletionStatusTest,
    ACompletionAHookHoldsBackIsCountedForTheRunCheckOnly) {
  const FlexNodeParams params{
      .simCtx = &simCtx_,
      .name = "holding_node",
      .flexId = FlexDeviceId("1:0:1:1:0:0:0:0"),
      .lpmTable = nullptr,
      .topo = nullptr,
  };
  HoldingWorkloadNode node(params, createNodeConfig(), nicMap_);
  auto* rank = installRank(node, AckDisposition::FALL_THROUGH);
  Message received;
  received.workloadId = kWorkloadId;
  received.completionStatus = Message::CompletionStatus::FLUSHED_IN_ERROR;
  Message acked;
  acked.workloadId = kWorkloadId;
  acked.completionStatus = Message::CompletionStatus::FLUSHED_IN_ERROR;

  node.onMessageReceived(&received);
  node.onMessageAcked(&acked);

  EXPECT_EQ(simCtx_.messagesCompletedInError, 2u);
  EXPECT_EQ(rank->receivedCount, 0u);
  EXPECT_EQ(rank->tryHandleCount, 0u);
  EXPECT_EQ(*rank->getThriftResult().messagesCompletedInError(), 0u);
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    ReceiveGoesToTheRankItNamesWhenTheWorkloadHasTwoRanksHere) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank0 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/0);
  auto* rank1 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/1);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.srcRank = 0;
  msg.dstRank = 1;

  node->onMessageReceived(&msg);

  EXPECT_EQ(rank1->receivedCount, 1u);
  EXPECT_EQ(rank1->receivedMessage, &msg);
  EXPECT_EQ(rank0->receivedCount, 0u);
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    AckGoesToTheSendingRankWhenTheWorkloadHasTwoRanksHere) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank0 = installRank(*node, AckDisposition::CONSUME, /*rank=*/0);
  auto* rank1 = installRank(*node, AckDisposition::CONSUME, /*rank=*/1);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.srcRank = 1;
  msg.dstRank = 0;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank1->tryHandleCount, 1u);
  EXPECT_EQ(rank1->tryHandleMessage, &msg);
  EXPECT_EQ(rank0->tryHandleCount, 0u);
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    AckForAFinishedRankReachesItWhileTheOtherRankRuns) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  uint32_t allCompleteCount = 0;
  node->setOnAllWorkloadsCompleteCallback(
      [&](const FlexDeviceId& /* id */) { ++allCompleteCount; });
  auto* rank0 = installRank(*node, AckDisposition::CONSUME, /*rank=*/0);
  auto* rank1 = installRank(*node, AckDisposition::CONSUME, /*rank=*/1);
  markRunning(*node, /*rank=*/0);
  markRunning(*node, /*rank=*/1);
  node->onWorkloadComplete(kWorkloadId, /*rank=*/0);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.srcRank = 0;
  msg.dstRank = 1;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank0->tryHandleCount, 1u);
  EXPECT_EQ(rank0->tryHandleMessage, &msg);
  EXPECT_EQ(rank1->tryHandleCount, 0u);
  EXPECT_EQ(allCompleteCount, 0u);
}

// A READ's srcRank is the peer it reads from; it completes at its dstRank, the
// rank that posted it.
TEST_F(
    WorkloadNodeRankRoutingTest,
    ReadCompletionGoesToTheReadingRankWhenTheWorkloadHasTwoRanksHere) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank0 = installRank(*node, AckDisposition::CONSUME, /*rank=*/0);
  auto* rank1 = installRank(*node, AckDisposition::CONSUME, /*rank=*/1);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.isRead = true;
  msg.srcRank = 0;
  msg.dstRank = 1;

  node->onMessageAcked(&msg);

  EXPECT_EQ(rank1->tryHandleCount, 1u);
  EXPECT_EQ(rank1->tryHandleMessage, &msg);
  EXPECT_EQ(rank0->tryHandleCount, 0u);
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    MessageNamingNoRankHereThrowsWhenTheWorkloadHasTwoRanksHere) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/0);
  installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/1);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.dstRank = 5;

  EXPECT_THAT(
      [&]() { node->onMessageReceived(&msg); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("workload 17 has no rank 5 on this node")));
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    MessageNamingNoRankHereThrowsWhenTheWorkloadHasOneRankHere) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/0);
  Message msg;
  msg.workloadId = kWorkloadId;
  msg.dstRank = 3;

  EXPECT_THAT(
      [&]() { node->onMessageReceived(&msg); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("workload 17 has no rank 3 on this node")));
}

TEST_F(
    WorkloadNodeRankRoutingTest,
    RunningAWorkloadStartsEachOfItsRanksHereInRankOrder) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank1 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/1);
  auto* rank0 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/0);
  std::vector<uint32_t> runOrder;
  rank0->runLog = &runOrder;
  rank1->runLog = &runOrder;

  node->runWorkload(kWorkloadId);
  scheduler_.run();

  EXPECT_THAT(runOrder, ElementsAre(0u, 1u));
}

TEST_F(WorkloadNodeRankRoutingTest, GetWorkloadsListsEachRankHereInRankOrder) {
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  auto* rank1 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/1);
  auto* rank0 = installRank(*node, AckDisposition::FALL_THROUGH, /*rank=*/0);

  EXPECT_THAT(node->getWorkloads(), ElementsAre(rank0, rank1));
}

class WorkloadNodeRankCompletionTest : public WorkloadNodeMessageTrackerTest {
 protected:
  void SetUp() override {
    WorkloadNodeMessageTrackerTest::SetUp();
    workload_.name() = "two_ranks_on_one_node";
    workload_.workloadId() = kWorkloadId;
    workload_.numEndpoints() = 2;
    workload_.config()->compute() = packsim::workload::thrift::Compute();
    node_ = createWorkloadNode();
    node_->setOnAllWorkloadsCompleteCallback(
        [this](const FlexDeviceId& /* id */) { ++allCompleteCount_; });
    node_->buildWorkloadRank(workload_, /*rank=*/0);
    node_->buildWorkloadRank(workload_, /*rank=*/1);
  }

  static constexpr uint32_t kWorkloadId = 23;

  packsim::workload::thrift::Workload workload_;
  std::unique_ptr<WorkloadNode> node_;
  // Times the node reported that no rank is left pending.
  uint32_t allCompleteCount_{0};
};

TEST_F(
    WorkloadNodeRankCompletionTest,
    FirstOfTwoRanksFinishingLeavesTheNodeIncomplete) {
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/0);

  EXPECT_EQ(allCompleteCount_, 0u);
}

TEST_F(
    WorkloadNodeRankCompletionTest,
    LastOfTwoRanksFinishingCompletesTheNode) {
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/0);
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/1);

  EXPECT_EQ(allCompleteCount_, 1u);
}

TEST_F(WorkloadNodeRankCompletionTest, EachFinishingRankRecordsItsOwnResult) {
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/1);
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/0);

  std::vector<uint32_t> resultRanks;
  for (const auto& result : node_->getWorkloadRankResults()) {
    resultRanks.emplace_back(*result.rank());
  }
  EXPECT_THAT(resultRanks, ElementsAre(1u, 0u));
}

TEST_F(WorkloadNodeRankCompletionTest, FinishingARankTwiceThrows) {
  node_->onWorkloadComplete(kWorkloadId, /*rank=*/0);

  EXPECT_THAT(
      [&]() { node_->onWorkloadComplete(kWorkloadId, /*rank=*/0); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Workload 23 rank 0 was already completed")));
}

TEST_F(WorkloadNodeRankCompletionTest, BuildingARankThatIsStillRunningThrows) {
  EXPECT_THAT(
      [&]() { node_->buildWorkloadRank(workload_, /*rank=*/1); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Workload 23 rank 1 is currently active")));
}

using WorkloadNodeBarrierTest = WorkloadNodeMessageTrackerTest;

TEST_F(WorkloadNodeBarrierTest, SecondBarrierForAWorkloadThrows) {
  constexpr uint32_t kWorkloadId = 29;
  auto node = createWorkloadNode();
  ASSERT_NE(node, nullptr);
  packsim::workload::thrift::Workload workload;
  workload.workloadId() = kWorkloadId;
  node->setCollectiveBarrier(
      kWorkloadId,
      std::make_unique<CollectiveBarrier>(
          &simCtx_, &simCtx_, /*rank=*/0, &workload));

  EXPECT_THAT(
      [&]() {
        node->setCollectiveBarrier(
            kWorkloadId,
            std::make_unique<CollectiveBarrier>(
                &simCtx_, &simCtx_, /*rank=*/1, &workload));
      },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("workload 29 already has a barrier on this node")));
}

TEST_F(
    WorkloadNodeMessageTrackerTest,
    TrackerIsNullWhenNoFeatureConfigProvided) {
  auto workloadNode = createWorkloadNode();
  ASSERT_NE(workloadNode, nullptr);
  EXPECT_EQ(workloadNode->messageTracker(), nullptr);
}

TEST_F(
    WorkloadNodeMessageTrackerTest,
    TrackerIsNullWhenTrackingExplicitlyDisabled) {
  packsim::thrift::NetworkLevelFeatureConfig featureConfig;
  featureConfig.enablePerMessageTracking() = false;
  auto workloadNode = createWorkloadNode(&featureConfig);
  ASSERT_NE(workloadNode, nullptr);
  EXPECT_EQ(workloadNode->messageTracker(), nullptr);
}

TEST_F(WorkloadNodeMessageTrackerTest, TrackerIsNonNullWhenTrackingEnabled) {
  packsim::thrift::NetworkLevelFeatureConfig featureConfig;
  featureConfig.enablePerMessageTracking() = true;
  auto workloadNode = createWorkloadNode(&featureConfig);
  ASSERT_NE(workloadNode, nullptr);
  EXPECT_NE(workloadNode->messageTracker(), nullptr);
}

TEST_F(
    WorkloadNodeMessageTrackerTest,
    FreshTrackerHasNoRecordsWhenTrackingEnabled) {
  packsim::thrift::NetworkLevelFeatureConfig featureConfig;
  featureConfig.enablePerMessageTracking() = true;
  auto workloadNode = createWorkloadNode(&featureConfig);
  ASSERT_NE(workloadNode, nullptr);

  const auto* tracker = workloadNode->messageTracker();
  ASSERT_NE(tracker, nullptr);
  EXPECT_TRUE(tracker->getSendRecords().empty());
  EXPECT_TRUE(tracker->getRecvTimes().empty());
}

TEST_F(
    WorkloadNodeMessageTrackerTest,
    MessageReceivedRecordsRecvForNonChunkedMessage) {
  packsim::thrift::NetworkLevelFeatureConfig featureConfig;
  featureConfig.enablePerMessageTracking() = true;
  auto workloadNode = createWorkloadNode(&featureConfig);
  ASSERT_NE(workloadNode, nullptr);

  Message msg;
  msg.workloadId = 1;
  msg.srcRank = 10;
  msg.dstRank = 20;
  msg.connectionMessageId = 42;
  msg.sizeBytes = 1024;

  workloadNode->onMessageReceived(&msg);

  const auto* tracker = workloadNode->messageTracker();
  ASSERT_NE(tracker, nullptr);
  const auto& recvTimes = tracker->getRecvTimes();
  ASSERT_EQ(recvTimes.size(), 1);
  MessageKey expectedKey{1, 10, 20, 42, 1};
  EXPECT_TRUE(recvTimes.contains(expectedKey));
}

TEST_F(
    WorkloadNodeMessageTrackerTest,
    MessageReceivedSafeWhenTrackingDisabled) {
  auto workloadNode = createWorkloadNode();
  ASSERT_NE(workloadNode, nullptr);

  Message msg;
  msg.workloadId = 1;
  msg.srcRank = 10;
  msg.dstRank = 20;
  msg.connectionMessageId = 42;
  msg.sizeBytes = 1024;

  workloadNode->onMessageReceived(&msg);
  EXPECT_EQ(workloadNode->messageTracker(), nullptr);
}

// Two WRITEs to one rank, as a RoCE socket hands them up: each has only
// workloadId and dstRank of the tracker's key, so recorded, the second would
// collide with the first.
TEST_F(
    WorkloadNodeMessageTrackerTest,
    AWriteTheSocketHandsUpIsNotRecordedAsReceived) {
  packsim::thrift::NetworkLevelFeatureConfig featureConfig;
  featureConfig.enablePerMessageTracking() = true;
  auto workloadNode = createWorkloadNode(&featureConfig);
  ASSERT_NE(workloadNode, nullptr);

  for (int i = 0; i < 2; ++i) {
    Message write;
    write.isWrite = true;
    write.writeAddr = 0x1000;
    write.sendOrWriteData = 1;
    write.sizeBytes = 2;
    write.workloadId = 1;
    write.dstRank = 20;
    workloadNode->onMessageReceived(&write);
  }

  const auto* tracker = workloadNode->messageTracker();
  ASSERT_NE(tracker, nullptr);
  EXPECT_TRUE(tracker->getRecvTimes().empty());
}

} // namespace packsim
