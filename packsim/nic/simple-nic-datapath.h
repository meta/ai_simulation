/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "packsim/if/gen-cpp2/nic_config_types.h"
#include "packsim/kernel/time.h"
#include "packsim/nic/nic-datapath.h"
#include "packsim/nic/simple-memory.h"

namespace packsim {

struct SimContext;

/**
 * A NIC datapath whose only cost is one memory with a flat latency and
 * bandwidth (SimpleMemory), set by the NIC's configuration: NicConfig.readGbps
 * for reads and writes alike, NicConfig.readLatencyNs for reads, and
 * NicConfig.writeLatencyNs, or readLatencyNs when that is unset, for writes.
 * Reads complete in the order they are reserved, and so do writes. A packet
 * that moves no payload passes in no time.
 *
 * Its owner may give it functions that set an access's fixed latency by the
 * address it touches, for a memory whose latency depends on where the payload
 * lives; the bandwidth stays NicConfig.readGbps.
 */
class SimpleNicDatapath final : public INicDatapath {
 public:
  // Fixed latency in ns of this NIC reading `sizeBytes` of payload at `addr`
  // in its own memory. `sizeBytes` is never zero.
  using GetPayloadReadLatencyNsFn =
      std::function<uint64_t(uint64_t addr, size_t sizeBytes)>;

  // Fixed latency in ns of this NIC writing `sizeBytes` of received payload at
  // `addr` in its own memory. `sizeBytes` is never zero.
  using GetPayloadWriteLatencyNsFn =
      std::function<uint64_t(uint64_t addr, size_t sizeBytes)>;

  // Fixed latency in ns of this NIC writing `sizeBytes` of received payload at
  // `addr` in its own memory, into the memory region `regionKey` names.
  // `sizeBytes` is never zero.
  using GetRegionWriteLatencyNsFn = std::function<
      uint64_t(uint64_t addr, size_t sizeBytes, uint32_t regionKey)>;

  // The fixed latencies of payload accesses by address. An empty read or
  // write function keeps the flat latency for that kind of access, and an
  // empty region write function leaves every write to the write function.
  struct PayloadLatencyFns {
    // Takes the place of NicConfig.readLatencyNs for a read of at least one
    // byte whose address the transport knows.
    GetPayloadReadLatencyNsFn getPayloadReadLatencyNsFn;
    // Takes the place of the NIC's write latency for a write of at least one
    // byte whose address the transport knows. Given, it also makes the NIC
    // charge every received payload write.
    GetPayloadWriteLatencyNsFn getPayloadWriteLatencyNsFn;
    // Takes the place of getPayloadWriteLatencyNsFn for a write that names a
    // memory region (PayloadAccess::regionKey). Read only when
    // getPayloadWriteLatencyNsFn is given.
    GetRegionWriteLatencyNsFn getRegionWriteLatencyNsFn;
  };

  /**
   * @param simCtx      The NIC's simulation context; it must outlive the
   *                    datapath.
   * @param nicConfig   The NIC's configuration; read only here.
   * @param latencyFns  The fixed latencies of payload accesses by address. The
   *                    datapath keeps a copy of each function.
   *
   * @throw std::invalid_argument  NicConfig.writeLatencyNs is set and is
   *                               negative or not finite.
   */
  SimpleNicDatapath(
      const SimContext& simCtx,
      const packsim::thrift::NicConfig& nicConfig,
      PayloadLatencyFns latencyFns = {});

  /**
   * @param packet  The packet.
   *
   * @return        Delay until the packet's payload read completes, after the
   *                reads reserved before it: zero for a packet that reads no
   *                payload. A read of at least one byte at a known address
   *                takes its fixed latency from the read latency function, when
   *                there is one; every other read, one of zero bytes included,
   *                takes NicConfig.readLatencyNs.
   */
  [[nodiscard]] Time reserveTx(const TxPacket& packet) override;

  /**
   * @param packet  The packet.
   *
   * @return        Delay until the packet's payload write completes, after the
   *                writes reserved before it, or std::nullopt for a packet that
   *                writes no payload, or when neither a write latency function
   *                nor NicConfig.writeLatencyNs is set: then nothing is
   *                reserved and the payload counts as written on arrival. A
   *                write of zero bytes takes no fixed latency. A write of at
   *                least one byte at a known address, when there is a write
   *                latency function, takes the region write function's
   *                latency if it names a region and that function is given,
   *                and the write latency function's otherwise. Any other write
   *                of at least one byte takes the NIC's write latency.
   */
  [[nodiscard]] std::optional<Time> reserveRx(const RxPacket& packet) override;

  // True when there is a write latency function, the only reader of a
  // payload write's address.
  [[nodiscard]] bool readsPayloadWriteAddresses() const noexcept override;

  /**
   * The flat memory a NIC's configuration describes, with nothing reserved:
   * the memory this datapath times its packets with, and what a transport
   * sizes its queues by.
   *
   * @param nicConfig  The NIC's configuration.
   *
   * @return           The memory.
   * @throw std::invalid_argument  NicConfig.writeLatencyNs is set and is
   *                               negative or not finite.
   */
  [[nodiscard]] static SimpleMemory makeMemory(
      const packsim::thrift::NicConfig& nicConfig);

 private:
  // Fixed latency of one payload read from the read latency function.
  // reserveTx() is its only caller and calls it only when there is one.
  [[nodiscard]] uint64_t getPayloadReadLatencyNsFn(
      uint64_t addr,
      size_t sizeBytes) const;

  // Fixed latency of one received payload write at a known address: from the
  // region write function when the write names a region and there is one,
  // else from the write latency function. reserveRx() is its only caller and
  // calls it only when there is a write latency function.
  [[nodiscard]] uint64_t getPayloadWriteLatencyNs(
      const PayloadAccess& write) const;

  const SimContext& simCtx_;
  // Whether a received payload write is charged: there is a write latency
  // function or NicConfig.writeLatencyNs is set.
  const bool chargesPayloadWrites_;
  const PayloadLatencyFns latencyFns_;
  SimpleMemory memory_;
};

} // namespace packsim
