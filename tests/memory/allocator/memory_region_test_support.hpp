#pragma once

#include <cstddef>

namespace naru::tests::allocator {
enum class MemoryRegionState { FREE, RESERVED, COMMITTED, OTHER };

struct MemoryRegion {
    const void* pBaseAddr = nullptr;
    std::size_t size = 0;
    MemoryRegionState state = MemoryRegionState::OTHER;
};

MemoryRegion queryMemoryRegion(const void* pAddress);

// Only for allocator heaps with a contiguous committed prefix and a reserved tail.
// Linux reports protection mappings, not the identity of an original mmap reservation.
MemoryRegion queryCommittedHeap(const void* pAddress);
} // namespace naru::tests::allocator
