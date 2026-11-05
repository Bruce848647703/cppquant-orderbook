#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace cppquant {

/// A bounded single-producer/single-consumer ring buffer.
///
/// `Capacity` must be a power of two. One thread may call `try_push` while one
/// other thread calls `try_pop`; calling either method from multiple threads
/// is undefined. The queue stores elements in place and destroys them on pop
/// or destruction.
template <typename T, std::size_t Capacity> class SpscQueue {
public:
  static_assert(Capacity > 0, "queue capacity must be positive");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "queue capacity must be a power of two");

  static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }
  static constexpr std::size_t kMask = Capacity - 1;
  static constexpr std::size_t kCacheLine = 64;

  SpscQueue() noexcept = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  ~SpscQueue() {
    std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    while (head != tail) {
      reinterpret_cast<T*>(&storage_[head & kMask])->~T();
      ++head;
    }
  }

  /// Producer side. Returns false when the queue is full.
  bool try_push(const T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail - head == Capacity) {
      return false;
    }
    new (&storage_[tail & kMask]) T(value);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool try_push(T&& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail - head == Capacity) {
      return false;
    }
    new (&storage_[tail & kMask]) T(std::move(value));
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Consumer side. Returns false when the queue is empty.
  bool try_pop(T& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    T* slot = reinterpret_cast<T*>(&storage_[head & kMask]);
    value = std::move(*slot);
    slot->~T();
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Approximate number of elements. Safe to call from either side, but the
  /// value can change immediately.
  std::size_t size_approx() const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t head = head_.load(std::memory_order_acquire);
    return tail - head;
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  bool full() const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t head = head_.load(std::memory_order_acquire);
    return tail - head == Capacity;
  }

private:
  using Slot = std::aligned_storage_t<sizeof(T), alignof(T)>;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  alignas(kCacheLine) Slot storage_[Capacity];
};

} // namespace cppquant
