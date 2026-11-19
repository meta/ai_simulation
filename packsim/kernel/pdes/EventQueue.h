/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <folly/CPortability.h>
#include <folly/logging/xlog.h>

#include "packsim/kernel/pdes/Event.h"

namespace packsim::pdes {

/**
 * A context's pending events, earliest first: by time, then by sort id (see
 * EventComparator). A binary heap like std::priority_queue's, which can also
 * remove any queued event: each queued event holds its position in the heap
 * (RawEvent::queueIndex), and the queue rewrites it whenever the event moves.
 *
 * Example usage:
 *
 *      EventQueue queue;
 *      queue.push(event);
 *      queue.erase(event); // the event was cancelled
 *
 * NOTE: Not thread-safe. Only the worker that runs the context touches its
 * queue during a frame; between frames, the thread driving the engine does.
 */
class EventQueue {
 public:
  EventQueue() = default;

  EventQueue(const EventQueue&) = delete;
  EventQueue(EventQueue&&) = delete;
  EventQueue& operator=(const EventQueue&) = delete;
  EventQueue& operator=(EventQueue&&) = delete;

  ~EventQueue() = default;

  // True if no event is queued.
  bool empty() const noexcept {
    return heap_.empty();
  }

  // Number of queued events.
  size_t size() const noexcept {
    return heap_.size();
  }

  // The earliest queued event; the queue must not be empty.
  RawEvent* top() const noexcept {
    return heap_.front();
  }

  /**
   * Queue `event`, which must be neither queued nor cancelled.
   *
   * @param event  Event to queue.
   */
  void push(RawEvent* const event) {
    XDCHECK_EQ(event->queueIndex, RawEvent::kNotQueued);
    XDCHECK_LT(heap_.size(), RawEvent::kCancelled);
    heap_.emplace_back(event);
    siftUp(heap_.size() - 1, event);
  }

  /**
   * Remove the earliest event; the queue must not be empty.
   */
  void pop() noexcept {
    removeAt(0);
  }

  /**
   * Remove `event`, which must be in this queue.
   *
   * @param event  Queued event to remove.
   */
  void erase(RawEvent* const event) noexcept {
    const size_t index = event->queueIndex;
    XDCHECK_LT(index, heap_.size());
    XDCHECK_EQ(heap_[index], event);
    removeAt(index);
  }

 private:
  /**
   * Compare two events by when they run: by time, then by sort id.
   *
   * @param lhs  Event to compare.
   * @param rhs  Event to compare it with.
   *
   * @return     True if `lhs` runs before `rhs`.
   */
  static FOLLY_ALWAYS_INLINE bool runsBefore(
      RawEvent* const lhs,
      RawEvent* const rhs) noexcept {
    return EventComparator{}(rhs, lhs);
  }

  /**
   * Put `event` at `index` and record the position in the event.
   *
   * @param index  Position in the heap.
   * @param event  Event to put there.
   */
  FOLLY_ALWAYS_INLINE void place(
      const size_t index,
      RawEvent* const event) noexcept {
    heap_[index] = event;
    event->queueIndex = static_cast<uint32_t>(index);
  }

  /**
   * Remove the event at `index`, which must be a queued position.
   *
   * @param index  Position of the event to remove.
   */
  FOLLY_ALWAYS_INLINE void removeAt(const size_t index) noexcept {
    RawEvent* const last = heap_.back();
    heap_[index]->queueIndex = RawEvent::kNotQueued;
    heap_.pop_back();
    if (index == heap_.size()) {
      return;
    }
    // Move the hole down to a leaf along the earlier child, then bring the last
    // event up from there, past `index` if the order needs it: one comparison
    // per level on the way down, as std::pop_heap does, where a plain sift-down
    // needs two.
    size_t hole = index;
    const size_t size = heap_.size();
    for (size_t child = 2 * hole + 1; child < size; child = 2 * hole + 1) {
      if (child + 1 < size && runsBefore(heap_[child + 1], heap_[child])) {
        ++child;
      }
      place(hole, heap_[child]);
      hole = child;
    }
    siftUp(hole, last);
  }

  /**
   * Place `event`, now in a hole at `index`, by moving later parents down.
   *
   * @param index  Position of the hole.
   * @param event  Event to place.
   */
  void siftUp(size_t index, RawEvent* const event) noexcept {
    while (index > 0) {
      const size_t parent = (index - 1) / 2;
      if (!runsBefore(event, heap_[parent])) {
        break;
      }
      place(index, heap_[parent]);
      index = parent;
    }
    place(index, event);
  }

  // Binary heap, earliest event at the front.
  std::vector<RawEvent*> heap_;
};

} // namespace packsim::pdes
