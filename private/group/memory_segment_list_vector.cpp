#include "memory_segment_list_vector.hpp"

#include <functional>
#include <new>

#include "group/memory_segment_list.hpp"
#include "memory_config.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"
#include "sorted_free_segment_vector.hpp"

namespace naru::allocator::group {
MemorySegListVector::MemorySegListVector(const InitInfo& info)
    : m_pVectorHeapBaseAddr(info.pVectorHeapBaseAddr), m_listCount(info.listCount),
      m_pageSize(info.pageSize), m_lastCommittedPageIndex(info.lastCommittedPageIndex),
      m_pSortedFreeSegVector(info.pSortedFreeSegVector) {
}

MemorySegListVector MemorySegListVector::createVal() {
    const InitInfo initInfo{};
    return MemorySegListVector(initInfo);
}

MemorySegListVector MemorySegListVector::createVal(const CreateInfo& createInfo) {
    const bool isFree = createInfo.isFree;
    const std::size_t pageSize = platform::getCommonPageSize();
    NARU_CHECK(pageSize != 0);
    const std::size_t reserveCount = config::METADATA_RESERVE_SIZE / pageSize;
    const std::size_t commitCount = (config::METADATA_LIST_COMMIT_SIZE + pageSize - 1) / pageSize;

    const platform::VmMemoryInfo vmInfo{
        .pageSize = pageSize,
        .reservePageCount = reserveCount,
        .commitPageCount = commitCount,
    };

    std::byte* pBaseAddr = static_cast<std::byte*>(platform::reserveCommonMemory(vmInfo));
    platform::commitCommonMemory(pBaseAddr, vmInfo);

    SortedFreeSegVector* pSortedFreeSegVector = nullptr;
    if (isFree) {
        pSortedFreeSegVector = SortedFreeSegVector::createPtr();
    }

    const InitInfo info{
        .pVectorHeapBaseAddr = pBaseAddr,
        .listCount = 0,
        .pageSize = pageSize,
        .lastCommittedPageIndex = commitCount - 1,
        .pSortedFreeSegVector = pSortedFreeSegVector,
    };
    NARU_LOG(DEBUG,
              "List vector heap created: heap_base={} reserved_bytes={} "
              "committed_bytes={} capacity={}",
              static_cast<void*>(pBaseAddr), reserveCount * pageSize, commitCount * pageSize,
              commitCount * pageSize / config::METADATA_LIST_SLOT_SIZE);

    return MemorySegListVector(info);
}

void MemorySegListVector::destroy() {
    for (std::size_t index = 0; index < m_listCount; ++index) {
        MemorySegList& list = (*this)[index];
        list.destroy();
    }

    if (m_pSortedFreeSegVector != nullptr) {
        const platform::VmMemoryInfo sortedPageInfo{
            .pageSize = m_pageSize,
            .reservePageCount = config::METADATA_RESERVE_SIZE / m_pageSize,
        };
        platform::releaseEntireMemory(m_pSortedFreeSegVector, sortedPageInfo);
    }

    const platform::VmMemoryInfo pageInfo{
        .pageSize = m_pageSize,
        .reservePageCount = config::METADATA_RESERVE_SIZE / m_pageSize,
    };
    platform::releaseEntireMemory(m_pVectorHeapBaseAddr, pageInfo);

    reset();
}

void MemorySegListVector::reset() {
    m_pVectorHeapBaseAddr = nullptr;
    m_listCount = 0;
    m_pageSize = 0;
    m_lastCommittedPageIndex = 0;
    m_pSortedFreeSegVector = nullptr;
}

MemorySegList& MemorySegListVector::operator[](std::size_t index) {
    NARU_CHECK(index < m_listCount);
    std::byte* pTargetAddr = m_pVectorHeapBaseAddr + index * config::METADATA_LIST_SLOT_SIZE;
    return *reinterpret_cast<MemorySegList*>(pTargetAddr);
}

EX_VOID MemorySegListVector::emplaceBack(std::uint8_t groupId, std::size_t segSize) {
    NARU_CHECK(m_pVectorHeapBaseAddr != nullptr);
    const std::size_t capacity =
        (m_lastCommittedPageIndex + 1) * m_pageSize / config::METADATA_LIST_SLOT_SIZE;
    NARU_CHECK(m_listCount <= capacity);

    if (m_listCount == capacity) {
        return ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE;
    }

    std::byte* pTargetAddr = m_pVectorHeapBaseAddr + m_listCount * config::METADATA_LIST_SLOT_SIZE;

    new (static_cast<void*>(pTargetAddr))
        MemorySegList(MemorySegList::createVal({.groupId = groupId, .segSize = segSize}));

    ++m_listCount;

    return {};
}

void MemorySegListVector::commitNewPageVectorHeap() {
    std::byte* pTargetAddr = m_pVectorHeapBaseAddr + (m_lastCommittedPageIndex + 1) * m_pageSize;

    const std::size_t lastReservedPageIndex = config::METADATA_RESERVE_SIZE / m_pageSize - 1;
    std::size_t capacity =
        (m_lastCommittedPageIndex + 1) * m_pageSize / config::METADATA_LIST_SLOT_SIZE;
    const std::size_t commitPageCount = config::COMMIT_SIZE / m_pageSize;
    if (lastReservedPageIndex - m_lastCommittedPageIndex < commitPageCount) {
        NARU_LOG(FATAL,
                  "List vector heap reservation exhausted: heap_base={} "
                  "requested_bytes={} remaining_bytes={} capacity={}",
                  static_cast<void*>(m_pVectorHeapBaseAddr), commitPageCount * m_pageSize,
                  (lastReservedPageIndex - m_lastCommittedPageIndex) * m_pageSize, capacity);
        fatalExit();
    }

    const platform::VmMemoryInfo vmInfo{
        .pageSize = m_pageSize,
        .reservePageCount = 0,
        .commitPageCount = commitPageCount,
    };

    platform::commitCommonMemory(pTargetAddr, vmInfo);

    m_lastCommittedPageIndex += commitPageCount;
    capacity = (m_lastCommittedPageIndex + 1) * m_pageSize / config::METADATA_LIST_SLOT_SIZE;
    NARU_LOG(INFO,
              "List vector heap committed: heap_base={} added_bytes={} "
              "committed_bytes={} capacity={}",
              static_cast<void*>(m_pVectorHeapBaseAddr), commitPageCount * m_pageSize,
              (m_lastCommittedPageIndex + 1) * m_pageSize, capacity);
}

EX_REF<MemorySegList> MemorySegListVector::find(std::size_t segSize) {
    for (std::size_t index = 0; index < m_listCount; ++index) {
        MemorySegList& list = (*this)[index];
        if (list.getSegSize() == segSize) {
            return std::ref(list);
        }
    }

    return ErrorCode::MEMORY_DATA_NOT_FOUND;
}
} // namespace naru::allocator::group
