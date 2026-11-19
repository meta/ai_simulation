/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/workloads/dataflow.h"
#include "packsim/workloads/oss_tests/rank-harness.h"

namespace wt = packsim::workload::thrift;

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::ThrowsMessage;
using ::testing::UnorderedElementsAre;

namespace packsim {

namespace {

constexpr uint32_t kMessageBytes = 1000;
constexpr uint64_t kComputeNs = 5000;
constexpr uint64_t kStartDelayNs = 3000;

wt::DataflowOp makeOp(
    const wt::DataflowOpKind kind,
    const uint32_t queue,
    const std::vector<uint32_t>& deps) {
  wt::DataflowOp op;
  op.kind() = kind;
  op.queue() = queue;
  op.deps() = deps;
  return op;
}

// A RECV of kMessageBytes from an ABSOLUTE peer on channel 0.
wt::DataflowOp recvOp(
    const uint32_t peer,
    const uint32_t queue,
    const std::vector<uint32_t>& deps = {}) {
  auto op = makeOp(wt::DataflowOpKind::RECV, queue, deps);
  op.peer() = peer;
  op.sizeBytes() = kMessageBytes;
  return op;
}

// A SEND of kMessageBytes to an ABSOLUTE peer on channel 0.
wt::DataflowOp sendOp(
    const uint32_t peer,
    const uint32_t queue,
    const std::vector<uint32_t>& deps = {}) {
  auto op = makeOp(wt::DataflowOpKind::SEND, queue, deps);
  op.peer() = peer;
  op.sizeBytes() = kMessageBytes;
  return op;
}

wt::DataflowOp computeOp(
    const uint64_t durationNs,
    const uint32_t queue,
    const std::vector<uint32_t>& deps = {}) {
  auto op = makeOp(wt::DataflowOpKind::COMPUTE, queue, deps);
  op.durationNs() = durationNs;
  return op;
}

wt::DataflowOp withChannel(wt::DataflowOp op, const uint32_t channel) {
  op.channel() = channel;
  return op;
}

wt::DataflowOp withPeerMode(
    wt::DataflowOp op,
    const wt::DataflowPeerMode mode) {
  op.peerMode() = mode;
  return op;
}

wt::DataflowOp withQueueWait(
    wt::DataflowOp op,
    const wt::DataflowQueueWait queueWait) {
  op.queueWait() = queueWait;
  return op;
}

wt::DataflowProgram makeProgram(std::vector<wt::DataflowOp> ops) {
  wt::DataflowProgram program;
  program.ops() = std::move(ops);
  return program;
}

wt::Workload makeWorkload(
    std::vector<wt::DataflowProgram> programs,
    std::vector<uint32_t> rankPrograms) {
  wt::Workload w;
  w.name() = "Dataflow";
  w.workloadId() = 0;
  w.numEndpoints() = static_cast<uint32_t>(rankPrograms.size());
  wt::Dataflow dataflow;
  dataflow.programs() = std::move(programs);
  dataflow.rankPrograms() = std::move(rankPrograms);
  w.config()->dataflow() = std::move(dataflow);
  return w;
}

// One program per rank: rank i runs programs[i].
wt::Workload makeWorkload(std::vector<wt::DataflowProgram> programs) {
  std::vector<uint32_t> rankPrograms(programs.size());
  std::iota(rankPrograms.begin(), rankPrograms.end(), 0);
  return makeWorkload(std::move(programs), std::move(rankPrograms));
}

wt::QPNPair makeQpnPair(const uint32_t local, const uint32_t remote) {
  wt::QPNPair pair;
  pair.local() = local;
  pair.remote() = remote;
  return pair;
}

} // namespace

class DataflowTest : public WorkloadRankTestHarness, public ::testing::Test {
 protected:
  struct SentMessage {
    uint32_t dstRank{0};
    uint32_t localQpn{0};
    uint64_t timeNs{0};
  };

  void sendMessage(Message* msg) override {
    sentBy_[msg->srcRank].emplace_back(
        SentMessage{
            .dstRank = msg->dstRank,
            .localQpn = msg->localQPN,
            .timeNs = simCtx_.nowNs()});
    WorkloadRankTestHarness::sendMessage(msg);
  }

  void setWorkload(wt::Workload w) {
    workCfg_ = std::move(w);
    sentBy_.assign(*workCfg_.numEndpoints(), {});
  }

  // Rank 0 sends one message to rank 1.
  void setPair() {
    setWorkload(makeWorkload(
        {makeProgram({sendOp(1, 0)}), makeProgram({recvOp(0, 0)})}));
  }

  wt::DataflowOp& opOf(const uint32_t program, const uint32_t op) {
    return workCfg_.config()->dataflow()->programs()->at(program).ops()->at(op);
  }

  void expectRunRejects(const std::string& message) {
    EXPECT_THAT(
        [&]() { run(); },
        ThrowsMessage<std::invalid_argument>(HasSubstr(message)));
  }

  void expectCheckRejects(const std::string& message) {
    EXPECT_THAT(
        [&]() { DataflowRank::checkWorkload(workCfg_); },
        ThrowsMessage<std::invalid_argument>(HasSubstr(message)));
  }

  // The ranks that rank sent messages to, in the order it sent them.
  std::vector<uint32_t> dstRanksSentBy(const uint32_t rank) const {
    std::vector<uint32_t> dstRanks;
    for (const auto& msg : sentBy_.at(rank)) {
      dstRanks.emplace_back(msg.dstRank);
    }
    return dstRanks;
  }

  // Messages each rank has sent, in the order it sent them.
  std::vector<std::vector<SentMessage>> sentBy_;
};

TEST_F(DataflowTest, MessageBetweenTwoRanksCompletesBoth) {
  setPair();
  run();
  validateStats();
  EXPECT_EQ(ranks_.at(0)->getExpectedBytes(), 0U);
  EXPECT_EQ(ranks_.at(1)->getExpectedBytes(), kMessageBytes);
}

TEST_F(DataflowTest, SharedRingProgramSendsToTheNextRank) {
  constexpr uint32_t kNumRanks = 8;
  const auto left = [](wt::DataflowOp op) {
    op.peer() = kNumRanks - 1;
    return withPeerMode(std::move(op), wt::DataflowPeerMode::RELATIVE);
  };
  const auto right = [](wt::DataflowOp op) {
    op.peer() = 1;
    return withPeerMode(std::move(op), wt::DataflowPeerMode::RELATIVE);
  };
  // Each rank sends one piece and forwards the piece it receives first.
  setWorkload(makeWorkload(
      {makeProgram(
          {left(recvOp(0, 0)),
           right(sendOp(0, 1)),
           left(recvOp(0, 0)),
           right(sendOp(0, 1, {0}))})},
      std::vector<uint32_t>(kNumRanks, 0)));
  run();
  validateStats();
  for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
    ASSERT_EQ(sentBy_.at(rank).size(), 2U) << "rank " << rank;
    for (const auto& msg : sentBy_.at(rank)) {
      EXPECT_EQ(msg.dstRank, (rank + 1) % kNumRanks) << "rank " << rank;
    }
  }
}

TEST_F(DataflowTest, GroupRelativePeersStayInTheGroup) {
  constexpr uint32_t kNumRanks = 8;
  constexpr uint32_t kGroupSize = 4;
  auto recv = withPeerMode(recvOp(0, 0), wt::DataflowPeerMode::GROUP_RELATIVE);
  recv.peer() = kGroupSize - 1;
  auto send = withPeerMode(sendOp(0, 1), wt::DataflowPeerMode::GROUP_RELATIVE);
  send.peer() = 1;
  auto program = makeProgram({recv, send});
  program.groupSize() = kGroupSize;
  setWorkload(
      makeWorkload({std::move(program)}, std::vector<uint32_t>(kNumRanks, 0)));
  run();
  validateStats();
  for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
    const uint32_t base = rank - rank % kGroupSize;
    ASSERT_EQ(sentBy_.at(rank).size(), 1U) << "rank " << rank;
    EXPECT_EQ(
        sentBy_.at(rank).at(0).dstRank, base + (rank - base + 1) % kGroupSize)
        << "rank " << rank;
  }
}

TEST_F(DataflowTest, OperationWaitsForTheOperationAheadOfItInItsQueue) {
  // Operation 2 has no dependencies, but operation 1 ahead of it in queue 0
  // waits for the COMPUTE.
  setWorkload(makeWorkload(
      {makeProgram({computeOp(kComputeNs, 1), sendOp(1, 0, {0}), sendOp(1, 0)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  EXPECT_EQ(sentBy_.at(0).at(0).timeNs, kComputeNs);
  EXPECT_EQ(sentBy_.at(0).at(1).timeNs, kComputeNs);
}

TEST_F(DataflowTest, QueuesIssueIndependently) {
  // As above, but operation 2 has a queue and channel of its own.
  setWorkload(makeWorkload(
      {makeProgram(
           {computeOp(kComputeNs, 1),
            sendOp(1, 0, {0}),
            withChannel(sendOp(1, 2), 1)}),
       makeProgram({recvOp(0, 0), withChannel(recvOp(0, 0), 1)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  EXPECT_EQ(sentBy_.at(0).at(0).timeNs, 0U);
  EXPECT_EQ(sentBy_.at(0).at(1).timeNs, kComputeNs);
}

TEST_F(DataflowTest, QueuesReadyAtTheStartIssueInAscendingOrder) {
  // Queue 0 holds the SEND to rank 2, queue 1 the one to rank 3, and queue 2
  // the one to rank 1.
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 2), sendOp(2, 0), sendOp(3, 1)}),
       makeProgram({recvOp(0, 0)}),
       makeProgram({recvOp(0, 0)}),
       makeProgram({recvOp(0, 0)})}));
  run();
  validateStats();
  EXPECT_THAT(dstRanksSentBy(0), ElementsAre(2U, 3U, 1U));
}

TEST_F(DataflowTest, QueuesFreedByOneOperationIssueInProgramOrder) {
  // The RECV frees the SEND to rank 3 in queue 2 and then the SEND to rank 2
  // in queue 1.
  setWorkload(makeWorkload(
      {makeProgram({recvOp(1, 0), sendOp(3, 2, {0}), sendOp(2, 1, {0})}),
       makeProgram({sendOp(0, 0)}),
       makeProgram({recvOp(0, 0)}),
       makeProgram({recvOp(0, 0)})}));
  run();
  validateStats();
  EXPECT_THAT(dstRanksSentBy(0), ElementsAre(3U, 2U));
}

TEST_F(DataflowTest, QueueWaitAllWaitsForEarlierOperationsOfTheQueue) {
  setWorkload(makeWorkload(
      {makeProgram(
           {sendOp(1, 0),
            withQueueWait(sendOp(1, 0), wt::DataflowQueueWait::ALL)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  // The second SEND waits for the first one's acknowledgement.
  EXPECT_GT(sentBy_.at(0).at(1).timeNs, sentBy_.at(0).at(0).timeNs);
}

TEST_F(DataflowTest, QueueWaitRecvsWaitsForAnEarlierRecv) {
  // Rank 1 answers only after rank 0's message, sent after the COMPUTE, has
  // arrived. The two messages use channel 0 in opposite directions.
  setWorkload(makeWorkload(
      {makeProgram({computeOp(kComputeNs, 0), sendOp(1, 0, {0}), recvOp(1, 1)}),
       makeProgram(
           {recvOp(0, 0),
            withQueueWait(sendOp(0, 0), wt::DataflowQueueWait::RECVS)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(1).size(), 1U);
  EXPECT_GT(sentBy_.at(1).at(0).timeNs, kComputeNs);
}

TEST_F(DataflowTest, QueueWaitRecvsDoesNotWaitForAnEarlierSend) {
  setWorkload(makeWorkload(
      {makeProgram(
           {sendOp(1, 0),
            withQueueWait(sendOp(1, 0), wt::DataflowQueueWait::RECVS)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  EXPECT_EQ(sentBy_.at(0).at(1).timeNs, sentBy_.at(0).at(0).timeNs);
}

TEST_F(DataflowTest, FenceHoldsBackLaterOperationsOfTheQueue) {
  auto fenced = sendOp(1, 0);
  fenced.fence() = true;
  setWorkload(makeWorkload(
      {makeProgram({fenced, sendOp(1, 0)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  EXPECT_GT(sentBy_.at(0).at(1).timeNs, sentBy_.at(0).at(0).timeNs);
}

TEST_F(DataflowTest, ComputeDelaysItsDependentsByItsDuration) {
  setWorkload(makeWorkload(
      {makeProgram({computeOp(kComputeNs, 0), sendOp(1, 1, {0})}),
       makeProgram({recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 1U);
  EXPECT_EQ(sentBy_.at(0).at(0).timeNs, kComputeNs);
}

TEST_F(DataflowTest, JoinCompletesWhenItsLastDependencyCompletes) {
  setWorkload(makeWorkload(
      {makeProgram(
           {computeOp(kComputeNs / 2, 0),
            computeOp(kComputeNs, 1),
            makeOp(wt::DataflowOpKind::JOIN, 2, {0, 1}),
            sendOp(1, 3, {2})}),
       makeProgram({recvOp(0, 0)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 1U);
  EXPECT_EQ(sentBy_.at(0).at(0).timeNs, kComputeNs);
}

TEST_F(DataflowTest, StartDelayHoldsSendsButNotRecvs) {
  // Rank 0's message reaches rank 1 during rank 1's start delay; the harness
  // throws if no receive is posted for it.
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), recvOp(1, 1)}),
       makeProgram({recvOp(0, 0), sendOp(0, 1)})}));
  workCfg_.explicitStaggeredStartTimeNs() = {0, kStartDelayNs};
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(1).size(), 1U);
  EXPECT_EQ(sentBy_.at(1).at(0).timeNs, kStartDelayNs);
  EXPECT_EQ(*ranks_.at(1)->getThriftResult().startTimeNs(), kStartDelayNs);
}

TEST_F(DataflowTest, RankWithoutOperationsCompletesAtItsStart) {
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0)}),
       makeProgram({recvOp(0, 0)}),
       makeProgram({})}));
  workCfg_.explicitStaggeredStartTimeNs() = {0, 0, kStartDelayNs};
  run();
  validateStats();
  const auto result = ranks_.at(2)->getThriftResult();
  EXPECT_EQ(*result.startTimeNs(), kStartDelayNs);
  EXPECT_EQ(*result.endTimeNs(), kStartDelayNs);
}

TEST_F(DataflowTest, RankPairGetsQueuePairsUpToItsHighestChannelPlusNotify) {
  // Ranks 0 and 1 use channels 0 and 2, so their pair gets data queue pairs
  // 0, 1 and 2 and the notify queue pair; rank 2 exchanges nothing.
  setWorkload(makeWorkload(
      {makeProgram(
           {withChannel(sendOp(1, 0), 2), withChannel(recvOp(1, 1), 0)}),
       makeProgram(
           {withChannel(recvOp(0, 0), 2), withChannel(sendOp(0, 1), 0)}),
       makeProgram({})}));
  run();
  validateStats();
  EXPECT_EQ(workCfg_.qpnPairMap()->at(0).at(1).size(), 4U);
  EXPECT_EQ(workCfg_.qpnPairMap()->at(1).at(0).size(), 4U);
  EXPECT_FALSE(workCfg_.qpnPairMap()->contains(2));
}

TEST_F(DataflowTest, ChannelsUseDistinctQueuePairs) {
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), withChannel(sendOp(1, 1), 1)}),
       makeProgram({recvOp(0, 0), withChannel(recvOp(0, 0), 1)})}));
  run();
  validateStats();
  ASSERT_EQ(sentBy_.at(0).size(), 2U);
  EXPECT_NE(sentBy_.at(0).at(0).localQpn, sentBy_.at(0).at(1).localQpn);
}

TEST_F(DataflowTest, GetChannelCountsTakesTheHighestChannelOfEitherRank) {
  // Only rank 0 uses channel 2 with rank 1; rank 2 exchanges nothing.
  const auto w = makeWorkload(
      {makeProgram({withChannel(sendOp(1, 0), 2)}),
       makeProgram({withChannel(recvOp(0, 0), 0)}),
       makeProgram({})});
  const auto counts = DataflowRank::getChannelCounts(w);
  ASSERT_EQ(counts.size(), 3U);
  EXPECT_THAT(counts.at(0), UnorderedElementsAre(Pair(1U, 3U)));
  EXPECT_THAT(counts.at(1), UnorderedElementsAre(Pair(0U, 3U)));
  EXPECT_THAT(counts.at(2), IsEmpty());
}

TEST_F(DataflowTest, GetChannelCountsRejectsAnotherWorkloadType) {
  wt::Workload w;
  w.name() = "Compute";
  w.numEndpoints() = 2;
  w.config()->compute() = wt::Compute();
  EXPECT_THAT(
      [&]() { DataflowRank::getChannelCounts(w); },
      ThrowsMessage<std::invalid_argument>(
          HasSubstr("is not a Dataflow workload")));
}

TEST_F(DataflowTest, CheckWorkloadAcceptsMessagesThatPairUp) {
  // Two channels from rank 0 to rank 1, which rank 1 posts in the other
  // order, one channel back, and a rank without operations.
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), withChannel(sendOp(1, 1), 1), recvOp(1, 2)}),
       makeProgram({withChannel(recvOp(0, 0), 1), recvOp(0, 1), sendOp(0, 2)}),
       makeProgram({})}));
  EXPECT_NO_THROW(DataflowRank::checkWorkload(workCfg_));
}

TEST_F(DataflowTest, CheckWorkloadRejectsASendWithoutARecv) {
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), sendOp(1, 0)}),
       makeProgram({recvOp(0, 0)})}));
  expectCheckRejects(
      "on channel 0 from rank 0 to rank 1, the number of SENDs (2) differs "
      "from the number of RECVs (1)");
}

TEST_F(DataflowTest, CheckWorkloadRejectsASendWithoutARecvOnALowerChannel) {
  // Channel 1 pairs up; channel 0 has only the SEND.
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), withChannel(sendOp(1, 1), 1)}),
       makeProgram({withChannel(recvOp(0, 0), 1)})}));
  expectCheckRejects(
      "on channel 0 from rank 0 to rank 1, the number of SENDs (1) differs "
      "from the number of RECVs (0)");
}

TEST_F(DataflowTest, CheckWorkloadRejectsARecvWithoutASendOnALowerChannel) {
  // Channel 1 pairs up; channel 0 has only the RECV.
  setWorkload(makeWorkload(
      {makeProgram({withChannel(sendOp(1, 0), 1)}),
       makeProgram({recvOp(0, 0), withChannel(recvOp(0, 1), 1)})}));
  expectCheckRejects(
      "on channel 0 from rank 0 to rank 1, the number of SENDs (0) differs "
      "from the number of RECVs (1)");
}

TEST_F(DataflowTest, CheckWorkloadRejectsMessagesOfDifferentSizes) {
  setPair();
  opOf(1, 0).sizeBytes() = kMessageBytes / 2;
  expectCheckRejects(
      "rank 0 operation 0 sends 1000 bytes to rank 1 on channel 0, but the "
      "RECV that matches it, rank 1 operation 0, receives 500 bytes");
}

TEST_F(DataflowTest, CheckWorkloadPairsMessagesInProgramOrder) {
  // Both ranks list a 1000-byte and a 500-byte message, in opposite orders.
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), sendOp(1, 0)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  opOf(0, 1).sizeBytes() = kMessageBytes / 2;
  opOf(1, 0).sizeBytes() = kMessageBytes / 2;
  expectCheckRejects(
      "rank 0 operation 0 sends 1000 bytes to rank 1 on channel 0, but the "
      "RECV that matches it, rank 1 operation 0, receives 500 bytes");
}

TEST_F(DataflowTest, CheckWorkloadRejectsChannelSettings) {
  setPair();
  workCfg_.channelSettings() = wt::ChannelSettings();
  expectCheckRejects("a Dataflow workload cannot set channelSettings");
}

TEST_F(DataflowTest, RejectsDependencyOnALaterOperation) {
  setPair();
  opOf(1, 0).deps() = {1};
  workCfg_.config()->dataflow()->programs()->at(1).ops()->emplace_back(
      makeOp(wt::DataflowOpKind::JOIN, 0, {}));
  expectRunRejects("depends on operation 1, which does not come before it");
}

TEST_F(DataflowTest, RejectsDependencyOnItself) {
  setPair();
  opOf(1, 0).deps() = {0};
  expectRunRejects("depends on operation 0, which does not come before it");
}

TEST_F(DataflowTest, RejectsZeroByteMessage) {
  setPair();
  opOf(0, 0).sizeBytes() = 0;
  expectRunRejects("a RECV or SEND moves at least 1 byte");
}

TEST_F(DataflowTest, RejectsPeerThatIsTheRankItself) {
  setPair();
  opOf(0, 0).peer() = 0;
  expectRunRejects("its peer is the rank itself");
}

TEST_F(DataflowTest, RejectsAbsolutePeerOutsideTheWorkload) {
  setPair();
  opOf(0, 0).peer() = 5;
  expectRunRejects("peer 5 is not below the 2 ranks");
}

TEST_F(DataflowTest, RejectsRelativePeerNotBelowTheRankCount) {
  setPair();
  opOf(0, 0) = withPeerMode(opOf(0, 0), wt::DataflowPeerMode::RELATIVE);
  opOf(0, 0).peer() = 2;
  expectRunRejects("RELATIVE peer 2 is not below the 2 ranks");
}

TEST_F(DataflowTest, RejectsGroupSizeThatDoesNotDivideTheRanks) {
  setPair();
  opOf(0, 0) = withPeerMode(opOf(0, 0), wt::DataflowPeerMode::GROUP_RELATIVE);
  workCfg_.config()->dataflow()->programs()->at(0).groupSize() = 3;
  expectRunRejects("GROUP_RELATIVE needs a groupSize that divides the 2 ranks");
}

TEST_F(DataflowTest, RejectsGroupRelativePeerNotBelowTheGroupSize) {
  setPair();
  opOf(0, 0) = withPeerMode(opOf(0, 0), wt::DataflowPeerMode::GROUP_RELATIVE);
  opOf(0, 0).peer() = 2;
  workCfg_.config()->dataflow()->programs()->at(0).groupSize() = 2;
  expectRunRejects("GROUP_RELATIVE peer 2 is not below groupSize 2");
}

TEST_F(DataflowTest, RejectsChannelAtTheLimit) {
  setPair();
  opOf(0, 0).channel() = 2047;
  expectRunRejects("channel 2047 is not below 2047");
}

TEST_F(DataflowTest, RejectsQueueNotBelowTheOperationCount) {
  setPair();
  opOf(1, 0).queue() = 1;
  expectRunRejects("queue 1 is not below the program's 1 operations");
}

TEST_F(DataflowTest, RejectsAnOperationWithoutAKind) {
  setPair();
  opOf(1, 0).kind() = wt::DataflowOpKind::UNKNOWN;
  expectRunRejects("its kind is not set");
}

TEST_F(DataflowTest, RejectsUnknownKind) {
  setPair();
  workCfg_.config()->dataflow()->programs()->at(1).ops()->emplace_back(
      makeOp(static_cast<wt::DataflowOpKind>(7), 0, {}));
  expectRunRejects("unknown kind 7");
}

TEST_F(DataflowTest, RejectsUnknownQueueWait) {
  setPair();
  opOf(0, 0).queueWait() = static_cast<wt::DataflowQueueWait>(7);
  expectRunRejects("unknown queueWait 7");
}

TEST_F(DataflowTest, RejectsUnknownPeerMode) {
  setPair();
  opOf(0, 0).peerMode() = static_cast<wt::DataflowPeerMode>(7);
  expectRunRejects("unknown peerMode 7");
}

TEST_F(DataflowTest, RejectsTwoQueuesSendingOnOneChannel) {
  setWorkload(makeWorkload(
      {makeProgram({sendOp(1, 0), sendOp(1, 1)}),
       makeProgram({recvOp(0, 0), recvOp(0, 0)})}));
  expectRunRejects(
      "queues 0 and 1 both send to rank 1 on channel 0; one queue must own "
      "each (peer, channel, direction)");
}

TEST_F(DataflowTest, RejectsRankProgramsOfAnotherLength) {
  setWorkload(makeWorkload({makeProgram({})}, {0}));
  workCfg_.numEndpoints() = 2;
  expectRunRejects("Dataflow rankPrograms has 1 entries for 2 ranks");
}

TEST_F(DataflowTest, RejectsProgramIndexOutOfRange) {
  setWorkload(makeWorkload({makeProgram({})}, {0, 3}));
  expectRunRejects("rank 1 runs program 3, but there are 1 programs");
}

TEST_F(DataflowTest, RejectsChannelSettings) {
  setPair();
  workCfg_.channelSettings() = wt::ChannelSettings();
  expectRunRejects("a Dataflow workload cannot set channelSettings");
}

TEST_F(DataflowTest, RejectsBeingABatchChild) {
  setPair();
  auto child = workCfg_;
  child.name() = "DataflowChild";
  workCfg_.config()->batch() = wt::Batch();
  workCfg_.config()->batch()->children()->emplace_back(std::move(child));
  expectRunRejects("a Dataflow workload cannot be a Batch child");
}

TEST_F(DataflowTest, RejectsChannelWithoutAQueuePair) {
  setPair();
  opOf(0, 0).channel() = 1;
  opOf(1, 0).channel() = 1;
  // One data queue pair and the notify queue pair.
  workCfg_.qpnPairMap() = {
      {0, {{1, {makeQpnPair(1, 11), makeQpnPair(2, 12)}}}},
      {1, {{0, {makeQpnPair(11, 1), makeQpnPair(12, 2)}}}}};
  expectRunRejects("the rank pair has only 1 data queue pairs");
}

TEST_F(DataflowTest, RejectsQueuePairsListedInDifferentOrders) {
  setPair();
  // Rank 1 lists the two data queue pairs in the opposite order.
  workCfg_.qpnPairMap() = {
      {0, {{1, {makeQpnPair(1, 11), makeQpnPair(2, 12), makeQpnPair(3, 13)}}}},
      {1, {{0, {makeQpnPair(12, 2), makeQpnPair(11, 1), makeQpnPair(13, 3)}}}}};
  expectRunRejects("the two ranks name different queue pairs");
}

} // namespace packsim
