/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/transport/transport.h"
#include "packsim/workloads/oss_tests/rank-harness.h"
#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {

// The QP templates of a permutation 0 -> 1 -> ... -> N-1 -> 0.
struct PermutationQpLayout {
  // Names the test instance, e.g. "NicPairs211".
  std::string name;
  // QPs on NIC pair (n, n) of each rank pair, by sending rank and then n:
  // {{2, 2}, {2}, {2}} puts two QPs on each of NIC pairs (0, 0) and (1, 1)
  // from rank 0 to rank 1, and two on (0, 0) from rank 1 to rank 2 and from
  // rank 2 to rank 0.
  std::vector<std::vector<uint32_t>> qpsByNicPair;
};

/**
 * A permutation 0 -> 1 -> ... -> N-1 -> 0 whose rank pairs have different
 * numbers of data QPs, as when qpTemplates connect some rank pairs on more
 * NICs or with more QPs than others. The last QP of a NIC pair is reserved for
 * notifies unless it is the NIC pair's only QP. SetUp builds the config; a
 * test calls run().
 */
class PermutationUnequalQpCountTest
    : public packsim::WorkloadRankTestHarness,
      public ::testing::TestWithParam<PermutationQpLayout> {
 public:
  // Divisible by the data-QP count of every rank pair in layouts().
  static constexpr uint32_t kNumChunks = 12;

  /**
   * The layouts the suites run. In each, some rank receives on more data QPs
   * than it sends on.
   *
   * @return  The layouts, in instance order.
   */
  static std::vector<PermutationQpLayout> layouts() {
    return {
        // Rank 1 receives on 2 data QPs and sends on 1.
        {.name = "NicPairs211", .qpsByNicPair = {{2, 2}, {2}, {2}}},
        // Rank 1 receives on 3 and sends on 2; rank 2 receives on 2 and sends
        // on 1.
        {.name = "NicPairs321", .qpsByNicPair = {{2, 2, 2}, {2, 2}, {2}}},
        // Rank 1 receives on 2 and sends on 3, a count 2 does not divide;
        // rank 2 receives on 3 and sends on 1.
        {.name = "NicPairs231", .qpsByNicPair = {{2, 2}, {2, 2, 2}, {2}}},
        // More ranks than NICs. Rank 1 receives on 4 data QPs, three on NIC 0
        // and one on NIC 1, whose only QP carries data, and sends on 2 data
        // QPs, both on NIC 0.
        {.name = "FourRanksThreeDataQpsOnNic0OneOnNic1",
         .qpsByNicPair = {{4, 1}, {3}, {2}, {2}}},
    };
  }

  /**
   * Name a layout's test instance.
   *
   * @param info  The layout and its index.
   *
   * @return      The layout's name, e.g. "NicPairs211".
   */
  static std::string layoutName(
      const ::testing::TestParamInfo<PermutationQpLayout>& info) {
    return info.param.name;
  }

 protected:
  // Size of every chunk of a rank's transfer.
  static constexpr uint32_t kChunkSizeBytes = 1024;

  void SetUp() override {
    workCfg_.name() = "PERM_UNEQUAL_QPS";
    workCfg_.workloadId() = 0;
    workCfg_.numEndpoints() = numRanks();
    auto perm = packsim::workload::thrift::Permutation();
    perm.sizeBytes() = kChunkSizeBytes * kNumChunks;
    perm.numChunks() = kNumChunks;
    for (uint32_t rank = 0; rank < numRanks(); ++rank) {
      perm.dstRanks()->emplace_back((rank + 1) % numRanks());
    }
    customize(perm);
    workCfg_.config()->permutation() = perm;

    const auto& qpsByNicPair = GetParam().qpsByNicPair;
    size_t numNics = 0;
    for (const auto& nicPairs : qpsByNicPair) {
      numNics = std::max(numNics, nicPairs.size());
    }
    for (uint32_t rank = 0; rank < numRanks(); ++rank) {
      std::vector<std::vector<uint32_t>> endpointIds;
      endpointIds.reserve(numNics);
      for (uint32_t nic = 0; nic < numNics; ++nic) {
        endpointIds.emplace_back(std::vector<uint32_t>{rank, nic});
      }
      workCfg_.rankToIdList()->emplace(rank, std::move(endpointIds));
    }
    for (uint32_t srcRank = 0; srcRank < numRanks(); ++srcRank) {
      for (uint32_t nic = 0; nic < qpsByNicPair[srcRank].size(); ++nic) {
        addQps(srcRank, nic, qpsByNicPair[srcRank][nic]);
      }
    }
  }

  /**
   * Adjust the Permutation config before the ranks are built. The base leaves
   * it as SetUp built it.
   *
   * @param perm  The config the ranks will run.
   */
  virtual void customize(packsim::workload::thrift::Permutation& /*perm*/) {}

  void recvMessage(Message* msg) override {
    if (msg->connectionMessageId != kNotifyConnectionMessageId) {
      ++postedDataRecvsByNic_[msg->dstRank][msg->endpointId];
    }
    WorkloadRankTestHarness::recvMessage(msg);
  }

  // Ranks in the layout under test.
  uint32_t numRanks() const {
    return static_cast<uint32_t>(GetParam().qpsByNicPair.size());
  }

  /**
   * Spread kNumChunks data receives evenly over the data QPs of the rank pair
   * a rank receives on, and count them by the receiving NIC.
   *
   * @param rank  The receiving rank, e.g. 1 for rank pair 0 -> 1.
   *
   * @return      NIC -> data receives, e.g. {{0, 9}, {1, 3}} for 12 chunks
   *              over three data QPs on NIC 0 and one on NIC 1.
   */
  std::map<uint32_t, uint32_t> evenDataRecvsByNic(const uint32_t rank) const {
    const auto& incoming =
        GetParam().qpsByNicPair[(rank + numRanks() - 1) % numRanks()];
    uint32_t numDataQps = 0;
    for (const uint32_t qps : incoming) {
      numDataQps += dataQps(qps);
    }
    std::map<uint32_t, uint32_t> recvsByNic;
    for (uint32_t nic = 0; nic < incoming.size(); ++nic) {
      recvsByNic[nic] = kNumChunks / numDataQps * dataQps(incoming[nic]);
    }
    return recvsByNic;
  }

  // Data receives each rank posted, by the receiving NIC: rank -> NIC -> count.
  std::map<uint32_t, std::map<uint32_t, uint32_t>> postedDataRecvsByNic_;

 private:
  /**
   * Count the data QPs of a NIC pair.
   *
   * @param qps  The NIC pair's QPs, e.g. 3.
   *
   * @return     All but the one reserved for notifies, e.g. 2; 1 for a NIC
   *             pair with one QP.
   */
  static uint32_t dataQps(const uint32_t qps) {
    return qps > 1 ? qps - 1 : 1;
  }

  /**
   * Add QP templates on NIC pair (nic, nic) from a rank to the rank it sends
   * to.
   *
   * @param srcRank  The sending rank; it sends to srcRank + 1, modulo the rank
   *                 count.
   * @param nic      The NIC index at both ends.
   * @param qps      The templates to add, e.g. 2.
   */
  void addQps(const uint32_t srcRank, const uint32_t nic, const uint32_t qps) {
    for (uint32_t qp = 0; qp < qps; ++qp) {
      packsim::thrift::QueuePairTemplate qpTemplate;
      qpTemplate.ranks()->srcRank() = srcRank;
      qpTemplate.ranks()->dstRank() = (srcRank + 1) % numRanks();
      qpTemplate.nics()->srcNicIdx() = nic;
      qpTemplate.nics()->dstNicIdx() = nic;
      workCfg_.qpTemplates()->emplace_back(std::move(qpTemplate));
    }
  }
};

} // namespace packsim
