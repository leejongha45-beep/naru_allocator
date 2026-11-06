#pragma once
#include <cstddef>

#include "naru/support/log.hpp"
#include "naru/support/return_code.hpp"

namespace naru::allocator::group {
class SortedFreeSegVector final
{
    // ==================== sorted free segments ====================
public:
    struct Segment {
        std::byte* pBaseAddr = nullptr;
        std::size_t size = 0;
    };

private:
    std::byte* m_pVectorHeapBaseAddr = nullptr;
    std::size_t m_segCount = 0;
    std::size_t m_capacity = 0;
    std::size_t m_pageSize = 0;
    std::size_t m_lastReservedPageIndex = 0;
    std::size_t m_lastCommittedPageIndex = 0;
    Segment* m_pCurrent = nullptr;

public:
    SortedFreeSegVector() = delete;
    ~SortedFreeSegVector() = default;

    SortedFreeSegVector(const SortedFreeSegVector&) = delete;
    SortedFreeSegVector& operator=(const SortedFreeSegVector&) = delete;
    SortedFreeSegVector(SortedFreeSegVector&&) = delete;
    SortedFreeSegVector& operator=(SortedFreeSegVector&&) = delete;

    [[nodiscard]] static SortedFreeSegVector* createPtr();

private:
    struct InitInfo {
        std::byte* pVectorHeapBaseAddr = nullptr;
        std::size_t segCount = 0;
        std::size_t capacity = 0;
        std::size_t pageSize = 0;
        std::size_t lastReservedPageIndex = 0;
        std::size_t lastCommittedPageIndex = 0;
        Segment* pCurrent = nullptr;
    };

    explicit SortedFreeSegVector(const InitInfo& info);

public:
    void commitNewPageVectorHeap();

    [[nodiscard]] Segment* begin() {
        return m_pCurrent;
    }
    [[nodiscard]] Segment* end() {
        return m_pCurrent + m_segCount;
    }
    [[nodiscard]] Segment& operator[](std::size_t index) {
        NARU_CHECK(index < m_segCount);
        return m_pCurrent[index];
    }

    EX_VOID emplaceBack(Segment seg);

    void clear() {
        m_segCount = 0;
    }

    [[nodiscard]] std::size_t getSegCount() {
        return m_segCount;
    }
};
} // namespace naru::allocator::group
