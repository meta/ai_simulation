/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <version>

#include <folly/CPortability.h>
#include <folly/CppAttributes.h>
#include <folly/logging/xlog.h>

#include "packsim/packet/allocator/GlobalManager.h"
#include "packsim/packet/base.h"
#include "packsim/packet/type_mapper.h"

namespace packsim {

class PacketPtr;

template <PacketType packetType>
PacketPtr createPacket(uint32_t payloadSize = 0);

/**
 * A shared, reference-counted handle to a packet taken from the global packet
 * pool. The count lives in the packet (`Packet::refs`), so a handle is one
 * pointer and creating a packet allocates nothing. The packet goes back to the
 * pool of the thread that drops the last handle.
 */
class PacketPtr {
 public:
  PacketPtr() noexcept = default;

  /* implicit */ PacketPtr(std::nullptr_t) noexcept {}

  PacketPtr(const PacketPtr& other) noexcept : pkt_(other.pkt_) {
    addRef();
  }

  PacketPtr(PacketPtr&& other) noexcept
      : pkt_(std::exchange(other.pkt_, nullptr)) {}

  PacketPtr& operator=(const PacketPtr& other) noexcept {
    if (pkt_ != other.pkt_) {
      other.addRef();
      release();
      pkt_ = other.pkt_;
    }
    return *this;
  }

  PacketPtr& operator=(PacketPtr&& other) noexcept {
    if (this != &other) {
      release();
      pkt_ = std::exchange(other.pkt_, nullptr);
    }
    return *this;
  }

  ~PacketPtr() {
    release();
  }

  packsim::Packet* operator->() const noexcept {
    return get();
  }

  packsim::Packet& operator*() const noexcept {
    return *get();
  }

  bool operator==(const PacketPtr& other) const noexcept {
    return get() == other.get();
  }

  bool operator==(std::nullptr_t) const noexcept {
    return get() == nullptr;
  }

  /**
   * Cast the internal pointer as pointer of another type using C++ type.
   *
   * NOTE: This function will crash if the internal pointer is NULL, only call
   * this on pointers that have been loaded with a packet.
   */
  template <typename PacketCppType, bool UseDynamicCast = false>
  PacketCppType* FOLLY_NONNULL as() const {
    Packet* ptr = get();
    if constexpr (getPacketTypeEnum<PacketCppType>() == PacketType::RAW) {
      return ptr;
    }
    if constexpr (UseDynamicCast) {
      return dynamic_cast<PacketCppType*>(ptr);
    }
    return static_cast<PacketCppType*>(ptr);
  }

  /**
   * Whether this holds a `PacketCppType` or something derived from one.
   *
   * Answered from the packet's own `pktType` via `PacketTypeMatcher`, not a
   * `dynamic_cast`, so it is cheap enough for the per-frame paths. A null
   * `PacketPtr` is nothing, including nothing derived from `Packet`.
   */
  template <typename PacketCppType>
  [[nodiscard]] bool is() const noexcept {
    Packet* ptr = get();
    return ptr != nullptr &&
        PacketTypeMatcher<PacketCppType>::matches(ptr->pktType);
  }

  /**
   * The packet as a `PacketCppType*`, or nullptr when it is not one.
   *
   * The nullable counterpart of `as()`, which is `FOLLY_NONNULL` and must only
   * be called once the type is known. Replaces writing the test and the cast
   * separately.
   */
  template <typename PacketCppType>
  [[nodiscard]] PacketCppType* tryAs() const noexcept {
    Packet* ptr = get();
    if (ptr != nullptr &&
        PacketTypeMatcher<PacketCppType>::matches(ptr->pktType)) {
      return static_cast<PacketCppType*>(ptr);
    }
    return nullptr;
  }

  /**
   * Cast the internal pointer as pointer of another type using enum.
   *
   * NOTE: This function will crash if the internal pointer is NULL, only call
   * this on pointers that have been loaded with a packet.
   */
  template <
      PacketType packetType,
      bool UseDynamicCast = true,
      typename PacketCppType = typename PacketTypeMapper<packetType>::type>
  PacketCppType* FOLLY_NONNULL as() const {
    Packet* ptr = get();
    if constexpr (packetType == PacketType::RAW) {
      return ptr;
    }
    if constexpr (UseDynamicCast) {
      return dynamic_cast<PacketCppType*>(ptr);
    }
    return static_cast<PacketCppType*>(ptr);
  }

  bool operator!() const noexcept {
    return pkt_ == nullptr;
  }

  FOLLY_ALWAYS_INLINE packsim::Packet* get() const noexcept {
    return pkt_;
  }

 private:
  template <PacketType packetType>
  friend PacketPtr createPacket(uint32_t payloadSize);

  // Takes over a packet just taken from the pool, as its only holder.
  explicit PacketPtr(Packet* FOLLY_NONNULL pkt) noexcept : pkt_(pkt) {
    pkt_->refs.count_.store(1, std::memory_order_relaxed);
#ifdef LOG_POOL_ALLOCATIONS
    XLOGF(
        INFO,
        "==> Took address {} from global pool.",
        static_cast<void*>(pkt_));
#endif
  }

  void addRef() const noexcept {
    if (pkt_ == nullptr) {
      return;
    }

    [[maybe_unused]] const uint16_t holders =
        pkt_->refs.count_.fetch_add(1, std::memory_order_relaxed);
    XDCHECK_LT(holders, std::numeric_limits<uint16_t>::max());
  }

  void release() noexcept {
    if (pkt_ == nullptr) {
      return;
    }

    if (pkt_->refs.count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      GlobalManager::returnPacket(pkt_);
#ifdef LOG_POOL_ALLOCATIONS
      XLOGF(
          INFO,
          "<== Returned address {} to global pool.",
          static_cast<void*>(pkt_));
#endif
    }
  }

  // The packet this handle holds, or nullptr.
  Packet* pkt_{nullptr};
};

/**
 * Use this to create a new packet loaded from global pool.
 */
template <PacketType packetType>
PacketPtr createPacket(uint32_t const payloadSize) {
  auto pktPtr = PacketPtr(GlobalManager::takePacket<packetType>());
  pktPtr.as<packetType, /*UseDynamicCast=*/false>()->reset();
  pktPtr->payloadSize = payloadSize;
  pktPtr->pktType = packetType;
  return pktPtr;
}

} // namespace packsim

template <>
struct std::hash<packsim::PacketPtr> {
  std::size_t operator()(const packsim::PacketPtr& ptr) const noexcept {
    return std::hash<packsim::Packet*>{}(ptr.get());
  }
};
