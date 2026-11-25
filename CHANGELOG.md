# Changelog

Notable changes to this repository are recorded here, newest release first, in
the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
repository is published continuously from Meta's internal tree and has no
numbered releases yet, so changes collect under Unreleased; the commit history
on `main` has the detail.

## [Unreleased]

### Added

- The `dataflow` workload in `packsim/if/workload.thrift`: each rank runs a
  program of RECV, SEND, COMPUTE and JOIN operations in queues, and an
  operation starts once the operations it depends on have completed. It
  describes a collective algorithm message by message, with only the waits the
  algorithm has. Ranks in symmetric positions share one program through peers
  written relative to the rank. Each rank pair gets one queue pair for every
  channel up to the highest its operations use.
- `NicConfig.writeLatencyNs` in `packsim/if/nic_config.thrift`: the fixed
  latency of a NIC writing a received payload into its memory, for a transport
  that holds each received packet until its payload is written. Unset, nothing
  changes. RoCEv2 never charges it: a valid value has no effect with it, and a
  negative or non-finite one is rejected when the transport is built.
- PackSim, a packet-level, discrete-event simulator for AI training fabrics. It
  models endpoints and their NICs, RoCEv2 transport with Go-Back-N loss
  recovery and DCQCN congestion control, switches and their buffers, and links
  with link-layer retry and credit-based flow control. See
  `packsim/README.md`.
- A CMake build for Linux with a getdeps manifest, and a GitHub Actions
  workflow that builds PackSim and runs its unit tests on every push and pull
  request against `main`.
- A documentation site, rendered from `packsim/README.md` and
  `packsim/transport/DESIGN.md` and published with GitHub Pages.
- The BSD 3-Clause License, in `LICENSE`.
- `Duration`, a span of simulated time, in `packsim/kernel/duration.h`. It and
  `Time` do not convert to each other, so a function that takes a `Duration`
  rejects a `Time`, and the reverse. `Scheduler::scheduleAfter` and
  `SimContext::schedule` also accept a `Duration` delay; their `Time` overloads
  stay.
- `Scheduler::runUntil(Time)`, which runs the simulation up to a point in time.
  `Scheduler::run(Time)` runs for a span, and its zero runs until no events are
  left; `runUntil` at the current time runs nothing.
- `packsim/examples/make_example_config.py`, the Python script that writes
  `packsim/examples/incast-2host.json`, using nothing beyond the standard
  library.
- `Duration::fromUs(double, mode)`, which rounds a floating-point length with
  one of the `Rounding` rules: `TruncateToUnit` drops the fraction of a
  microsecond, `NearestFs` rounds to the nearest femtosecond, and `CeilFs` rounds
  up to the next one. A floating-point length has to name its rule:
  `Duration::fromUs(1.7)` and the other factories given a floating-point value
  do not compile.
- `Ticker`, in `packsim/kernel/ticker.h`, which raises a callback once per
  period of simulated time until it is stopped or destroyed, and rejects a
  zero period.
- `fmt` formatters for `Time` and `Duration`, which print the value in the
  largest unit it fills without losing a femtosecond: "1.5us", "300ns", "1fs",
  "0s". The Go-Back-N NACK debug log prints its times this way instead of as
  whole nanoseconds.
- `PortReachability::dstPortsOnFirstHopSwitch`, in
  `packsim/node/port-reachability.h`, which lists the destination ports the
  first-hop switch delivers to itself, without crossing another switch. A
  transport receives it as `Transport::PortTopology::firstHopDstPorts`, beside
  `reachableDstPorts`.
- `INicDatapath`, in `packsim/nic/nic-datapath.h`: the NIC hardware that
  times each packet a transport sends or receives, including the NIC's payload
  reads and writes. Every transport times its packets through one:
  `SimpleNicDatapath`, in `packsim/nic/simple-nic-datapath.h`, which keeps
  `NicConfig`'s flat memory, unless its owner installs another with
  `Transport::setNicDatapath`. `Transport::shareNicDatapath` makes the
  transports of one NIC, such as the planes of a `PerPlaneTransport`, reserve
  the same datapath. With `SimpleNicDatapath`, results do not change.
- `Message::writeAddr` in `packsim/transport/transport.h`: a RoCEv2 WRITE that
  carries one is handed to the receiving workload, once its last packet is in
  memory, as a message of its own with `isWrite`, `writeAddr`,
  `sendOrWriteData`, `sizeBytes`, `workloadId` and `dstRank` set. Per-message
  tracking does not record it as received. As with the RDMA Extended Transport
  Header (RETH) of RoCEv2, only its first packet carries the address: a
  `RoceWritePacket`, in `packsim/packet/roce.h`, of the new type
  `PacketType::ROCE_WRITE`. `PacketPtr::is<RocePacket>()` matches it, and a
  `pktType == PacketType::ROCE` test does not. Unset, nothing changes.
- `RoceV2TransportConfig.creditFlowControl` in
  `packsim/if/roce_v2_config.thrift`: credit-based flow control of RoCEv2
  SENDs and WRITEs with immediate. Every ACK reports the receives the
  acknowledging side has posted, and a SEND or WRITE with immediate the sender
  holds no credit for goes as its first packet alone, the rest following the
  next ACK. Plain WRITEs need no credit. Off by default, since whether real
  networks and hardware behave this way is not yet clear and may need
  validating. Off, nothing changes.
- `Message::samplePacketLatency`, in `packsim/transport/transport.h`: whether a
  transport may sample the one-way latency of the message's packets.
  `WorkloadRank::notify` clears it, so that the packets of a notify, which
  carry no data of their own, add no latency samples.
- `Tracer::isConditionalTraceEnabled(category)` and
  `Tracer::anyConditionalTraceEnabled(categoryPrefix)`, in
  `packsim/kernel/trace.h`: whether `logPeriodicConditional()` would schedule a
  series of the category, or of some category that starts with the prefix, so
  that a caller can skip building series that would never be traced.

### Changed

- `WorkloadRank::send()` and `recv()` no longer take an `endpointId`: the
  queue pair they use decides the NICs. A call that still passes one can
  compile with each later argument landing in the parameter after its own, so
  remove it.
- `RingNicSelectionMode::ALL_AVAILABLE_NICS` in `packsim/if/workload.thrift`
  stripes a ring edge's chunks over as many NICs as can each be paired with a
  distinct NIC of the peer, and leaves the rest unused. It used to reject the
  workload unless every NIC could be paired; it now rejects it only when no NIC
  pair has a path. Where every NIC can be paired, the selected paths are
  unchanged.
- `GoBackNRecovery::rnrFieldToTime()` is now `rnrFieldToDuration()` and
  returns a `Duration`: the RNR back-off is a span of time, not a point.
- `LlrConfig::dataAgeTimerMax` and `LlrConfig::pcsLostStatusTimerMax` in
  `packsim/link-layer/llr/llr-types.h` are now `std::optional<Time>`, empty
  meaning the timer is disabled, and a timer set to 0 in the config is left
  empty instead of becoming one hour. A disabled timer is never started, so a
  run with link-layer retry no longer keeps an event queued for each
  acknowledgement, and needs much less memory.
- Each thread keeps at most 4,096 spare packets of each type and frees any
  more it is given back, so packets freed on switch threads return to the
  heap. Runs with link-layer retry need much less memory at higher thread
  counts; results do not change.
- `WorkloadNode`, in `packsim/endpoint/runtime/workload-node.h`, can host
  several ranks of one workload; it used to reject a second rank of a workload
  on one node. A message goes to the rank it names, `dstRank` for a delivery
  or a READ's completion and `srcRank` for any other acknowledgement, and the
  workload completes on the node once every rank it has there has finished. A
  workload in a DAG still places at most one rank on a node, and a second
  collective barrier for it on one node is now rejected instead of replacing
  the first. A node that holds one rank of each workload behaves as before.
- `RoceSocketBase::fetchDataAndSend` takes the `Message` instead of its
  sequence number and WRITE fields, and whether the bytes start the message;
  an override must change with it.
- `Scheduler::EventHandle::cancel()` now removes the event from its context's
  queue at once, instead of leaving it queued until its time. A timer re-armed
  by cancelling and rescheduling its event no longer leaves one dead event per
  re-arm, so such runs need less memory; results do not change. For an event in
  its context's queue, the handle reads as stale at once, so `getEventTimeFs()`
  returns 0. Debug builds abort when a context cancels another context's event
  during a run.
- `RoceSocketBase::ackRcvd` takes the receive credit the ACK carries, and
  `emitAckPacket` the credit to report, as a third argument, and `isPsnOk`
  takes whether the received packet's message holds a posted receive, before
  `ackRequested`; an override or a caller must change with them.

### Removed

- `Transport::setGetPayloadReadLatencyNsFn`, `setGetPayloadWriteLatencyNsFn`,
  their function types, `readBytesFromMemory`, `writeBytesToMemory` and
  `setSharedMemory`. A transport reserves its NIC's datapath instead
  (`Transport::nicDatapath`), and an owner that sets payload latencies by
  address gives them to `SimpleNicDatapath`
  (`SimpleNicDatapath::PayloadLatencyFns`).
- `Scheduler::getTimeUntilEvent` in `packsim/kernel/scheduler.h` and
  `SimContext::getTimeUntilEvent` in `packsim/kernel/sim-context.h`, which
  nothing in PackSim calls any more. A pending event's due time is still on its
  handle, `Scheduler::EventHandle::getEventTimeFs()`.
- `RawEvent::isActive` in `packsim/kernel/pdes/Event.h`. `RawEvent::queueIndex`
  records the event's position in its context's queue, or that it was cancelled
  outside it.
- `PdesEngine::eventPool()` in `packsim/kernel/pdes/PdesEngine.h`.
  `EventPool::get()`, in `packsim/kernel/pdes/EventPool.h`, returns the same
  per-thread pool.

### Fixed

- A workload in a `WorkloadDag` (`packsim/if/workload.thrift`) now starts when
  some of its ranks run on nodes that none of its dependencies use. Those
  ranks waited for a dependency that never finished on their node, so the
  workload never started and the run reported its ranks incomplete. A
  workload still waits for each dependency on the nodes the two share. A DAG
  in which a workload shares no node with any of its dependencies, which
  never completed before, is now rejected, since the workload would wait for
  none of them.
- Collectives that send through `WorkloadRank::send()` and `recv()` -- the
  double binary tree and recursive algorithms, and the ring in
  `RingNicSelectionMode.SINGLE_NIC` -- send their data between the NICs of the
  NIC pair that owns its queue pair. They took the queue pair from the rank
  pair's first NIC pair but always sent from NIC 0 to the peer's NIC 0, so a
  workload whose first NIC pair is not (0, 0), such as one whose `qpTemplates`
  leave out NIC 0, sent its data on the wrong NICs. A workload whose first NIC
  pair is (0, 0) runs as before.
- The documentation site deploys again. The repository's GitHub organisation
  was renamed from `facebook` to `meta`, and the deploy job ran only in
  `facebook/ai_simulation`. The job, the site's configuration and the
  repository's links now name `meta/ai_simulation`, and the site's host
  `https://meta.github.io`.
- A workload rank no longer posts a new notify receive when a notification
  reaches it after it has completed.

### Changed

- A frame's link-layer retry sequence number now travels with that transmission
  in `EthernetFrame` (`packsim/packet/ethernet-frame.h`) instead of on the
  packet: `EthernetPacket::llrHeader`, `LlrHeader` and `Llr::stampLlrHeader`
  are removed, and `EthernetPort::receive` and the `EthernetChannel` send and
  receive calls take an `EthernetFrame`.

### Fixed

- Link-layer retry no longer misclassifies a frame when a replay resends one
  the next hop has already accepted. Each transmission now carries its own
  sequence number instead of every hop sharing one on the packet, so runs that
  replay accepted frames no longer depend on the number of simulation threads.
- A link-layer retry replay counts the ECN congestion-experienced and TTL marks
  the frame carried when it was first sent, instead of the packet's current
  marks: a CE mark added downstream, or a TTL mark a downstream hop-limit
  decrement cleared.

[Unreleased]: https://github.com/meta/ai_simulation/commits/main
