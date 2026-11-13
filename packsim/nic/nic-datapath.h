/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>

#include "packsim/kernel/time.h"

namespace packsim {

/**
 * The NIC hardware between a transport and the NIC's ports: the pipeline a
 * packet passes from the moment the transport hands it over to be sent until it
 * enters its egress port, and the pipeline a received packet passes before the
 * transport processes it, including the NIC's reads and writes of payloads in
 * its memory. The transport decides which packets to send and what a received
 * packet means; the datapath decides how long the NIC takes to move each one.
 *
 * Example usage, by a transport that sends a data packet:
 *
 *      const Time delay = datapath.reserveTx({
 *          .payloadRead = INicDatapath::PayloadAccess{
 *              .bytes = 4096, .addr = 0x2000000}});
 *      simCtx->schedule(delay, &MyTransport::sendToPort, this, pkt);
 */
class INicDatapath {
 public:
  // Payload bytes the NIC moves between a packet and its memory.
  struct PayloadAccess {
    // Bytes moved. An access of zero bytes moves no data, but still takes its
    // turn behind the accesses reserved before it.
    uint64_t bytes{0};
    // Where the payload is in the NIC's memory, when the transport knows it,
    // for example 0x2000000.
    std::optional<uint64_t> addr;
  };

  // A packet the transport hands to the NIC to send.
  struct TxPacket {
    // The payload the NIC reads from its memory for the packet, or none for a
    // packet whose bytes do not come from memory, such as an ACK.
    std::optional<PayloadAccess> payloadRead;
    // The packet's egress port, as its position from 0 among the ports
    // registered with the transport, when the transport chooses the port.
    std::optional<uint16_t> portSlot;
  };

  // A packet the NIC received for the transport.
  struct RxPacket {
    // The payload the NIC writes to its memory before the transport processes
    // the packet, or none for a packet whose payload is not written, such as an
    // ACK.
    std::optional<PayloadAccess> payloadWrite;
    // The port the packet arrived on, as its position from 0 among the ports
    // registered with the transport, when the transport knows it.
    std::optional<uint16_t> portSlot;
  };

  virtual ~INicDatapath() = default;

  /**
   * Reserve the NIC's transmit pipeline for one packet the transport sends. A
   * transport reserves its packets in the order it hands them over.
   *
   * @param packet  The packet.
   *
   * @return        Delay from now until the packet may enter its egress port.
   */
  [[nodiscard]] virtual Time reserveTx(const TxPacket& packet) = 0;

  /**
   * Reserve the NIC's receive pipeline for one packet the NIC received. A
   * transport reserves its packets in the order they arrive.
   *
   * @param packet  The packet.
   *
   * @return        Delay from now until the transport may process the packet,
   *                or std::nullopt when the NIC hands it over as it arrives.
   */
  [[nodiscard]] virtual std::optional<Time> reserveRx(
      const RxPacket& packet) = 0;
};

} // namespace packsim
