/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/transport/transport.h"
#include "packsim/workloads/oss_tests/rank-harness.h"
#include "packsim/workloads/rank/workload-rank.h"

using packsim::workload::thrift::AllReduceAlgorithm;

namespace packsim {

namespace {
// A power of two, which the tree and recursive algorithms need.
constexpr uint32_t kNumRanks = 4;
// The NICs with QPs, each in NIC pair (nic, nic). NIC 0 has none, as when it
// cables to a rack-local switch that cannot reach the peers.
constexpr uint32_t kFirstQpNicIdx = 1;
constexpr uint32_t kSecondQpNicIdx = 2;

// (endpointId, dstEndpointId) of a message: its own NIC and its peer's.
using NicPairs = std::set<std::pair<uint32_t, uint32_t>>;

// Add the NIC pair of `msg` to `nicPairs` unless `msg` is a notify.
void addDataNicPair(const Message& msg, NicPairs& nicPairs) {
  if (msg.connectionMessageId != kNotifyConnectionMessageId) {
    nicPairs.emplace(msg.endpointId, msg.dstEndpointId);
  }
}
} // namespace

/**
 * An all-reduce whose algorithm sends its data with send() and recv(), on the
 * rank pair's first data QP.
 */
class WorkloadRankDataNicTest
    : public WorkloadRankTestHarness,
      public ::testing::TestWithParam<AllReduceAlgorithm> {
 protected:
  void SetUp() override {
    workCfg_.name() = "DATA_NIC_TEST";
    workCfg_.workloadId() = 0;
    workCfg_.numEndpoints() = kNumRanks;

    auto ar = packsim::workload::thrift::AllReduce();
    ar.sizeBytes() = 4096;
    ar.algorithm() = GetParam();
    workCfg_.config()->allReduce() = ar;

    // Three NIC addresses per rank, and a QP template for every ordered rank
    // pair on each NIC with QPs, so that each rank pair has two QPs, a data QP
    // and a notify QP, on each of its two NIC pairs.
    for (uint32_t rank = 0; rank < kNumRanks; ++rank) {
      workCfg_.rankToIdList()->emplace(
          rank,
          std::vector<std::vector<uint32_t>>{{rank, 1}, {rank, 2}, {rank, 3}});
      for (uint32_t peer = 0; peer < kNumRanks; ++peer) {
        if (peer == rank) {
          continue;
        }
        for (const uint32_t nicIdx : {kFirstQpNicIdx, kSecondQpNicIdx}) {
          packsim::thrift::QueuePairTemplate qpTemplate;
          qpTemplate.ranks()->srcRank() = rank;
          qpTemplate.ranks()->dstRank() = peer;
          qpTemplate.nics()->srcNicIdx() = nicIdx;
          qpTemplate.nics()->dstNicIdx() = nicIdx;
          workCfg_.qpTemplates()->emplace_back(std::move(qpTemplate));
        }
      }
    }
    run();
  }

  void sendMessage(Message* msg) override {
    addDataNicPair(*msg, sentDataNicPairs_);
    WorkloadRankTestHarness::sendMessage(msg);
  }

  void recvMessage(Message* msg) override {
    addDataNicPair(*msg, postedDataRecvNicPairs_);
    WorkloadRankTestHarness::recvMessage(msg);
  }

  // NIC pairs of every data message sent and of every data receive posted.
  NicPairs sentDataNicPairs_;
  NicPairs postedDataRecvNicPairs_;
};

TEST_P(WorkloadRankDataNicTest, EveryDataMessageTravelsOnTheFirstNicPair) {
  const NicPairs firstNicPair{{kFirstQpNicIdx, kFirstQpNicIdx}};
  EXPECT_EQ(sentDataNicPairs_, firstNicPair);
  EXPECT_EQ(postedDataRecvNicPairs_, firstNicPair);
}

INSTANTIATE_TEST_SUITE_P(
    AllReduceAlgorithms,
    WorkloadRankDataNicTest,
    ::testing::Values(
        AllReduceAlgorithm::RING,
        AllReduceAlgorithm::DOUBLE_BINARY_TREE,
        AllReduceAlgorithm::RECURSIVE_MSG_OPT),
    [](const ::testing::TestParamInfo<AllReduceAlgorithm>& info)
        -> std::string {
      return apache::thrift::util::enumNameSafe(info.param);
    });

} // namespace packsim
