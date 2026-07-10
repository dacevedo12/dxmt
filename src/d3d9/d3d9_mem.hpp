/*
 * This file is part of DXMT.
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include "thread.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

/* On a 32-bit guest the CPU mirrors of MANAGED / SYSTEMMEM resources
   compete with the game for the small process address space, and a
   heap allocation holds its pages mapped forever. Backing the mirrors
   with pagefile file mappings lets an idle mirror release its address
   range while its bytes survive in the mapping object; consumers map
   a view for the duration of a lock or copy and unmap after. On a
   64-bit guest address space is not scarce, so the allocator degrades
   to plain heap allocations whose Map / Unmap are no-ops. This is
   DXVK's d3d9_mem allocator, kept structurally close to the source. */
#if defined(_WIN32) && !defined(_WIN64)
#define D3D9_ALLOW_UNMAPPING
#endif

#ifdef D3D9_ALLOW_UNMAPPING
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace dxmt {

class D3D9MemoryAllocator;
class D3D9Memory;

#ifdef D3D9_ALLOW_UNMAPPING

class D3D9MemoryChunk;

constexpr uint32_t D3D9ChunkSize = 64 << 20;

struct D3D9MemoryRange {
  uint32_t offset;
  uint32_t length;
};

struct D3D9MappingRange {
  uint32_t refCount = 0;
  void *ptr = nullptr;
};

class D3D9MemoryChunk {
  friend D3D9MemoryAllocator;

public:
  D3D9MemoryChunk(D3D9MemoryAllocator *Allocator, uint32_t Size);
  ~D3D9MemoryChunk();

  D3D9MemoryChunk(const D3D9MemoryChunk &) = delete;
  D3D9MemoryChunk &operator=(const D3D9MemoryChunk &) = delete;
  D3D9MemoryChunk(D3D9MemoryChunk &&other) = delete;
  D3D9MemoryChunk &operator=(D3D9MemoryChunk &&other) = delete;

  D3D9MemoryAllocator *Allocator() const;

private:
  bool IsEmpty() const;
  uint32_t
  Size() const {
    return m_size;
  }

  D3D9Memory AllocLocked(uint32_t Size);
  void FreeLocked(D3D9Memory *Memory);
  void *MapLocked(D3D9Memory *memory, uint32_t &mappedSize);
  uint32_t UnmapLocked(D3D9Memory *memory);

  D3D9MemoryAllocator *m_allocator;
  uint32_t m_size;
  HANDLE m_mapping;
  std::vector<D3D9MemoryRange> m_freeRanges;
  std::vector<D3D9MappingRange> m_mappingRanges;
};

class D3D9Memory {
  friend D3D9MemoryChunk;
  friend D3D9MemoryAllocator;

public:
  D3D9Memory() {}
  ~D3D9Memory();

  D3D9Memory(const D3D9Memory &) = delete;
  D3D9Memory &operator=(const D3D9Memory &) = delete;

  D3D9Memory(D3D9Memory &&other);
  D3D9Memory &operator=(D3D9Memory &&other);

  explicit
  operator bool() const {
    return m_chunk != nullptr;
  }

  void Map();
  void Unmap();
  void *Ptr();

private:
  D3D9Memory(D3D9MemoryChunk *Chunk, size_t Offset, size_t Size);
  void Free();
  D3D9MemoryChunk *
  GetChunk() const {
    return m_chunk;
  }
  size_t
  GetOffset() const {
    return m_offset;
  }
  size_t
  GetSize() const {
    return m_size;
  }

  D3D9MemoryChunk *m_chunk = nullptr;
  void *m_ptr = nullptr;
  size_t m_offset = 0;
  size_t m_size = 0;
};

class D3D9MemoryAllocator {
  friend D3D9MemoryChunk;

public:
  D3D9MemoryAllocator();
  ~D3D9MemoryAllocator() = default;
  D3D9Memory Alloc(uint32_t Size);
  void Free(D3D9Memory *Memory);
  void *Map(D3D9Memory *Memory);
  void Unmap(D3D9Memory *Memory);
  uint32_t MappedMemory() const;
  uint32_t UsedMemory() const;
  uint32_t AllocatedMemory() const;
  uint32_t
  AllocationGranularity() const {
    return m_allocationGranularity;
  }
  uint32_t
  MappingGranularity() const {
    return m_mappingGranularity;
  }

private:
  void FreeChunk(D3D9MemoryChunk *Chunk);

  dxmt::mutex m_mutex;
  std::vector<std::unique_ptr<D3D9MemoryChunk>> m_chunks;
  std::atomic<size_t> m_mappedMemory = 0;
  std::atomic<size_t> m_allocatedMemory = 0;
  std::atomic<size_t> m_usedMemory = 0;
  uint32_t m_allocationGranularity;
  uint32_t m_mappingGranularity;
};

#else

class D3D9Memory {
  friend D3D9MemoryAllocator;

public:
  D3D9Memory() {}
  ~D3D9Memory();

  D3D9Memory(const D3D9Memory &) = delete;
  D3D9Memory &operator=(const D3D9Memory &) = delete;

  D3D9Memory(D3D9Memory &&other);
  D3D9Memory &operator=(D3D9Memory &&other);

  explicit
  operator bool() const {
    return m_ptr != nullptr;
  }

  void
  Map() {}
  void
  Unmap() {}
  void *
  Ptr() {
    return m_ptr;
  }

private:
  D3D9Memory(D3D9MemoryAllocator *pAllocator, size_t Size);
  void Free();

  D3D9MemoryAllocator *m_allocator = nullptr;
  void *m_ptr = nullptr;
  size_t m_size = 0;
};

class D3D9MemoryAllocator {
public:
  D3D9Memory Alloc(uint32_t Size);
  uint32_t MappedMemory() const;
  uint32_t UsedMemory() const;
  uint32_t AllocatedMemory() const;
  void
  NotifyFreed(uint32_t Size) {
    m_allocatedMemory -= Size;
  }

private:
  std::atomic<size_t> m_allocatedMemory = 0;
};

#endif

} // namespace dxmt
