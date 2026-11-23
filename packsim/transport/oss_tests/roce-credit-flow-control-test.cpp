/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/kernel/time.h"
#include "packsim/packet/pfc.h"
#include "packsim/packet/ptr/PacketPtr.h"
#include "packsim/packet/roce.h"
#include "packsim/transport/oss_tests/roce-transport-sim-fixture.h"
#include "packsim/transport/roce-base.h"
#include "packsim/transport/roce-constants.h"
#include "packsim/transport/transport.h"

namespace packsim {

namespace {
// Payload bytes of one full packet.
constexpr auto kPacketBytes = static_cast<uint32_t>(kMaxRdmaPayload);
// PSN of the first packet a socket sends.
constexpr uint32_t kFirstPsn = 1;
// When the receiver posts its receive in the tests that make the SEND wait.
constexpr uint64_t kLateReceiveNs = 50'000;
// Size of a one-packet message.
constexpr uint32_t kOnePacketBytes = 100;
// RoceTransportBase::byteQuota_, the 64 KiB a socket sends of its queue in one
// turn, in full packets: the packets of one send chunk.
constexpr uint32_t kPacketsPerSendChunk = 64 * 1024 / kPacketBytes;
// The longest a loopback runs: ten times the 1 ms Go-Back-N ACK timeout the
// slowest test waits for, so a transport that stops sending ends the run
// rather than hanging the test on its retries.
constexpr uint64_t kRunLimitNs = 10'000'000;

// What happened on the loopback.
struct LoopbackRun {
  // Each data packet from the sender: when it reached the receiver, in ns,
  // and its PSN.
  struct DataArrival {
    uint64_t arrivalNs{0};
    uint32_t psn{0};
  };
  std::vector<DataArrival> dataArrivals;
  // Each ACK the receiver sent, in order: the PSN it acknowledges and the
  // credit it reports.
  struct Ack {
    uint32_t psn{0};
    uint16_t credit{0};
  };
  std::vector<Ack> acks;
  // When the sender saw its first message acked, in ns, if it did.
  std::optional<uint64_t> ackedNs;
  // How many of the sender's messages were acked.
  uint32_t ackedMessages{0};
};

// The PSNs that @p run's ACKs acknowledge, in order.
std::vector<uint32_t> getAckedPsns(const LoopbackRun& run) {
  std::vector<uint32_t> psns;
  psns.reserve(run.acks.size());
  for (const auto& ack : run.acks) {
    psns.emplace_back(ack.psn);
  }
  return psns;
}
} // namespace

// SENDs and WRITEs between two RoCEv2 transports with Go-Back-N over a
// loopback wire.
class RoceCreditFlowControlTest : public RoceTransportSimTest {
 protected:
  // What the two ends of the loopback send and receive.
  struct LoopbackSetup {
    // The sender's messages, all sent at time 0 in this order.
    std::vector<Message> messages;
    // How many receives the receiver posts, each a copy of the first message.
    uint32_t receives{0};
    // When the receiver posts its first receive.
    uint64_t receivesAtNs{0};
    // Whether the receiver posts receivesPostedFirst receives at receivesAtNs
    // and then one more each time a SEND arrives whole, rather than all of
    // them at receivesAtNs.
    bool receivesOneAtATime{false};
    // How many receives the receiver posts at receivesAtNs when it posts them
    // one at a time.
    uint32_t receivesPostedFirst{1};
    // RoceTransportConfig::creditFlowControl of both ends.
    bool creditFlowControl{true};
    // The PSN of a data packet whose first copy the wire loses, if any, e.g.
    // kFirstPsn for the first packet the sender sends.
    std::optional<uint32_t> losesPsn;
    // Whether the wire loses the first ACK the receiver sends.
    bool losesFirstAck{false};
    // Whether the receiver takes packets out of PSN order
    // (NicConfig.outofOrderPktSupport), with Go-Back-N's selective gap repair
    // on both ends.
    bool outOfOrderDelivery{false};
  };

  /**
   * Add to @p messages a SEND of @p sizeBytes from the sender to the receiver.
   *
   * @param messages   The messages to add it to.
   * @param sizeBytes  The SEND's size.
   *
   * @return           The SEND, which a caller may turn into a WRITE.
   */
  Message& addMessage(std::vector<Message>& messages, const uint32_t sizeBytes)
      const {
    Message& msg = messages.emplace_back();
    msg.sizeBytes = sizeBytes;
    msg.srcIdx = srcId_;
    msg.dstIdx = dstId_;
    return msg;
  }

  /**
   * Send one SEND of @p sendBytes while the receiver posts @p receives
   * receives at @p receivesAtNs, and run the loopback.
   *
   * @param sendBytes          The SEND's size.
   * @param receives           How many receives the receiver posts.
   * @param receivesAtNs       When it posts them.
   * @param creditFlowControl  RoceTransportConfig::creditFlowControl of both
   *                           ends.
   *
   * @return                   What happened on the loopback.
   */
  LoopbackRun sendOne(
      const uint32_t sendBytes,
      const uint32_t receives,
      const uint64_t receivesAtNs,
      const bool creditFlowControl = true) {
    LoopbackSetup setup;
    addMessage(setup.messages, sendBytes);
    setup.receives = receives;
    setup.receivesAtNs = receivesAtNs;
    setup.creditFlowControl = creditFlowControl;
    return runLoopback(setup);
  }

  /**
   * Make three SENDs of three packets each, PSNs 1 to 3, 4 to 6 and 7 to 9,
   * and a receive for each.
   *
   * @return  What the two ends send and receive.
   */
  LoopbackSetup makeThreeSendsOfThreePackets() const {
    constexpr uint32_t kSends = 3;
    LoopbackSetup setup;
    for (uint32_t send = 0; send < kSends; ++send) {
      addMessage(setup.messages, 3 * kPacketBytes);
    }
    setup.receives = kSends;
    return setup;
  }

  /**
   * Run @p setup on the loopback until no events are left, or for
   * kRunLimitNs.
   *
   * @param setup  What the two ends send and receive; at least one message.
   *
   * @return       What happened on the loopback.
   */
  LoopbackRun runLoopback(const LoopbackSetup& setup) {
    RoceTransportConfig roceCfg;
    roceCfg.errorRecovery = RoceTransportConfig::ErrorRecovery::GO_BACK_N;
    roceCfg.creditFlowControl = setup.creditFlowControl;
    roceCfg.selectiveGapRepair = setup.outOfOrderDelivery;
    nicCfg_.outofOrderPktSupport() = setup.outOfOrderDelivery;
    RoceTransportBase src(&simCtx_, srcId_, &nicCfg_, roceCfg);
    RoceTransportBase dst(&simCtx_, dstId_, &nicCfg_, roceCfg);
    LoopbackRun run;
    bool dataPacketLost = false;
    src.setSendPacketFn([this, &setup, &src, &dst, &run, &dataPacketLost](
                            const PacketPtr& pkt) {
      const RocePacket& roce = *pkt.as<RocePacket>();
      if (!roce.isAck && !roce.isNack && !roce.isCnp) {
        if (setup.losesPsn == roce.psn && !dataPacketLost) {
          dataPacketLost = true;
          src.onPacketSent(pkt, nullptr);
          return true;
        }
        auto& arrival = run.dataArrivals.emplace_back();
        arrival.arrivalNs = simCtx_.nowNs() + kWireDelayNs;
        arrival.psn = roce.psn;
      }
      return deliver(src, dst, pkt);
    });
    bool ackLost = false;
    dst.setSendPacketFn(
        [this, &setup, &src, &dst, &run, &ackLost](const PacketPtr& pkt) {
          const RocePacket& roce = *pkt.as<RocePacket>();
          if (roce.isAck) {
            auto& ack = run.acks.emplace_back();
            ack.psn = roce.psn;
            ack.credit = roce.recvCredit;
            if (setup.losesFirstAck && !ackLost) {
              ackLost = true;
              dst.onPacketSent(pkt, nullptr);
              return true;
            }
          }
          return deliver(dst, src, pkt);
        });
    src.setOnPfcStateChangedCallback([](uint8_t, PfcState) {});
    dst.setOnPfcStateChangedCallback([](uint8_t, PfcState) {});
    src.setOnMessageAckedCallback([this, &run](Message*) {
      run.ackedNs = run.ackedNs.value_or(simCtx_.nowNs());
      ++run.ackedMessages;
    });
    std::vector<Message> recvs(setup.receives, setup.messages.front());
    size_t recvsPosted = 0;
    Socket* dstSocket = nullptr;
    const auto postNextRecv = [&recvs, &recvsPosted, &dstSocket]() {
      if (recvsPosted < recvs.size()) {
        dstSocket->recv(&recvs[recvsPosted++]);
      }
    };
    dst.setOnMessageReceivedCallback([&setup, &postNextRecv](Message* msg) {
      if (setup.receivesOneAtATime && !msg->isWrite) {
        postNextRecv();
      }
    });
    Socket* const srcSocket = src.connect(dstId_, 1234, 0, 0);
    dstSocket = dst.connect(srcId_, 2345, 0, 0);
    if (srcSocket == nullptr || dstSocket == nullptr) {
      ADD_FAILURE() << "connect() returned no socket";
      return run;
    }
    std::vector<Message> messages = setup.messages;
    simCtx_.scheduleNow([srcSocket, &messages]() {
      for (Message& msg : messages) {
        srcSocket->send(&msg);
      }
    });
    simCtx_.schedule(
        Time::fromNs(setup.receivesAtNs), [&setup, &recvs, &postNextRecv]() {
          const size_t count = setup.receivesOneAtATime
              ? setup.receivesPostedFirst
              : recvs.size();
          for (size_t i = 0; i < count; ++i) {
            postNextRecv();
          }
        });
    scheduler_.run(Time::fromNs(kRunLimitNs));
    return run;
  }
};

TEST_F(
    RoceCreditFlowControlTest,
    AnAckReportsTheReceivesPostedBeyondTheOneItsSendTakes) {
  const LoopbackRun run = sendOne(kOnePacketBytes, /*receives=*/3, 0);

  ASSERT_FALSE(run.acks.empty());
  EXPECT_EQ(run.acks.front().credit, 2);
}

TEST_F(
    RoceCreditFlowControlTest,
    ASendWithoutCreditGoesAsItsFirstPacketUntilAReceiveIsPosted) {
  const LoopbackRun run =
      sendOne(3 * kPacketBytes, /*receives=*/1, kLateReceiveNs);

  std::vector<uint32_t> psnsBeforeReceive;
  for (const auto& arrival : run.dataArrivals) {
    if (arrival.arrivalNs < kLateReceiveNs) {
      psnsBeforeReceive.emplace_back(arrival.psn);
    }
  }
  ASSERT_FALSE(psnsBeforeReceive.empty());
  EXPECT_EQ(
      psnsBeforeReceive,
      std::vector<uint32_t>(psnsBeforeReceive.size(), kFirstPsn));
  EXPECT_TRUE(run.ackedNs.has_value()) << "the SEND never completed";
}

// The control for the test above.
TEST_F(RoceCreditFlowControlTest, WithoutCreditFlowControlASendGoesWhole) {
  const LoopbackRun run = sendOne(
      3 * kPacketBytes,
      /*receives=*/1,
      kLateReceiveNs,
      /*creditFlowControl=*/false);

  EXPECT_TRUE(
      std::any_of(
          run.dataArrivals.begin(),
          run.dataArrivals.end(),
          [](const LoopbackRun::DataArrival& arrival) {
            return arrival.arrivalNs < kLateReceiveNs &&
                arrival.psn > kFirstPsn;
          }));
}

// A packet asks for an ACK when it starts a send chunk or ends its message. The
// first SEND has no credit, so it goes as its first packet alone, whose ACK
// reports the other two receives. The other two SENDs each spend one of them
// and go whole, so each asks for an ACK only on its first and last packets.
TEST_F(RoceCreditFlowControlTest, ASendWithCreditGoesInOneChunk) {
  const LoopbackRun run = runLoopback(makeThreeSendsOfThreePackets());

  const std::vector<uint32_t> expected{1, 2, 3, 4, 6, 7, 9};
  EXPECT_EQ(getAckedPsns(run), expected);
  EXPECT_EQ(run.ackedMessages, 3u);
}

// Two receives are posted first, and the third once the first SEND has arrived
// whole. The first SEND's ACK reports one receive, which the second SEND
// spends. The third SEND then has no credit, so it goes as its first packet
// alone, and the rest of it follows in a send chunk of its own, whose first
// packet asks for an ACK.
TEST_F(
    RoceCreditFlowControlTest,
    ASendAfterTheCreditRunsOutGoesAsItsFirstPacket) {
  LoopbackSetup setup = makeThreeSendsOfThreePackets();
  setup.receivesOneAtATime = true;
  setup.receivesPostedFirst = 2;

  const LoopbackRun run = runLoopback(setup);

  const std::vector<uint32_t> expected{1, 2, 3, 4, 6, 7, 8, 9};
  EXPECT_EQ(getAckedPsns(run), expected);
  EXPECT_EQ(run.ackedMessages, 3u);
}

// runLoopback builds both transports without DCQCN congestion control, so each
// keeps at most 8 send turns in flight (RoceTransportBase::maxActiveSends_),
// and a turn counts until its last packet calls back once sent. A turn that
// sends a WRITE and then stops at a SEND waiting for credit must still end with
// such a packet: each turn that does not holds one of the 8 for good, and after
// 8 the transport stops sending.
TEST_F(
    RoceCreditFlowControlTest,
    WritesBetweenSendsWaitingForCreditDoNotStopTheTransport) {
  constexpr uint32_t kPairs = 20;
  LoopbackSetup setup;
  for (uint32_t pair = 0; pair < kPairs; ++pair) {
    addMessage(setup.messages, kOnePacketBytes);
    addMessage(setup.messages, kOnePacketBytes).isWrite = true;
  }
  setup.receives = kPairs;
  setup.receivesOneAtATime = true;

  const LoopbackRun run = runLoopback(setup);

  EXPECT_EQ(run.ackedMessages, 2 * kPairs);
}

// A SEND without credit goes as its first packet alone, which asks for an ACK.
// No later packet shows the receiver a gap when that packet is lost, so
// Go-Back-N has to resend it when no ACK comes.
TEST_F(
    RoceCreditFlowControlTest,
    ALostFirstPacketOfASendWithoutCreditIsResent) {
  LoopbackSetup setup;
  addMessage(setup.messages, 3 * kPacketBytes);
  setup.receives = 1;
  setup.losesPsn = kFirstPsn;

  const LoopbackRun run = runLoopback(setup);

  EXPECT_TRUE(run.ackedNs.has_value()) << "the SEND never completed";
}

// The ACK of a SEND's lone first packet is lost, so Go-Back-N resends the
// packet, and the receiver ACKs it again while the SEND is still arriving.
// That ACK, too, leaves out the receive the SEND holds: 1 of the 2 posted.
TEST_F(
    RoceCreditFlowControlTest,
    ARepeatedAckLeavesOutTheReceiveOfASendStillArriving) {
  LoopbackSetup setup;
  addMessage(setup.messages, 2 * kPacketBytes);
  setup.receives = 2;
  setup.losesFirstAck = true;

  const LoopbackRun run = runLoopback(setup);

  ASSERT_GE(run.acks.size(), 2u);
  EXPECT_EQ(run.acks.at(1).psn, kFirstPsn);
  EXPECT_EQ(run.acks.at(1).credit, 1);
}

// With out-of-order delivery, the wire loses the last packet of the SEND's
// first full send chunk, so the receiver holds back the ACK that the next
// chunk's first packet asks for until the resend fills the gap. The SEND is
// still arriving then, so that ACK leaves out the receive the SEND holds: 1 of
// the 2 posted.
TEST_F(
    RoceCreditFlowControlTest,
    AHeldBackAckLeavesOutTheReceiveOfASendStillArriving) {
  // For want of credit the SEND goes as its first packet alone, then in send
  // chunks of kPacketsPerSendChunk packets: PSNs 2 to 17, then 18 on.
  constexpr uint32_t kLostPsn = kFirstPsn + kPacketsPerSendChunk;
  constexpr uint32_t kHeldBackPsn = kLostPsn + 1;
  LoopbackSetup setup;
  addMessage(setup.messages, 2 * kPacketsPerSendChunk * kPacketBytes);
  setup.receives = 2;
  setup.outOfOrderDelivery = true;
  setup.losesPsn = kLostPsn;

  const LoopbackRun run = runLoopback(setup);

  const auto heldBack = std::find_if(
      run.acks.begin(), run.acks.end(), [](const LoopbackRun::Ack& ack) {
        return ack.psn == kHeldBackPsn;
      });
  ASSERT_NE(heldBack, run.acks.end()) << "no ACK of PSN " << kHeldBackPsn;
  EXPECT_EQ(heldBack->credit, 1);
}

// The ACK of a one-packet SEND is lost, so Go-Back-N resends the packet after
// the SEND has arrived whole, and the receiver ACKs it again. The SEND's
// receive is no longer posted then, so that ACK counts every receive still
// posted: 1 of the 2.
TEST_F(
    RoceCreditFlowControlTest,
    ARepeatedAckAfterItsSendHasArrivedCountsEveryReceiveStillPosted) {
  LoopbackSetup setup;
  addMessage(setup.messages, kOnePacketBytes);
  setup.receives = 2;
  setup.losesFirstAck = true;

  const LoopbackRun run = runLoopback(setup);

  ASSERT_GE(run.acks.size(), 2u);
  EXPECT_EQ(run.acks.at(1).psn, kFirstPsn);
  EXPECT_EQ(run.acks.at(1).credit, 1);
}

// With out-of-order delivery, the wire loses the last packet of the first of
// two SENDs, so the receiver holds back the ACKs the second SEND asks for until
// the resend fills the gap. The second SEND has arrived whole by then and its
// receive is no longer posted, so those ACKs count every receive still posted:
// 2 of the 3.
TEST_F(
    RoceCreditFlowControlTest,
    AHeldBackAckAfterItsSendHasArrivedCountsEveryReceiveStillPosted) {
  // PSNs 1 and 2 are the first SEND, 3 and 4 the second.
  constexpr uint32_t kLostPsn = kFirstPsn + 1;
  constexpr uint32_t kHeldBackPsn = kFirstPsn + 3;
  LoopbackSetup setup;
  addMessage(setup.messages, 2 * kPacketBytes);
  addMessage(setup.messages, 2 * kPacketBytes);
  setup.receives = 3;
  setup.outOfOrderDelivery = true;
  setup.losesPsn = kLostPsn;

  const LoopbackRun run = runLoopback(setup);

  const auto heldBack = std::find_if(
      run.acks.begin(), run.acks.end(), [](const LoopbackRun::Ack& ack) {
        return ack.psn == kHeldBackPsn;
      });
  ASSERT_NE(heldBack, run.acks.end()) << "no ACK of PSN " << kHeldBackPsn;
  EXPECT_EQ(heldBack->credit, 2);
}

} // namespace packsim
