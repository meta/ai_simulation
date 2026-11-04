# Changelog

Notable changes to this repository are recorded here, newest release first, in
the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
repository is published continuously from Meta's internal tree and has no
numbered releases yet, so changes collect under Unreleased; the commit history
on `main` has the detail.

## [Unreleased]

### Added

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

### Changed

- `WorkloadRank::send()` and `recv()` no longer take an `endpointId`: the
  queue pair they use decides the NICs. A call that still passes one can
  compile with each later argument landing in the parameter after its own, so
  remove it.

### Fixed

- Collectives that send through `WorkloadRank::send()` and `recv()` -- the
  double binary tree and recursive algorithms, and the ring in
  `RingNicSelectionMode.SINGLE_NIC` -- send their data between the NICs of the
  NIC pair that owns its queue pair. They took the queue pair from the rank
  pair's first NIC pair but always sent from NIC 0 to the peer's NIC 0, so a
  workload whose first NIC pair is not (0, 0), such as one whose `qpTemplates`
  leave out NIC 0, sent its data on the wrong NICs. A workload whose first NIC
  pair is (0, 0) runs as before.

[Unreleased]: https://github.com/facebook/ai_simulation/commits/main
