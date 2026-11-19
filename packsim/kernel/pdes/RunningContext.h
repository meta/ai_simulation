/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace packsim::pdes {

struct SimulationContext;

/**
 * The context whose events the calling thread is running, for code that cannot
 * depend on the engine, such as Event::cancel(). Each engine worker thread
 * names, once when it starts, the field in which it records its current
 * context, and get() reads that field.
 *
 * Example usage:
 *
 *      // On an engine worker thread, when it starts:
 *      RunningContext::trackThisThread(worker.currentContext);
 *      ...
 *      const SimulationContext* runningContext = RunningContext::get();
 *
 * NOTE: Each thread tracks its own field; on a thread that tracks none, get()
 * returns nullptr.
 */
class RunningContext {
 public:
  /**
   * Make get() on the calling thread read `currentContext`, which must stay
   * alive for as long as the thread calls get().
   *
   * @param currentContext  Field in which the calling worker records the
   *                        context whose events it runs.
   */
  static void trackThisThread(
      SimulationContext* const& currentContext) noexcept;
  // Rejects a temporary, which would be gone before get() reads it.
  static void trackThisThread(SimulationContext* const&&) = delete;

  /**
   * Returns the context whose events the calling thread is running.
   *
   * @return  That context; nullptr between runs, off the pool, and while a
   *          worker drains its backlog.
   */
  static const SimulationContext* get() noexcept;
};

} // namespace packsim::pdes
