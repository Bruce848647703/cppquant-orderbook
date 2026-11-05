#include "cppquant/pool.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace cppquant {

PoolResource::PoolResource(std::size_t max_blocks, std::size_t blocks_per_chunk)
    : blocks_per_chunk_(blocks_per_chunk) {
  if (max_blocks == 0 || blocks_per_chunk == 0) {
    throw std::invalid_argument(
        "pool capacity and chunk size must be positive");
  }
  stats_.capacity = max_blocks;
  const std::size_t chunk_count = max_blocks / blocks_per_chunk +
                                  (max_blocks % blocks_per_chunk != 0 ? 1 : 0);
  chunks_.reserve(chunk_count);
}

PoolResource::~PoolResource() {
  for (const auto& chunk : chunks_) {
    ::operator delete(chunk.memory,
                      std::align_val_t{alignof(std::max_align_t)});
  }
}

void PoolResource::grow() {
  if (stats_.blocks_reserved >= stats_.capacity) {
    throw std::bad_alloc();
  }

  const std::size_t remaining = stats_.capacity - stats_.blocks_reserved;
  const std::size_t blocks = std::min(blocks_per_chunk_, remaining);
  auto* memory = static_cast<std::byte*>(::operator new(
      blocks * kBlockSize, std::align_val_t{alignof(std::max_align_t)}));

  chunks_.push_back(Chunk{memory, blocks});
  ++stats_.heap_allocations;
  ++stats_.chunks;

  for (std::size_t index = 0; index < blocks; ++index) {
    void* block = memory + index * kBlockSize;
    *static_cast<void**>(block) = free_list_;
    free_list_ = block;
  }
  stats_.blocks_reserved += blocks;
}

void* PoolResource::allocate() {
  if (free_list_ == nullptr) {
    grow();
  }
  void* block = free_list_;
  free_list_ = *static_cast<void**>(block);
  ++stats_.blocks_in_use;
  return block;
}

void PoolResource::deallocate(void* block) noexcept {
  if (block == nullptr) {
    return;
  }
  *static_cast<void**>(block) = free_list_;
  free_list_ = block;
  --stats_.blocks_in_use;
}

void PoolResource::reset() noexcept {
  free_list_ = nullptr;
  stats_.blocks_in_use = 0;

  for (const auto& chunk : chunks_) {
    for (std::size_t index = 0; index < chunk.blocks; ++index) {
      void* block = chunk.memory + index * kBlockSize;
      *static_cast<void**>(block) = free_list_;
      free_list_ = block;
    }
  }
}

} // namespace cppquant
