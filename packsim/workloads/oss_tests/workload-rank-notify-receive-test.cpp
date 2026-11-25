/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/kernel/scheduler.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/transport/transport.h"
#include "packsim/workloads/rank/workload-rank.h"

using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

namespace packsim {

namespace {
constexpr uint32_t kNumRanks = 2;
// The rank under test and its one peer.
constexpr uint32_t kRank = 0;
constexpr uint32_t kPeer = 1;
} // namespace

// A rank with no workload of its own. Its progress() does nothing, so a
// notification the rank counts stays for checkNotifyReceived() to report.
class IdleRank : public WorkloadRank {
 public:
  explicit IdleRank(const WorkloadRankParams& params) : WorkloadRank(params) {}

  void run() override {}

  uint64_t getExpectedBytes() override {
    return 0;
  }

  bool complete() override {
    return true;
  }

  // Raise the rank-complete event, as a rank does once its work is done.
  void finish() {
    onRankComplete(workloadId_, rank_);
  }
};

/**
 * Rank 0 of two, with a data QP and a notify QP to rank 1. When it is built,
 * the rank posts one notify receive from rank 1, as every rank does for each
 * peer. The fixture counts every receive the rank posts.
 */
class WorkloadRankNotifyReceiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    workCfg_.name() = "NOTIFY_RECEIVE_TEST";
    workCfg_.numEndpoints() = kNumRanks;
    for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
      workCfg_.rankToIdList()->emplace(
          rank, std::vector<std::vector<uint32_t>>{{rank}});
    }
    // The last QP to a peer is its notify QP.
    (*workCfg_.qpnPairMap())[kRank][kPeer] = {
        makeQpnPair(/*local=*/0, /*remote=*/1),
        makeQpnPair(/*local=*/2, /*remote=*/3)};
    const WorkloadRankParams params{
        .simCtx = &simCtx_,
        .workloadId = 0,
        .workCfg = workCfg_,
        .rank = kRank,
        .smcb = submitSendFn_,
        .rmcb = submitRecvFn_,
        .rccb = onRankComplete_,
        .parent = nullptr};
    rank_ = std::make_unique<IdleRank>(params);
  }

  static packsim::workload::thrift::QPNPair makeQpnPair(
      const uint32_t local,
      const uint32_t remote) {
    packsim::workload::thrift::QPNPair qpnPair;
    qpnPair.local() = local;
    qpnPair.remote() = remote;
    return qpnPair;
  }

  // The notify receive from the peer that the rank holds.
  [[nodiscard]] Message& notifyReceive() {
    return *rank_->recvNotifyMessages_.at(kPeer).front();
  }

  Scheduler scheduler_;
  SimContext simCtx_{&scheduler_, 0};
  packsim::workload::thrift::Workload workCfg_;
  // The receives the rank has posted.
  uint32_t receivesPosted_{0};
  WorkloadRankParams::SubmitMessageFn submitSendFn_{[](Message*) {}};
  WorkloadRankParams::SubmitMessageFn submitRecvFn_{
      [this](Message*) { ++receivesPosted_; }};
  WorkloadRankParams::OnRankCompleteCallback onRankComplete_{
      [](uint32_t, uint32_t) {}};
  std::unique_ptr<IdleRank> rank_;
};

// A notify receive that a QP in error flushes carries no notification. The
// rank releases it, counts no notification and posts no further notify
// receive, which the QP would flush in turn.
TEST_F(
    WorkloadRankNotifyReceiveTest,
    AReceiveFlushedInErrorIsNeitherCountedNorReplaced) {
  // Arrange
  ASSERT_EQ(receivesPosted_, 1u) << "the constructor posts one";
  notifyReceive().completionStatus =
      Message::CompletionStatus::FLUSHED_IN_ERROR;

  // Act
  rank_->notifyReceived(kPeer);

  // Assert
  EXPECT_TRUE(rank_->recvNotifyMessages_.at(kPeer).empty());
  EXPECT_EQ(receivesPosted_, 1u);
  EXPECT_FALSE(rank_->checkNotifyReceived(kPeer));
}

// A notify receive that completes normally is a notification: the rank counts
// it and posts the next notify receive in its place.
TEST_F(WorkloadRankNotifyReceiveTest, ASuccessfulReceiveIsCountedAndReplaced) {
  // Arrange
  ASSERT_EQ(receivesPosted_, 1u) << "the constructor posts one";
  notifyReceive().completionStatus = Message::CompletionStatus::SUCCESS;

  // Act
  rank_->notifyReceived(kPeer);

  // Assert
  EXPECT_EQ(receivesPosted_, 2u);
  EXPECT_TRUE(rank_->checkNotifyReceived(kPeer));
}

// A peer's notification can reach a rank that has completed. The rank counts
// it but posts no notify receive in its place: in a schedule with workloadDags
// the node has unregistered the completed rank's QPs, and the receive would
// connect a QP that the next iteration's rank takes over.
TEST_F(
    WorkloadRankNotifyReceiveTest,
    ACompletedRankCountsALateNotificationButPostsNoReceive) {
  // Arrange
  rank_->finish();
  ASSERT_EQ(receivesPosted_, 1u) << "the constructor posts one";
  notifyReceive().completionStatus = Message::CompletionStatus::SUCCESS;

  // Act
  rank_->notifyReceived(kPeer);

  // Assert
  EXPECT_EQ(receivesPosted_, 1u);
  EXPECT_TRUE(rank_->recvNotifyMessages_.at(kPeer).empty());
  EXPECT_TRUE(rank_->checkNotifyReceived(kPeer));
}

// After a notify receive from the peer completes in error the rank holds none,
// so a further notify completion from the peer is refused.
TEST_F(WorkloadRankNotifyReceiveTest, ACompletionWithNoReceiveHeldThrows) {
  // Arrange
  notifyReceive().completionStatus =
      Message::CompletionStatus::FLUSHED_IN_ERROR;
  rank_->notifyReceived(kPeer);
  ASSERT_TRUE(rank_->recvNotifyMessages_.at(kPeer).empty());

  // Act + Assert
  EXPECT_THAT(
      [this] { rank_->notifyReceived(kPeer); },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("Rank 0 has no notify receive from rank 1 to complete")));
}

} // namespace packsim
