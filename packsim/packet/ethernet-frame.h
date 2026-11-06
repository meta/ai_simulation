/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>

#include "packsim/packet/ptr/PacketPtr.h"

namespace packsim {

/**
 * One transmission of a packet on one link: a data frame, a MAC control frame
 * or a control ordered set.
 *
 * Every hop on a packet's path shares the same `Packet` object, so anything
 * that belongs to a single transmission travels here instead: the sender
 * builds the frame when it pulls the packet for the wire, and the receive
 * event holds its own copy, so no other hop can change it in flight.
 */
struct EthernetFrame {
  // The packet the frame carries.
  PacketPtr pkt;
  // The LLR (Link Level Retry) sequence number the sender assigned to this
  // transmission (UEC 1.0.3 §5.1); nullopt for an LLR-ineligible frame and
  // for every control ordered set. It rides in the preamble, so it adds
  // nothing to the frame size.
  std::optional<uint32_t> llrSeq;
};

} // namespace packsim
