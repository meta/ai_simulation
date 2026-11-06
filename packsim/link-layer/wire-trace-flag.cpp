/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Implementation includes its own header first — keeps the DECLARE in
// `wire-trace-flag.h` and the DEFINE below in sync (e.g., on a type change),
// and matches this project's include grouping.
#include "packsim/link-layer/wire-trace-flag.h"

#include <folly/portability/GFlags.h>

#include "packsim/packet/base.h"
#include "packsim/packet/ethernet-frame.h"

namespace packsim {

namespace {
constexpr int kTraceLlrData = 0;

// The `llr` column reserves 0 for a data frame, so the control types start at
// 1. This mapping is written out rather than cast from `LlrCtlosType` because
// renumbering the column would break diffs against older capture files.
int traceType(const LlrCtlosType type) noexcept {
  switch (type) {
    case LlrCtlosType::LLR_ACK:
      return 1;
    case LlrCtlosType::LLR_NACK:
      return 2;
    case LlrCtlosType::LLR_INIT:
      return 3;
    case LlrCtlosType::LLR_INIT_ECHO:
      return 4;
  }
  return -1;
}
} // namespace

// The `as<>` here is guarded by an `is<>` test and does not use the
// `UseDynamicCast` overload. `PacketPtr::as<>` is annotated `FOLLY_NONNULL`
// for every instantiation, including that overload, so a `dynamic_cast` that
// legitimately returns null through it is undefined behaviour that only
// surfaces under a sanitizer build.
LlrTraceFields LlrTraceFields::extractFrom(
    const EthernetFrame& frame) noexcept {
  if (frame.pkt.is<LlrCtlosFrame>()) {
    const auto* ctlos = frame.pkt.as<LlrCtlosFrame>();
    return {.type = traceType(ctlos->payload.type), .seq = ctlos->payload.seq};
  }
  if (frame.llrSeq.has_value()) {
    return {.type = kTraceLlrData, .seq = *frame.llrSeq};
  }
  return {};
}

} // namespace packsim

// Per-packet wire-event trace. When true, every
// `EthernetChannel::startSerialize` (TX) and
// `EthernetPort::doReceive` (RX) emits a `TRACE-TX`/`TRACE-RX` line
// (defined in `eth-channel.cpp` / `eth-port.cpp`), plus the
// `TRACE-PERIODIC` / `TRACE-BYTE` / `TRACE-ENQ` sites in `Llr`. The wire
// driver's `TRACE-DRV` sites (`link-layer-scheduler.cpp`) key off the same
// flag and include `wire-trace-flag.h` to say so.
// Fixed-column format suitable for `diff` between two runs (e.g., two builds
// at the same seed with `--pdes_threads=1`). Off by default; turning on adds
// O(packets) log lines, so only enable for small per-seed runs.
//
// Hosted in this single-purpose translation unit so the gflag symbol is
// available to every link-layer user — the LLR code, the Ethernet port, and
// any future scheduler code, plus the test binaries that use only one of
// them.
FOLLY_GFLAGS_DEFINE_bool(
    packsim_trace_wire_events,
    false,
    "Emit per-packet wire-event TRACE lines for diffing wire-event order "
    "between two runs.");
