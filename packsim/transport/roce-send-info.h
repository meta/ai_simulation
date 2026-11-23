/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>

namespace packsim {

/**
 * Per-packet send metadata recorded by RoCE sockets and replayed by the
 * Go-Back-N error-recovery component. Kept in its own leaf header (rather than
 * roce-base.h) so the error-recovery component can use the complete type
 * without including roce-base.h, which itself includes error-recovery.h.
 */
struct RoceSendInfo {
  uint32_t packetSize{0};
  bool isFirstPkt{false};
  bool isLastPkt{false};
  bool isControlPkt{false};
  bool isAck{false};
  bool isCnp{false};
  bool isNack{false};
  bool isRetransmit{false};
  bool isWriteImm{false};
  bool isWrite{false};
  uint8_t nackTime{UINT8_MAX};
  // In an ACK, the acknowledging side's receive credit (see
  // RoceV2TransportConfig.creditFlowControl).
  uint16_t recvCredit{0};
  uint32_t psn{0};
  uint32_t sequenceNum{UINT32_MAX};
  bool callbackOnSend{false};
  // Whether this packet carries its WRITE's destination (see
  // Message::writeAddr), as the first packet of a WRITE that has one. Unlike
  // isFirstPkt, it is not set again on the first packet of a later send chunk.
  bool carriesWriteDest{false};
  // With credit-based flow control, whether this packet asks for an ACK as the
  // first packet of a send chunk, so the sender learns the receiver's credit
  // early.
  bool creditAckReq{false};
  uint32_t writeImmData{0};
  uint32_t sendOrWriteData{0};

  // Whether this data packet asks the receiver for an ACK: the last packet of
  // every message does, and creditAckReq adds the first packet of a send chunk.
  bool asksForAck() const noexcept {
    return isLastPkt || creditAckReq;
  }
};

} // namespace packsim
