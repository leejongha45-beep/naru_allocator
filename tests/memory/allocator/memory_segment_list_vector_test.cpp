#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <type_traits>

#include "allocator_test_support.hpp"
#include "group/memory_config.hpp"
#include "group/memory_segment_list.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "naru/support/return_code.hpp"
#include "platform/memory.hpp"

namespace naru::tests::allocator {
namespace {
static_assert(!std::is_default_constructible_v<alloc::group::MemorySegListVector>);
static_assert(!std::is_copy_constructible_v<alloc::group::MemorySegListVector>);
static_assert(!std::is_copy_assignable_v<alloc::group::MemorySegListVector>);
static_assert(!std::is_move_constructible_v<alloc::group::MemorySegListVector>);
static_assert(!std::is_move_assignable_v<alloc::group::MemorySegListVector>);
static_assert(std::is_same_v<decltype(alloc::group::MemorySegListVector::createVal()),
                             alloc::group::MemorySegListVector>);

class MemorySegmentListVectorTest : public ::testing::Test
{
protected:
    alloc::group::MemorySegListVector lists =
        alloc::group::MemorySegListVector::createVal({.isFree = false});
};

TEST_F(MemorySegmentListVectorTest, CreatedVectorIsEmpty) {
    EXPECT_TRUE(lists.isEmpty());
    EXPECT_EQ(lists.getListCount(), 0);
    EX_REF<alloc::group::MemorySegList> missing = lists.find(16);
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error(), ErrorCode::MEMORY_DATA_NOT_FOUND);
}

TEST_F(MemorySegmentListVectorTest, AppendedListsRetainTheirSizes) {
    ASSERT_EQ(sizeof(alloc::group::MemorySegList), config::METADATA_LIST_SLOT_SIZE);
    const std::array<std::size_t, 3> sizes{16, 32, 48};
    for (std::size_t size : sizes) {
        ASSERT_TRUE(lists.emplaceBack(21, size).hasValue());
    }

    EXPECT_FALSE(lists.isEmpty());
    ASSERT_EQ(lists.getListCount(), sizes.size());
    for (std::size_t index = 0; index < lists.getListCount(); ++index) {
        alloc::group::MemorySegList& list = lists[index];
        EXPECT_EQ(list.getSegSize(), sizes[index]);
        EXPECT_TRUE(list.isEmpty());
        EX_REF<alloc::group::MemorySegList> found = lists.find(sizes[index]);
        ASSERT_TRUE(found.hasValue());
        EXPECT_EQ(&found.value(), &list);
    }

    EX_REF<alloc::group::MemorySegList> missing = lists.find(64);
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error(), ErrorCode::MEMORY_DATA_NOT_FOUND);
}

TEST_F(MemorySegmentListVectorTest, IndexingAndBackReferToExistingLists) {
    ASSERT_TRUE(lists.emplaceBack(4, 16).hasValue());
    ASSERT_TRUE(lists.emplaceBack(4, 48).hasValue());
    ASSERT_TRUE(lists.emplaceBack(4, 80).hasValue());

    ASSERT_EQ(lists.getListCount(), 3);
    EXPECT_EQ(lists[0].getSegSize(), 16);
    EXPECT_EQ(lists[1].getSegSize(), 48);
    EXPECT_EQ(lists[2].getSegSize(), 80);
    EXPECT_EQ(&lists[2], &lists.back());
}

TEST_F(MemorySegmentListVectorTest, CommitGrowthPreservesListAddressesAndNodes) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::size_t committedSize =
        (config::METADATA_LIST_COMMIT_SIZE + pageSize - 1) / pageSize * pageSize;
    const std::size_t capacity = committedSize / config::METADATA_LIST_SLOT_SIZE;
    for (std::size_t index = 0; index < capacity; ++index) {
        SCOPED_TRACE(index);
        ASSERT_TRUE(lists.emplaceBack(21, (index + 1) * 16).hasValue());
    }

    std::array<std::byte, 16> payload{};
    alloc::group::MemorySegList* pFirst = &lists[0];
    ASSERT_TRUE(pFirst->emplaceBack({payload.data()}).hasValue());
    const std::size_t oldCount = lists.getListCount();
    const std::byte* pNextSlot =
        reinterpret_cast<std::byte*>(pFirst) + oldCount * config::METADATA_LIST_SLOT_SIZE;
    const std::size_t nextSize = (capacity + 1) * 16;
    EX_VOID full = lists.emplaceBack(21, nextSize);
    ASSERT_FALSE(full.hasValue());
    EXPECT_EQ(full.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);
    EXPECT_EQ(lists.getListCount(), oldCount);
    EXPECT_EQ(lists.back().getSegSize(), capacity * 16);

    lists.commitNewPageVectorHeap();
    ASSERT_TRUE(lists.emplaceBack(21, nextSize).hasValue());
    EXPECT_EQ(&lists[0], pFirst);
    ASSERT_NO_FATAL_FAILURE(expectSegments(*pFirst, {payload.data()}));
    EX_REF<alloc::group::MemorySegList> added = lists.find(nextSize);
    ASSERT_TRUE(added.hasValue());
    EXPECT_EQ(reinterpret_cast<std::byte*>(&added.value()), pNextSlot);
    EXPECT_EQ(&added.value(), &lists.back());

    ASSERT_EQ(lists.getListCount(), capacity + 1);
    for (std::size_t index = 0; index < lists.getListCount(); ++index) {
        EXPECT_EQ(lists[index].getSegSize(), (index + 1) * 16);
    }
}

#ifndef NDEBUG
using MemorySegmentListVectorDeathTest = MemorySegmentListVectorTest;

TEST_F(MemorySegmentListVectorDeathTest, IndexEqualToListCountTerminates) {
    ASSERT_TRUE(lists.emplaceBack(4, 16).hasValue());

    ASSERT_DEATH(
        { static_cast<void>(lists[lists.getListCount()]); },
        "Check failed: expression=index < m_listCount");
}

TEST_F(MemorySegmentListVectorDeathTest, BackOnEmptyVectorTerminates) {
    ASSERT_DEATH({ static_cast<void>(lists.back()); }, "Check failed: expression=m_listCount != 0");
}
#endif
} // namespace
} // namespace naru::tests::allocator
