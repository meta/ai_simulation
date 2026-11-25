/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/kernel/time.h"
#include "packsim/transport/transport.h"
#include "packsim/workloads/all-reduce-dbt.h"
#include "packsim/workloads/oss_tests/rank-harness.h"
#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {
namespace {

// Checks the structural invariants a double binary tree over `n` ranks must
// satisfy, independent of orientation: sizes, a single root with no parent, the
// expected branching factor, consistent parent<->child back-references, every
// non-root node referenced as a child exactly once, reachability of all nodes
// from the root, and a leaf set equal to the set of childless nodes.
void expectValidDoubleBinaryTree(const BinaryMessageTree& tree, uint32_t n) {
  ASSERT_EQ(tree.parent.size(), n);
  ASSERT_EQ(tree.children.size(), n);
  ASSERT_LT(tree.root, n);
  EXPECT_EQ(tree.parent.at(tree.root), UINT32_MAX);

  std::vector<uint32_t> childRefCount(n, 0);
  for (uint32_t parent = 0; parent < n; ++parent) {
    const auto& children = tree.children.at(parent);
    // The root fans out to a single child; interior nodes are binary; leaves
    // have none.
    if (parent == tree.root) {
      EXPECT_EQ(children.size(), 1u);
    } else if (!children.empty()) {
      EXPECT_EQ(children.size(), 2u);
    }
    for (uint32_t child : children) {
      ASSERT_LT(child, n);
      EXPECT_EQ(tree.parent.at(child), parent);
      childRefCount.at(child)++;
    }
  }
  for (uint32_t node = 0; node < n; ++node) {
    // The root is nobody's child; every other node has exactly one parent.
    EXPECT_EQ(childRefCount.at(node), node == tree.root ? 0u : 1u);
  }

  // In-degree constraints alone permit a detached cycle, so walk from the root
  // and require every node to be reachable.
  std::set<uint32_t> reachable;
  std::vector<uint32_t> stack{tree.root};
  while (!stack.empty()) {
    const uint32_t node = stack.back();
    stack.pop_back();
    if (!reachable.insert(node).second) {
      continue;
    }
    for (uint32_t child : tree.children.at(node)) {
      stack.emplace_back(child);
    }
  }
  EXPECT_EQ(reachable.size(), n);

  std::set<uint32_t> leafSet;
  for (uint32_t leaf : tree.leaves) {
    ASSERT_LT(leaf, n);
    leafSet.insert(leaf);
  }
  EXPECT_EQ(leafSet.size(), tree.leaves.size()); // leaves are distinct
  for (uint32_t node = 0; node < n; ++node) {
    EXPECT_EQ(leafSet.contains(node), tree.children.at(node).empty());
  }
}

TEST(BuildDoubleBinaryTreeTest, OddLeavesTreeForEightRanks) {
  const auto& tree = buildDoubleBinaryTree(8, /*oddLeaves=*/true);
  EXPECT_EQ(tree.root, 0u);
  EXPECT_EQ(tree.children.at(0), (std::vector<uint32_t>{4}));
  EXPECT_EQ(tree.children.at(4), (std::vector<uint32_t>{2, 6}));
  EXPECT_EQ(tree.children.at(2), (std::vector<uint32_t>{1, 3}));
  EXPECT_EQ(tree.children.at(6), (std::vector<uint32_t>{5, 7}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{1, 3, 5, 7}));
  EXPECT_EQ(tree.parent.at(4), 0u);
  EXPECT_EQ(tree.parent.at(2), 4u);
}

TEST(BuildDoubleBinaryTreeTest, EvenLeavesTreeForEightRanks) {
  const auto& tree = buildDoubleBinaryTree(8, /*oddLeaves=*/false);
  EXPECT_EQ(tree.root, 7u);
  EXPECT_EQ(tree.children.at(7), (std::vector<uint32_t>{3}));
  EXPECT_EQ(tree.children.at(3), (std::vector<uint32_t>{1, 5}));
  EXPECT_EQ(tree.children.at(1), (std::vector<uint32_t>{0, 2}));
  EXPECT_EQ(tree.children.at(5), (std::vector<uint32_t>{4, 6}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{0, 2, 4, 6}));
  EXPECT_EQ(tree.parent.at(7), UINT32_MAX);
}

TEST(BuildDoubleBinaryTreeTest, OddLeavesTreeForFourRanks) {
  const auto& tree = buildDoubleBinaryTree(4, /*oddLeaves=*/true);
  EXPECT_EQ(tree.root, 0u);
  EXPECT_EQ(tree.children.at(0), (std::vector<uint32_t>{2}));
  EXPECT_EQ(tree.children.at(2), (std::vector<uint32_t>{1, 3}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{1, 3}));
}

TEST(BuildDoubleBinaryTreeTest, EvenLeavesTreeForFourRanks) {
  const auto& tree = buildDoubleBinaryTree(4, /*oddLeaves=*/false);
  EXPECT_EQ(tree.root, 3u);
  EXPECT_EQ(tree.children.at(3), (std::vector<uint32_t>{1}));
  EXPECT_EQ(tree.children.at(1), (std::vector<uint32_t>{0, 2}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{0, 2}));
}

TEST(BuildDoubleBinaryTreeTest, TwoRankOddLeavesTreeIsRootAndSingleLeaf) {
  const auto& tree = buildDoubleBinaryTree(2, /*oddLeaves=*/true);
  EXPECT_EQ(tree.root, 0u);
  EXPECT_EQ(tree.children.at(0), (std::vector<uint32_t>{1}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{1}));
}

TEST(BuildDoubleBinaryTreeTest, TwoRankEvenLeavesTreeIsRootAndSingleLeaf) {
  const auto& tree = buildDoubleBinaryTree(2, /*oddLeaves=*/false);
  EXPECT_EQ(tree.root, 1u);
  EXPECT_EQ(tree.children.at(1), (std::vector<uint32_t>{0}));
  EXPECT_EQ(tree.leaves, (std::vector<uint32_t>{0}));
}

TEST(BuildDoubleBinaryTreeTest, OddLeavesStructureIsValidAcrossSizes) {
  for (uint32_t n : {2u, 4u, 8u, 16u}) {
    SCOPED_TRACE(n);
    expectValidDoubleBinaryTree(
        buildDoubleBinaryTree(n, /*oddLeaves=*/true), n);
  }
}

TEST(BuildDoubleBinaryTreeTest, EvenLeavesStructureIsValidAcrossSizes) {
  for (uint32_t n : {2u, 4u, 8u, 16u}) {
    SCOPED_TRACE(n);
    expectValidDoubleBinaryTree(
        buildDoubleBinaryTree(n, /*oddLeaves=*/false), n);
  }
}

TEST(BuildDoubleBinaryTreeTest, OddAndEvenLeavesAreMirroredRoots) {
  // The two orientations root at opposite ends so their union spans both
  // reduction directions.
  const auto& oddTree = buildDoubleBinaryTree(8, /*oddLeaves=*/true);
  const auto& evenTree = buildDoubleBinaryTree(8, /*oddLeaves=*/false);
  EXPECT_EQ(oddTree.root, 0u);
  EXPECT_EQ(evenTree.root, 7u);
}

TEST(BuildDoubleBinaryTreeTest, ReturnsCachedInstanceForRepeatedArgs) {
  const auto& first = buildDoubleBinaryTree(8, /*oddLeaves=*/true);
  const auto& second = buildDoubleBinaryTree(8, /*oddLeaves=*/true);
  EXPECT_EQ(&first, &second);
  // Different arguments memoize to distinct instances.
  const auto& other = buildDoubleBinaryTree(8, /*oddLeaves=*/false);
  EXPECT_NE(&first, &other);
}

TEST(BuildDoubleBinaryTreeTest, OddAndEvenLeavesPartitionAllRanks) {
  // The defining double-binary-tree property: the two orientations' leaf sets
  // are disjoint and together cover every rank, so each rank is a leaf in
  // exactly one of the two trees.
  for (uint32_t n : {2u, 4u, 8u, 16u}) {
    SCOPED_TRACE(n);
    const auto& oddTree = buildDoubleBinaryTree(n, /*oddLeaves=*/true);
    const auto& evenTree = buildDoubleBinaryTree(n, /*oddLeaves=*/false);
    const std::set<uint32_t> oddLeaves(
        oddTree.leaves.begin(), oddTree.leaves.end());
    const std::set<uint32_t> evenLeaves(
        evenTree.leaves.begin(), evenTree.leaves.end());
    for (uint32_t node = 0; node < n; ++node) {
      EXPECT_NE(oddLeaves.contains(node), evenLeaves.contains(node));
    }
    EXPECT_EQ(oddLeaves.size() + evenLeaves.size(), n);
  }
}

/**
 * A double binary tree all-reduce run through the rank harness, which checks
 * that every message lands in the receive posted for its chunk on its queue
 * pair. A test sets the rank count and start delays, then calls run().
 */
class AllReduceDbtOrderTest : public packsim::WorkloadRankTestHarness,
                              public ::testing::Test {
 protected:
  // Size of each tree's chunks: 2 KiB, so a 2-chunk tree moves 4 KiB.
  static constexpr uint32_t kChunkSizeBytes = 2048;

  /**
   * Configure the all-reduce.
   *
   * @param numRanks   Ranks in the trees, a power of 2, e.g. 4.
   * @param numChunks  Chunks per tree, e.g. 2.
   */
  void configure(const uint32_t numRanks, const uint32_t numChunks) {
    workCfg_.name() = "AR_DBT_ORDER";
    workCfg_.workloadId() = 0;
    workCfg_.numEndpoints() = numRanks;
    auto ar = packsim::workload::thrift::AllReduce();
    ar.sizeBytes() = 2 * kChunkSizeBytes * numChunks;
    ar.numChunks() = numChunks;
    ar.algorithm() =
        packsim::workload::thrift::AllReduceAlgorithm::DOUBLE_BINARY_TREE;
    workCfg_.config()->allReduce() = ar;
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

  // How long the network holds the first notify from heldNotifySrc_ to
  // heldNotifyDst_.
  static constexpr uint64_t kNotifyHoldNs = 10'000;
  // Ranks whose first notify the network holds; none by default.
  uint32_t heldNotifySrc_{UINT32_MAX};
  uint32_t heldNotifyDst_{UINT32_MAX};
  bool isNotifyHeld_{false};
};

// Rank 1 starts 100 us late, so rank 0 sends its even-tree chunks to it only
// then. Rank 1 sends its odd-tree chunks at once, and rank 0, the odd root,
// sends them back down. Rank 1 posted the receives for rank 0's even-tree
// chunks first, so the odd-tree chunks wait behind them.
TEST_F(AllReduceDbtOrderTest, DownTreeChunksWaitForTheUpTreeChunksOfALateRank) {
  configure(/*numRanks=*/2, /*numChunks=*/2);
  workCfg_.explicitStaggeredStartTimeNs() = {0, 100'000};

  ASSERT_NO_THROW(run());

  validateStats();
  EXPECT_GE(*ranks_.at(1)->getThriftResult().endTimeNs(), 100'000);
}

// The network holds rank 3's notify to rank 1, so rank 1 sends its odd-tree
// chunks to rank 2 late. Rank 1 gets the even tree's reduction from rank 3 on
// time and forwards it to rank 2, which posted the receives for rank 1's
// odd-tree chunks first.
TEST_F(AllReduceDbtOrderTest, DownTreeChunksWaitForUpTreeChunksHeldByANotify) {
  configure(/*numRanks=*/4, /*numChunks=*/4);
  heldNotifySrc_ = 3;
  heldNotifyDst_ = 1;

  ASSERT_NO_THROW(run());

  validateStats();
  EXPECT_TRUE(isNotifyHeld_);
}

} // namespace
} // namespace packsim
