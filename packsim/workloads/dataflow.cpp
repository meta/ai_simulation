/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/workloads/dataflow.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <folly/Conv.h>
#include <folly/Utility.h>
#include <folly/logging/xlog.h>

#include "packsim/if/gen-cpp2/qp_pair_config_types.h"
#include "packsim/kernel/time.h"

using packsim::workload::thrift::DataflowOp;
using packsim::workload::thrift::DataflowOpKind;
using packsim::workload::thrift::DataflowPeerMode;
using packsim::workload::thrift::DataflowProgram;
using packsim::workload::thrift::DataflowQueueWait;
using packsim::workload::thrift::QPNPair;
using packsim::workload::thrift::Workload;
using packsim::workload::thrift::WorkloadSpecificConfig;

namespace packsim {

namespace {

// A connectionMessageId packs (channel, direction, message index) as
// channel:11 | direction:1 | index:20, so that both ranks of a message compute
// the same id without exchanging anything. Channels stop below 2^11 - 1 so that
// no id reaches kNotifyConnectionMessageId (all ones).
constexpr uint32_t kMessageIndexBits = 20;
constexpr uint32_t kMessageIndexMask = (uint32_t{1} << kMessageIndexBits) - 1;
constexpr uint32_t kMaxChannels = (uint32_t{1} << 11) - 1;
// At most this many blocked queues are described when an incomplete rank is
// destroyed.
constexpr size_t kMaxBlockedQueuesReported = 8;

[[nodiscard]] std::invalid_argument makeInvalidOpError(
    const Workload& w,
    const uint32_t rank,
    const uint32_t op,
    const std::string& why) {
  return std::invalid_argument(
      fmt::format(
          "Workload {}: rank {} operation {}: {}", *w.name(), rank, op, why));
}

bool isKnownKind(const DataflowOpKind kind) noexcept {
  switch (kind) {
    case DataflowOpKind::JOIN:
    case DataflowOpKind::RECV:
    case DataflowOpKind::SEND:
    case DataflowOpKind::COMPUTE:
      return true;
    case DataflowOpKind::UNKNOWN:
      return false;
  }
  return false;
}

bool isKnownQueueWait(const DataflowQueueWait queueWait) noexcept {
  switch (queueWait) {
    case DataflowQueueWait::NONE:
    case DataflowQueueWait::ALL:
    case DataflowQueueWait::RECVS:
      return true;
  }
  return false;
}

bool isMessageOp(const DataflowOp& op) {
  return *op.kind() == DataflowOpKind::RECV ||
      *op.kind() == DataflowOpKind::SEND;
}

uint64_t makePeerKey(const uint32_t peer, const uint32_t value) noexcept {
  return (uint64_t{peer} << 32) | value;
}

// Key of a (peer, channel, direction) in the per-rank maps.
uint64_t makeChannelKey(
    const uint32_t peer,
    const uint32_t channel,
    const DataflowOpKind kind) noexcept {
  return makePeerKey(
      peer, (channel << 1) | (kind == DataflowOpKind::SEND ? 1 : 0));
}

void rejectChannelSettings(const Workload& w) {
  if (w.channelSettings().has_value()) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {}: a Dataflow workload cannot set channelSettings; its "
            "operations name their channels",
            *w.name()));
  }
}

// The SEND or the RECV of one message, as the program that posts it lists it.
struct MessageEnd {
  uint32_t src{0};
  uint32_t dst{0};
  uint32_t channel{0};
  uint32_t sizeBytes{0};
  // Index of the SEND or RECV in the program of the rank that posts it.
  uint32_t op{0};
};

bool isSameChannel(const MessageEnd& a, const MessageEnd& b) noexcept {
  return std::tie(a.src, a.dst, a.channel) == std::tie(b.src, b.dst, b.channel);
}

bool isChannelLess(const MessageEnd& a, const MessageEnd& b) noexcept {
  return std::tie(a.src, a.dst, a.channel) < std::tie(b.src, b.dst, b.channel);
}

} // namespace

DataflowRank::DataflowRank(const WorkloadRankParams& params)
    : WorkloadRank(params), program_(getProgram(*workCfg_, rank_)) {
  this->setOnRequestAckedCallback(
      [this](Message* msg) { onMessageDone(msg->dstRank, *msg); });
  this->setOnRequestReceivedCallback(
      [this](Message* msg) { onMessageDone(msg->srcRank, *msg); });
  if (parent_ != nullptr) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {}: a Dataflow workload cannot be a Batch child",
            workloadName_));
  }
  rejectChannelSettings(*workCfg_);
  buildOps();
}

DataflowRank::~DataflowRank() {
  if (!complete_) {
    XLOGF(
        WARN,
        "Workload {} rank {} ended with {} of {} operations complete.{}",
        workloadName_,
        rank_,
        numDone_,
        isDone_.size(),
        describeBlockedQueues());
  }
}

const DataflowProgram& DataflowRank::getProgram(
    const Workload& w,
    const uint32_t rank) {
  if (w.config()->getType() != WorkloadSpecificConfig::Type::dataflow) {
    throw std::invalid_argument(
        fmt::format("Workload {} is not a Dataflow workload", *w.name()));
  }
  const auto& dataflow = *w.config()->dataflow();
  const auto& rankPrograms = *dataflow.rankPrograms();
  if (rankPrograms.size() != *w.numEndpoints()) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {}: Dataflow rankPrograms has {} entries for {} ranks",
            *w.name(),
            rankPrograms.size(),
            *w.numEndpoints()));
  }
  const uint32_t index = rankPrograms.at(rank);
  if (index >= dataflow.programs()->size()) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {}: rank {} runs program {}, but there are {} programs",
            *w.name(),
            rank,
            index,
            dataflow.programs()->size()));
  }
  return dataflow.programs()->at(index);
}

uint32_t DataflowRank::resolveMessagePeer(
    const Workload& w,
    const DataflowProgram& program,
    const uint32_t rank,
    const uint32_t opIndex) {
  const auto& op = program.ops()->at(opIndex);
  if (*op.sizeBytes() == 0) {
    throw makeInvalidOpError(
        w, rank, opIndex, "a RECV or SEND moves at least 1 byte");
  }
  if (*op.channel() >= kMaxChannels) {
    throw makeInvalidOpError(
        w,
        rank,
        opIndex,
        fmt::format("channel {} is not below {}", *op.channel(), kMaxChannels));
  }
  const uint32_t numRanks = *w.numEndpoints();
  const uint32_t offset = *op.peer();
  const uint32_t peer = [&]() -> uint32_t {
    switch (*op.peerMode()) {
      case DataflowPeerMode::ABSOLUTE:
        return offset;
      case DataflowPeerMode::RELATIVE:
        if (offset >= numRanks) {
          throw makeInvalidOpError(
              w,
              rank,
              opIndex,
              fmt::format(
                  "RELATIVE peer {} is not below the {} ranks",
                  offset,
                  numRanks));
        }
        return (rank + offset) % numRanks;
      case DataflowPeerMode::GROUP_RELATIVE: {
        const uint32_t groupSize = *program.groupSize();
        if (groupSize == 0 || numRanks % groupSize != 0) {
          throw makeInvalidOpError(
              w,
              rank,
              opIndex,
              fmt::format(
                  "GROUP_RELATIVE needs a groupSize that divides the {} "
                  "ranks, got {}",
                  numRanks,
                  groupSize));
        }
        if (offset >= groupSize) {
          throw makeInvalidOpError(
              w,
              rank,
              opIndex,
              fmt::format(
                  "GROUP_RELATIVE peer {} is not below groupSize {}",
                  offset,
                  groupSize));
        }
        const uint32_t base = rank - rank % groupSize;
        return base + (rank - base + offset) % groupSize;
      }
    }
    // Also reached by a value outside the enum.
    throw makeInvalidOpError(
        w,
        rank,
        opIndex,
        fmt::format(
            "unknown peerMode {}", folly::to_underlying(*op.peerMode())));
  }();
  if (peer >= numRanks) {
    throw makeInvalidOpError(
        w,
        rank,
        opIndex,
        fmt::format("peer {} is not below the {} ranks", peer, numRanks));
  }
  if (peer == rank) {
    throw makeInvalidOpError(w, rank, opIndex, "its peer is the rank itself");
  }
  return peer;
}

std::vector<folly::F14FastMap<uint32_t, uint32_t>>
DataflowRank::getChannelCounts(const Workload& w) {
  const uint32_t numRanks = *w.numEndpoints();
  std::vector<folly::F14FastMap<uint32_t, uint32_t>> counts(numRanks);
  for (uint32_t rank = 0; rank < numRanks; ++rank) {
    const auto& program = getProgram(w, rank);
    const auto numOps = folly::to<uint32_t>(program.ops()->size());
    for (uint32_t i = 0; i < numOps; ++i) {
      const auto& op = program.ops()->at(i);
      if (!isMessageOp(op)) {
        continue;
      }
      const uint32_t peer = resolveMessagePeer(w, program, rank, i);
      const uint32_t numChannels = *op.channel() + 1;
      auto& forward = counts[rank][peer];
      forward = std::max(forward, numChannels);
      auto& backward = counts[peer][rank];
      backward = std::max(backward, numChannels);
    }
  }
  return counts;
}

void DataflowRank::checkWorkload(const Workload& w) {
  rejectChannelSettings(w);
  std::vector<MessageEnd> sends;
  std::vector<MessageEnd> recvs;
  const uint32_t numRanks = *w.numEndpoints();
  for (uint32_t rank = 0; rank < numRanks; ++rank) {
    const auto& program = getProgram(w, rank);
    const auto numOps = folly::to<uint32_t>(program.ops()->size());
    for (uint32_t i = 0; i < numOps; ++i) {
      const auto& op = program.ops()->at(i);
      if (!isMessageOp(op)) {
        continue;
      }
      const uint32_t peer = resolveMessagePeer(w, program, rank, i);
      const bool isSend = *op.kind() == DataflowOpKind::SEND;
      const MessageEnd end{
          .src = isSend ? rank : peer,
          .dst = isSend ? peer : rank,
          .channel = *op.channel(),
          .sizeBytes = *op.sizeBytes(),
          .op = i};
      (isSend ? sends : recvs).emplace_back(end);
    }
  }
  // A stable sort keeps the messages of each channel in program order, the
  // order in which the k-th SEND meets the k-th RECV.
  std::ranges::stable_sort(sends, isChannelLess);
  std::ranges::stable_sort(recvs, isChannelLess);
  const auto [send, recv] = std::ranges::mismatch(
      sends, recvs, [](const MessageEnd& s, const MessageEnd& r) {
        return isSameChannel(s, r) && s.sizeBytes == r.sizeBytes;
      });
  if (send == sends.end() && recv == recvs.end()) {
    return;
  }
  if (send != sends.end() && recv != recvs.end() &&
      isSameChannel(*send, *recv)) {
    throw std::invalid_argument(
        fmt::format(
            "Workload {}: rank {} operation {} sends {} bytes to rank {} on "
            "channel {}, but the RECV that matches it, rank {} operation {}, "
            "receives {} bytes",
            *w.name(),
            send->src,
            send->op,
            send->sizeBytes,
            send->dst,
            send->channel,
            recv->dst,
            recv->op,
            recv->sizeBytes));
  }
  // Up to the mismatch both lists agree, so the channel that comes first there
  // has more messages on one side than on the other.
  const bool sendComesFirst = recv == recvs.end() ||
      (send != sends.end() && isChannelLess(*send, *recv));
  const MessageEnd& unmatched = sendComesFirst ? *send : *recv;
  const auto numOnChannel = [&](const std::vector<MessageEnd>& ends) {
    return std::ranges::equal_range(ends, unmatched, isChannelLess).size();
  };
  throw std::invalid_argument(
      fmt::format(
          "Workload {}: on channel {} from rank {} to rank {}, the number of "
          "SENDs ({}) differs from the number of RECVs ({})",
          *w.name(),
          unmatched.channel,
          unmatched.src,
          unmatched.dst,
          numOnChannel(sends),
          numOnChannel(recvs)));
}

void DataflowRank::buildOps() {
  const auto& ops = *program_.ops();
  const auto numOps = folly::to<uint32_t>(ops.size());
  peers_.assign(numOps, 0);
  remainingDeps_.assign(numOps, 0);
  isDone_.assign(numOps, false);
  messages_.resize(numOps);
  std::vector<uint32_t> numDependents(numOps, 0);
  // Maps each (peer, channel, direction) to the queue whose operations use it.
  folly::F14FastMap<uint64_t, uint32_t> ownerQueues;
  uint32_t numQueues = 0;
  for (uint32_t i = 0; i < numOps; ++i) {
    checkOp(i);
    numQueues = std::max(numQueues, *ops[i].queue() + 1);
    for (const uint32_t dep : *ops[i].deps()) {
      ++numDependents[dep];
    }
    remainingDeps_[i] = folly::to<uint32_t>(ops[i].deps()->size());
    if (isMessageOp(ops[i])) {
      addMessageOp(i, ownerQueues);
    }
  }
  buildDependents(numDependents);
  queues_.resize(numQueues);
  for (uint32_t i = 0; i < numOps; ++i) {
    queues_[*ops[i].queue()].ops.emplace_back(i);
  }
}

void DataflowRank::checkOp(const uint32_t op) const {
  const auto& cfg = program_.ops()->at(op);
  const size_t numOps = program_.ops()->size();
  if (*cfg.kind() == DataflowOpKind::UNKNOWN) {
    throw makeInvalidOpError(*workCfg_, rank_, op, "its kind is not set");
  }
  if (!isKnownKind(*cfg.kind())) {
    throw makeInvalidOpError(
        *workCfg_,
        rank_,
        op,
        fmt::format("unknown kind {}", folly::to_underlying(*cfg.kind())));
  }
  if (!isKnownQueueWait(*cfg.queueWait())) {
    throw makeInvalidOpError(
        *workCfg_,
        rank_,
        op,
        fmt::format(
            "unknown queueWait {}", folly::to_underlying(*cfg.queueWait())));
  }
  if (*cfg.queue() >= numOps) {
    throw makeInvalidOpError(
        *workCfg_,
        rank_,
        op,
        fmt::format(
            "queue {} is not below the program's {} operations",
            *cfg.queue(),
            numOps));
  }
  for (const uint32_t dep : *cfg.deps()) {
    if (dep >= op) {
      throw makeInvalidOpError(
          *workCfg_,
          rank_,
          op,
          fmt::format(
              "it depends on operation {}, which does not come before it",
              dep));
    }
  }
}

void DataflowRank::addMessageOp(
    const uint32_t op,
    folly::F14FastMap<uint64_t, uint32_t>& ownerQueues) {
  const auto& cfg = program_.ops()->at(op);
  const uint32_t peer = resolveMessagePeer(*workCfg_, program_, rank_, op);
  peers_[op] = peer;
  const auto [it, inserted] = ownerQueues.try_emplace(
      makeChannelKey(peer, *cfg.channel(), *cfg.kind()), *cfg.queue());
  if (inserted) {
    checkChannel(peer, *cfg.channel());
  } else if (it->second != *cfg.queue()) {
    throw makeInvalidOpError(
        *workCfg_,
        rank_,
        op,
        fmt::format(
            "queues {} and {} both {} rank {} on channel {}; one queue must "
            "own each (peer, channel, direction)",
            it->second,
            *cfg.queue(),
            *cfg.kind() == DataflowOpKind::SEND ? "send to" : "receive from",
            peer,
            *cfg.channel()));
  }
  if (*cfg.kind() == DataflowOpKind::RECV) {
    expectedBytes_ += *cfg.sizeBytes();
  }
}

void DataflowRank::checkChannel(const uint32_t peer, const uint32_t channel)
    const {
  const auto fail = [&](const std::string& why) {
    return std::invalid_argument(
        fmt::format(
            "Workload {}: channel {} between ranks {} and {}: {}",
            workloadName_,
            channel,
            rank_,
            peer,
            why));
  };
  if (!isNicAwareModeActive()) {
    throw fail("a Dataflow workload needs a nicQpnPairMap");
  }
  const uint32_t numQps = getTotalDataQPCount(peer);
  if (channel >= numQps) {
    throw fail(
        fmt::format("the rank pair has only {} data queue pairs", numQps));
  }
  uint32_t srcNicIdx = 0;
  uint32_t dstNicIdx = 0;
  const auto& mine =
      getDataQPNByIndexWithNic(peer, channel, srcNicIdx, dstNicIdx);
  // The peer lists its data queue pairs the same way: it walks its NIC pairs
  // in key order and takes its channel-th one.
  packsim::thrift::RankPair reverse;
  reverse.srcRank() = peer;
  reverse.dstRank() = rank_;
  const auto it = workCfg_->nicQpnPairMap()->find(reverse);
  if (it == workCfg_->nicQpnPairMap()->end()) {
    throw fail("the peer has no queue pairs to this rank");
  }
  uint32_t index = channel;
  const QPNPair* theirs = nullptr;
  for (const auto& [nicPair, qpns] : it->second) {
    const uint32_t numData = getDataQPCountFromTotal(qpns.size());
    if (index < numData) {
      theirs = &qpns.at(index);
      break;
    }
    index -= numData;
  }
  if (theirs == nullptr || *theirs->local() != *mine.remote() ||
      *theirs->remote() != *mine.local()) {
    throw fail("the two ranks name different queue pairs");
  }
}

void DataflowRank::buildDependents(const std::vector<uint32_t>& numDependents) {
  const auto& ops = *program_.ops();
  const auto numOps = folly::to<uint32_t>(ops.size());
  dependentsBegin_.assign(numOps + 1, 0);
  for (uint32_t i = 0; i < numOps; ++i) {
    dependentsBegin_[i + 1] = dependentsBegin_[i] + numDependents[i];
  }
  dependents_.resize(dependentsBegin_[numOps]);
  std::vector<uint32_t> next(dependentsBegin_.begin(), dependentsBegin_.end());
  for (uint32_t i = 0; i < numOps; ++i) {
    for (const uint32_t dep : *ops[i].deps()) {
      dependents_[next.at(dep)++] = i;
    }
  }
}

void DataflowRank::run() {
  traceEvent_ = simCtx_->tracer.startEvent(
      "DataflowRank", "Workloads", workloadId_, rank_);
  // Receives are posted at once; every other kind waits for the start delay.
  for (uint32_t queue = 0; queue < queues_.size(); ++queue) {
    markPending(queue);
  }
  drainPendingQueues();
  runAfterStartDelay([this]() {
    startDelayElapsed_ = true;
    startTimeNs_ = getTimeNs();
    for (uint32_t queue = 0; queue < queues_.size(); ++queue) {
      markPending(queue);
    }
    drainPendingQueues();
    if (!complete_ && numDone_ == isDone_.size()) {
      finish();
    }
  });
}

void DataflowRank::markPending(const uint32_t queue) {
  auto& state = queues_[queue];
  if (!state.isPending) {
    state.isPending = true;
    pendingQueues_.emplace_back(queue);
  }
}

void DataflowRank::drainPendingQueues() {
  while (!pendingQueues_.empty()) {
    const uint32_t queue = pendingQueues_.front();
    pendingQueues_.pop_front();
    queues_[queue].isPending = false;
    tryIssue(queue);
  }
}

void DataflowRank::tryIssue(const uint32_t queue) {
  auto& state = queues_[queue];
  const auto& ops = *program_.ops();
  while (state.next < state.ops.size() && !state.fence.has_value()) {
    const uint32_t op = state.ops[state.next];
    const auto& cfg = ops[op];
    if (remainingDeps_[op] > 0 ||
        (*cfg.queueWait() == DataflowQueueWait::ALL && state.numInFlight > 0) ||
        (*cfg.queueWait() == DataflowQueueWait::RECVS &&
         state.numRecvsInFlight > 0) ||
        (*cfg.kind() != DataflowOpKind::RECV && !startDelayElapsed_)) {
      return;
    }
    ++state.next;
    if (*cfg.fence()) {
      state.fence = op;
    }
    issue(op);
  }
}

void DataflowRank::issue(const uint32_t op) {
  const auto& cfg = program_.ops()->at(op);
  auto& queue = queues_[*cfg.queue()];
  ++queue.numInFlight;
  switch (*cfg.kind()) {
    case DataflowOpKind::RECV:
      ++queue.numRecvsInFlight;
      post(op);
      return;
    case DataflowOpKind::SEND:
      post(op);
      return;
    case DataflowOpKind::COMPUTE:
      if (*cfg.durationNs() > 0) {
        simCtx_->schedule(Time::fromNs(*cfg.durationNs()), [this, op]() {
          completeOp(op);
          drainPendingQueues();
        });
        return;
      }
      completeOp(op);
      return;
    case DataflowOpKind::JOIN:
      completeOp(op);
      return;
    case DataflowOpKind::UNKNOWN:
      break;
  }
  // checkOp rejects any other kind when the rank is built.
  throw std::runtime_error(
      fmt::format(
          "Workload {} rank {}: operation {} has unknown kind {}",
          workloadName_,
          rank_,
          op,
          folly::to_underlying(*cfg.kind())));
}

void DataflowRank::post(const uint32_t op) {
  const auto& cfg = program_.ops()->at(op);
  const uint32_t peer = peers_[op];
  const uint32_t id =
      takeNextConnectionMessageId(peer, *cfg.channel(), *cfg.kind());
  const auto [it, inserted] =
      opByMessage_.try_emplace(makePeerKey(peer, id), op);
  if (!inserted) {
    throw std::runtime_error(
        fmt::format(
            "Workload {} rank {}: operations {} and {} both wait for message "
            "{} with rank {}; a channel carries at most 2^20 messages in "
            "flight in each direction",
            workloadName_,
            rank_,
            it->second,
            op,
            id,
            peer));
  }
  messages_[op] = *cfg.kind() == DataflowOpKind::SEND
      ? sendWithQPIndex(peer, *cfg.sizeBytes(), id, *cfg.channel())
      : recvWithQPIndex(peer, *cfg.sizeBytes(), id, *cfg.channel());
}

uint32_t DataflowRank::takeNextConnectionMessageId(
    const uint32_t peer,
    const uint32_t channel,
    const DataflowOpKind kind) {
  const uint32_t index = messageCounts_[makeChannelKey(peer, channel, kind)]++;
  const bool isSend = kind == DataflowOpKind::SEND;
  const uint32_t srcRank = isSend ? rank_ : peer;
  const uint32_t dstRank = isSend ? peer : rank_;
  const uint32_t direction = srcRank > dstRank ? 1 : 0;
  // The index wraps after 2^20 messages on one channel and direction.
  return (((channel << 1) | direction) << kMessageIndexBits) |
      (index & kMessageIndexMask);
}

void DataflowRank::onMessageDone(const uint32_t peer, const Message& msg) {
  const auto it = opByMessage_.find(makePeerKey(peer, msg.connectionMessageId));
  if (it == opByMessage_.end()) {
    throw std::runtime_error(
        fmt::format(
            "Workload {} rank {}: no operation waits for message {} with rank "
            "{}",
            workloadName_,
            rank_,
            msg.connectionMessageId,
            peer));
  }
  const uint32_t op = it->second;
  opByMessage_.erase(it);
  messages_[op].reset();
  completeOp(op);
  drainPendingQueues();
}

void DataflowRank::completeOp(const uint32_t op) {
  const auto& cfg = program_.ops()->at(op);
  auto& queue = queues_[*cfg.queue()];
  isDone_[op] = true;
  ++numDone_;
  --queue.numInFlight;
  if (*cfg.kind() == DataflowOpKind::RECV) {
    --queue.numRecvsInFlight;
  }
  if (queue.fence == op) {
    queue.fence.reset();
  }
  markPending(*cfg.queue());
  for (uint32_t i = dependentsBegin_[op]; i < dependentsBegin_[op + 1]; ++i) {
    const uint32_t dependent = dependents_[i];
    if (--remainingDeps_[dependent] == 0) {
      markPending(*program_.ops()->at(dependent).queue());
    }
  }
  if (startDelayElapsed_ && numDone_ == isDone_.size()) {
    finish();
  }
}

void DataflowRank::finish() {
  // completeOp calls this when the last operation completes after the start
  // delay, and the start-delay callback calls it only while the rank is not
  // complete, so it runs once.
  if (complete_) {
    throw std::runtime_error(
        fmt::format(
            "Workload {} rank {}: the rank finished twice",
            workloadName_,
            rank_));
  }
  complete_ = true;
  endTimeNs_ = getTimeNs();
  traceEvent_->end(simCtx_->now());
  onRankComplete(workloadId_, rank_);
}

uint64_t DataflowRank::getExpectedBytes() {
  return expectedBytes_;
}

bool DataflowRank::complete() {
  return complete_;
}

std::string DataflowRank::describeBlockedQueues() const {
  const auto& ops = *program_.ops();
  std::string out;
  size_t numReported = 0;
  for (uint32_t queue = 0; queue < queues_.size(); ++queue) {
    const auto& state = queues_[queue];
    if (state.next == state.ops.size() && state.numInFlight == 0) {
      continue;
    }
    if (numReported == kMaxBlockedQueuesReported) {
      out += " More queues are blocked.";
      break;
    }
    ++numReported;
    if (state.next == state.ops.size()) {
      out += fmt::format(
          " Queue {} waits for {} issued operations.",
          queue,
          state.numInFlight);
      continue;
    }
    const uint32_t op = state.ops[state.next];
    std::vector<uint32_t> waitsFor;
    for (const uint32_t dep : *ops[op].deps()) {
      if (!isDone_[dep]) {
        waitsFor.emplace_back(dep);
      }
    }
    out += fmt::format(
        " Queue {} stops at operation {}, which waits for operations [{}]; {} "
        "issued operations of the queue have not completed{}.",
        queue,
        op,
        fmt::join(waitsFor, ","),
        state.numInFlight,
        state.fence.has_value()
            ? fmt::format(", and operation {} fences the queue", *state.fence)
            : "");
  }
  return out;
}

} // namespace packsim
