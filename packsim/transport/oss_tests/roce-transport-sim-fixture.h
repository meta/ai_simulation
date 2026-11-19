/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>

#include <gtest/gtest.h>

#include "packsim/if/gen-cpp2/config_types.h"
#include "packsim/kernel/device-id.h"
#include "packsim/kernel/sim-context.h"
#include "packsim/kernel/time.h"
#include "packsim/packet/ptr/PacketPtr.h"
#include "packsim/transport/roce-base.h"
#include "packsim/transport/transport.h"

namespace packsim {

/*
 * A NIC config and simulation context sized so a RoCE transport can actually
 * run: 400Gbps, buffers large enough that nothing back-pressures, DCQCN
 * enabled, and a loopback wire to connect two transports with. Shared by the
 * RoCEv2 suites in this directory.
 */

class RoceTransportSimTest : public ::testing::Test {
 public:
  void SetUp() override {
    // 1us lookahead so DCQCN timers can fire and the sim can drain.
    scheduler_.setLookaheadTimeFs(1'000'000'000);
    nicCfg_.writeEff() = 100;
    nicCfg_.readGbps() = 400;
    nicCfg_.readLatencyNs() = 100;
    nicCfg_.perCosBufferSize() = {1024 * 1024 * 1024, 1024 * 1024 * 1024};
    nicCfg_.perCosXoff() = {1000 * 1024 * 1024, 1000 * 1024 * 1024};
    nicCfg_.perCosXon() = {1000 * 1024 * 1024, 1000 * 1024 * 1024};
    nicCfg_.speed() = "400Gbps";
    *nicCfg_.dcqcnCfg()->dcqcnEnable() = true;
  }

 protected:
  // One-way delay of the loopback wire.
  static constexpr uint64_t kWireDelayNs = 100;

  /**
   * Deliver @p pkt, which @p from sent, to @p to after kWireDelayNs.
   *
   * @param from  The transport that sent the packet.
   * @param to    The transport at the other end of the wire.
   * @param pkt   The packet.
   *
   * @return      True: the wire takes every packet.
   */
  bool deliver(RoceTransportBase& from, Transport& to, const PacketPtr& pkt) {
    from.onPacketSent(pkt, nullptr);
    simCtx_.schedule(
        Time::fromNs(kWireDelayNs),
        &Transport::onPacketRcvd,
        &to,
        pkt,
        nullptr);
    return true;
  }

  /**
   * Make a send function for @p from that delivers every packet to @p to after
   * kWireDelayNs.
   *
   * @param from  The transport that sends.
   * @param to    The transport at the other end of the wire.
   *
   * @return      The function, for from.setSendPacketFn().
   */
  Transport::SendPacketFn wire(RoceTransportBase& from, Transport& to) {
    return [this, &from, &to](const PacketPtr& pkt) {
      return deliver(from, to, pkt);
    };
  }

  Tracer::ConditionalMap conditionalMap_;
  Scheduler scheduler_;
  SimContext simCtx_{&scheduler_, "SimContext", 0, &conditionalMap_};
  FlexDeviceId srcId_{{1, 0, 0, 1}};
  FlexDeviceId dstId_{{1, 0, 0, 2}};
  packsim::thrift::NicConfig nicCfg_;
};

} // namespace packsim
