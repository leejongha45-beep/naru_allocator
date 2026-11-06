#pragma once
#include <cstddef>
#include <cstdint>

#include "naru/support/log.hpp"
#include "naru/support/return_code.hpp"

namespace naru::allocator::group {
class MemorySegList final
{
    friend class GroupAllocator;
    friend class MemorySegListStorage;

    // ==================== segments ====================
public:
    MemorySegList(const MemorySegList&) = delete;
    MemorySegList& operator=(const MemorySegList&) = delete;
    MemorySegList(MemorySegList&&) = delete;
    MemorySegList& operator=(MemorySegList&&) = delete;

    struct Segment {
        std::byte* pBaseAddr = nullptr;
    };

private:
    struct Node {
        Segment data = {};
        Node* pPrevSeg = nullptr;
        Node* pNextSeg = nullptr;
    };

public:
    class Iterator final
    {
        friend class MemorySegList;

    private:
        Node* m_pCurrent = nullptr;

    public:
        explicit Iterator(Node* pCurrent) : m_pCurrent(pCurrent) {
        }

        [[nodiscard]] Segment& operator*() const {
            NARU_CHECK(m_pCurrent != nullptr);
            return m_pCurrent->data;
        }
        [[nodiscard]] Segment* operator->() const {
            NARU_CHECK(m_pCurrent != nullptr);
            return &m_pCurrent->data;
        }
        Iterator& operator++() {
            NARU_CHECK(m_pCurrent != nullptr);
            m_pCurrent = m_pCurrent->pNextSeg;
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const {
            return m_pCurrent == other.m_pCurrent;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const {
            return m_pCurrent != other.m_pCurrent;
        }
    };

private:
    Node* m_pHead = nullptr;
    Node* m_pTail = nullptr;

public:
    [[nodiscard]] bool isEmpty() const {
        return m_pHead == nullptr && m_pTail == nullptr;
    }

    [[nodiscard]] Iterator begin() {
        return Iterator(m_pHead);
    }
    [[nodiscard]] Iterator end() {
        return Iterator(nullptr);
    }

    EX_TYPE<Iterator> find(std::byte* pBaseAddr);

    EX_TYPE<Iterator> emplaceBack(Segment seg);
    void eraseAt(Iterator target);
    void pop();

    // ==================== reusable nodes ====================
private:
    Node* m_pReusableNodeStack = nullptr;

    void emplaceReusableNodeStack(Node* pNode) {
        NARU_CHECK(pNode != nullptr);

        pNode->pPrevSeg = nullptr;
        pNode->pNextSeg = m_pReusableNodeStack;
        m_pReusableNodeStack = pNode;
    }
    [[nodiscard]] Node* popReusableNodeStack() {
        Node* pNode = m_pReusableNodeStack;
        m_pReusableNodeStack = pNode->pNextSeg;
        return pNode;
    }

    // ==================== segment properties ====================
public:
    struct CreateInfo {
        std::uint8_t groupId = 0;
        std::size_t segSize = 0;
    };
    [[nodiscard]] static MemorySegList createVal(const CreateInfo& info);
    void destroy();
    void reset();

private:
    struct InitInfo {
        std::uint8_t groupId = 0;
        std::size_t dataSize = 0;
        std::byte* pNodeHeapBaseAddr = nullptr;
        std::size_t nodeHeapCurrentOffset = 0;
        std::size_t pageSize = 0;
        std::size_t lastReservedPageIndex = 0;
        std::size_t lastCommittedPageIndex = 0;
    };

    MemorySegList() = delete;
    explicit MemorySegList(const InitInfo& info);

    std::uint8_t m_groupId = 0;
    std::size_t m_dataSize = 0;

public:
    [[nodiscard]] std::size_t getSegSize() const {
        return m_dataSize;
    }

    // ==================== node heap ====================
private:
    std::byte* m_pNodeHeapBaseAddr = nullptr;
    std::size_t m_pNodeHeapCurrentOffset = 0;
    std::size_t m_pageSize = 0;
    std::size_t m_lastReservedPageIndex = 0;
    std::size_t m_lastCommittedPageIndex = 0;

public:
    void commitNewPageNodeHeap();
};
} // namespace naru::allocator::group
