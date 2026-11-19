/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <folly/container/F14Map.h>

#include "packsim/if/gen-cpp2/workload_types.h"
#include "packsim/workloads/rank/workload-rank.h"

namespace packsim {

/**
 * One rank of a Dataflow workload (see Dataflow in if/workload.thrift). The
 * rank runs its DataflowProgram: each queue issues its operations in program
 * order, an operation is issued once its dependencies, its queue wait and any
 * fence ahead of it allow, and the rank completes when every operation has
 * completed and its start delay has passed. Queues that can issue at the same
 * moment issue in ascending order at the start; after an operation completes,
 * its own queue goes first, then the queues of the operations that depended on
 * it, in program order.
 */
class DataflowRank : public WorkloadRank {
 public:
  /**
   * @throw std::invalid_argument  If the workload is not a Dataflow workload,
   *                               is a Batch child, sets channelSettings, or
   *                               gives this rank no valid program: an
   *                               operation whose kind is UNKNOWN or outside
   *                               its enum, whose queueWait is outside its
   *                               enum, whose queue is not below the
   *                               number of operations, that depends on itself
   *                               or a later operation, names an invalid peer,
   *                               or a channel of 2047 or more or without a
   *                               queue pair that both ranks name, sends or
   *                               receives 0 bytes, or shares a (peer,
   *                               channel, direction) with another queue.
   */
  explicit DataflowRank(const WorkloadRankParams& params);
  ~DataflowRank() override;

  DataflowRank(const DataflowRank&) = delete;
  DataflowRank& operator=(const DataflowRank&) = delete;
  DataflowRank(DataflowRank&&) = delete;
  DataflowRank& operator=(DataflowRank&&) = delete;

  void run() override;
  uint64_t getExpectedBytes() override;
  bool complete() override;

  /**
   * Count the channels between every pair of ranks of a Dataflow workload.
   *
   * @param w  A Dataflow workload.
   *
   * @return   Per rank, each peer it exchanges messages with, mapped to one
   *           more than the highest channel either rank of the pair uses with
   *           the other, e.g. {1: 2} for rank 0 when the ranks 0 and 1 use
   *           channels 0 and 1 between them.
   * @throw std::invalid_argument  If a rank has no program, or a RECV or SEND
   *                               moves 0 bytes or names an invalid peer or
   *                               channel.
   */
  static std::vector<folly::F14FastMap<uint32_t, uint32_t>> getChannelCounts(
      const packsim::workload::thrift::Workload& w);

  /**
   * Check what the ranks of a Dataflow workload must agree on, before any rank
   * is built: on every channel, the SENDs one rank sends another and the RECVs
   * the other posts for them, each in program order, pair up one for one with
   * equal sizes. Also reject channelSettings.
   *
   * @param w  A Dataflow workload.
   *
   * @throw std::invalid_argument  If a channel's SENDs and RECVs differ in
   *                               number or size, the workload sets
   *                               channelSettings, a rank has no program, or a
   *                               RECV or SEND moves 0 bytes or names an
   *                               invalid peer or channel.
   */
  static void checkWorkload(const packsim::workload::thrift::Workload& w);

 private:
  struct QueueState {
    // The queue's operations, as indices into the program, in program order.
    std::vector<uint32_t> ops;
    // Position in ops of the next operation to issue.
    size_t next{0};
    // Operations issued but not completed, and the RECVs among them.
    uint32_t numInFlight{0};
    uint32_t numRecvsInFlight{0};
    // A fenced operation that has not completed.
    std::optional<uint32_t> fence;
    // Whether the queue waits in pendingQueues_ for tryIssue.
    bool isPending{false};
  };

  static const packsim::workload::thrift::DataflowProgram& getProgram(
      const packsim::workload::thrift::Workload& w,
      uint32_t rank);
  /**
   * Check a RECV or SEND of a rank's program and resolve its peer.
   *
   * @param w        The Dataflow workload.
   * @param program  The rank's program.
   * @param rank     The rank.
   * @param opIndex  Index of the RECV or SEND in program.ops.
   *
   * @return         The peer's rank.
   * @throw std::invalid_argument  If the operation moves 0 bytes, names a
   *                               channel of 2047 or more, or names a peer
   *                               that is not another rank of the workload.
   */
  static uint32_t resolveMessagePeer(
      const packsim::workload::thrift::Workload& w,
      const packsim::workload::thrift::DataflowProgram& program,
      uint32_t rank,
      uint32_t opIndex);

  void buildOps();
  void checkOp(uint32_t op) const;
  void addMessageOp(
      uint32_t op,
      folly::F14FastMap<uint64_t, uint32_t>& ownerQueues);
  void checkChannel(uint32_t peer, uint32_t channel) const;
  void buildDependents(const std::vector<uint32_t>& numDependents);
  void tryIssue(uint32_t queue);
  void issue(uint32_t op);
  void post(uint32_t op);
  uint32_t takeNextConnectionMessageId(
      uint32_t peer,
      uint32_t channel,
      packsim::workload::thrift::DataflowOpKind kind);
  void onMessageDone(uint32_t peer, const Message& msg);
  void completeOp(uint32_t op);
  void markPending(uint32_t queue);
  void drainPendingQueues();
  /**
   * Mark the rank complete and report it to its workload.
   *
   * @throw std::runtime_error  If the rank is already complete.
   */
  void finish();
  std::string describeBlockedQueues() const;

  const packsim::workload::thrift::DataflowProgram& program_;
  // Resolved peer of each RECV and SEND.
  std::vector<uint32_t> peers_;
  std::vector<uint32_t> remainingDeps_;
  // The operations that depend on operation i are
  // dependents_[dependentsBegin_[i], dependentsBegin_[i + 1]).
  std::vector<uint32_t> dependentsBegin_;
  std::vector<uint32_t> dependents_;
  // Whether each operation has completed.
  std::vector<bool> isDone_;
  std::vector<QueueState> queues_;
  // Queues to pass to tryIssue, in the order they were marked.
  std::deque<uint32_t> pendingQueues_;
  // Message of each issued RECV and SEND that has not completed.
  std::vector<std::unique_ptr<Message>> messages_;
  // Maps the (peer, connectionMessageId) of each message in messages_ to its
  // operation.
  folly::F14FastMap<uint64_t, uint32_t> opByMessage_;
  // Maps each (peer, channel, direction) to the number of messages posted on
  // it.
  folly::F14FastMap<uint64_t, uint32_t> messageCounts_;
  uint64_t expectedBytes_{0};
  size_t numDone_{0};
  bool startDelayElapsed_{false};
  bool complete_{false};
};

} // namespace packsim
