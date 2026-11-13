/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/nic/simple-nic-datapath.h"

#include <cmath>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "packsim/kernel/sim-context.h"

namespace packsim {

SimpleNicDatapath::SimpleNicDatapath(
    const SimContext& simCtx,
    const packsim::thrift::NicConfig& nicConfig,
    PayloadLatencyFns latencyFns)
    : simCtx_(simCtx),
      chargesPayloadWrites_(
          static_cast<bool>(latencyFns.getPayloadWriteLatencyNsFn) ||
          nicConfig.writeLatencyNs().has_value()),
      latencyFns_(std::move(latencyFns)),
      memory_(makeMemory(nicConfig)) {}

Time SimpleNicDatapath::reserveTx(const TxPacket& packet) {
  if (!packet.payloadRead.has_value()) {
    return Time{};
  }
  const PayloadAccess& read = *packet.payloadRead;
  const uint64_t nowNs = simCtx_.nowNs();
  // Having no function is the default configuration, not a wiring bug: a read
  // takes the flat latency unless the owner gives one.
  if (read.bytes == 0 || !read.addr.has_value() ||
      !latencyFns_.getPayloadReadLatencyNsFn) {
    return Time::fromNs(memory_.getReadDelayNs(read.bytes, nowNs));
  }
  return Time::fromNs(memory_.getReadDelayNs(
      read.bytes, nowNs, getPayloadReadLatencyNsFn(*read.addr, read.bytes)));
}

std::optional<Time> SimpleNicDatapath::reserveRx(const RxPacket& packet) {
  // Received payload writes are charged only on request; without a function or
  // a configured write latency every payload counts as written on arrival.
  if (!packet.payloadWrite.has_value() || !chargesPayloadWrites_) {
    return std::nullopt;
  }
  const PayloadAccess& write = *packet.payloadWrite;
  const uint64_t nowNs = simCtx_.nowNs();
  if (write.bytes == 0) {
    return Time::fromNs(memory_.getWriteDelayNs(0, nowNs, /*latencyNs=*/0.0));
  }
  if (!write.addr.has_value() || !latencyFns_.getPayloadWriteLatencyNsFn) {
    return Time::fromNs(memory_.getWriteDelayNs(write.bytes, nowNs));
  }
  return Time::fromNs(memory_.getWriteDelayNs(
      write.bytes,
      nowNs,
      getPayloadWriteLatencyNsFn(*write.addr, write.bytes)));
}

SimpleMemory SimpleNicDatapath::makeMemory(
    const packsim::thrift::NicConfig& nicConfig) {
  const double memoryBytesPerNs = *nicConfig.readGbps() / 8.0;
  const double readLatencyNs = *nicConfig.readLatencyNs();
  const std::optional<double> writeLatencyNs =
      nicConfig.writeLatencyNs().to_optional();
  if (writeLatencyNs.has_value() &&
      (!std::isfinite(*writeLatencyNs) || *writeLatencyNs < 0)) {
    throw std::invalid_argument(
        fmt::format(
            "NicConfig.writeLatencyNs must be finite and not negative, got {}",
            *writeLatencyNs));
  }
  return SimpleMemory(
      memoryBytesPerNs,
      memoryBytesPerNs,
      readLatencyNs,
      writeLatencyNs.value_or(readLatencyNs));
}

uint64_t SimpleNicDatapath::getPayloadReadLatencyNsFn(
    const uint64_t addr,
    const size_t sizeBytes) const {
  XCHECK(latencyFns_.getPayloadReadLatencyNsFn)
      << "no payload-read latency function given";
  return latencyFns_.getPayloadReadLatencyNsFn(addr, sizeBytes);
}

uint64_t SimpleNicDatapath::getPayloadWriteLatencyNsFn(
    const uint64_t addr,
    const size_t sizeBytes) const {
  XCHECK(latencyFns_.getPayloadWriteLatencyNsFn)
      << "no payload-write latency function given";
  return latencyFns_.getPayloadWriteLatencyNsFn(addr, sizeBytes);
}

} // namespace packsim
