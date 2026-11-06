#include "memory_segment_list_storage.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <utility>

#include "group/memory_config.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "naru/support/log.hpp"
#include "naru/support/retry.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"
#include "sorted_free_segment_vector.hpp"

namespace naru::allocator::group {
struct MemorySegListStorage::InitInfo {};

template <std::size_t... Indices>
MemorySegListStorage::MemorySegListStorage(const InitInfo&, std::index_sequence<Indices...>)
    : m_sameGroupUsedLists{(static_cast<void>(Indices), MemorySegListVector::createVal())...},
      m_sameGroupFreeLists{(static_cast<void>(Indices), MemorySegListVector::createVal())...} {
}

MemorySegListStorage MemorySegListStorage::createVal() {
    const InitInfo initInfo{};
    return MemorySegListStorage(initInfo, std::make_index_sequence<256>{});
}

void MemorySegListStorage::destroy() {
    for (std::size_t groupIndex = 0; groupIndex < m_groupsData.size(); ++groupIndex) {
        CoreGroupHeap& groupData = m_groupsData[groupIndex];
        if (groupData.isUsed == false) {
            continue;
        }

        m_sameGroupUsedLists[groupIndex].destroy();
        m_sameGroupFreeLists[groupIndex].destroy();

        const platform::VmMemoryInfo pageInfo{
            .pageSize = groupData.pageSize,
            .reservePageCount = groupData.lastReservedPageIndex + 1,
        };
        platform::releaseEntireMemory(groupData.pBaseAddr, pageInfo);
    }

    reset();
}

void MemorySegListStorage::reset() {
    for (std::size_t groupIndex = 0; groupIndex < m_groupsData.size(); ++groupIndex) {
        m_sameGroupUsedLists[groupIndex].reset();
        m_sameGroupFreeLists[groupIndex].reset();
        m_groupsData[groupIndex] = CoreGroupHeap{};
    }
}
void MemorySegListStorage::createNewGroup(std::uint8_t groupId) {
    NARU_CHECK(isExistGroup(groupId) == false);

    const std::size_t pageSize = platform::getCommonPageSize();
    NARU_CHECK(pageSize != 0);

    const std::size_t reservePageCount = config::GROUP_RESERVE_SIZE / pageSize;
    const std::size_t commitPageCount = config::COMMIT_SIZE / pageSize;
    platform::VmMemoryInfo vmInfo{
        .pageSize = pageSize,
        .reservePageCount = reservePageCount,
        .commitPageCount = commitPageCount,
    };

    std::byte* pBaseAddr = static_cast<std::byte*>(platform::reserveCommonMemory(vmInfo));
    platform::commitCommonMemory(pBaseAddr, vmInfo);

    setGroupBaseAddr(groupId, pBaseAddr);
    setPageSize(groupId, pageSize);
    setLastReservedPageIndex(groupId, reservePageCount - 1);
    setLastCommittedPageIndex(groupId, commitPageCount - 1);
    createGroupVectors(groupId);
    setGroupUsedState(groupId, true);
    NARU_LOG(INFO, "Group created: group={} base={} reserved_bytes={} committed_bytes={}", groupId,
              static_cast<void*>(pBaseAddr), reservePageCount * pageSize,
              commitPageCount * pageSize);
}

std::size_t MemorySegListStorage::calcFrontPaddingSize(const std::byte* pCurrentAddr,
                                                       std::size_t segSize) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pCurrentAddr);
    NARU_CHECK(address % config::SEGMENT_SIZE_UNIT == 0);

    const std::size_t remainder = address % segSize;
    return remainder == 0 ? 0 : segSize - remainder;
}

std::size_t MemorySegListStorage::calcRequiredCommitPageCount(std::uint8_t groupId,
                                                              std::size_t segSize,
                                                              std::size_t segCount) const {
    NARU_CHECK(isExistGroup(groupId) == true);
    NARU_CHECK(segSize > 0 && segSize % config::SEGMENT_SIZE_UNIT == 0);
    NARU_CHECK(segCount > 0);

    const std::size_t currentOffset = getCurrentOffset(groupId);
    const std::size_t pageSize = getPageSize(groupId);
    const std::size_t committedSize = (getLastCommittedPageIndex(groupId) + 1) * pageSize;
    NARU_CHECK(currentOffset <= committedSize);

    const std::size_t frontPaddingSize =
        calcFrontPaddingSize(getGroupBaseHeapAddr(groupId) + currentOffset, segSize);

    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    if (segCount > (maxSize - frontPaddingSize) / segSize) {
        NARU_LOG(FATAL,
                  "Segment request size overflow: group={} segment_bytes={} count={} "
                  "front_padding_bytes={}",
                  groupId, segSize, segCount, frontPaddingSize);
        fatalExit();
    }

    const std::size_t requiredSize = frontPaddingSize + segSize * segCount;
    const std::size_t remainingSize = committedSize - currentOffset;
    NARU_CHECK(requiredSize > remainingSize);

    const std::size_t shortage = requiredSize - remainingSize;
    std::size_t requiredCommitPageCount = shortage / pageSize;
    if (shortage % pageSize != 0) {
        ++requiredCommitPageCount;
    }
    NARU_LOG(DEBUG,
              "Group heap growth required: group={} segment_bytes={} count={} "
              "required_bytes={} remaining_bytes={} additional_pages={}",
              groupId, segSize, segCount, requiredSize, remainingSize, requiredCommitPageCount);
    return requiredCommitPageCount;
}

void MemorySegListStorage::requestCommitNewPage(std::uint8_t groupId, std::size_t commitPageCount) {
    NARU_CHECK(isExistGroup(groupId) == true);
    NARU_CHECK(commitPageCount > 0);

    std::size_t pageSize = getPageSize(groupId);
    std::byte* pGroupBaseAddr = getGroupBaseHeapAddr(groupId);
    std::size_t committedPageCount = getLastCommittedPageIndex(groupId) + 1;
    std::size_t reservedPageCount = getLastReservedPageIndex(groupId) + 1;
    std::byte* pTargetAddr = pGroupBaseAddr + committedPageCount * pageSize;

    const std::size_t remainingPageCount = reservedPageCount - committedPageCount;
    if (commitPageCount > remainingPageCount) {
        NARU_LOG(FATAL,
                  "Group heap reservation exhausted: group={} requested_bytes={} "
                  "remaining_bytes={} committed_bytes={} reserved_bytes={}",
                  groupId, commitPageCount * pageSize, remainingPageCount * pageSize,
                  committedPageCount * pageSize, reservedPageCount * pageSize);
        fatalExit();
    }

    const std::size_t defaultCommitPageCount = config::COMMIT_SIZE / pageSize;
    commitPageCount = (std::max)(commitPageCount, defaultCommitPageCount);
    commitPageCount = (std::min)(commitPageCount, remainingPageCount);

    const platform::VmMemoryInfo vmInfo{
        .pageSize = pageSize,
        .reservePageCount = 0,
        .commitPageCount = commitPageCount,
    };

    platform::commitCommonMemory(pTargetAddr, vmInfo);

    committedPageCount += commitPageCount;
    setLastCommittedPageIndex(groupId, committedPageCount - 1);
    NARU_LOG(INFO, "Group heap committed: group={} added_bytes={} committed_bytes={}", groupId,
              commitPageCount * pageSize, committedPageCount * pageSize);
}

EX_TYPE<MemorySegList::Segment> MemorySegListStorage::acquireAlignedHeapRange(
    std::uint8_t groupId, std::size_t segSize, std::size_t segCount) {
    NARU_CHECK(isExistGroup(groupId) == true);
    NARU_CHECK(segCount > 0);
    NARU_CHECK(segSize > 0 && segSize % config::SEGMENT_SIZE_UNIT == 0);

    std::byte* const pGroupBaseAddr = getGroupBaseHeapAddr(groupId);
    const std::size_t currentOffset = getCurrentOffset(groupId);
    const std::size_t pageSize = getPageSize(groupId);
    const std::size_t committedSize = (getLastCommittedPageIndex(groupId) + 1) * pageSize;
    NARU_CHECK(currentOffset <= committedSize);

    const std::size_t remainingSize = committedSize - currentOffset;
    const std::size_t frontPaddingSize =
        calcFrontPaddingSize(pGroupBaseAddr + currentOffset, segSize);

    if (frontPaddingSize > remainingSize) {
        return ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE;
    }

    if (segCount > (remainingSize - frontPaddingSize) / segSize) {
        return ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE;
    }

    const std::size_t firstOffset = currentOffset + frontPaddingSize;
    std::size_t frontOffset = currentOffset;

    while (frontOffset < firstOffset) {
        std::byte* const pFrontAddr = pGroupBaseAddr + frontOffset;
        const std::uintptr_t frontAddress = reinterpret_cast<std::uintptr_t>(pFrontAddr);
        const std::size_t remainingFrontSize = firstOffset - frontOffset;
        std::size_t frontSegSize = remainingFrontSize;

        if (frontAddress % remainingFrontSize != 0) {
            const int maxSizeShift = std::bit_width(remainingFrontSize) - 1;
            const int alignmentShift = std::countr_zero(frontAddress);
            frontSegSize = std::size_t{1} << std::min(maxSizeShift, alignmentShift);
        }

        NARU_CHECK(frontSegSize > 0 && frontSegSize % config::SEGMENT_SIZE_UNIT == 0);
        NARU_CHECK(frontAddress % frontSegSize == 0);

        EX_REF<MemorySegList> exFrontList =
            NARU_TRY(getSameSizeFreeList(groupId, frontSegSize), 2,
                      emplaceBackNewSizeFreeList(groupId, frontSegSize));
        if (!exFrontList.hasValue()) {
            NARU_LOG(FATAL,
                      "Front padding list unavailable after creation: group={} "
                      "segment_bytes={} offset_bytes={} error_code={}",
                      groupId, frontSegSize, frontOffset, std::to_underlying(exFrontList.error()));
            fatalExit();
        }

        MemorySegList::Segment frontSeg{.pBaseAddr = pFrontAddr};
        emplaceBackSegNode(frontSeg, exFrontList.value());
        NARU_LOG(DEBUG, "Front padding recycled: group={} segment_bytes={} offset_bytes={}",
                  groupId, frontSegSize, frontOffset);

        frontOffset += frontSegSize;
    }

    MemorySegList::Segment segs{.pBaseAddr = pGroupBaseAddr + firstOffset};
    NARU_CHECK(reinterpret_cast<std::uintptr_t>(segs.pBaseAddr) % segSize == 0);

    const std::size_t endOffset = firstOffset + segSize * segCount;
    setCurrentOffset(groupId, endOffset);

    NARU_LOG(DEBUG,
              "Group heap acquired: group={} segment_bytes={} count={} "
              "front_padding_bytes={} offset_bytes={}",
              groupId, segSize, segCount, frontPaddingSize, endOffset);
    return segs;
}

void MemorySegListStorage::createGroupVectors(std::uint8_t groupId) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == false);

    MemorySegListVector& usedLists = segListStorage.getSameGroupUsedLists(groupId);
    MemorySegListVector& freeLists = segListStorage.getSameGroupFreeLists(groupId);

    new (static_cast<void*>(&usedLists))
        MemorySegListVector(MemorySegListVector::createVal({.isFree = false}));
    new (static_cast<void*>(&freeLists))
        MemorySegListVector(MemorySegListVector::createVal({.isFree = true}));
}

void MemorySegListStorage::emplaceBackNewSizeUsedList(std::uint8_t groupId, std::size_t segSize) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);

    MemorySegListVector& lists = segListStorage.getSameGroupUsedLists(groupId);

    const EX_VOID emplaceResult =
        NARU_TRY(lists.emplaceBack(groupId, segSize), 2, lists.commitNewPageVectorHeap());
    if (!emplaceResult.hasValue()) {
        NARU_LOG(FATAL,
                  "Used list creation failed after retry: group={} segment_bytes={} "
                  "error_code={}",
                  groupId, segSize, std::to_underlying(emplaceResult.error()));
        fatalExit();
    }

    NARU_CHECK(lists.back().getSegSize() == segSize);
    NARU_LOG(DEBUG, "Used list created: group={} segment_bytes={}", groupId, segSize);
}

void MemorySegListStorage::emplaceBackNewSizeFreeList(std::uint8_t groupId, std::size_t segSize) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);

    MemorySegListVector& lists = segListStorage.getSameGroupFreeLists(groupId);

    const EX_VOID emplaceResult =
        NARU_TRY(lists.emplaceBack(groupId, segSize), 2, lists.commitNewPageVectorHeap());
    if (!emplaceResult.hasValue()) {
        NARU_LOG(FATAL,
                  "Free list creation failed after retry: group={} segment_bytes={} "
                  "error_code={}",
                  groupId, segSize, std::to_underlying(emplaceResult.error()));
        fatalExit();
    }

    NARU_CHECK(lists.back().getSegSize() == segSize);
    NARU_LOG(DEBUG, "Free list created: group={} segment_bytes={}", groupId, segSize);
}

EX_REF<MemorySegList> MemorySegListStorage::getSameSizeFreeList(std::uint8_t groupId,
                                                                std::size_t size) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);

    MemorySegListVector& lists = segListStorage.getSameGroupFreeLists(groupId);
    EX_REF<MemorySegList> exTargetList = lists.find(size);
    if (!exTargetList.hasValue()) {
        return ErrorCode::MEMORY_FREE_LIST_NOT_FOUND;
    }

    MemorySegList& list = exTargetList.value();
    return std::ref(list);
}

EX_REF<MemorySegList> MemorySegListStorage::getMultipleSizeUsableFreeList(std::uint8_t groupId,
                                                                          std::size_t segSize) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);
    NARU_CHECK(segSize != 0);

    MemorySegList* pList = nullptr;
    MemorySegListVector& lists = segListStorage.getSameGroupFreeLists(groupId);
    for (std::size_t index = 0; index < lists.getListCount(); ++index) {
        MemorySegList& list = lists[index];
        if (list.isEmpty() == true) {
            continue;
        }

        std::size_t size = list.getSegSize();
        if (size > segSize && size % segSize == 0) {
            pList = &list;
            break;
        }
    }

    if (pList == nullptr) {
        return ErrorCode::MEMORY_USABLE_FREE_LIST_NOT_FOUND;
    }

    return std::ref(*pList);
}

MemorySegList::Segment MemorySegListStorage::popSegNode(MemorySegList& list) {
    MemorySegList::Iterator it = list.begin();
    MemorySegList::Segment seg = *it;

    list.eraseAt(it);

    return seg;
}

EX_REF<MemorySegList> MemorySegListStorage::getSameSizeUsedList(std::uint8_t groupId,
                                                                std::size_t segSize) {
    MemorySegListStorage& segListStorage = *this;
    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);

    MemorySegListVector& lists = segListStorage.getSameGroupUsedLists(groupId);
    EX_REF<MemorySegList> exTargetList = lists.find(segSize);
    if (!exTargetList.hasValue()) {
        return ErrorCode::MEMORY_USED_LIST_NOT_FOUND;
    }

    MemorySegList& list = exTargetList.value();

    return std::ref(list);
}

void MemorySegListStorage::emplaceBackSegNode(MemorySegList::Segment seg, MemorySegList& list) {
    NARU_CHECK(seg.pBaseAddr != nullptr);
    NARU_CHECK(list.getSegSize() > 0 && list.getSegSize() % config::SEGMENT_SIZE_UNIT == 0);
    NARU_CHECK(reinterpret_cast<std::uintptr_t>(seg.pBaseAddr) % list.getSegSize() == 0);

    const EX_TYPE<MemorySegList::Iterator> emplaceResult =
        NARU_TRY(list.emplaceBack(seg), 2, list.commitNewPageNodeHeap());
    if (!emplaceResult.hasValue()) {
        NARU_LOG(FATAL,
                  "Segment node insertion failed after retry: segment_bytes={} "
                  "base={} error_code={}",
                  list.getSegSize(), static_cast<void*>(seg.pBaseAddr),
                  std::to_underlying(emplaceResult.error()));
        fatalExit();
    }
}

void MemorySegListStorage::separateSegList(std::uint8_t groupId, MemorySegList& targetList,
                                           MemorySegList& insertList) {
    NARU_CHECK(targetList.m_groupId == groupId);
    NARU_CHECK(insertList.m_groupId == groupId);

    std::size_t segSize = insertList.getSegSize();
    std::size_t targetSize = targetList.getSegSize();

    NARU_CHECK(targetList.isEmpty() == false);
    NARU_CHECK(segSize != 0);
    NARU_CHECK(targetSize > segSize && targetSize % segSize == 0);

    std::size_t multipleCount = targetSize / segSize;
    MemorySegList::Segment targetSeg = popSegNode(targetList);
    std::byte* pTargetAddr = targetSeg.pBaseAddr;

    for (std::size_t i = 0; i < multipleCount; ++i) {
        MemorySegList::Segment seg{
            .pBaseAddr = pTargetAddr,
        };

        emplaceBackSegNode(seg, insertList);
        pTargetAddr += segSize;
    }

    NARU_LOG(DEBUG, "Segment split: group={} source_bytes={} segment_bytes={} count={}", groupId,
              targetSize, segSize, multipleCount);
}

SortedFreeSegVector& MemorySegListStorage::prepareMergeSegs(MemorySegListVector& freeLists) {
    NARU_CHECK(freeLists.isFreeLists() == true);

    SortedFreeSegVector& sortedSegs = freeLists.getSortedFreeSegVector();
    for (std::size_t index = 0; index < freeLists.getListCount(); ++index) {
        MemorySegList& freeList = freeLists[index];
        while (freeList.isEmpty() == false) {
            MemorySegList::Segment listSeg = popSegNode(freeList);
            SortedFreeSegVector::Segment vecSeg{
                .pBaseAddr = listSeg.pBaseAddr,
                .size = freeList.getSegSize(),
            };

            EX_VOID emplaceResult =
                NARU_TRY(sortedSegs.emplaceBack(vecSeg), 2, sortedSegs.commitNewPageVectorHeap());
            if (!emplaceResult.hasValue()) {
                NARU_LOG(FATAL,
                          "Sorted free segment vector insertion failed after "
                          "retry: segment_bytes={} base={} error_code={}",
                          vecSeg.size, static_cast<void*>(vecSeg.pBaseAddr),
                          std::to_underlying(emplaceResult.error()));
                fatalExit();
            }
        }
    }
    return sortedSegs;
}

void MemorySegListStorage::mergeFreeSegs(std::uint8_t groupId, MemorySegListVector& freeLists) {
    NARU_CHECK(freeLists.isFreeLists() == true);

    SortedFreeSegVector& sortedSegs = freeLists.getSortedFreeSegVector();
    std::size_t sortedSegCount = sortedSegs.getSegCount();
    NARU_CHECK(sortedSegCount > 0);

    std::size_t index = 0;
    while (index < sortedSegCount) {
        SortedFreeSegVector::Segment mergedSeg = sortedSegs[index];
        ++index;

        while (index < sortedSegCount) {
            const SortedFreeSegVector::Segment& nextSeg = sortedSegs[index];
            if (mergedSeg.pBaseAddr + mergedSeg.size != nextSeg.pBaseAddr) {
                break;
            }

            mergedSeg.size += nextSeg.size;
            ++index;
        }

        std::size_t baseSize = mergedSeg.size;
        std::size_t targetSize = mergedSeg.size;
        std::uintptr_t baseAddr = reinterpret_cast<std::uintptr_t>(mergedSeg.pBaseAddr);

        while (targetSize != 0) {
            while (baseAddr % targetSize != 0) {
                targetSize -= config::SEGMENT_SIZE_UNIT;
            }

            EX_REF<MemorySegList> exList = NARU_TRY(
                freeLists.find(targetSize), 2, emplaceBackNewSizeFreeList(groupId, targetSize));
            if (!exList.hasValue()) {
                NARU_LOG(FATAL,
                          "Free segment list lookup failed after creation: "
                          "group={} segment_bytes={} error_code={}",
                          groupId, mergedSeg.size, std::to_underlying(exList.error()));
                fatalExit();
            }

            MemorySegList& list = exList.value();
            MemorySegList::Segment seg{
                .pBaseAddr = reinterpret_cast<std::byte*>(baseAddr),
            };

            emplaceBackSegNode(seg, list);

            baseAddr += targetSize;
            baseSize -= targetSize;
            targetSize = baseSize;
        }
    }
}

} // namespace naru::allocator::group
