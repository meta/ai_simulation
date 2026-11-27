/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/kernel/random.h"
#include "packsim/kernel/time.h"
#include "packsim/transport/transport.h"
#include "packsim/util/sim-fix.h"
#include "packsim/workloads/oss_tests/permutation-unequal-qp-count-test-fixture.h"
#include "packsim/workloads/oss_tests/rank-harness.h"

namespace packsim {

class PermutationWorkloadRankTest
    : public packsim::WorkloadRankTestHarness,
      public ::testing::TestWithParam<
          std::tuple<uint32_t, uint32_t, uint32_t>> {
 public:
  void SetUp() override {
    workCfg_.name() = "Permutation";
    workCfg_.workloadId() = 0;
    auto numRanks = std::get<0>(GetParam());
    workCfg_.numEndpoints() = numRanks;

    auto perm = packsim::workload::thrift::Permutation();
    perm.sizeBytes() = std::get<1>(GetParam());
    perm.numChunks() = std::get<2>(GetParam());

    std::vector<uint32_t> ranks(numRanks);
    std::iota(ranks.begin(), ranks.end(), 0U);
    simCtx_.random.shuffle(ranks.begin(), ranks.end());
    perm.dstRanks()->resize(numRanks);
    for (uint32_t i = 0; i < numRanks; i++) {
      perm.dstRanks()[ranks[i]] = ranks[(i + 1) % numRanks];
    }
    workCfg_.config()->permutation() = perm;

    run();
  }
};

TEST_P(PermutationWorkloadRankTest, Validate) {
  validateStats();
}

INSTANTIATE_TEST_SUITE_P(
    PermutationWorkloadRankTestSweep,
    PermutationWorkloadRankTest,
    ::testing::Combine(
        ::testing::Values(8, 128, 256),
        ::testing::Values(1024, 1024 * 1024, 512 * 1024 * 1024),
        ::testing::Values(1, 3, 7, 15)));

class PermutationZeroSizeTest : public packsim::WorkloadRankTestHarness,
                                public ::testing::Test {};

TEST_F(PermutationZeroSizeTest, RanksWithZeroSizeBytesComplete) {
  workCfg_.name() = "PERM_ZERO";
  workCfg_.workloadId() = 0;
  workCfg_.numEndpoints() = 2;
  auto perm = packsim::workload::thrift::Permutation();
  perm.sizeBytes() = 0;
  perm.numChunks() = 1;
  perm.dstRanks() = {1, 0};
  workCfg_.config()->permutation() = perm;

  run();

  validateStats();
}

class PermutationConfigValidationTest : public packsim::WorkloadRankTestHarness,
                                        public ::testing::Test {};

TEST_F(PermutationConfigValidationTest, RejectsZeroNumChunks) {
  workCfg_.name() = "PERM_CFG";
  workCfg_.workloadId() = 0;
  workCfg_.numEndpoints() = 2;
  auto perm = packsim::workload::thrift::Permutation();
  perm.sizeBytes() = 1024;
  perm.numChunks() = 0;
  perm.dstRanks() = {1, 0};
  workCfg_.config()->permutation() = perm;
  EXPECT_THAT(
      [&]() { buildRanks(); },
      ::testing::ThrowsMessage<std::invalid_argument>(::testing::HasSubstr(
          "Workload PERM_CFG: Permutation numChunks must be at least 1, got 0")));
}

/**
 * A permutation 0 -> 1 -> 2 -> 0 with a rank pair that is not listed
 * identically at its two ends: a hand-written map where one end lists a QP
 * the other does not, or NIC pairs that sort differently at the two ends.
 * Every rank then spreads its receives over the data QPs of the rank pair it
 * sends on. A test adds QP templates, may edit the expanded map, then calls
 * run().
 */
class PermutationListedDifferentlyTest
    : public packsim::WorkloadRankTestHarness,
      public ::testing::Test {
 protected:
  static constexpr uint32_t kNumRanks = 3;
  // QPNs of the QP only one end lists, above every QPN the templates take.
  static constexpr uint32_t kOneEndLocalQpn = 1000;
  static constexpr uint32_t kOneEndRemoteQpn = 1001;

  void SetUp() override {
    workCfg_.name() = "PERM_LISTED_DIFFERENTLY";
    workCfg_.workloadId() = 0;
    workCfg_.numEndpoints() = kNumRanks;
    for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
      workCfg_.rankToIdList()->emplace(
          rank, std::vector<std::vector<uint32_t>>{{rank, 0}, {rank, 1}});
    }
  }

  /**
   * Set the permutation 0 -> 1 -> 2 -> 0.
   *
   * @param numChunks  Chunks per rank, 1024 bytes each.
   */
  void setPermutation(const uint32_t numChunks) {
    auto perm = packsim::workload::thrift::Permutation();
    perm.sizeBytes() = 1024 * numChunks;
    perm.numChunks() = numChunks;
    perm.dstRanks() = {1, 2, 0};
    workCfg_.config()->permutation() = perm;
  }

  /**
   * Add QP templates from one rank to the next on one NIC pair.
   *
   * @param srcRank    The sending rank, e.g. 0 for rank pair 0 -> 1.
   * @param srcNicIdx  Its NIC.
   * @param dstNicIdx  The receiving rank's NIC.
   * @param qps        The templates to add, e.g. 2.
   */
  void addQps(
      const uint32_t srcRank,
      const uint32_t srcNicIdx,
      const uint32_t dstNicIdx,
      const uint32_t qps) {
    for (uint32_t qp = 0; qp < qps; ++qp) {
      packsim::thrift::QueuePairTemplate qpTemplate;
      qpTemplate.ranks()->srcRank() = srcRank;
      qpTemplate.ranks()->dstRank() = (srcRank + 1) % kNumRanks;
      qpTemplate.nics()->srcNicIdx() = srcNicIdx;
      qpTemplate.nics()->dstNicIdx() = dstNicIdx;
      workCfg_.qpTemplates()->emplace_back(std::move(qpTemplate));
    }
  }

  /**
   * Expand the templates as fix-up does, then give rank 1's end of rank pair
   * 0 -> 1 a QP on NIC pair (0, 0) that rank 0's end does not list, just
   * before the notify QP, so rank 1 counts one more data QP than rank 0.
   */
  void listOneMoreQpAtRank1() {
    populateRankToId(workCfg_);
    std::vector<packsim::workload::thrift::Workload> workCfgs({workCfg_});
    uint32_t nextQpn = 0;
    packsim::buildNicQPNMap(workCfgs, nextQpn);
    workCfg_ = std::move(workCfgs.at(0));
    packsim::thrift::RankPair rankPair;
    rankPair.srcRank() = 1;
    rankPair.dstRank() = 0;
    packsim::thrift::NicPair nicPair;
    nicPair.srcNicIdx() = 0;
    nicPair.dstNicIdx() = 0;
    auto& qpnPairs = workCfg_.nicQpnPairMap()->at(rankPair).at(nicPair);
    packsim::workload::thrift::QPNPair oneEndQpn;
    oneEndQpn.local() = kOneEndLocalQpn;
    oneEndQpn.remote() = kOneEndRemoteQpn;
    qpnPairs.insert(qpnPairs.end() - 1, std::move(oneEndQpn));
  }

  void recvMessage(Message* msg) override {
    if (msg->connectionMessageId != kNotifyConnectionMessageId) {
      ++postedDataRecvsByNic_[msg->dstRank][msg->endpointId];
    }
    WorkloadRankTestHarness::recvMessage(msg);
  }

  // Data receives each rank posted, by the receiving NIC: rank -> NIC -> count.
  std::map<uint32_t, std::map<uint32_t, uint32_t>> postedDataRecvsByNic_;
};

// Rank 1 lists 2 data QPs for rank pair 0 -> 1 and rank 0 lists 1. Spread over
// rank 1's outgoing count, 1, every receive lands on the QP both ends list,
// where rank 0 sends.
TEST_F(
    PermutationListedDifferentlyTest,
    CompletesWhenTheReceiverListsAQpTheSenderDoesNot) {
  setPermutation(/*numChunks=*/4);
  for (uint32_t srcRank = 0; srcRank < kNumRanks; ++srcRank) {
    addQps(srcRank, 0, 0, 2);
  }
  listOneMoreQpAtRank1();

  ASSERT_NO_THROW(run());

  validateStats();
}

// Rank 0 sends on 2 data QPs of rank pair 0 -> 1, and rank 1 lists them and a
// third. Spread over rank 1's outgoing count, 1, both receives land on the
// first QP, so rank 0's chunk on the second finds no receive.
TEST_F(
    PermutationListedDifferentlyTest,
    SpreadsReceivesOverTheOutgoingCountWhenEndsListDifferentQps) {
  setPermutation(/*numChunks=*/2);
  addQps(0, 0, 0, 3);
  addQps(1, 0, 0, 2);
  addQps(2, 0, 0, 2);
  listOneMoreQpAtRank1();

  EXPECT_THAT(
      [this]() { run(); },
      ::testing::ThrowsMessage<std::runtime_error>(
          ::testing::HasSubstr("Queue should not be empty src 0, dst 1")));
  const std::map<uint32_t, uint32_t> firstQpOnly{{0, 2}};
  EXPECT_EQ(postedDataRecvsByNic_[1], firstQpOnly);
}

// Rank pair 0 -> 1 uses NIC pairs (0, 1) and (1, 0), which sort in the other
// order at rank 1's end. Rank 2, which receives on 2 data QPs and sends on 1,
// posts every receive on NIC 0.
TEST_F(
    PermutationListedDifferentlyTest,
    SpreadsReceivesOverTheOutgoingCountWhenNicPairsSortDifferently) {
  setPermutation(/*numChunks=*/4);
  addQps(0, 0, 1, 2);
  addQps(0, 1, 0, 2);
  addQps(1, 0, 0, 2);
  addQps(1, 1, 1, 2);
  addQps(2, 0, 0, 2);

  EXPECT_THROW(run(), std::runtime_error);
  const std::map<uint32_t, uint32_t> nic0Only{{0, 4}};
  EXPECT_EQ(postedDataRecvsByNic_[2], nic0Only);
}

/**
 * A batch whose first child is a permutation 0 -> 1 -> 2 -> 3 -> 0 in which
 * rank 1 receives on 2 data QPs and sends on 1, all children on the batch's
 * QP templates. A test adds a second child and its templates, then calls
 * run().
 */
class PermutationInABatchTest : public packsim::WorkloadRankTestHarness,
                                public ::testing::Test {
 protected:
  static constexpr uint32_t kNumRanks = 4;
  static constexpr uint32_t kNumChunks = 6;
  // Size of every chunk of a permutation child.
  static constexpr uint32_t kChunkSizeBytes = 1024;
  // How long the network holds the first notify from heldNotifySrc_ to
  // heldNotifyDst_.
  static constexpr uint64_t kNotifyHoldNs = 10'000;

  void SetUp() override {
    workCfg_.name() = "PERM_BATCH";
    workCfg_.workloadId() = 0;
    workCfg_.numEndpoints() = kNumRanks;
    for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
      workCfg_.rankToIdList()->emplace(
          rank, std::vector<std::vector<uint32_t>>{{rank, 0}, {rank, 1}});
    }
    workCfg_.config()->batch() = packsim::workload::thrift::Batch();
    addChild(permutation("PERM_A", {1, 2, 3, 0}));
    addQps(0, 1, 0, 0, 2);
    addQps(0, 1, 1, 1, 2);
    addQps(1, 2, 0, 0, 2);
    addQps(2, 3, 0, 0, 2);
    addQps(3, 0, 0, 0, 2);
  }

  /**
   * Build a permutation child of kNumChunks chunks.
   *
   * @param name      The child's name, e.g. "PERM_A".
   * @param dstRanks  Each rank's destination, e.g. {1, 2, 3, 0}.
   *
   * @return          The child.
   */
  static packsim::workload::thrift::Workload permutation(
      const std::string& name,
      std::vector<uint32_t> dstRanks) {
    packsim::workload::thrift::Workload child;
    child.name() = name;
    child.workloadId() = 0;
    child.numEndpoints() = kNumRanks;
    auto perm = packsim::workload::thrift::Permutation();
    perm.sizeBytes() = kChunkSizeBytes * kNumChunks;
    perm.numChunks() = kNumChunks;
    perm.dstRanks() = std::move(dstRanks);
    child.config()->permutation() = perm;
    return child;
  }

  // Append a child to the batch.
  void addChild(packsim::workload::thrift::Workload child) {
    workCfg_.config()->batch()->children()->emplace_back(std::move(child));
  }

  /**
   * Add QP templates on one NIC pair of a rank pair.
   *
   * @param srcRank    The sending rank, e.g. 0.
   * @param dstRank    The receiving rank, e.g. 1.
   * @param srcNicIdx  The sender's NIC.
   * @param dstNicIdx  The receiver's NIC.
   * @param qps        The templates to add, e.g. 2.
   */
  void addQps(
      const uint32_t srcRank,
      const uint32_t dstRank,
      const uint32_t srcNicIdx,
      const uint32_t dstNicIdx,
      const uint32_t qps) {
    for (uint32_t qp = 0; qp < qps; ++qp) {
      packsim::thrift::QueuePairTemplate qpTemplate;
      qpTemplate.ranks()->srcRank() = srcRank;
      qpTemplate.ranks()->dstRank() = dstRank;
      qpTemplate.nics()->srcNicIdx() = srcNicIdx;
      qpTemplate.nics()->dstNicIdx() = dstNicIdx;
      workCfg_.qpTemplates()->emplace_back(std::move(qpTemplate));
    }
  }

  void sendMessage(Message* msg) override {
    if (msg->connectionMessageId == kNotifyConnectionMessageId &&
        msg->srcRank == heldNotifySrc_ && msg->dstRank == heldNotifyDst_ &&
        !isNotifyHeld_) {
      isNotifyHeld_ = true;
      scheduler_.scheduleAfter(
          &simCtx_, Time::fromNs(kNotifyHoldNs), [this, msg]() {
            WorkloadRankTestHarness::sendMessage(msg);
          });
      return;
    }
    WorkloadRankTestHarness::sendMessage(msg);
  }

  void recvMessage(Message* msg) override {
    if (msg->connectionMessageId != kNotifyConnectionMessageId) {
      ++postedDataRecvsByNic_[msg->dstRank][msg->endpointId];
    }
    WorkloadRankTestHarness::recvMessage(msg);
  }

  // Ranks whose first notify the network holds; none by default.
  uint32_t heldNotifySrc_{UINT32_MAX};
  uint32_t heldNotifyDst_{UINT32_MAX};
  bool isNotifyHeld_{false};
  // Data receives each rank posted, by the receiving NIC: rank -> NIC -> count.
  std::map<uint32_t, std::map<uint32_t, uint32_t>> postedDataRecvsByNic_;
};

// The second child's rank pair 0 -> 2 uses NIC pairs (0, 0), (0, 1) and
// (1, 0), which sort differently at rank 2's end, so its chunks cross
// receives when it runs. The permutation keeps every rank's receives on the
// outgoing count, and stalls before the second child runs.
TEST_F(
    PermutationInABatchTest,
    KeepsTheOutgoingSpreadWhenASiblingsPairIsListedDifferently) {
  addChild(permutation("PERM_B", {2, 3, 0, 1}));
  addQps(0, 2, 0, 0, 2);
  addQps(0, 2, 0, 1, 2);
  addQps(0, 2, 1, 0, 2);
  addQps(1, 3, 0, 0, 2);

  EXPECT_THAT(
      [this]() { run(); },
      ::testing::ThrowsMessage<std::runtime_error>(
          ::testing::HasSubstr("Queue should not be empty src 0, dst 1")));
  const std::map<uint32_t, uint32_t> nic0Only{{0, kNumChunks}};
  EXPECT_EQ(postedDataRecvsByNic_[1], nic0Only);
}

// The permutation now completes and lets a double binary tree all-reduce run
// whose rank 1 gets rank 3's notify late. Rank 1 forwards the even tree's
// reduction to rank 2 only after its own odd-tree chunks to rank 2.
TEST_F(PermutationInABatchTest, LetsALaterTreeAllReduceCompleteInOrder) {
  packsim::workload::thrift::Workload allReduce;
  allReduce.name() = "AR_DBT";
  allReduce.workloadId() = 0;
  allReduce.numEndpoints() = kNumRanks;
  auto ar = packsim::workload::thrift::AllReduce();
  ar.sizeBytes() = 2 * kChunkSizeBytes * kNumChunks;
  ar.numChunks() = kNumChunks;
  ar.algorithm() =
      packsim::workload::thrift::AllReduceAlgorithm::DOUBLE_BINARY_TREE;
  allReduce.config()->allReduce() = ar;
  addChild(std::move(allReduce));
  addQps(0, 2, 0, 0, 2);
  addQps(1, 3, 0, 0, 2);
  heldNotifySrc_ = 3;
  heldNotifyDst_ = 1;

  ASSERT_NO_THROW(run());

  validateStats();
  EXPECT_TRUE(isNotifyHeld_);
}

TEST_P(
    PermutationUnequalQpCountTest,
    CompletesWhenARankReceivesOnMoreQpsThanItSendsOn) {
  ASSERT_NO_THROW(run());

  validateStats();
  for (uint32_t rank = 0; rank < numRanks(); ++rank) {
    EXPECT_EQ(postedDataRecvsByNic_[rank], evenDataRecvsByNic(rank))
        << "rank " << rank;
  }
}

INSTANTIATE_TEST_SUITE_P(
    UnequalRankPairQpCounts,
    PermutationUnequalQpCountTest,
    ::testing::ValuesIn(PermutationUnequalQpCountTest::layouts()),
    PermutationUnequalQpCountTest::layoutName);

} // namespace packsim
