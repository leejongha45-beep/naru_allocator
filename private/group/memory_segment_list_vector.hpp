#pragma once
#include <cstddef>
#include <cstdint>

#include "memory_segment_list.hpp"
#include "naru/support/log.hpp"
#include "naru/support/return_code.hpp"

namespace naru::allocator::group {
class SortedFreeSegVector;

class MemorySegListVector final
{
    // ==================== lists ====================
private:
    std::byte* m_pVectorHeapBaseAddr = nullptr;
    std::size_t m_listCount = 0;
    std::size_t m_pageSize = 0;
    std::size_t m_lastCommittedPageIndex = 0;

public:
    MemorySegListVector() = delete;
    ~MemorySegListVector() = default;

    MemorySegListVector(const MemorySegListVector&) = delete;
    MemorySegListVector& operator=(const MemorySegListVector&) = delete;
    MemorySegListVector(MemorySegListVector&&) = delete;
    MemorySegListVector& operator=(MemorySegListVector&&) = delete;

    struct CreateInfo {
        bool isFree = false;
    };

    [[nodiscard]] static MemorySegListVector createVal();
    [[nodiscard]] static MemorySegListVector createVal(const CreateInfo& info);
    void destroy();
    void reset();

private:
    struct InitInfo {
        std::byte* pVectorHeapBaseAddr = nullptr;
        std::size_t listCount = 0;
        std::size_t pageSize = 0;
        std::size_t lastCommittedPageIndex = 0;
        SortedFreeSegVector* pSortedFreeSegVector = nullptr;
    };

    explicit MemorySegListVector(const InitInfo& info);

public:
    void commitNewPageVectorHeap();

    [[nodiscard]] std::size_t getListCount() const {
        return m_listCount;
    }
    [[nodiscard]] bool isEmpty() const {
        return m_listCount == 0;
    }
    [[nodiscard]] MemorySegList& operator[](std::size_t index);
    [[nodiscard]] MemorySegList& back() {
        NARU_CHECK(m_listCount != 0);
        return (*this)[m_listCount - 1];
    }

    EX_VOID emplaceBack(std::uint8_t groupId, std::size_t segSize);
    EX_REF<MemorySegList> find(std::size_t segSize);

    // ==================== sorted free segments ====================
private:
    SortedFreeSegVector* m_pSortedFreeSegVector = nullptr;

public:
    [[nodiscard]] bool isFreeLists() {
        return m_pSortedFreeSegVector != nullptr;
    }
    [[nodiscard]] SortedFreeSegVector& getSortedFreeSegVector() {
        NARU_CHECK(m_pSortedFreeSegVector != nullptr);
        return *m_pSortedFreeSegVector;
    }
};
} // namespace naru::allocator::group
