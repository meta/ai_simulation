/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/logging/xlog.h>

namespace packsim::pdes::detail {

/**
 * Pool of reusable objects of type `T`: pop() hands out a pooled object, or a
 * new one when the pool is empty, and push() resets an object and keeps it for
 * reuse. The pool owns the objects it holds and deletes them when it is
 * destroyed.
 *
 * NOTE: Not thread-safe. PdesEngine keeps one pool per thread (see
 * PdesEngine::eventPool()).
 */
template <typename T>
class ObjectAllocator {
 public:
  ObjectAllocator() = default;

  ObjectAllocator(const ObjectAllocator&) = delete;
  ObjectAllocator(ObjectAllocator&&) = delete;
  ObjectAllocator& operator=(const ObjectAllocator&) = delete;
  ObjectAllocator& operator=(ObjectAllocator&&) = delete;

  /**
   * Take an object from the pool, or allocate one if the pool is empty.
   *
   * @return  The object, which the caller owns until it pushes it back.
   */
  T* FOLLY_NONNULL pop() {
    if (objects_.empty()) {
      return new T();
    }
    T* ret = objects_.back();
    objects_.pop_back();
    return ret;
  }

  /**
   * Reset the object `ptr` points to, keep it in the pool and clear `ptr`. The
   * caller's pointer is cleared before anything can throw, so a failed append
   * (out of memory) leaks the object rather than leaving the caller holding a
   * reset one.
   *
   * @param ptr  Pointer to the object to recycle; must not be null.
   */
  void push(T*& ptr) {
    XCHECK(ptr != nullptr);
    T* const object = std::exchange(ptr, nullptr);
    object->reset();
    objects_.emplace_back(object);
  }

  ~ObjectAllocator() {
    for (auto& p : objects_) {
      delete p;
    }
  }

 private:
  // Objects ready for reuse; pop() takes the one pushed last.
  std::vector<T*> objects_;
};

} // namespace packsim::pdes::detail
