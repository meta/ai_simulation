/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/json/dynamic.h>

#include "packsim/kernel/pdes/Event.h"
#include "packsim/kernel/time.h"

namespace packsim {

struct SimContext;

struct TraceValue {
  uint64_t timeNs;
  double value;
};

class Tracer {
 public:
  using ConditionalMap = const folly::F14FastMap<std::string, uint64_t>;
  /**
   * @param simCtx          The context whose clock stamps the traces.
   * @param conditionalMap  Conditional-trace interval in ns per category
   *                        (`Simulation.conditionalTraceIntervalsNs`), or null
   *                        when conditional traces are not used.
   *
   * @throw std::invalid_argument if an interval in `conditionalMap` is 0.
   */
  explicit Tracer(SimContext* simCtx, ConditionalMap* conditionalMap);
  void trace(const std::string& name, double value);

  void logPeriodic(
      const std::string& name,
      const Time interval,
      const std::function<double()>& getVal);
  void logPeriodicConditional(
      const std::string& category,
      const std::string& name,
      const std::function<double()>& getVal);
  /**
   * Whether logPeriodicConditional() would schedule a series of `category`:
   * the category has an interval in the conditional map, or
   * --all_conditional_traces_interval_us is not 0.
   *
   * @param category  A conditional-trace category, e.g. "transport-qwnd".
   *
   * @return          True when such a series would be scheduled.
   * @throw std::runtime_error if the tracer has no conditional map, as
   *        logPeriodicConditional() does.
   */
  [[nodiscard]] bool isConditionalTraceEnabled(
      const std::string& category) const;
  /**
   * Whether isConditionalTraceEnabled() holds for some category that starts
   * with `categoryPrefix`, so that a caller can skip building a family of
   * series of which none would be scheduled.
   *
   * @param categoryPrefix  e.g. "transport-".
   *
   * @return                True when a series of some such category would be
   *                        scheduled.
   * @throw std::runtime_error if the tracer has no conditional map.
   */
  [[nodiscard]] bool anyConditionalTraceEnabled(
      const std::string& categoryPrefix) const;
  /**
   * Trace `getVal()` under `name` every `interval`, starting at the next
   * multiple of `interval`, or now when the current time is one.
   *
   * @param name      Name of the trace series.
   * @param interval  Time between samples; at least 1 ns.
   * @param getVal    Returns the value to record.
   *
   * @throw std::invalid_argument if `interval` is under 1 ns.
   */
  void startLogPeriodicSnapToInterval(
      const std::string& name,
      const Time interval,
      const std::function<double()>& getVal);
  void cancelPeriodic(const std::string& name);
  void tryCancelPeriodic(const std::string& name);
  void registerSnapshot(
      const std::string& name,
      const std::function<double()>& getVal);

  folly::F14FastMap<std::string, double> getSnapshot();

  struct TraceEvent {
    void end(const Time now);
    std::string name;
    std::string category;
    uint64_t pid{};
    uint64_t tid{};
    folly::dynamic args;
    uint64_t startTimeUs{};
    uint64_t endTimeUs{UINT64_MAX};
  };

  TraceEvent* startEvent(
      const std::string& name,
      const std::string& category,
      uint64_t pid,
      uint64_t tid,
      folly::dynamic args = {});

  folly::dynamic getTraceEvents();

  const folly::F14FastMap<std::string, std::vector<TraceValue>>& traces()
      const noexcept {
    return traces_;
  }

 private:
  // The interval at which logPeriodicConditional() traces a series of
  // `category`, or nullopt when it traces none. Needs a conditional map.
  [[nodiscard]] std::optional<Time> getConditionalInterval(
      const std::string& category) const;

  SimContext* const simCtx_;
  ConditionalMap* const conditionalMap_{nullptr};
  folly::F14FastMap<std::string, std::vector<TraceValue>> traces_;
  folly::F14FastMap<std::string, pdes::Event> periodicEvents_;
  folly::F14FastMap<std::string, std::function<double()>> snapshotFunctions_;
  std::vector<std::unique_ptr<TraceEvent>> traceEvents_;
  folly::F14FastMap<std::tuple<uint64_t, uint64_t>, TraceEvent*> lastEvent_;
};

} // namespace packsim
