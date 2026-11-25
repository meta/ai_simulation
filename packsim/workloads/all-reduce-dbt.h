/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>

#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {

struct BinaryMessageTree {
  uint32_t root{};
  std::vector<uint32_t> parent;
  std::vector<std::vector<uint32_t>> children;
  std::vector<uint32_t> leaves;
};

const BinaryMessageTree& buildDoubleBinaryTree(uint32_t N, bool oddLeaves);

class AllReduceDbtRank : public WorkloadRank {
 public:
  explicit AllReduceDbtRank(const WorkloadRankParams& params);
  void run() override;
  void requestReceived(Message* msg);
  void requestAcked(Message* msg);
  uint64_t getExpectedBytes() override;
  bool complete() override;
  void progress() override;

 protected:
  void postNotifies();
  void postInitialSends();

  /**
   * Send a chunk up a tree to this rank's parent in it, then release the
   * down-tree chunks held for that peer once every up-tree chunk is out.
   *
   * @param parent     The parent, e.g. 1.
   * @param sizeBytes  The chunk's size.
   * @param chunkId    The chunk, odd for the odd tree and even for the even.
   */
  void sendUp(uint32_t parent, uint32_t sizeBytes, uint32_t chunkId);

  /**
   * Send a chunk down a tree to a child, or hold it while this rank still has
   * up-tree chunks to send that child. The child posts every up-tree receive
   * from this rank before any down-tree one, and the transport fills the
   * receives of a queue pair in the order they were posted, so a down-tree
   * chunk sent first would land in an up-tree chunk's receive.
   *
   * @param child      The child, e.g. 1.
   * @param sizeBytes  The chunk's size.
   * @param chunkId    The chunk, odd for the odd tree and even for the even.
   */
  void sendDown(uint32_t child, uint32_t sizeBytes, uint32_t chunkId);

  // Send a chunk to a peer and record it in the chunk's tree.
  void sendChunk(uint32_t peer, uint32_t sizeBytes, uint32_t chunkId);

  // A down-tree chunk sendDown holds for a child.
  struct HeldSend {
    uint32_t sizeBytes{};
    uint32_t chunkId{};
  };

  struct TreeInfo {
    bool amRoot{};
    uint32_t parent{};
    std::vector<uint32_t> children;
    folly::F14FastMap<uint32_t, std::deque<std::unique_ptr<Message>>> sends;
    folly::F14FastMap<uint32_t, std::deque<std::unique_ptr<Message>>> recvs;
    folly::F14FastMap<uint32_t, uint32_t> upChunkRecvCounts;
  };
  TreeInfo oddInfo_;
  TreeInfo evenInfo_;

  std::vector<uint32_t> chunkSizes_;
  folly::F14FastSet<uint32_t> allPeers_;
  folly::F14FastSet<uint32_t> notifyPending_;

  uint64_t completeSends_{0};
  uint64_t completeRecvs_{0};
  uint64_t expectedSends_{0};
  uint64_t expectedRecvs_{0};

  // Up-tree chunks this rank has yet to send to each parent.
  folly::F14FastMap<uint32_t, uint32_t> upSendsLeft_;
  // Down-tree chunks held for each child until upSendsLeft_ for it is 0.
  folly::F14FastMap<uint32_t, std::deque<HeldSend>> heldDownSends_;
};
} // namespace packsim
