/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/link-layer/wire-trace-flag.h"

#include <utility>

#include <gtest/gtest.h>

#include "packsim/packet/base.h"
#include "packsim/packet/ethernet-frame.h"
#include "packsim/packet/ptr/PacketPtr.h"

namespace packsim {
namespace {

// (a) A frame sent without a sequence number reports no LLR state.
TEST(LlrTraceFieldsTest, ExtractFromAFrameWithoutSeqReturnsDefaults) {
  const auto fields = LlrTraceFields::extractFrom(
      {.pkt = createPacket<PacketType::ETHERNET>(100)});
  EXPECT_EQ(fields.type, -1);
  EXPECT_EQ(fields.seq, 0u);
}

// (b) An LLR data frame is an ordinary Ethernet frame sent with a sequence
// number, and reports the DATA column value.
TEST(LlrTraceFieldsTest, ExtractFromAnLlrDataFrameReportsDataAndSeq) {
  const auto fields = LlrTraceFields::extractFrom(
      {.pkt = createPacket<PacketType::ETHERNET>(100), .llrSeq = 42u});
  EXPECT_EQ(fields.type, 0) << "0 is the DATA column value";
  EXPECT_EQ(fields.seq, 42u);
}

// (c) Each LLR ordered set reports its own column value. The expected values
// are written as literals rather than cast from `LlrCtlosType`, because the
// encoding is deliberately independent of the enum's order; see `traceType`
// in wire-trace-flag.cpp.
TEST(LlrTraceFieldsTest, ExtractFromAnLlrCtlosFrameReportsItsType) {
  const std::pair<LlrCtlosType, int> cases[] = {
      {LlrCtlosType::LLR_ACK, 1},
      {LlrCtlosType::LLR_NACK, 2},
      {LlrCtlosType::LLR_INIT, 3},
      {LlrCtlosType::LLR_INIT_ECHO, 4},
  };
  for (const auto& [type, expected] : cases) {
    auto pkt = createPacket<PacketType::CTLOS_LLR>(0);
    auto* ctlos = pkt.as<LlrCtlosFrame>();
    ctlos->payload = {type, /*seq=*/7u, /*initData=*/0};

    const auto fields = LlrTraceFields::extractFrom({.pkt = pkt});
    EXPECT_EQ(fields.type, expected) << "type " << static_cast<int>(type);
    EXPECT_EQ(fields.seq, 7u);
  }
}

// (d) A CBFC ordered set is a `CtlosFrame` but not an LLR one. This case is
// what proves `extractFrom` tests for `LlrCtlosFrame` itself: a test against
// the shared `CtlosFrame` base would claim this frame as an LLR one.
TEST(LlrTraceFieldsTest, ExtractFromACfUpdateFrameReportsNoLlrState) {
  const auto fields = LlrTraceFields::extractFrom(
      {.pkt = createPacket<PacketType::CTLOS_CF_UPDATE>(0)});
  EXPECT_EQ(fields.type, -1);
  EXPECT_EQ(fields.seq, 0u);
}

} // namespace
} // namespace packsim
