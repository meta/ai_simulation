/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/container/F14Map.h>
#include <folly/container/HeterogeneousAccess.h>
#include <folly/lang/Hint.h>

#include "packsim/packet/base.h"
#include "packsim/packet/type_mapper.h"

namespace packsim {

namespace detail {

/**
 * One thread's spare packets, filed by type (see `GlobalManager`).
 *
 * Keeps at most `maxSparePerType` spares of each type and deletes a packet
 * returned past that, giving its memory back to the heap. A thread that frees
 * more packets than it takes, such as a switch thread dropping the last
 * reference an LLR replay buffer held, would otherwise keep every one: no
 * other thread takes from its pool.
 */
class PacketAllocator {
 public:
  // Default for `maxSparePerType`. A packet object is at most about 160 bytes,
  // so 4096 spares of one type stay under 1 MiB per thread, apart from the hop
  // lists that pooled ping packets keep.
  static constexpr size_t kMaxSparePerType = 4096;

  /**
   * @param maxSparePerType  Most spares kept of each packet type, e.g. 4096.
   */
  explicit PacketAllocator(size_t maxSparePerType = kMaxSparePerType)
      : maxSparePerType_{maxSparePerType} {}
  PacketAllocator(const PacketAllocator&) = delete;
  PacketAllocator& operator=(const PacketAllocator&) = delete;
  PacketAllocator(PacketAllocator&&) = delete;
  PacketAllocator& operator=(PacketAllocator&&) = delete;

  template <typename T>
  T* FOLLY_NONNULL pop() {
    auto& pool = objects_[typeid(T)];
    if (pool.empty()) {
      return new T();
    }
    auto ret = static_cast<T*>(pool.back());
    pool.pop_back();
    return ret;
  }

  /**
   * File a packet under its most-derived type.
   *
   * Keying on `typeid(*ptr)` rather than on a caller-supplied type is what
   * makes `pop<T>()`'s `static_cast<T*>` sound: a pointer only ever lands in
   * T's pool if the object it points at really is a T. A pool that already
   * holds `maxSparePerType` packets of that type deletes `ptr` instead.
   *
   * @param ptr  Packet to return to the pool.
   */
  void push(Packet* FOLLY_NONNULL ptr) {
    auto& pool = objects_[typeid(*ptr)];
    if (pool.size() >= maxSparePerType_) {
      delete ptr;
      return;
    }
    pool.emplace_back(ptr);
  }

  ~PacketAllocator() {
    for (auto& [_, pool] : objects_) {
      for (auto& p : pool) {
        delete p;
      }
    }
  }

 private:
  // Most spares kept of each packet type.
  const size_t maxSparePerType_;
  folly::F14FastMap<std::type_index, std::vector<Packet*>> objects_;
};
} // namespace detail

/**
 * A manager of packet memory for PackSim. This will allocate the memory
 * internally and let you reuse it. Make sure you reset the contents of the
 * allocated objects once you get a new pointer.
 *
 * NOTE: every thread has its own pool. A packet goes back to the pool of the
 * thread that returns it, which need not be the thread that took it, and a
 * pool keeps at most `detail::PacketAllocator::kMaxSparePerType` spares of
 * each type.
 */
class GlobalManager {
 public:
  GlobalManager() = delete;

  /**
   * Take a packet of type T from this thread's pool (actual packet type).
   *
   * @return Pointer to a packet of type T from the pool
   */
  template <typename T>
  static T* takePacket() {
    static_assert(std::is_convertible<T*, Packet*>::value);
    return getPoolManager()->pop<T>();
  }

  /**
   * Take a packet of packetType from this thread's pool (enum as template
   * argument).
   *
   * @return Pointer to a packet of the specified type from the pool
   */
  template <
      PacketType packetType,
      typename PacketCppType = typename PacketTypeMapper<packetType>::type>
  static PacketCppType* takePacket() {
    return getPoolManager()->pop<PacketCppType>();
  }

  /**
   * Return a packet previously taken using the methods above to this thread's
   * pool, which deletes it if it already holds its maximum of that type.
   *
   * The pool is keyed on the packet's C++ type, which it reads off the object
   * itself. It deliberately does not consult `pkt->pktType`: that is a mutable
   * public field callers overwrite (`createPacket` assigns it, and tests
   * reassign it to route a packet down a different code path), so it states
   * what a packet is being *treated* as, not what it *is*. Trusting it would
   * let a UdpPacket allocation be filed in RocePacket's pool and handed back
   * as a RocePacket*.
   *
   * @param pkt Pointer to the packet to return to the pool
   */
  static void returnPacket(Packet* FOLLY_NONNULL pkt) {
    getPoolManager()->push(pkt);
  }

 private:
  static detail::PacketAllocator* getPoolManager();
};

} // namespace packsim
