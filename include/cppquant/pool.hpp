#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <vector>

namespace cppquant {

struct PoolStats {
  std::size_t capacity{};
  std::size_t blocks_reserved{};
  std::size_t blocks_in_use{};
  std::size_t chunks{};
  /// Number of backing chunks obtained from the global allocator.
  std::size_t heap_allocations{};
};

/// A chunked fixed-block pool. All blocks are the same size and alignment, so
/// it is suitable for container nodes such as `std::list` links. The pool
/// reserves memory in chunks and never returns it to the global allocator
/// until destruction.
class PoolResource {
public:
  static constexpr std::size_t kBlockSize = 128;
  static constexpr std::size_t kDefaultCapacity = 1U << 16;
  static constexpr std::size_t kDefaultChunkBlocks = 1U << 10;

  explicit PoolResource(std::size_t max_blocks = kDefaultCapacity,
                        std::size_t blocks_per_chunk = kDefaultChunkBlocks);

  ~PoolResource();

  PoolResource(const PoolResource&) = delete;
  PoolResource& operator=(const PoolResource&) = delete;
  PoolResource(PoolResource&&) = delete;
  PoolResource& operator=(PoolResource&&) = delete;

  void* allocate();
  void deallocate(void* block) noexcept;
  void reset() noexcept;

  const PoolStats& stats() const noexcept {
    return stats_;
  }

private:
  struct Chunk {
    std::byte* memory{};
    std::size_t blocks{};
  };

  void grow();

  std::vector<Chunk> chunks_;
  void* free_list_{};
  std::size_t blocks_per_chunk_{};
  PoolStats stats_{};
};

/// Allocator adapter that forwards to a shared `PoolResource`.
template <typename T> class PoolAllocator {
public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::false_type;

  PoolAllocator() noexcept = default;
  explicit PoolAllocator(PoolResource* resource) noexcept
      : resource_(resource) {}

  template <typename U>
  PoolAllocator(const PoolAllocator<U>& other) noexcept
      : resource_(other.resource()) {}

  T* allocate(std::size_t count) {
    static_assert(sizeof(T) <= PoolResource::kBlockSize,
                  "pool block is too small for this node type");
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "pool blocks are not over-aligned");
    if (count != 1 || resource_ == nullptr) {
      throw std::bad_alloc();
    }
    return static_cast<T*>(resource_->allocate());
  }

  void deallocate(T* pointer, std::size_t count) noexcept {
    if (count == 1) {
      resource_->deallocate(pointer);
    }
  }

  template <typename U>
  bool operator==(const PoolAllocator<U>& other) const noexcept {
    return resource_ == other.resource();
  }

  template <typename U>
  bool operator!=(const PoolAllocator<U>& other) const noexcept {
    return !(*this == other);
  }

  PoolResource* resource() const noexcept {
    return resource_;
  }

  template <typename U> struct rebind {
    using other = PoolAllocator<U>;
  };

private:
  PoolResource* resource_{};
};

} // namespace cppquant
