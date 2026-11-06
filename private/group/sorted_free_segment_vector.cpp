#include "sorted_free_segment_vector.hpp"

#include <new>

#include "memory_config.hpp"
#include "naru/support/log.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::allocator::group {
SortedFreeSegVector::SortedFreeSegVector(const InitInfo& info)
    : m_pVectorHeapBaseAddr(info.pVectorHeapBaseAddr), m_segCount(info.segCount),
      m_capacity(info.capacity), m_pageSize(info.pageSize),
      m_lastReservedPageIndex(info.lastReservedPageIndex),
      m_lastCommittedPageIndex(info.lastCommittedPageIndex), m_pCurrent(info.pCurrent) {
}

SortedFreeSegVector* SortedFreeSegVector::createPtr() {
    const std::size_t pageSize = platform::getCommonPageSize();
    NARU_CHECK(pageSize != 0);
    const std::size_t reserveCount = config::METADATA_RESERVE_SIZE / pageSize;
    const std::size_t commitCount =
        (config::METADATA_SORTED_FREE_SEG_COMMIT_SIZE + pageSize - 1) / pageSize;
    NARU_CHECK(commitCount != 0);
    NARU_CHECK(commitCount <= reserveCount);

    const platform::VmMemoryInfo vmInfo{
        .pageSize = pageSize,
        .reservePageCount = reserveCount,
        .commitPageCount = commitCount,
    };

    std::byte* pBaseAddr = static_cast<std::byte*>(platform::reserveCommonMemory(vmInfo));
    platform::commitCommonMemory(pBaseAddr, vmInfo);

    constexpr std::size_t segmentOffset =
        (sizeof(SortedFreeSegVector) + alignof(Segment) - 1) / alignof(Segment) * alignof(Segment);
    const std::size_t committedSize = commitCount * pageSize;
    NARU_CHECK(segmentOffset <= committedSize);

    const InitInfo info{
        .pVectorHeapBaseAddr = pBaseAddr,
        .segCount = 0,
        .capacity = (committedSize - segmentOffset) / sizeof(Segment),
        .pageSize = pageSize,
        .lastReservedPageIndex = reserveCount - 1,
        .lastCommittedPageIndex = commitCount - 1,
        .pCurrent = reinterpret_cast<Segment*>(pBaseAddr + segmentOffset),
    };
    NARU_LOG(DEBUG,
              "Sorted free segment vector heap created: heap_base={} "
              "reserved_bytes={} committed_bytes={} capacity={}",
              static_cast<void*>(pBaseAddr), reserveCount * pageSize, commitCount * pageSize,
              info.capacity);

    return new (static_cast<void*>(pBaseAddr)) SortedFreeSegVector(info);
}

void SortedFreeSegVector::commitNewPageVectorHeap() {
    const std::size_t commitPageCount = config::COMMIT_SIZE / m_pageSize;
    const std::size_t remainingPageCount = m_lastReservedPageIndex - m_lastCommittedPageIndex;
    if (remainingPageCount < commitPageCount) {
        NARU_LOG(FATAL,
                  "Sorted free segment vector heap reservation exhausted: "
                  "heap_base={} requested_bytes={} remaining_bytes={} "
                  "capacity={}",
                  static_cast<void*>(m_pVectorHeapBaseAddr), commitPageCount * m_pageSize,
                  remainingPageCount * m_pageSize, m_capacity);
        fatalExit();
    }

    std::byte* pTargetAddr = m_pVectorHeapBaseAddr + (m_lastCommittedPageIndex + 1) * m_pageSize;
    const platform::VmMemoryInfo vmInfo{
        .pageSize = m_pageSize,
        .reservePageCount = 0,
        .commitPageCount = commitPageCount,
    };

    platform::commitCommonMemory(pTargetAddr, vmInfo);

    m_lastCommittedPageIndex += commitPageCount;
    const std::size_t segmentOffset =
        static_cast<std::size_t>(reinterpret_cast<std::byte*>(m_pCurrent) - m_pVectorHeapBaseAddr);
    const std::size_t committedSize = (m_lastCommittedPageIndex + 1) * m_pageSize;
    m_capacity = (committedSize - segmentOffset) / sizeof(Segment);
    NARU_LOG(INFO,
              "Sorted free segment vector heap committed: heap_base={} "
              "added_bytes={} committed_bytes={} capacity={}",
              static_cast<void*>(m_pVectorHeapBaseAddr), commitPageCount * m_pageSize,
              committedSize, m_capacity);
}

EX_VOID SortedFreeSegVector::emplaceBack(Segment seg) {
    if (m_segCount >= m_capacity) {
        return ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE;
    }

    Segment* pTarget = m_pCurrent + m_segCount;
    new (static_cast<void*>(pTarget)) Segment(seg);

    ++m_segCount;

    return {};
}
} // namespace naru::allocator::group
