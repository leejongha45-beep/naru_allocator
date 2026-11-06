#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

#include "group/memory_segment_list.hpp"
#include "memory_config.hpp"
#include "memory_segment_list_vector.hpp"
#include "naru/support/return_code.hpp"

namespace naru::allocator::group {
class MemorySegListStorage final
{
    friend class GroupAllocator;

    // ==================== group lists ====================
private:
    struct InitInfo;

    template <std::size_t... Indices>
    explicit MemorySegListStorage(const InitInfo& info, std::index_sequence<Indices...>);

    std::array<MemorySegListVector, 256> m_sameGroupUsedLists;
    std::array<MemorySegListVector, 256> m_sameGroupFreeLists;

public:
    MemorySegListStorage() = delete;
    ~MemorySegListStorage() {
        destroy();
    }

    MemorySegListStorage(const MemorySegListStorage&) = delete;
    MemorySegListStorage& operator=(const MemorySegListStorage&) = delete;
    MemorySegListStorage(MemorySegListStorage&&) = delete;
    MemorySegListStorage& operator=(MemorySegListStorage&&) = delete;

    [[nodiscard]] static MemorySegListStorage createVal();

    void destroy();
    void reset();

    [[nodiscard]] MemorySegListVector& getSameGroupUsedLists(std::uint8_t groupId) {
        return m_sameGroupUsedLists[groupId];
    }
    [[nodiscard]] MemorySegListVector& getSameGroupFreeLists(std::uint8_t groupId) {
        return m_sameGroupFreeLists[groupId];
    }

    // ==================== group heap ====================
private:
    struct CoreGroupHeap {
        std::byte* pBaseAddr = nullptr;
        std::size_t pageSize = 0;
        std::size_t lastReservedPageIndex = config::INVALID_PAGE_INDEX;
        std::size_t lastCommittedPageIndex = config::INVALID_PAGE_INDEX;
        std::size_t currentOffset = 0;
        bool isUsed = false;
    };
    std::array<CoreGroupHeap, 256> m_groupsData{};
    std::array<std::mutex, 256> m_groupsMutex{};

public:
    void setGroupBaseAddr(std::uint8_t groupId, std::byte* pBaseAddr) {
        m_groupsData[groupId].pBaseAddr = pBaseAddr;
    }
    [[nodiscard]] std::byte* getGroupBaseHeapAddr(std::uint8_t groupId) const {
        return m_groupsData[groupId].pBaseAddr;
    }

    void setPageSize(std::uint8_t groupId, std::size_t pageSize) {
        m_groupsData[groupId].pageSize = pageSize;
    }
    [[nodiscard]] std::size_t getPageSize(std::uint8_t groupId) const {
        return m_groupsData[groupId].pageSize;
    }

    void setLastReservedPageIndex(std::uint8_t groupId, std::size_t pageIndex) {
        m_groupsData[groupId].lastReservedPageIndex = pageIndex;
    }
    [[nodiscard]] std::size_t getLastReservedPageIndex(std::uint8_t groupId) const {
        return m_groupsData[groupId].lastReservedPageIndex;
    }

    void setLastCommittedPageIndex(std::uint8_t groupId, std::size_t segSize) {
        m_groupsData[groupId].lastCommittedPageIndex = segSize;
    }
    [[nodiscard]] std::size_t getLastCommittedPageIndex(std::uint8_t groupId) const {
        return m_groupsData[groupId].lastCommittedPageIndex;
    }

    void setCurrentOffset(std::uint8_t groupId, std::size_t offset) {
        m_groupsData[groupId].currentOffset = offset;
    }
    [[nodiscard]] std::size_t getCurrentOffset(std::uint8_t groupId) const {
        return m_groupsData[groupId].currentOffset;
    }

    void setGroupUsedState(std::uint8_t groupId, bool isUsed) {
        m_groupsData[groupId].isUsed = isUsed;
    }
    [[nodiscard]] bool isExistGroup(std::uint8_t groupId) const {
        return m_groupsData[groupId].isUsed == true;
    }

    [[nodiscard]] std::mutex& getGroupMutex(std::uint8_t groupId) {
        return m_groupsMutex[groupId];
    }

private:
    // ==================== group heap ====================
    void createNewGroup(std::uint8_t groupId);
    [[nodiscard]] static std::size_t calcFrontPaddingSize(const std::byte* pCurrentAddr,
                                                         std::size_t segSize);
    [[nodiscard]] std::size_t calcRequiredCommitPageCount(std::uint8_t groupId, std::size_t segSize,
                                                          std::size_t segCount = 1) const;
    void requestCommitNewPage(std::uint8_t groupId, std::size_t commitPageCount);

    // ==================== segments ====================
    /**
     * Acquires contiguous storage for segCount segments of segSize bytes each.
     * Recycles front padding into Free lists. The caller registers the acquired segments.
     * @param segCount Number of segments to acquire.
     * @return On success, a Segment containing only the first segment's
     *         starting address. Segment i starts at pBaseAddr + i * segSize,
     *         where 0 <= i < segCount.
     */
    EX_TYPE<MemorySegList::Segment> acquireAlignedHeapRange(
        std::uint8_t groupId, std::size_t segSize, std::size_t segCount = 1);

    void createGroupVectors(std::uint8_t groupId);

    void emplaceBackNewSizeFreeList(std::uint8_t groupId, std::size_t segSize);
    EX_REF<MemorySegList> getSameSizeFreeList(std::uint8_t groupId, std::size_t size);
    EX_REF<MemorySegList> getMultipleSizeUsableFreeList(std::uint8_t groupId, std::size_t segSize);

    void emplaceBackNewSizeUsedList(std::uint8_t groupId, std::size_t segSize);
    EX_REF<MemorySegList> getSameSizeUsedList(std::uint8_t groupId, std::size_t segSize);

    void emplaceBackSegNode(MemorySegList::Segment seg, MemorySegList& list);
    [[nodiscard]] MemorySegList::Segment popSegNode(MemorySegList& list);
    void separateSegList(std::uint8_t groupId, MemorySegList& targetList,
                         MemorySegList& insertList);
    SortedFreeSegVector& prepareMergeSegs(MemorySegListVector& freeLists);
    void mergeFreeSegs(std::uint8_t groupId, MemorySegListVector& freeLists);
};
} // namespace naru::allocator::group
