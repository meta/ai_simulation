/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "packsim/if/gen-cpp2/workload_types.h"

namespace packsim {

/**
 * Checks that the queue pair maps of a simulation list every rank pair
 * identically at its two ends: the sender's nicQpnPairMap entry for the pair
 * and the receiver's entry for the reverse pair hold the same NIC pairs in the
 * same order, each with the same queue pairs in the same order, every NIC pair
 * and QPN pair seen from the other end, and no queue pair twice. Then a flat
 * data-QP index, and the notify queue pair, name one queue pair at both ends.
 * Template expansion produces such maps from diagonal or order-preserving NIC
 * pairs, and so do legacy qpnPairMaps. The check is sufficient, not exact: two
 * ends that differ only in a queue pair no workload uses fail it.
 *
 * Example usage:
 *
 *      const auto listing = QpListing::check(workloads);
 *      if (!listing.isEveryRankPairListedIdentically) {
 *        const auto& mismatch = *listing.firstMismatch;
 *        ...
 *      }
 */
class QpListing {
 public:
  // A rank pair whose two ends are not listed identically.
  struct Mismatch {
    // The workload whose map holds it, e.g. "PERMUTATION_0".
    std::string workloadName;
    uint32_t srcRank{};
    uint32_t dstRank{};
  };

  // What check found.
  struct Result {
    bool isEveryRankPairListedIdentically{true};
    // The first rank pair, in workload and map order, that is not.
    std::optional<Mismatch> firstMismatch;
    // Rank pairs that are not, counting each pair of ranks once.
    uint32_t numMismatchedRankPairs{0};
  };

  /**
   * Check every rank pair of every workload's nicQpnPairMap. A batch child
   * routes through its top-level workload's map, so the top-level maps cover
   * it.
   *
   * @param workloads  The simulation's workloads, with nicQpnPairMap built.
   *
   * @return           What the check found.
   */
  static Result check(
      const std::vector<packsim::workload::thrift::Workload>& workloads);

 private:
  /**
   * Check one rank pair of one map.
   *
   * @param nicQpnPairMap  A workload's map.
   * @param srcRank        One rank of the pair, e.g. 0.
   * @param dstRank        The other, e.g. 1.
   *
   * @return               Whether both ends list the pair identically; false
   *                       when either end lists nothing for it.
   */
  static bool isListedIdenticallyAtBothEnds(
      const packsim::workload::thrift::NicQPNPairMapMap& nicQpnPairMap,
      uint32_t srcRank,
      uint32_t dstRank);
};

} // namespace packsim
