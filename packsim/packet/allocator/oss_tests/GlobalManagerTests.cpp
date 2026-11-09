/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <array>
#include <string>
#include <typeinfo>

#include <gtest/gtest.h>

#include "packsim/packet/allocator/GlobalManager.h"
#include "packsim/packet/base.h"
#include "packsim/packet/roce.h"

namespace packsim {

TEST(GlobalManagerTests, TestTakeAndReturn) {
  auto* ethernetPfcPacketPkt =
      packsim::GlobalManager::takePacket<packsim::EthernetPfcPacket>();
  auto* udpPacketPkt =
      packsim::GlobalManager::takePacket<packsim::PacketType::UDP>();

  ASSERT_EQ(ethernetPfcPacketPkt->pktType, packsim::PacketType::ETHERNET_PFC);
  ASSERT_EQ(udpPacketPkt->pktType, packsim::PacketType::UDP);

  packsim::GlobalManager::returnPacket(ethernetPfcPacketPkt);
  packsim::GlobalManager::returnPacket(udpPacketPkt);
}

// `pktType` is a mutable public field, and callers do overwrite it -- e.g.
// switch/oss_tests/fb-switch-test.cpp allocates a UdpPacket and forces
// PacketType::ROCE onto it to drive a RoCE code path. The pool must file the
// packet under what it *is*, not what the field claims: filing this UdpPacket
// allocation in RocePacket's pool would hand it back from
// takePacket<RocePacket>(), and writing RocePacket's members (psn, opcode,
// the two std::optionals ...) would run off the end of the smaller object.
TEST(GlobalManagerTests, FilesPacketByDynamicTypeNotPktTypeField) {
  auto* udp = packsim::GlobalManager::takePacket<packsim::PacketType::UDP>();
  udp->pktType = packsim::PacketType::ROCE;
  packsim::GlobalManager::returnPacket(udp);

  // The mislabelled packet must not surface from RocePacket's pool.
  auto* roce = packsim::GlobalManager::takePacket<packsim::PacketType::ROCE>();
  EXPECT_NE(static_cast<void*>(roce), static_cast<void*>(udp));
  EXPECT_EQ(typeid(*roce), typeid(packsim::RocePacket));

  // It must still be at the top of the pool it was actually allocated from.
  auto* udpAgain =
      packsim::GlobalManager::takePacket<packsim::PacketType::UDP>();
  EXPECT_EQ(static_cast<void*>(udpAgain), static_cast<void*>(udp));

  packsim::GlobalManager::returnPacket(roce);
  packsim::GlobalManager::returnPacket(udpAgain);
}

// Counts its destructions, so a test can tell a packet the pool deleted from
// one it kept.
struct CountedPacket : Packet {
  ~CountedPacket() override {
    ++destroyedCount;
  }
  static inline int destroyedCount = 0;
};

// A thread that frees more packets than it takes -- a switch thread dropping
// the last reference an LLR replay buffer held -- must not keep them all.
TEST(PacketAllocatorTests, DeletesPacketsReturnedPastTheCap) {
  // Arrange
  CountedPacket::destroyedCount = 0;
  detail::PacketAllocator allocator{/*maxSparePerType=*/2};
  const std::array<CountedPacket*, 3> packets{
      new CountedPacket(), new CountedPacket(), new CountedPacket()};

  // Act
  for (auto* packet : packets) {
    allocator.push(packet);
  }

  // Assert: the third was deleted, and the two kept come back newest first.
  EXPECT_EQ(CountedPacket::destroyedCount, 1);
  auto* const newest = allocator.pop<CountedPacket>();
  auto* const oldest = allocator.pop<CountedPacket>();
  EXPECT_EQ(newest, packets[1]);
  EXPECT_EQ(oldest, packets[0]);
  // Delete what came back: if the cap failed, the pool still holds a packet,
  // which its destructor deletes.
  delete newest;
  delete oldest;
}

} // namespace packsim
