/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/link-layer/eth-channel.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

#include <folly/Utility.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>

#include "packsim/link-layer/eth-port.h"
#include "packsim/link-layer/wire-trace-flag.h"
#include "packsim/packet/base.h"
#include "packsim/packet/ptr/PacketPtr.h"

namespace packsim {

namespace {
// A PDES event keeps a callable of up to this many bytes inline
// (`pdes::RawEvent::func`), so scheduling it does not allocate.
constexpr std::size_t kInlineEventBytes = 48;
// The receive event `sendPacket` schedules stores the member-function pointer,
// the port and the frame. A frame that outgrows this makes every receive on
// every link allocate.
static_assert(
    sizeof(&EthernetPort::receive) + sizeof(EthernetPort*) +
            sizeof(EthernetFrame) <=
        kInlineEventBytes,
    "the receive event no longer fits inline in a PDES event");
static_assert(std::is_nothrow_move_constructible_v<EthernetFrame>);

// Per-packet TX trace emitter. Gated on `FLAGS_packsim_trace_wire_events`
// (default off).
void emitTraceTx(const EthernetFrame& frame, const EthernetPort& src) noexcept {
  if (!FLAGS_packsim_trace_wire_events) {
    return;
  }
  const PacketPtr& pkt = frame.pkt;
  const auto llr = LlrTraceFields::extractFrom(frame);
  XLOGF(
      INFO,
      "TRACE-TX T={:014} src={}/{} pkt={} llr={} seq={} size={}",
      src.getSimCtx()->nowNs(),
      src.getFlexDeviceId().toString(),
      src.getPortNum(),
      folly::to_underlying(pkt->pktType),
      llr.type,
      llr.seq,
      pkt->getSize());
}

packsim::thrift::FlexLinkDelayConfig makeFlexLinkDelayConfig(
    const uint64_t delayNs) {
  packsim::thrift::FlexLinkDelayConfig delayCfg;
  delayCfg.delayNs() = delayNs;
  return delayCfg;
}
} // namespace

EthernetChannel::EthernetChannel(
    const packsim::thrift::FlexLinkDelayConfig& delayCfg,
    const uint64_t flapDelayNs,
    const double log10Ber)
    : log10Ber_(log10Ber),
      ber_(std::pow(10, log10Ber)),
      flapDelayNs_(flapDelayNs),
      delayCfg_(delayCfg) {}

EthernetChannel::EthernetChannel(const uint64_t delayNs)
    : EthernetChannel(makeFlexLinkDelayConfig(delayNs), 0, 1) {}

void EthernetChannel::attach(EthernetPort* port) {
  if (numAttached_ >= kNumEnds) {
    throw std::runtime_error(
        "EthernetChannel::attach: both ends of the channel are already connected");
  }

  ends_.at(numAttached_++) = port;

  // Schedule flap if configured. This needs to be done in the context of each
  // EthernetPort and needs to be done on each end of the link at the
  // appropriate time
  if (flapDelayNs_ > 0) {
    auto ctx = port->getSimCtx();
    ctx->schedule(
        Time::fromNs(flapDelayNs_),
        &EthernetPort::setChannelState,
        port,
        ChannelState::DOWN);
  }
}

Time EthernetChannel::startSerialize(
    const EthernetFrame& frame,
    EthernetPort* FOLLY_NONNULL src) const {
  const uint64_t delayNs = *delayCfg_.delayNs();
  const uint64_t expDelayNs =
      src->getSimCtx()->random.sampleExponential(*delayCfg_.expDelayNs());
  // The caller (`LinkLayerScheduler`) holds the returned propDelay until
  // serialization actually completes and then passes it back via
  // `scheduleRemoteReceive`.
  emitTraceTx(frame, *src);
  return Time::fromNs(delayNs + expDelayNs);
}

Scheduler::EventHandle EthernetChannel::scheduleRemoteReceive(
    const EthernetFrame& frame,
    EthernetPort* FOLLY_NONNULL src,
    Time delayUntilReceive) const {
  XDCHECK(src == ends_[0] || src == ends_[1])
      << "src must be one of the two attached ports";
  auto* dst = (src == ends_[0]) ? ends_[1] : ends_[0];
  return sendPacket(frame, dst, delayUntilReceive);
}

Scheduler::EventHandle EthernetChannel::sendPacket(
    const EthernetFrame& frame,
    EthernetPort* FOLLY_NONNULL dst,
    Time pktDelay) const {
  // Always schedule receive - BER check happens in doReceive
  return dst->getSimCtx()->schedule(
      pktDelay, &EthernetPort::receive, dst, frame);
}

EthernetPort* FOLLY_NONNULL
EthernetChannel::getPeer(const EthernetPort& self) const {
  const bool isEnd0 = &self == ends_[0];
  if (!isEnd0 && &self != ends_[1]) {
    throw std::runtime_error(
        "EthernetChannel::getPeer: the port is not attached to this channel");
  }
  EthernetPort* const peer = isEnd0 ? ends_[1] : ends_[0];
  if (peer == nullptr || peer == &self) {
    throw std::runtime_error(
        "EthernetChannel::getPeer: no other port is attached at the far end");
  }
  return peer;
}

} // namespace packsim
