#include "naru/allocator/group/group_allocator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

#include "group/memory_config.hpp"
#include "group/memory_segment_list.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "memory_segment_list_storage.hpp"
#include "naru/support/log.hpp"
#include "naru/support/retry.hpp"
#include "naru/support/return_code.hpp"
#include "naru/support/static_pimpl.hpp"
#include "sorted_free_segment_vector.hpp"

namespace naru::allocator::group {
struct GroupAllocator::Impl {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    MemorySegListStorage m_segListStorage = MemorySegListStorage::createVal();

    [[nodiscard]] std::byte* galloc(std::uint8_t groupId, std::size_t typeSize);
    void greserve(std::uint8_t groupId, std::size_t typeSize, std::size_t segCount);
    void gfree(std::uint8_t groupId, std::size_t typeSize, std::byte* pTargetAddr);
    void gclean(std::uint8_t groupId);

private:
    [[nodiscard]] static std::size_t toSegSize(std::size_t requestedSize);
};

GroupAllocator& GroupAllocator::getInstance() {
    static GroupAllocator allocator{};
    return allocator;
}

GroupAllocator::GroupAllocator() = default;
GroupAllocator::~GroupAllocator() = default;

std::byte* GroupAllocator::galloc(std::uint8_t groupId, TypeInfo typeInfo) {
    Impl& impl = *m_impl;
    return impl.galloc(groupId, typeInfo.size);
}

void GroupAllocator::greserve(std::uint8_t groupId, std::size_t typeSize, std::size_t segCount) {
    Impl& impl = *m_impl;
    impl.greserve(groupId, typeSize, segCount);
}

void GroupAllocator::gfree(std::uint8_t groupId, std::size_t typeSize, std::byte* pTargetAddr) {
    Impl& impl = *m_impl;
    impl.gfree(groupId, typeSize, pTargetAddr);
}

void GroupAllocator::gclean(std::uint8_t groupId) {
    Impl& impl = *m_impl;
    impl.gclean(groupId);
}

std::size_t GroupAllocator::Impl::toSegSize(std::size_t requestedSize) {
    constexpr std::size_t unit = config::SEGMENT_SIZE_UNIT;

    NARU_CHECK(requestedSize > 0);
    NARU_CHECK(requestedSize <= (std::numeric_limits<std::size_t>::max)() - (unit - 1));

    return (requestedSize + (unit - 1)) / unit * unit;
}

std::byte* GroupAllocator::Impl::galloc(std::uint8_t groupId, std::size_t typeSize) {
    const std::size_t segSize = toSegSize(typeSize);
    MemorySegListStorage& segListStorage = m_segListStorage;
    std::mutex& groupMutex = segListStorage.getGroupMutex(groupId);
    std::scoped_lock groupLock(groupMutex);

    if (!segListStorage.isExistGroup(groupId)) {
        segListStorage.createNewGroup(groupId);
    }

    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);

    EX_REF<MemorySegList> exFreeList =
        NARU_TRY(segListStorage.getSameSizeFreeList(groupId, segSize), 2,
                  segListStorage.emplaceBackNewSizeFreeList(groupId, segSize));
    if (!exFreeList.hasValue()) {
        NARU_LOG(FATAL,
                  "Free list unavailable after creation: group={} segment_bytes={} "
                  "error_code={}",
                  groupId, segSize, std::to_underlying(exFreeList.error()));
        fatalExit();
    }

    MemorySegList& freeList = exFreeList.value();
    if (freeList.isEmpty() == true) {
        EX_REF<MemorySegList> exMultipleSizeFreeList =
            segListStorage.getMultipleSizeUsableFreeList(groupId, segSize);
        if (!exMultipleSizeFreeList.hasValue()) {
            EX_TYPE<MemorySegList::Segment> exSegs = NARU_TRY(
                segListStorage.acquireAlignedHeapRange(groupId, segSize, 1), 2,
                segListStorage.requestCommitNewPage(
                    groupId, segListStorage.calcRequiredCommitPageCount(groupId, segSize)));
            if (!exSegs.hasValue()) {
                NARU_LOG(FATAL,
                          "Allocation failed after retry: group={} segment_bytes={} "
                          "error_code={}",
                          groupId, segSize, std::to_underlying(exSegs.error()));
                fatalExit();
            }

            MemorySegList::Segment seg = exSegs.value();
            NARU_CHECK(seg.pBaseAddr + segSize == segListStorage.getGroupBaseHeapAddr(groupId) +
                                                       segListStorage.getCurrentOffset(groupId));

            segListStorage.emplaceBackSegNode(seg, freeList);
        } else {
            MemorySegList& multipleSizeFreeList = exMultipleSizeFreeList.value();
            segListStorage.separateSegList(groupId, multipleSizeFreeList, freeList);
        }
    }
    MemorySegList::Segment seg = segListStorage.popSegNode(freeList);

    EX_REF<MemorySegList> exUsedList =
        NARU_TRY(segListStorage.getSameSizeUsedList(groupId, segSize), 2,
                  segListStorage.emplaceBackNewSizeUsedList(groupId, segSize));
    if (!exUsedList.hasValue()) {
        NARU_LOG(FATAL,
                  "Used list unavailable after creation: group={} segment_bytes={} "
                  "error_code={}",
                  groupId, segSize, std::to_underlying(exUsedList.error()));
        fatalExit();
    }

    MemorySegList& usedList = exUsedList.value();
    segListStorage.emplaceBackSegNode(seg, usedList);

    NARU_LOG(DEBUG, "Segment allocated: group={} segment_bytes={} base={}", groupId, segSize,
              static_cast<void*>(seg.pBaseAddr));
    return seg.pBaseAddr;
}

void GroupAllocator::Impl::greserve(std::uint8_t groupId, std::size_t typeSize,
                                    std::size_t segCount) {
    NARU_CHECK(segCount > 0);

    const std::size_t segSize = toSegSize(typeSize);
    MemorySegListStorage& segListStorage = m_segListStorage;
    std::mutex& groupMutex = segListStorage.getGroupMutex(groupId);
    std::scoped_lock groupLock(groupMutex);

    if (!segListStorage.isExistGroup(groupId)) {
        segListStorage.createNewGroup(groupId);
    }

    EX_TYPE<MemorySegList::Segment> exSegs = NARU_TRY(
        segListStorage.acquireAlignedHeapRange(groupId, segSize, segCount), 2,
        segListStorage.requestCommitNewPage(
            groupId, segListStorage.calcRequiredCommitPageCount(groupId, segSize, segCount)));
    if (!exSegs.hasValue()) {
        NARU_LOG(FATAL,
                  "Segment reservation failed after retry: group={} "
                  "segment_bytes={} "
                  "count={} error_code={}",
                  groupId, segSize, segCount, std::to_underlying(exSegs.error()));
        fatalExit();
    }

    MemorySegList::Segment segs = exSegs.value();

    EX_REF<MemorySegList> exFreeList =
        NARU_TRY(segListStorage.getSameSizeFreeList(groupId, segSize), 2,
                  segListStorage.emplaceBackNewSizeFreeList(groupId, segSize));
    if (!exFreeList.hasValue()) {
        NARU_LOG(FATAL,
                  "Free list unavailable after creation: group={} "
                  "segment_bytes={} "
                  "error_code={}",
                  groupId, segSize, std::to_underlying(exFreeList.error()));
        fatalExit();
    }

    MemorySegList& freeList = exFreeList.value();
    for (std::size_t count = 0; count < segCount; ++count) {
        MemorySegList::Segment seg{
            .pBaseAddr = segs.pBaseAddr,
        };
        segListStorage.emplaceBackSegNode(seg, freeList);
        segs.pBaseAddr += segSize;
    }

    NARU_CHECK(segs.pBaseAddr == segListStorage.getGroupBaseHeapAddr(groupId) +
                                      segListStorage.getCurrentOffset(groupId));
    NARU_LOG(DEBUG, "Segments reserved: group={} segment_bytes={} count={}", groupId, segSize,
              segCount);
}

void GroupAllocator::Impl::gfree(std::uint8_t groupId, std::size_t typeSize,
                                 std::byte* pTargetAddr) {
    NARU_CHECK(pTargetAddr != nullptr);

    const std::size_t segSize = toSegSize(typeSize);
    MemorySegListStorage& segListStorage = m_segListStorage;

    std::mutex& groupMutex = segListStorage.getGroupMutex(groupId);
    std::scoped_lock groupLock(groupMutex);

    NARU_CHECK(segListStorage.isExistGroup(groupId) == true);
    NARU_CHECK(segSize > 0);

    EX_REF<MemorySegList> exUsedList = segListStorage.getSameSizeUsedList(groupId, segSize);
    if (!exUsedList.hasValue()) {
        NARU_LOG(FATAL,
                  "Used list not found for release: group={} "
                  "segment_bytes={} base={} "
                  "error_code={}",
                  groupId, segSize, static_cast<void*>(pTargetAddr),
                  std::to_underlying(exUsedList.error()));
        fatalExit();
    }

    MemorySegList& usedList = exUsedList.value();
    EX_TYPE<MemorySegList::Iterator> exIt = usedList.find(pTargetAddr);
    if (!exIt.hasValue()) {
        NARU_LOG(FATAL,
                  "Used segment not found for release: group={} "
                  "segment_bytes={} base={} "
                  "error_code={}",
                  groupId, segSize, static_cast<void*>(pTargetAddr),
                  std::to_underlying(exIt.error()));
        fatalExit();
    }

    MemorySegList::Iterator segIt = exIt.value();
    MemorySegList::Segment seg{
        .pBaseAddr = segIt->pBaseAddr,
    };

    EX_REF<MemorySegList> exFreeList =
        NARU_TRY(segListStorage.getSameSizeFreeList(groupId, segSize), 2,
                  segListStorage.emplaceBackNewSizeFreeList(groupId, segSize));
    if (!exFreeList.hasValue()) {
        fatalExit();
    }

    MemorySegList& freeList = exFreeList.value();
    segListStorage.emplaceBackSegNode(seg, freeList);
    usedList.eraseAt(segIt);
}

void GroupAllocator::Impl::gclean(std::uint8_t groupId) {
    MemorySegListStorage& segListStorage = m_segListStorage;
    std::mutex& groupMutex = segListStorage.getGroupMutex(groupId);
    std::scoped_lock groupLock(groupMutex);

    NARU_CHECK(segListStorage.isExistGroup(groupId));

    MemorySegListVector& freeLists = segListStorage.getSameGroupFreeLists(groupId);
    NARU_CHECK(freeLists.isFreeLists());
    NARU_CHECK(freeLists.isEmpty() == false);

    SortedFreeSegVector& sortedSegs = segListStorage.prepareMergeSegs(freeLists);
    std::ranges::sort(sortedSegs, [](const SortedFreeSegVector::Segment& left,
                                     const SortedFreeSegVector::Segment& right) {
        return std::less<std::byte*>{}(left.pBaseAddr, right.pBaseAddr);
    });

    segListStorage.mergeFreeSegs(groupId, freeLists);

    sortedSegs.clear();
}

} // namespace naru::allocator::group
