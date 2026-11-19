/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/packet/base.h"
#include "packsim/packet/pfc.h"
#include "packsim/packet/ptr/PacketPtr.h"
#include "packsim/packet/roce.h"
#include "packsim/transport/oss_tests/roce-transport-sim-fixture.h"
#include "packsim/transport/roce-base.h"
#include "packsim/transport/roce-constants.h"
#include "packsim/transport/transport.h"

namespace packsim {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

namespace {
// A ready flag's size, and the value a WRITE raises it to.
constexpr uint32_t kFlagBytes = 2;
constexpr uint32_t kFlagValue = 7;
// Where the flag lives at the destination.
constexpr uint64_t kFlagAddr = 0x1000;
// The workload and rank the WRITE is for.
constexpr uint32_t kWorkloadId = 3;
constexpr uint32_t kDstRank = 5;
// A WRITE of two packets: one full packet and 2 bytes.
constexpr uint32_t kTwoPacketBytes =
    static_cast<uint32_t>(kMaxRdmaPayload) + kFlagBytes;
// RoceTransportBase::byteQuota_: the most a socket sends of its queue in one
// turn.
constexpr uint32_t kSendTurnBytes = 64 * 1024;
// RoceTransportBase::maxActiveSends_ of a transport built without DCQCN, as
// every transport in this suite is: the send turns it keeps in flight.
constexpr uint32_t kSendTurnsInFlight = 8;

// What the receiving transport handed up.
struct Delivery {
  bool isWrite{false};
  std::optional<uint64_t> writeAddr;
  uint32_t sendOrWriteData{0};
  uint32_t sizeBytes{0};
  uint32_t workloadId{0};
  uint32_t dstRank{0};
};
} // namespace

// WRITEs between two RoCEv2 transports over a loopback wire, and the messages
// the receiving transport hands up for them.
class RoceWriteDeliveryTest : public RoceTransportSimTest {
 protected:
  /**
   * Send, one after the other, a WRITE of each of @p sizesBytes that raises the
   * flag, with or without its destination address, and run the simulation to
   * the end.
   *
   * @param sizesBytes     The WRITEs' sizes, in sending order.
   * @param writeAddr      Each WRITE's Message::writeAddr.
   * @param alterPacketFn  If set, changes each packet the sender puts on the
   *                       wire before the wire delivers it.
   * @param losePacketFn   If set, tells for each packet the sender puts on the
   *                       wire, after alterPacketFn, whether the wire loses it.
   *
   * @return               The messages the receiving transport handed up.
   */
  std::vector<Delivery> sendWrites(
      const std::vector<uint32_t>& sizesBytes,
      const std::optional<uint64_t> writeAddr,
      const std::function<void(RocePacket&)>& alterPacketFn = nullptr,
      const std::function<bool(const RocePacket&)>& losePacketFn = nullptr) {
    RoceTransportBase src(&simCtx_, srcId_, &nicCfg_, roceCfg_);
    RoceTransportBase dst(&simCtx_, dstId_, &nicCfg_, roceCfg_);
    src.setSendPacketFn([this, &src, &dst, &alterPacketFn, &losePacketFn](
                            const PacketPtr& pkt) {
      if (alterPacketFn) {
        alterPacketFn(*pkt.as<RocePacket>());
      }
      if (losePacketFn && losePacketFn(*pkt.as<RocePacket>())) {
        src.onPacketSent(pkt, nullptr);
        return true;
      }
      return deliver(src, dst, pkt);
    });
    dst.setSendPacketFn(wire(dst, src));
    src.setOnPfcStateChangedCallback([](uint8_t, PfcState) {});
    dst.setOnPfcStateChangedCallback([](uint8_t, PfcState) {});
    src.setOnMessageAckedCallback([](Message*) {});
    std::vector<Delivery> deliveries;
    dst.setOnMessageReceivedCallback([&deliveries](Message* msg) {
      Delivery& delivery = deliveries.emplace_back();
      delivery.isWrite = msg->isWrite;
      delivery.writeAddr = msg->writeAddr;
      delivery.sendOrWriteData = msg->sendOrWriteData;
      delivery.sizeBytes = msg->sizeBytes;
      delivery.workloadId = msg->workloadId;
      delivery.dstRank = msg->dstRank;
    });
    Socket* const srcSocket = src.connect(dstId_, 1234, 0, 0);
    if (dst.connect(srcId_, 2345, 0, 0) == nullptr || srcSocket == nullptr) {
      ADD_FAILURE() << "connect() returned no socket";
      return deliveries;
    }
    std::vector<Message> writes;
    writes.reserve(sizesBytes.size());
    for (const uint32_t sizeBytes : sizesBytes) {
      Message& write = writes.emplace_back();
      write.sizeBytes = sizeBytes;
      write.srcIdx = srcId_;
      write.dstIdx = dstId_;
      write.isWrite = true;
      write.dstRank = kDstRank;
      write.workloadId = kWorkloadId;
      write.sendOrWriteData = kFlagValue;
      write.writeAddr = writeAddr;
    }
    simCtx_.scheduleNow([srcSocket, &writes]() {
      for (Message& write : writes) {
        srcSocket->send(&write);
      }
    });
    scheduler_.run();
    return deliveries;
  }

  // The config both transports are built with.
  RoceTransportConfig roceCfg_;
};

TEST_F(RoceWriteDeliveryTest, AWriteWithAnAddressIsHandedToTheReceiver) {
  const auto deliveries = sendWrites({kFlagBytes}, kFlagAddr);

  ASSERT_EQ(deliveries.size(), 1u);
  const Delivery& write = deliveries.front();
  EXPECT_TRUE(write.isWrite);
  EXPECT_EQ(write.writeAddr, kFlagAddr);
  EXPECT_EQ(write.sendOrWriteData, kFlagValue);
  EXPECT_EQ(write.sizeBytes, kFlagBytes);
  EXPECT_EQ(write.workloadId, kWorkloadId);
  EXPECT_EQ(write.dstRank, kDstRank);
}

TEST_F(RoceWriteDeliveryTest, AWriteWithoutAnAddressIsNotHandedOn) {
  EXPECT_TRUE(sendWrites({kFlagBytes}, std::nullopt).empty());
}

TEST_F(RoceWriteDeliveryTest, AWriteOfTwoPacketsIsHandedOnOnceWithItsSize) {
  const auto deliveries = sendWrites({kTwoPacketBytes}, kFlagAddr);

  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_EQ(deliveries.front().sizeBytes, kTwoPacketBytes);
}

TEST_F(RoceWriteDeliveryTest, OnlyTheFirstPacketOfAWriteCarriesItsAddress) {
  std::vector<PacketType> types;
  sendWrites({kTwoPacketBytes}, kFlagAddr, [&types](RocePacket& pkt) {
    types.emplace_back(pkt.pktType);
  });

  EXPECT_THAT(types, ElementsAre(PacketType::ROCE_WRITE, PacketType::ROCE));
}

// A WRITE one send turn and 2 bytes long goes as the turn's 16 full packets,
// then a 2-byte packet in the next turn, which the socket marks as the first
// packet of that turn's share of the WRITE. It still carries no address.
TEST_F(RoceWriteDeliveryTest, TheNextSendTurnOfAWriteCarriesNoAddress) {
  std::vector<PacketType> types;
  sendWrites(
      {kSendTurnBytes + kFlagBytes}, kFlagAddr, [&types](RocePacket& pkt) {
        types.emplace_back(pkt.pktType);
      });

  std::vector<PacketType> expected(
      kSendTurnBytes / kMaxRdmaPayload + 1, PacketType::ROCE);
  expected.front() = PacketType::ROCE_WRITE;
  EXPECT_EQ(types, expected);
}

TEST_F(RoceWriteDeliveryTest, AWriteWithoutAnAddressTravelsAsRocePackets) {
  std::vector<PacketType> types;
  sendWrites({kTwoPacketBytes}, std::nullopt, [&types](RocePacket& pkt) {
    types.emplace_back(pkt.pktType);
  });

  EXPECT_THAT(types, ElementsAre(PacketType::ROCE, PacketType::ROCE));
}

// The wire loses the first copy of the WRITE's last packet. Go-Back-N resends
// it once the tail-loss timer fires, without the destination, which only the
// first packet carries. The receiving socket kept it from the first packet, so
// the WRITE still goes up once, with its destination.
TEST_F(RoceWriteDeliveryTest, AWriteWhoseLastPacketIsLostGoesUpAfterTheResend) {
  roceCfg_.errorRecovery = RoceTransportConfig::ErrorRecovery::GO_BACK_N;
  uint32_t packetsOnWire = 0;
  const auto loseFirstCopyOfLastPacket = [&packetsOnWire](const RocePacket&) {
    // The WRITE has two packets, so the second on the wire is its last.
    return ++packetsOnWire == 2;
  };

  const auto deliveries = sendWrites(
      {kTwoPacketBytes}, kFlagAddr, nullptr, loseFirstCopyOfLastPacket);

  EXPECT_GT(packetsOnWire, 2u);
  ASSERT_EQ(deliveries.size(), 1u);
  const Delivery& write = deliveries.front();
  EXPECT_EQ(write.writeAddr, kFlagAddr);
  EXPECT_EQ(write.sizeBytes, kTwoPacketBytes);
  EXPECT_EQ(write.workloadId, kWorkloadId);
  EXPECT_EQ(write.dstRank, kDstRank);
}

// The wire loses the first copy of the WRITE's first packet, the only one that
// carries the destination. The receiving socket drops the second packet, which
// now comes out of order, and Go-Back-N resends both, the first again with the
// destination, so the WRITE still goes up once, with it.
TEST_F(
    RoceWriteDeliveryTest,
    AWriteWhoseFirstPacketIsLostGetsItsAddressFromTheResend) {
  roceCfg_.errorRecovery = RoceTransportConfig::ErrorRecovery::GO_BACK_N;
  uint32_t packetsOnWire = 0;
  const auto loseFirstCopyOfFirstPacket = [&packetsOnWire](const RocePacket&) {
    return ++packetsOnWire == 1;
  };

  const auto deliveries = sendWrites(
      {kTwoPacketBytes}, kFlagAddr, nullptr, loseFirstCopyOfFirstPacket);

  EXPECT_GT(packetsOnWire, 2u);
  ASSERT_EQ(deliveries.size(), 1u);
  const Delivery& write = deliveries.front();
  EXPECT_EQ(write.writeAddr, kFlagAddr);
  EXPECT_EQ(write.sizeBytes, kTwoPacketBytes);
  EXPECT_EQ(write.workloadId, kWorkloadId);
  EXPECT_EQ(write.dstRank, kDstRank);
}

// The transports here run without DCQCN, so each keeps at most
// kSendTurnsInFlight send turns in flight: a first WRITE of one turn more, less
// a byte, is still being sent when the flag is queued behind it. Its last turn
// then also takes the flag's first byte, the flag's last packet carries only
// its second byte, and the flag still goes up with both.
TEST_F(RoceWriteDeliveryTest, AWriteCutByTheEndOfASendTurnKeepsItsSize) {
  constexpr uint32_t kFirstWriteBytes =
      (kSendTurnsInFlight + 1) * kSendTurnBytes - 1;

  const auto deliveries = sendWrites({kFirstWriteBytes, kFlagBytes}, kFlagAddr);

  ASSERT_EQ(deliveries.size(), 2u);
  EXPECT_EQ(deliveries.at(0).sizeBytes, kFirstWriteBytes);
  EXPECT_EQ(deliveries.at(1).sizeBytes, kFlagBytes);
}

// A socket counts the payload of one WRITE at a time, so a packet of the next
// WRITE that arrives before the last packet of the one before it stops the run
// instead of adding to that WRITE's size. No sender sends that order: the wire
// makes it by marking the first WRITE's last packet as one of the second
// WRITE's, keeping its PSN, so isPsnOk still accepts it.
TEST_F(
    RoceWriteDeliveryTest,
    AWriteThatStartsBeforeTheOneBeforeItEndsIsRejected) {
  uint32_t packetsOnWire = 0;
  const auto markSecondPacketAsNextWrite = [&packetsOnWire](RocePacket& pkt) {
    // The first WRITE has two packets, so the second packet is its last.
    if (++packetsOnWire == 2) {
      ++pkt.sequenceNum;
    }
  };

  EXPECT_THAT(
      [&] {
        sendWrites(
            {kTwoPacketBytes, kFlagBytes},
            kFlagAddr,
            markSecondPacketAsNextWrite);
      },
      ThrowsMessage<std::runtime_error>(
          HasSubstr("before the last packet of WRITE")));
}

} // namespace packsim
