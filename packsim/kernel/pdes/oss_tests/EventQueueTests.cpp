/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "packsim/kernel/pdes/EventQueue.h"
#include "packsim/kernel/pdes/RawEvent.h"

namespace packsim::pdes {

namespace {
// One event per time, each with its index as sort id.
std::vector<std::unique_ptr<RawEvent>> makeEvents(
    const std::vector<uint64_t>& timesFs) {
  std::vector<std::unique_ptr<RawEvent>> events;
  events.reserve(timesFs.size());
  for (size_t i = 0; i < timesFs.size(); ++i) {
    auto& event = events.emplace_back(std::make_unique<RawEvent>());
    event->timeFs = timesFs[i];
    event->sortId = i;
  }
  return events;
}

void pushAll(
    EventQueue& queue,
    const std::vector<std::unique_ptr<RawEvent>>& events) {
  for (const auto& event : events) {
    queue.push(event.get());
  }
}

// Empties the queue, returning its events in the order popped.
std::vector<RawEvent*> popAll(EventQueue& queue) {
  std::vector<RawEvent*> popped;
  while (!queue.empty()) {
    popped.emplace_back(queue.top());
    queue.pop();
  }
  return popped;
}

std::pair<uint64_t, uint64_t> sortKey(const RawEvent* event) {
  return {event->timeFs, event->sortId};
}
} // namespace

TEST(EventQueue, PopsByTimeThenSortId) {
  const auto events = makeEvents({30, 10, 20, 10});
  EventQueue queue;
  pushAll(queue, events);

  const std::vector<RawEvent*> expected{
      events[1].get(), events[3].get(), events[2].get(), events[0].get()};
  EXPECT_EQ(popAll(queue), expected);
}

TEST(EventQueue, EraseRemovesOnlyThatEvent) {
  const auto events = makeEvents({50, 10, 40, 20, 30});
  EventQueue queue;
  pushAll(queue, events);

  queue.erase(events[1].get()); // the earliest
  queue.erase(events[2].get()); // one in the middle
  queue.erase(events[0].get()); // the latest

  EXPECT_EQ(events[1]->queueIndex, RawEvent::kNotQueued);
  const std::vector<RawEvent*> expected{events[3].get(), events[4].get()};
  EXPECT_EQ(popAll(queue), expected);
}

TEST(EventQueue, MatchesASortedSetUnderRandomPushesPopsAndErases) {
  // Any fixed seed keeps the operations, and so the test, the same every run.
  constexpr uint32_t kSeed = 7;
  std::mt19937 rng(kSeed);
  std::vector<std::unique_ptr<RawEvent>> events;
  std::vector<RawEvent*> queued;
  std::set<std::pair<uint64_t, uint64_t>> expectedKeys;
  EventQueue queue;

  for (int step = 0; step < 10'000; ++step) {
    // Push on half the steps, so the queue grows to dozens of events and many
    // erases have to move the last event above the erased one.
    const auto action = rng() % 4;
    if (action <= 1 || queued.empty()) {
      auto& event = events.emplace_back(std::make_unique<RawEvent>());
      // Few distinct times, so the sort id often decides.
      event->timeFs = rng() % 100;
      event->sortId = events.size();
      queue.push(event.get());
      queued.emplace_back(event.get());
      expectedKeys.emplace(sortKey(event.get()));
    } else if (action == 2) {
      RawEvent* const top = queue.top();
      ASSERT_EQ(sortKey(top), *expectedKeys.begin());
      queue.pop();
      expectedKeys.erase(expectedKeys.begin());
      std::erase(queued, top);
    } else {
      const size_t pick = rng() % queued.size();
      RawEvent* const erased = queued[pick];
      queue.erase(erased);
      expectedKeys.erase(sortKey(erased));
      queued[pick] = queued.back();
      queued.pop_back();
    }
    ASSERT_EQ(queue.size(), expectedKeys.size());
    if (!expectedKeys.empty()) {
      ASSERT_EQ(sortKey(queue.top()), *expectedKeys.begin());
    }
  }
}

} // namespace packsim::pdes
