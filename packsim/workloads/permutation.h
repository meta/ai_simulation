/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <folly/container/F14Map.h>

// @oss-disable
#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {

class PermutationRank : public WorkloadRank {
 public:
  explicit PermutationRank(const WorkloadRankParams& params);
  void run() override;
  virtual void requestReceived(Message* msg);
  virtual void requestAcked(Message* msg);
  uint64_t getExpectedBytes() override;
  bool complete() override;
  void progress() override;

 protected:
  uint64_t sendSize_;
  uint32_t numChunks_;
  uint32_t srcRank_;
  uint32_t dstRank_;
  // @oss-disable
  // Use maps keyed by chunkId to handle out-of-order message delivery
  // when using multiple QPs/NICs for round-robin distribution
  folly::F14FastMap<uint32_t, std::unique_ptr<Message>> sendMsgs_;
  folly::F14FastMap<uint32_t, std::unique_ptr<Message>> recvMsgs_;
  bool notifyRcvd_{false};

 private:
  void sendMsgs();
  void sendNotify();
  void validateDstRanks(const std::vector<uint32_t>& dstRanks);

  /**
   * Choose the data-QP index of each receive this rank posts. When every rank
   * pair of the simulation is listed identically at its two ends, they follow
   * the sender's round-robin over the incoming rank pair's data QPs, so each
   * chunk's receive and send meet on one QP; otherwise they keep the
   * round-robin over the outgoing pair's count.
   *
   * @param numMsgs  The receives to post, one per chunk.
   *
   * @return         One index per chunk, for recvWithQPIndex.
   */
  std::vector<uint32_t> getRecvQpIndices(uint32_t numMsgs) const;

  /**
   * Round-robin chunks over a rank pair's data QPs from index 0, as the
   * sender does.
   *
   * @param numQps   The data QPs to spread over; 0 puts every chunk on 0.
   * @param numMsgs  The chunks.
   *
   * @return         One index per chunk, e.g. {0, 1, 0, 1} for 2 QPs.
   */
  std::vector<uint32_t> spreadOverQps(uint32_t numQps, uint32_t numMsgs) const;
};

} // namespace packsim
