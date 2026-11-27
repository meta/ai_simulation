/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/workloads/rank/qp-listing.h"

namespace packsim {

/**
 * A workload whose nicQpnPairMap a test writes entry by entry, then hands to
 * QpListing::check.
 */
class QpListingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    workload_.name() = "LISTED";
  }

  /**
   * List one QP of rank pair (srcRank, dstRank) on a NIC pair, at the sender's
   * end and, transposed, at the receiver's.
   *
   * @param srcRank    The sender, e.g. 0.
   * @param dstRank    The receiver, e.g. 1.
   * @param srcNicIdx  The sender's NIC.
   * @param dstNicIdx  The receiver's NIC.
   * @param srcQpn     The sender's QPN; the receiver's is srcQpn + 1.
   */
  void listAtBothEnds(
      const uint32_t srcRank,
      const uint32_t dstRank,
      const uint32_t srcNicIdx,
      const uint32_t dstNicIdx,
      const uint32_t srcQpn) {
    listAtOneEnd(srcRank, dstRank, srcNicIdx, dstNicIdx, srcQpn, srcQpn + 1);
    listAtOneEnd(dstRank, srcRank, dstNicIdx, srcNicIdx, srcQpn + 1, srcQpn);
  }

  /**
   * List one QP at one end only.
   *
   * @param ownRank    The end that lists it, e.g. 1.
   * @param peerRank   The other end, e.g. 0.
   * @param ownNicIdx  The listing end's NIC.
   * @param peerNicIdx The other end's NIC.
   * @param localQpn   The QPN at the listing end.
   * @param remoteQpn  The QPN at the other end.
   */
  void listAtOneEnd(
      const uint32_t ownRank,
      const uint32_t peerRank,
      const uint32_t ownNicIdx,
      const uint32_t peerNicIdx,
      const uint32_t localQpn,
      const uint32_t remoteQpn) {
    packsim::thrift::RankPair rankPair;
    rankPair.srcRank() = ownRank;
    rankPair.dstRank() = peerRank;
    packsim::thrift::NicPair nicPair;
    nicPair.srcNicIdx() = ownNicIdx;
    nicPair.dstNicIdx() = peerNicIdx;
    packsim::workload::thrift::QPNPair qpnPair;
    qpnPair.local() = localQpn;
    qpnPair.remote() = remoteQpn;
    (*workload_.nicQpnPairMap())[rankPair][nicPair].emplace_back(
        std::move(qpnPair));
  }

  packsim::workload::thrift::Workload workload_;
};

TEST_F(QpListingTest, AcceptsPairsListedAtBothEndsInTheSameOrder) {
  listAtBothEnds(0, 1, 0, 0, 1);
  listAtBothEnds(0, 1, 0, 0, 3);
  listAtBothEnds(0, 1, 1, 1, 5);
  listAtBothEnds(1, 2, 0, 1, 7);

  const auto result = QpListing::check({workload_});

  EXPECT_TRUE(result.isEveryRankPairListedIdentically);
  EXPECT_FALSE(result.firstMismatch.has_value());
  EXPECT_EQ(result.numMismatchedRankPairs, 0);
}

TEST_F(QpListingTest, RejectsNicPairsThatSortDifferentlyAtTheTwoEnds) {
  listAtBothEnds(0, 1, 0, 1, 1);
  listAtBothEnds(0, 1, 1, 0, 3);
  listAtBothEnds(1, 2, 0, 0, 5);

  const auto result = QpListing::check({workload_});

  EXPECT_FALSE(result.isEveryRankPairListedIdentically);
  ASSERT_TRUE(result.firstMismatch.has_value());
  EXPECT_EQ(result.firstMismatch->workloadName, "LISTED");
  EXPECT_EQ(result.firstMismatch->srcRank, 0);
  EXPECT_EQ(result.firstMismatch->dstRank, 1);
  EXPECT_EQ(result.numMismatchedRankPairs, 1);
}

TEST_F(QpListingTest, RejectsAQpOnlyOneEndLists) {
  listAtBothEnds(0, 1, 0, 0, 1);
  listAtOneEnd(1, 0, 0, 0, 100, 101);

  EXPECT_FALSE(QpListing::check({workload_}).isEveryRankPairListedIdentically);
}

TEST_F(QpListingTest, RejectsAQpListedTwice) {
  listAtBothEnds(0, 1, 0, 0, 1);
  listAtBothEnds(0, 1, 0, 0, 1);

  EXPECT_FALSE(QpListing::check({workload_}).isEveryRankPairListedIdentically);
}

TEST_F(QpListingTest, RejectsAPairListedAtOneEndOnly) {
  listAtOneEnd(0, 1, 0, 0, 1, 2);

  const auto result = QpListing::check({workload_});

  EXPECT_FALSE(result.isEveryRankPairListedIdentically);
  EXPECT_EQ(result.numMismatchedRankPairs, 1);
}

TEST_F(QpListingTest, NamesTheFirstWorkloadWithAMismatch) {
  listAtBothEnds(0, 1, 0, 0, 1);
  packsim::workload::thrift::Workload other = workload_;
  other.name() = "OTHER";
  listAtOneEnd(1, 0, 0, 0, 100, 101);

  const auto result = QpListing::check({other, workload_});

  ASSERT_TRUE(result.firstMismatch.has_value());
  EXPECT_EQ(result.firstMismatch->workloadName, "LISTED");
}

} // namespace packsim
