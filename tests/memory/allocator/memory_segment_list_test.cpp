#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <type_traits>
#include <vector>

#include "allocator_test_support.hpp"
#include "group/memory_config.hpp"
#include "group/memory_segment_list.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "naru/support/return_code.hpp"
#include "platform/memory.hpp"

namespace naru::tests::allocator {
namespace {
static_assert(!std::is_copy_constructible_v<alloc::group::MemorySegList>);
static_assert(!std::is_copy_assignable_v<alloc::group::MemorySegList>);
static_assert(!std::is_move_constructible_v<alloc::group::MemorySegList>);
static_assert(!std::is_move_assignable_v<alloc::group::MemorySegList>);

TEST(MemorySegmentListLifecycleTest, DestroyResetsLiveAndReusableNodesWithoutAssignment) {
    alloc::group::MemorySegList list =
        alloc::group::MemorySegList::createVal({.groupId = 12, .segSize = 16});
    std::array<std::byte, 32> payload{};
    ASSERT_TRUE(list.emplaceBack({payload.data()}).hasValue());
    ASSERT_TRUE(list.emplaceBack({payload.data() + 16}).hasValue());
    list.eraseAt(list.begin());
    ASSERT_FALSE(list.isEmpty());

    list.destroy();

    EXPECT_TRUE(list.isEmpty());
    EXPECT_EQ(list.begin(), list.end());
    EXPECT_EQ(list.getSegSize(), 0);
    list.reset();
    EXPECT_TRUE(list.isEmpty());
    EXPECT_EQ(list.getSegSize(), 0);
}

class MemorySegmentListTest : public ::testing::Test
{
protected:
    void SetUp() override {
        ASSERT_TRUE(lists.emplaceBack(12, 16).hasValue());
        m_pList = &lists[0];
    }

    void append(std::initializer_list<std::size_t> offsets) {
        for (std::size_t offset : offsets) {
            ASSERT_TRUE(m_pList->emplaceBack({payload.data() + offset}).hasValue());
        }
    }

    alloc::group::MemorySegListVector lists =
        alloc::group::MemorySegListVector::createVal({.isFree = false});
    alloc::group::MemorySegList* m_pList = nullptr;
    alignas(64) std::array<std::byte, 128> payload{};
};

TEST_F(MemorySegmentListTest, AppendsInInsertionOrder) {
    EXPECT_TRUE(m_pList->isEmpty());
    EXPECT_EQ(m_pList->begin(), m_pList->end());
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32}));

    ASSERT_NO_FATAL_FAILURE(
        expectSegments(*m_pList, {payload.data(), payload.data() + 16, payload.data() + 32}));
}

TEST_F(MemorySegmentListTest, EmplaceReturnsTheInsertedAndReusedNode) {
    EX_TYPE<alloc::group::MemorySegList::Iterator> first = m_pList->emplaceBack({payload.data()});
    EX_TYPE<alloc::group::MemorySegList::Iterator> second =
        m_pList->emplaceBack({payload.data() + 16});
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value(), m_pList->begin());
    EXPECT_EQ(second.value()->pBaseAddr, payload.data() + 16);
    auto next = first.value();
    ++next;
    EXPECT_EQ(next, second.value());

    const alloc::group::MemorySegList::Segment* pSlot = &*first.value();
    m_pList->eraseAt(first.value());
    EX_TYPE<alloc::group::MemorySegList::Iterator> reused =
        m_pList->emplaceBack({payload.data() + 32});
    ASSERT_TRUE(reused.hasValue());
    EXPECT_EQ(&*reused.value(), pSlot);
    EXPECT_EQ(reused.value()->pBaseAddr, payload.data() + 32);
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 16, payload.data() + 32}));
}

TEST_F(MemorySegmentListTest, FindReturnsMatchingNodeOrMissingError) {
    EX_TYPE<alloc::group::MemorySegList::Iterator> empty = m_pList->find(payload.data());
    ASSERT_FALSE(empty.hasValue());
    EXPECT_EQ(empty.error(), ErrorCode::MEMORY_DATA_NOT_FOUND);
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32}));

    EX_TYPE<alloc::group::MemorySegList::Iterator> found = m_pList->find(payload.data() + 16);
    ASSERT_TRUE(found.hasValue());
    EXPECT_EQ(found.value()->pBaseAddr, payload.data() + 16);
    auto next = found.value();
    ++next;
    ASSERT_NE(next, m_pList->end());
    EXPECT_EQ(next->pBaseAddr, payload.data() + 32);

    EX_TYPE<alloc::group::MemorySegList::Iterator> missing = m_pList->find(payload.data() + 48);
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error(), ErrorCode::MEMORY_DATA_NOT_FOUND);
}

TEST_F(MemorySegmentListTest, PopRemovesOnlyTheFirstNode) {
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32}));
    m_pList->pop();
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 16, payload.data() + 32}));
    m_pList->pop();
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 32}));
    m_pList->pop();
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {}));

    ASSERT_NO_FATAL_FAILURE(append({48, 64}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 48, payload.data() + 64}));
}

TEST_F(MemorySegmentListTest, EraseSoleNodeAllowsReinsertion) {
    ASSERT_NO_FATAL_FAILURE(append({0}));
    m_pList->eraseAt(m_pList->begin());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {}));

    ASSERT_NO_FATAL_FAILURE(append({16, 32}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 16, payload.data() + 32}));
}

TEST_F(MemorySegmentListTest, EraseHeadKeepsTheNewHeadLinked) {
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32}));
    m_pList->eraseAt(m_pList->begin());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 16, payload.data() + 32}));
    m_pList->eraseAt(m_pList->begin());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data() + 32}));
}

TEST_F(MemorySegmentListTest, EraseTailAllowsAppendingToTheNewTail) {
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32}));
    EX_TYPE<alloc::group::MemorySegList::Iterator> tail = m_pList->find(payload.data() + 32);
    ASSERT_TRUE(tail.hasValue());
    m_pList->eraseAt(tail.value());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data(), payload.data() + 16}));

    ASSERT_NO_FATAL_FAILURE(append({48}));
    EX_TYPE<alloc::group::MemorySegList::Iterator> newTail = m_pList->find(payload.data() + 48);
    ASSERT_TRUE(newTail.hasValue());
    m_pList->eraseAt(newTail.value());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data(), payload.data() + 16}));
}

TEST_F(MemorySegmentListTest, EraseMiddleKeepsBothNeighborsLinked) {
    ASSERT_NO_FATAL_FAILURE(append({0, 16, 32, 48}));
    EX_TYPE<alloc::group::MemorySegList::Iterator> middle = m_pList->find(payload.data() + 16);
    ASSERT_TRUE(middle.hasValue());
    m_pList->eraseAt(middle.value());
    ASSERT_NO_FATAL_FAILURE(
        expectSegments(*m_pList, {payload.data(), payload.data() + 32, payload.data() + 48}));

    EX_TYPE<alloc::group::MemorySegList::Iterator> successor = m_pList->find(payload.data() + 32);
    ASSERT_TRUE(successor.hasValue());
    m_pList->eraseAt(successor.value());
    ASSERT_NO_FATAL_FAILURE(expectSegments(*m_pList, {payload.data(), payload.data() + 48}));
}

TEST_F(MemorySegmentListTest, FullNodeHeapPreservesNodesAndCommitAllowsRetry) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::size_t committedSize =
        (config::METADATA_NODE_COMMIT_SIZE + pageSize - 1) / pageSize * pageSize;
    const std::size_t capacity = committedSize / config::METADATA_NODE_SLOT_SIZE;
    std::vector<std::byte> bytes((capacity + 1) * 16);

    for (std::size_t index = 0; index < capacity; ++index) {
        SCOPED_TRACE(index);
        ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + index * 16}).hasValue());
    }

    EX_TYPE<alloc::group::MemorySegList::Iterator> full =
        m_pList->emplaceBack({bytes.data() + capacity * 16});
    ASSERT_FALSE(full.hasValue());
    EXPECT_EQ(full.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);

    auto it = m_pList->begin();
    for (std::size_t index = 0; index < capacity; ++index) {
        ASSERT_NE(it, m_pList->end());
        EXPECT_EQ(it->pBaseAddr, bytes.data() + index * 16);
        ++it;
    }
    EXPECT_EQ(it, m_pList->end());

    m_pList->commitNewPageNodeHeap();
    ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + capacity * 16}).hasValue());
    it = m_pList->begin();
    for (std::size_t index = 0; index <= capacity; ++index) {
        ASSERT_NE(it, m_pList->end());
        EXPECT_EQ(it->pBaseAddr, bytes.data() + index * 16);
        ++it;
    }
    EXPECT_EQ(it, m_pList->end());
}

TEST_F(MemorySegmentListTest, FullNodeHeapReusesErasedNodesWithoutCommit) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::size_t committedSize =
        (config::METADATA_NODE_COMMIT_SIZE + pageSize - 1) / pageSize * pageSize;
    const std::size_t capacity = committedSize / config::METADATA_NODE_SLOT_SIZE;
    std::vector<std::byte> bytes((capacity + 3) * 16);

    for (std::size_t index = 0; index < capacity; ++index) {
        ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + index * 16}).hasValue());
    }

    const std::array<std::size_t, 3> erased{0, capacity / 2, capacity - 1};
    for (std::size_t index : erased) {
        EX_TYPE<alloc::group::MemorySegList::Iterator> found =
            m_pList->find(bytes.data() + index * 16);
        ASSERT_TRUE(found.hasValue());
        m_pList->eraseAt(found.value());
    }

    for (std::size_t index = capacity; index < capacity + erased.size(); ++index) {
        ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + index * 16}).hasValue());
    }

    EX_TYPE<alloc::group::MemorySegList::Iterator> full = m_pList->emplaceBack({payload.data()});
    ASSERT_FALSE(full.hasValue());
    EXPECT_EQ(full.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);

    auto it = m_pList->begin();
    for (std::size_t index = 0; index < capacity + erased.size(); ++index) {
        if (index == erased[0] || index == erased[1] || index == erased[2]) {
            continue;
        }
        ASSERT_NE(it, m_pList->end());
        EXPECT_EQ(it->pBaseAddr, bytes.data() + index * 16);
        ++it;
    }
    EXPECT_EQ(it, m_pList->end());
}

TEST_F(MemorySegmentListTest, FullNodeHeapReusesPoppedNodesAfterEmptying) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::size_t committedSize =
        (config::METADATA_NODE_COMMIT_SIZE + pageSize - 1) / pageSize * pageSize;
    const std::size_t capacity = committedSize / config::METADATA_NODE_SLOT_SIZE;
    std::vector<std::byte> bytes(capacity * 16);

    for (std::size_t index = 0; index < capacity; ++index) {
        ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + index * 16}).hasValue());
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        m_pList->pop();
    }
    ASSERT_TRUE(m_pList->isEmpty());

    for (std::size_t index = capacity; index != 0; --index) {
        ASSERT_TRUE(m_pList->emplaceBack({bytes.data() + (index - 1) * 16}).hasValue());
    }

    EX_TYPE<alloc::group::MemorySegList::Iterator> full = m_pList->emplaceBack({payload.data()});
    ASSERT_FALSE(full.hasValue());
    EXPECT_EQ(full.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);

    auto it = m_pList->begin();
    for (std::size_t index = capacity; index != 0; --index) {
        ASSERT_NE(it, m_pList->end());
        EXPECT_EQ(it->pBaseAddr, bytes.data() + (index - 1) * 16);
        ++it;
    }
    EXPECT_EQ(it, m_pList->end());
}
} // namespace
} // namespace naru::tests::allocator
