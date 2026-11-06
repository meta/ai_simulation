/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>

#include <folly/portability/GFlags.h>

// Declaration of the `packsim_trace_wire_events` gflag, defined in
// `wire-trace-flag.cpp`. Every file that references
// `FLAGS_packsim_trace_wire_events` should `#include` this header, so that its
// dependency on that translation unit is expressed in the source rather than
// being link-time-only.
FOLLY_GFLAGS_DECLARE_bool(packsim_trace_wire_events);

namespace packsim {

struct EthernetFrame;

/**
 * Trace-line columns for TRACE-TX / TRACE-RX, fixed-column for stable
 * `sort | diff`:
 *
 *   T       : sim time at the event (ns; 14-digit zero-pad)
 *   src/dst : <flexid>/<port> of the port emitting the event
 *   pkt     : `pkt->pktType` as int
 *   llr     : LLR type as int: -1 none, 0 DATA, 1 ACK, 2 NACK, 3 INIT,
 *             4 INIT_ECHO
 *   seq     : LLR sequence number (0 if the frame carries none)
 *   size    : `pkt->getSize()` (bytes excluding preamble + IFG)
 *
 * The `llr` and `seq` columns are produced by `extractFrom()` below;
 * the rest are computed directly at each TRACE-TX / TRACE-RX call site.
 */
struct LlrTraceFields {
  // The `llr` column value, encoded as listed above. It is -1 when the frame
  // carries no LLR state: it is neither sent with an LLR sequence number nor
  // an `LlrCtlosFrame`.
  int type{-1};

  // The `seq` column value, 0 when the frame carries no sequence number.
  uint32_t seq{0};

  /**
   * Read the LLR trace columns out of `frame`.
   *
   * This is a static member rather than a free function so that the header
   * declares none, and the definition lives in `wire-trace-flag.cpp` so that
   * the header can forward-declare `EthernetFrame` instead of pulling
   * `packet/base.h` and `packet/ethernet-frame.h` into every consumer that
   * only needs the gflag.
   *
   * @param frame  Any frame. One that carries no LLR state is safe to pass
   *               and yields the defaults.
   * @return       The columns for an `LlrCtlosFrame` or a frame sent with an
   *               LLR sequence number, otherwise `{-1, 0}`.
   */
  static LlrTraceFields extractFrom(const EthernetFrame& frame) noexcept;
};

} // namespace packsim
