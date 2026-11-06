#include "memory_segment_list.hpp"

#include <new>

#include "memory_config.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::allocator::group {
MemorySegList::MemorySegList(const InitInfo& info)
    : m_groupId(info.groupId), m_dataSize(info.dataSize),
      m_pNodeHeapBaseAddr(info.pNodeHeapBaseAddr),
      m_pNodeHeapCurrentOffset(info.nodeHeapCurrentOffset), m_pageSize(info.pageSize),
      m_lastReservedPageIndex(info.lastReservedPageIndex),
      m_lastCommittedPageIndex(info.lastCommittedPageIndex) {
}

MemorySegList MemorySegList::createVal(const CreateInfo& createInfo) {
    const std::uint8_t groupId = createInfo.groupId;
    const std::size_t segSize = createInfo.segSize;
    NARU_CHECK(segSize != 0);

    const std::size_t pageSize = platform::getCommonPageSize();
    NARU_CHECK(pageSize != 0);
    const std::size_t reserveCount = config::METADATA_RESERVE_SIZE / pageSize;
    const std::size_t commitCount = (config::METADATA_NODE_COMMIT_SIZE + pageSize - 1) / pageSize;

    const platform::VmMemoryInfo vmInfo{
        .pageSize = pageSize,
        .reservePageCount = reserveCount,
        .commitPageCount = commitCount,
    };

    std::byte* pBaseAddr = static_cast<std::byte*>(platform::reserveCommonMemory(vmInfo));
    platform::commitCommonMemory(pBaseAddr, vmInfo);

    const InitInfo info{
        .groupId = groupId,
        .dataSize = segSize,
        .pNodeHeapBaseAddr = pBaseAddr,
        .nodeHeapCurrentOffset = 0,
        .pageSize = pageSize,
        .lastReservedPageIndex = reserveCount - 1,
        .lastCommittedPageIndex = commitCount - 1,
    };
    NARU_LOG(DEBUG,
              "Node heap created: group={} segment_bytes={} heap_base={} "
              "reserved_bytes={} committed_bytes={}",
              groupId, segSize, static_cast<void*>(pBaseAddr), reserveCount * pageSize,
              commitCount * pageSize);

    return MemorySegList(info);
}

void MemorySegList::destroy() {
    const platform::VmMemoryInfo pageInfo{
        .pageSize = m_pageSize,
        .reservePageCount = m_lastReservedPageIndex + 1,
    };
    platform::releaseEntireMemory(m_pNodeHeapBaseAddr, pageInfo);

    reset();
}

void MemorySegList::reset() {
    if (m_pNodeHeapBaseAddr == nullptr) {
        return;
    }

    m_pHead = nullptr;
    m_pTail = nullptr;
    m_pReusableNodeStack = nullptr;
    m_groupId = 0;
    m_dataSize = 0;
    m_pNodeHeapBaseAddr = nullptr;
    m_pNodeHeapCurrentOffset = 0;
    m_pageSize = 0;
    m_lastReservedPageIndex = 0;
    m_lastCommittedPageIndex = 0;
}

void MemorySegList::commitNewPageNodeHeap() {
    std::byte* pTargetAddr = m_pNodeHeapBaseAddr + (m_lastCommittedPageIndex + 1) * m_pageSize;

    const std::size_t commitPageCount = config::COMMIT_SIZE / m_pageSize;
    if (m_lastReservedPageIndex - m_lastCommittedPageIndex < commitPageCount) {
        NARU_LOG(FATAL,
                  "Node heap reservation exhausted: group={} segment_bytes={} "
                  "heap_base={} requested_bytes={} remaining_bytes={}",
                  m_groupId, m_dataSize, static_cast<void*>(m_pNodeHeapBaseAddr),
                  commitPageCount * m_pageSize,
                  (m_lastReservedPageIndex - m_lastCommittedPageIndex) * m_pageSize);
        fatalExit();
    }

    const platform::VmMemoryInfo vmInfo{
        .pageSize = m_pageSize,
        .reservePageCount = 0,
        .commitPageCount = commitPageCount,
    };

    platform::commitCommonMemory(pTargetAddr, vmInfo);

    m_lastCommittedPageIndex += commitPageCount;
    NARU_LOG(INFO,
              "Node heap committed: group={} segment_bytes={} heap_base={} "
              "added_bytes={} committed_bytes={}",
              m_groupId, m_dataSize, static_cast<void*>(m_pNodeHeapBaseAddr),
              commitPageCount * m_pageSize, (m_lastCommittedPageIndex + 1) * m_pageSize);
}

EX_TYPE<MemorySegList::Iterator> MemorySegList::find(std::byte* pBaseAddr) {
    NARU_CHECK(pBaseAddr != nullptr);

    for (Iterator it = begin(); it != end(); ++it) {
        if (it->pBaseAddr == pBaseAddr) {
            return it;
        }
    }

    return ErrorCode::MEMORY_DATA_NOT_FOUND;
}

EX_TYPE<MemorySegList::Iterator> MemorySegList::emplaceBack(Segment seg) {
    static_assert(sizeof(Node) <= config::METADATA_NODE_SLOT_SIZE);
    static_assert(config::METADATA_NODE_SLOT_SIZE % alignof(Node) == 0);
    NARU_CHECK(m_pNodeHeapBaseAddr != nullptr);

    void* pTargetNodeHeapAddr = nullptr;
    if (m_pReusableNodeStack != nullptr) {
        pTargetNodeHeapAddr = popReusableNodeStack();
    } else {
        const std::size_t committedSize = (m_lastCommittedPageIndex + 1) * m_pageSize;
        if (m_pNodeHeapCurrentOffset > committedSize - config::METADATA_NODE_SLOT_SIZE) {
            return ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE;
        }

        pTargetNodeHeapAddr = m_pNodeHeapBaseAddr + m_pNodeHeapCurrentOffset;
        m_pNodeHeapCurrentOffset += config::METADATA_NODE_SLOT_SIZE;
    }

    Node* pNewNode = new (static_cast<void*>(pTargetNodeHeapAddr)) Node{
        .data = seg,
        .pPrevSeg = m_pTail,
        .pNextSeg = nullptr,
    };

    if (m_pHead == nullptr) {
        m_pHead = pNewNode;
    } else {
        m_pTail->pNextSeg = pNewNode;
    }
    m_pTail = pNewNode;

    return Iterator(pNewNode);
}

void MemorySegList::eraseAt(Iterator target) {
    Node* pTarget = target.m_pCurrent;

    NARU_CHECK(pTarget != nullptr);
    NARU_CHECK(m_pHead->pPrevSeg == nullptr);
    NARU_CHECK(m_pTail->pNextSeg == nullptr);

    if (m_pHead == pTarget) {
        m_pHead = pTarget->pNextSeg;
        if (m_pHead == nullptr) {
            m_pTail = nullptr;
        } else {
            m_pHead->pPrevSeg = nullptr;
        }
    } else if (m_pTail == pTarget) {
        m_pTail = pTarget->pPrevSeg;
        m_pTail->pNextSeg = nullptr;
    } else {
        Node* pPrev = pTarget->pPrevSeg;
        Node* pNext = pTarget->pNextSeg;
        pPrev->pNextSeg = pNext;
        pNext->pPrevSeg = pPrev;
    }

    emplaceReusableNodeStack(pTarget);
}

void MemorySegList::pop() {
    NARU_CHECK(m_pHead != nullptr);

    eraseAt(begin());
}

} // namespace naru::allocator::group
