/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/nic_config_types.h"
#include "packsim/kernel/scheduler.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/kernel/time.h"
#include "packsim/nic/nic-datapath.h"
#include "packsim/nic/simple-nic-datapath.h"

namespace packsim {

namespace {

using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

// 8 Gbps moves one byte per ns, so a payload of N bytes adds N ns.
constexpr double kReadGbps = 8.0;
constexpr double kReadLatencyNs = 500.0;
constexpr uint64_t kAddr = 0x2000000;
constexpr uint64_t kPayloadBytes = 100;
// What an address latency function returns in these tests, in ns.
constexpr uint64_t kAddressLatencyNs = 50;

// A NIC whose memory moves one byte per ns with a flat read latency of
// kReadLatencyNs, and the given write latency.
packsim::thrift::NicConfig makeNicConfig(
    const std::optional<double> writeLatencyNs = std::nullopt) {
  packsim::thrift::NicConfig nicConfig;
  nicConfig.readGbps() = kReadGbps;
  nicConfig.readLatencyNs() = kReadLatencyNs;
  if (writeLatencyNs.has_value()) {
    nicConfig.writeLatencyNs() = *writeLatencyNs;
  }
  return nicConfig;
}

// A packet to send whose payload of `bytes` the NIC reads at `addr`.
INicDatapath::TxPacket makeRead(
    const uint64_t bytes,
    const std::optional<uint64_t> addr = std::nullopt) {
  return {
      .payloadRead = INicDatapath::PayloadAccess{.bytes = bytes, .addr = addr}};
}

// A received packet whose payload of `bytes` the NIC writes at `addr`.
INicDatapath::RxPacket makeWrite(
    const uint64_t bytes,
    const std::optional<uint64_t> addr = std::nullopt) {
  return {
      .payloadWrite =
          INicDatapath::PayloadAccess{.bytes = bytes, .addr = addr}};
}

// The femtoseconds in `ns` nanoseconds, to compare with a reserved delay.
uint64_t nsToFs(const uint64_t ns) {
  return Time::fromNs(ns).getFs();
}

// A latency function that answers kAddressLatencyNs at kAddr and 0 elsewhere.
uint64_t getAddressLatencyNs(const uint64_t addr, const size_t /*sizeBytes*/) {
  return addr == kAddr ? kAddressLatencyNs : 0;
}

} // namespace

class SimpleNicDatapathTest : public ::testing::Test {
 protected:
  Scheduler scheduler_;
  SimContext simCtx_{&scheduler_, "SimpleNicDatapathTest"};
};

TEST_F(SimpleNicDatapathTest, PacketWithoutPayloadLeavesAtOnce) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());

  // Act
  const Time delay = datapath.reserveTx({});

  // Assert
  EXPECT_EQ(delay.getFs(), 0U);
}

TEST_F(SimpleNicDatapathTest, PacketWithoutPayloadReservesNoRead) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());
  std::ignore = datapath.reserveTx({});

  // Act
  const Time delay = datapath.reserveTx(makeRead(kPayloadBytes));

  // Assert: the read waits for nothing ahead of it.
  EXPECT_EQ(delay.getFs(), nsToFs(600));
}

TEST_F(SimpleNicDatapathTest, ReadTakesTheFlatLatencyAndItsBytes) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());

  // Act
  const Time delay = datapath.reserveTx(makeRead(kPayloadBytes, kAddr));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(600));
}

TEST_F(SimpleNicDatapathTest, ReadCompletesAfterTheReadAheadOfIt) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());
  std::ignore = datapath.reserveTx(makeRead(kPayloadBytes));

  // Act
  const Time delay = datapath.reserveTx(makeRead(kPayloadBytes));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(700));
}

TEST_F(SimpleNicDatapathTest, ZeroByteReadTakesTheFlatLatency) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());

  // Act
  const Time delay = datapath.reserveTx(makeRead(0));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(500));
}

TEST_F(SimpleNicDatapathTest, ReadAtAKnownAddressTakesTheFunctionsLatency) {
  // Arrange
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadReadLatencyNsFn = &getAddressLatencyNs});

  // Act
  const Time delay = datapath.reserveTx(makeRead(kPayloadBytes, kAddr));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(kAddressLatencyNs + kPayloadBytes));
}

TEST_F(SimpleNicDatapathTest, ReadFunctionGetsTheAddressAndSize) {
  // Arrange
  std::vector<std::pair<uint64_t, size_t>> queries;
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadReadLatencyNsFn = [&queries](uint64_t addr, size_t size) {
        queries.emplace_back(addr, size);
        return uint64_t{0};
      }});

  // Act
  std::ignore = datapath.reserveTx(makeRead(kPayloadBytes, kAddr));

  // Assert
  const std::vector<std::pair<uint64_t, size_t>> expected{
      {kAddr, kPayloadBytes}};
  EXPECT_EQ(queries, expected);
}

TEST_F(SimpleNicDatapathTest, ReadWithoutAnAddressKeepsTheFlatLatency) {
  // Arrange
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadReadLatencyNsFn = &getAddressLatencyNs});

  // Act
  const Time delay = datapath.reserveTx(makeRead(kPayloadBytes));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(600));
}

TEST_F(SimpleNicDatapathTest, ZeroByteReadKeepsTheFlatLatency) {
  // Arrange
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadReadLatencyNsFn = &getAddressLatencyNs});

  // Act
  const Time delay = datapath.reserveTx(makeRead(0, kAddr));

  // Assert
  EXPECT_EQ(delay.getFs(), nsToFs(500));
}

TEST_F(SimpleNicDatapathTest, PacketWithoutPayloadIsHandedOverOnArrival) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig(/*writeLatencyNs=*/200));

  // Act
  const std::optional<Time> delay = datapath.reserveRx({});

  // Assert
  EXPECT_FALSE(delay.has_value());
}

TEST_F(SimpleNicDatapathTest, WriteIsFreeWithoutAWriteLatency) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig());

  // Act
  const std::optional<Time> delay =
      datapath.reserveRx(makeWrite(kPayloadBytes, kAddr));

  // Assert
  EXPECT_FALSE(delay.has_value());
}

TEST_F(SimpleNicDatapathTest, WriteTakesTheConfiguredWriteLatency) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig(/*writeLatencyNs=*/200));

  // Act
  const std::optional<Time> delay =
      datapath.reserveRx(makeWrite(kPayloadBytes, kAddr));

  // Assert
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->getFs(), nsToFs(300));
}

TEST_F(SimpleNicDatapathTest, ZeroByteWriteTakesNoFixedLatency) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig(/*writeLatencyNs=*/200));

  // Act
  const std::optional<Time> delay = datapath.reserveRx(makeWrite(0));

  // Assert: the memory's 1 ns tie break, not the 200 ns write latency.
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->getFs(), nsToFs(1));
}

TEST_F(SimpleNicDatapathTest, ZeroByteWriteCompletesJustAfterTheWriteAhead) {
  // Arrange
  SimpleNicDatapath datapath(simCtx_, makeNicConfig(/*writeLatencyNs=*/200));
  std::ignore = datapath.reserveRx(makeWrite(kPayloadBytes));

  // Act
  const std::optional<Time> delay = datapath.reserveRx(makeWrite(0));

  // Assert: one ns after the 300 ns write, the memory's tie break.
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->getFs(), nsToFs(301));
}

TEST_F(SimpleNicDatapathTest, WriteFunctionChargesEveryWrite) {
  // Arrange: no NicConfig.writeLatencyNs, so a write at no known address takes
  // the flat read latency.
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadWriteLatencyNsFn = &getAddressLatencyNs});

  // Act
  const std::optional<Time> delay =
      datapath.reserveRx(makeWrite(kPayloadBytes));

  // Assert
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->getFs(), nsToFs(600));
}

TEST_F(SimpleNicDatapathTest, WriteAtAKnownAddressTakesTheFunctionsLatency) {
  // Arrange
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(/*writeLatencyNs=*/200),
      {.getPayloadWriteLatencyNsFn = &getAddressLatencyNs});

  // Act
  const std::optional<Time> delay =
      datapath.reserveRx(makeWrite(kPayloadBytes, kAddr));

  // Assert
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->getFs(), nsToFs(kAddressLatencyNs + kPayloadBytes));
}

TEST_F(SimpleNicDatapathTest, ReadFunctionLeavesWritesFree) {
  // Arrange
  SimpleNicDatapath datapath(
      simCtx_,
      makeNicConfig(),
      {.getPayloadReadLatencyNsFn = &getAddressLatencyNs});

  // Act
  const std::optional<Time> delay =
      datapath.reserveRx(makeWrite(kPayloadBytes, kAddr));

  // Assert
  EXPECT_FALSE(delay.has_value());
}

TEST_F(SimpleNicDatapathTest, NegativeWriteLatencyIsRejected) {
  // Arrange
  const packsim::thrift::NicConfig nicConfig =
      makeNicConfig(/*writeLatencyNs=*/-1);

  // Act + Assert
  EXPECT_THAT(
      [&] { SimpleNicDatapath datapath(simCtx_, nicConfig); },
      ThrowsMessage<std::invalid_argument>(HasSubstr(
          "NicConfig.writeLatencyNs must be finite and not negative")));
}

TEST_F(SimpleNicDatapathTest, NonFiniteWriteLatencyIsRejected) {
  // Arrange
  const packsim::thrift::NicConfig nicConfig =
      makeNicConfig(/*writeLatencyNs=*/std::nan(""));

  // Act + Assert
  EXPECT_THAT(
      [&] { SimpleNicDatapath datapath(simCtx_, nicConfig); },
      ThrowsMessage<std::invalid_argument>(HasSubstr(
          "NicConfig.writeLatencyNs must be finite and not negative")));
}

} // namespace packsim
