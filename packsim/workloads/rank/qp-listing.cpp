/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/workloads/rank/qp-listing.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/if/gen-cpp2/workload_types.h"

namespace packsim {

using packsim::workload::thrift::NicPairToQPNList;
using packsim::workload::thrift::NicQPNPairMapMap;
using packsim::workload::thrift::QPNPair;

namespace {
packsim::thrift::RankPair makeRankPair(
    const uint32_t srcRank,
    const uint32_t dstRank) {
  packsim::thrift::RankPair rankPair;
  rankPair.srcRank() = srcRank;
  rankPair.dstRank() = dstRank;
  return rankPair;
}

// Whether a rank pair's entry names a queue pair twice, by its local or its
// remote QPN.
bool hasRepeatedQpn(const NicPairToQPNList& qpnsByNicPair) {
  std::vector<uint32_t> localQpns;
  std::vector<uint32_t> remoteQpns;
  for (const auto& [_, qpnPairs] : qpnsByNicPair) {
    for (const auto& qpnPair : qpnPairs) {
      localQpns.emplace_back(*qpnPair.local());
      remoteQpns.emplace_back(*qpnPair.remote());
    }
  }
  const auto hasRepeat = [](std::vector<uint32_t>& qpns) {
    std::ranges::sort(qpns);
    return std::ranges::adjacent_find(qpns) != qpns.end();
  };
  return hasRepeat(localQpns) || hasRepeat(remoteQpns);
}
} // namespace

QpListing::Result QpListing::check(
    const std::vector<packsim::workload::thrift::Workload>& workloads) {
  Result result;
  for (const auto& workload : workloads) {
    const auto& nicQpnPairMap = *workload.nicQpnPairMap();
    for (const auto& [rankPair, _] : nicQpnPairMap) {
      const uint32_t srcRank = *rankPair.srcRank();
      const uint32_t dstRank = *rankPair.dstRank();
      // Count each pair of ranks once: from its lower rank, or from the only
      // end that lists it.
      if (srcRank > dstRank &&
          nicQpnPairMap.contains(makeRankPair(dstRank, srcRank))) {
        continue;
      }
      if (isListedIdenticallyAtBothEnds(nicQpnPairMap, srcRank, dstRank)) {
        continue;
      }
      result.isEveryRankPairListedIdentically = false;
      ++result.numMismatchedRankPairs;
      if (!result.firstMismatch.has_value()) {
        result.firstMismatch = Mismatch{
            .workloadName = *workload.name(),
            .srcRank = srcRank,
            .dstRank = dstRank};
      }
    }
  }
  return result;
}

bool QpListing::isListedIdenticallyAtBothEnds(
    const NicQPNPairMapMap& nicQpnPairMap,
    const uint32_t srcRank,
    const uint32_t dstRank) {
  const auto senderView = nicQpnPairMap.find(makeRankPair(srcRank, dstRank));
  const auto receiverView = nicQpnPairMap.find(makeRankPair(dstRank, srcRank));
  if (senderView == nicQpnPairMap.end() ||
      receiverView == nicQpnPairMap.end() ||
      hasRepeatedQpn(senderView->second)) {
    return false;
  }
  return std::ranges::equal(
      senderView->second,
      receiverView->second,
      [](const auto& sent, const auto& received) {
        const auto& [sentNics, sentQpns] = sent;
        const auto& [receivedNics, receivedQpns] = received;
        return *sentNics.srcNicIdx() == *receivedNics.dstNicIdx() &&
            *sentNics.dstNicIdx() == *receivedNics.srcNicIdx() &&
            std::ranges::equal(
                sentQpns,
                receivedQpns,
                [](const QPNPair& sentQpn, const QPNPair& receivedQpn) {
                  return *sentQpn.local() == *receivedQpn.remote() &&
                      *sentQpn.remote() == *receivedQpn.local();
                });
      });
}

} // namespace packsim
