/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "packsim/transport/transport.h"

#include <memory>
#include <utility>

#include <folly/logging/xlog.h>

#include "packsim/kernel/data-rate.h"
#include "packsim/nic/simple-nic-datapath.h"

namespace packsim {

Socket::Socket(
    Transport* transport,
    const FlexDeviceId& src,
    const FlexDeviceId& dst,
    const uint16_t srcPort,
    const uint32_t localQPN,
    const uint32_t remoteQPN,
    const packsim::workload::thrift::Workload* workload)
    : simCtx_(transport->getSimCtx()),
      transport_(transport),
      srcPort_(srcPort),
      conn_(std::make_unique<QPConnection>(src, dst, localQPN, remoteQPN)),
      workload_(workload) {
  if (src == dst) {
    XLOG(WARNING, "Transport is doing a loopback connection");
  }
}

Transport::Transport(
    SimContext* simCtx,
    const FlexDeviceId& id,
    const packsim::thrift::NicConfig* nicConfig)
    : simCtx_(simCtx),
      id_(id),
      nicConfig_(nicConfig),
      ownedNicDatapath_(
          std::make_unique<SimpleNicDatapath>(*simCtx, *nicConfig)),
      nicBps_(DataRate(*nicConfig->speed()).getBps()) {}

void Transport::setNicDatapath(std::unique_ptr<INicDatapath> datapath) {
  XCHECK(datapath != nullptr) << "a transport needs a NIC datapath";
  ownedNicDatapath_ = std::move(datapath);
  sharedNicDatapath_ = nullptr;
}

} // namespace packsim
