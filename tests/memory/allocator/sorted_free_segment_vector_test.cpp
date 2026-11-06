#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "allocator_test_support.hpp"
#include "group/memory_config.hpp"
#include "group/sorted_free_segment_vector.hpp"
#include "memory_region_test_support.hpp"
#include "naru/support/return_code.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::tests::allocator {
namespace {
class SortedFreeSegmentVectorTest : public ::testing::Test
{
protected:
    alloc::group::SortedFreeSegVector* pVector = alloc::group::SortedFreeSegVector::createPtr();

    void TearDown() override {
        const std::size_t pageSize = alloc::platform::getCommonPageSize();
        const alloc::platform::VmMemoryInfo info{
            .pageSize = pageSize,
            .reservePageCount = config::METADATA_RESERVE_SIZE / pageSize,
        };
        std::destroy_at(pVector);
        alloc::platform::releaseEntireMemory(pVector, info);
    }
};

TEST_F(SortedFreeSegmentVectorTest, CreatedVectorHasAnEmptyRange) {
    EXPECT_EQ(pVector->getSegCount(), 0);
    EXPECT_NE(pVector->begin(), nullptr);
    EXPECT_EQ(pVector->begin(), pVector->end());
}

TEST_F(SortedFreeSegmentVectorTest, AppendsPreserveAddressSizePairsAndCount) {
    alignas(16) std::array<std::byte, 64> payload{};
    const std::array<alloc::group::SortedFreeSegVector::Segment, 3> segments{{
        {payload.data(), 16},
        {payload.data() + 16, 32},
        {payload.data() + 48, 16},
    }};
    for (std::size_t index = 0; index < segments.size(); ++index) {
        ASSERT_TRUE(pVector->emplaceBack(segments[index]).hasValue());
        EXPECT_EQ(pVector->getSegCount(), index + 1);
    }

    EXPECT_EQ(pVector->end() - pVector->begin(), segments.size());
    for (std::size_t index = 0; index < segments.size(); ++index) {
        EXPECT_EQ((*pVector)[index].pBaseAddr, segments[index].pBaseAddr);
        EXPECT_EQ((*pVector)[index].size, segments[index].size);
    }
}

TEST_F(SortedFreeSegmentVectorTest, FullVectorRejectsThenRetriesAfterCommit) {
    const MemoryRegion region = queryCommittedHeap(pVector);
    ASSERT_EQ(region.state, MemoryRegionState::COMMITTED);
    std::byte* pBaseAddr = reinterpret_cast<std::byte*>(pVector);
    alloc::group::SortedFreeSegVector::Segment* pOriginalBegin = pVector->begin();
    const std::size_t segmentOffset =
        static_cast<std::size_t>(reinterpret_cast<std::byte*>(pOriginalBegin) - pBaseAddr);
    const std::size_t capacity =
        (region.size - segmentOffset) / sizeof(alloc::group::SortedFreeSegVector::Segment);
    ASSERT_GT(capacity, 0);
    std::vector<std::byte> payload((capacity + 1) * 16);
    for (std::size_t index = 0; index < capacity; ++index) {
        ASSERT_TRUE(pVector->emplaceBack({payload.data() + index * 16, 16}).hasValue());
    }

    EX_VOID full = pVector->emplaceBack({payload.data() + capacity * 16, 16});
    ASSERT_FALSE(full.hasValue());
    EXPECT_EQ(full.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);
    EXPECT_EQ(pVector->getSegCount(), capacity);
    EXPECT_EQ(pVector->end(), pOriginalBegin + capacity);
    for (std::size_t index = 0; index < capacity; ++index) {
        EXPECT_EQ((*pVector)[index].pBaseAddr, payload.data() + index * 16);
        EXPECT_EQ((*pVector)[index].size, 16);
    }

    pVector->commitNewPageVectorHeap();
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + capacity * 16, 16}).hasValue());
    EXPECT_EQ(pVector->begin(), pOriginalBegin);
    EXPECT_EQ(pVector->getSegCount(), capacity + 1);
    for (std::size_t index = 0; index <= capacity; ++index) {
        EXPECT_EQ((*pVector)[index].pBaseAddr, payload.data() + index * 16);
        EXPECT_EQ((*pVector)[index].size, 16);
    }
}

TEST_F(SortedFreeSegmentVectorTest, ClearReusesStorageWithoutOldEntries) {
    alignas(16) std::array<std::byte, 64> payload{};
    alloc::group::SortedFreeSegVector::Segment* pOriginalBegin = pVector->begin();
    ASSERT_TRUE(pVector->emplaceBack({payload.data(), 16}).hasValue());
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + 16, 16}).hasValue());
    pVector->clear();

    EXPECT_EQ(pVector->getSegCount(), 0);
    EXPECT_EQ(pVector->begin(), pOriginalBegin);
    EXPECT_EQ(pVector->begin(), pVector->end());
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + 32, 32}).hasValue());
    ASSERT_EQ(pVector->getSegCount(), 1);
    EXPECT_EQ((*pVector)[0].pBaseAddr, payload.data() + 32);
    EXPECT_EQ((*pVector)[0].size, 32);
    EXPECT_EQ(pVector->end(), pOriginalBegin + 1);
}

TEST_F(SortedFreeSegmentVectorTest, SortingKeepsAddressesPairedWithSizes) {
    alignas(16) std::array<std::byte, 128> payload{};
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + 64, 16}).hasValue());
    ASSERT_TRUE(pVector->emplaceBack({payload.data(), 48}).hasValue());
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + 96, 32}).hasValue());
    ASSERT_TRUE(pVector->emplaceBack({payload.data() + 48, 16}).hasValue());

    std::ranges::sort(*pVector, [](const alloc::group::SortedFreeSegVector::Segment& left,
                                   const alloc::group::SortedFreeSegVector::Segment& right) {
        return std::less<std::byte*>{}(left.pBaseAddr, right.pBaseAddr);
    });

    const std::array<std::size_t, 4> offsets{0, 48, 64, 96};
    const std::array<std::size_t, 4> sizes{48, 16, 16, 32};
    ASSERT_EQ(pVector->getSegCount(), offsets.size());
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        EXPECT_EQ((*pVector)[index].pBaseAddr, payload.data() + offsets[index]);
        EXPECT_EQ((*pVector)[index].size, sizes[index]);
    }
}

TEST_F(SortedFreeSegmentVectorTest, CommitGrowthPreservesStorageAndExtendsCommittedRegion) {
    std::byte* pBaseAddr = reinterpret_cast<std::byte*>(pVector);
    alloc::group::SortedFreeSegVector::Segment* pSegments = pVector->begin();
    const MemoryRegion initialRegion = queryCommittedHeap(pBaseAddr);
    ASSERT_EQ(initialRegion.state, MemoryRegionState::COMMITTED);
    ASSERT_EQ(initialRegion.pBaseAddr, static_cast<void*>(pBaseAddr));
    ASSERT_GT(initialRegion.size, 0);

    std::byte* pOriginalLastByte = pBaseAddr + initialRegion.size - 1;
    *pOriginalLastByte = std::byte{0x31};

    for (std::size_t count = 1; count <= 2; ++count) {
        SCOPED_TRACE(count);
        pVector->commitNewPageVectorHeap();

        const MemoryRegion grownRegion = queryCommittedHeap(pBaseAddr);
        EXPECT_EQ(grownRegion.pBaseAddr, initialRegion.pBaseAddr);
        EXPECT_EQ(grownRegion.state, MemoryRegionState::COMMITTED);
        ASSERT_EQ(grownRegion.size, initialRegion.size + count * config::COMMIT_SIZE);
        EXPECT_EQ(pVector->begin(), pSegments);
        EXPECT_EQ(pVector->end(), pSegments);
        EXPECT_EQ(*pOriginalLastByte, std::byte{0x31});

        std::byte* pLastByte = pBaseAddr + grownRegion.size - 1;
        *pLastByte = std::byte{0x52};
        EXPECT_EQ(*pLastByte, std::byte{0x52});

        const MemoryRegion nextPageRegion = queryMemoryRegion(pBaseAddr + grownRegion.size);
        EXPECT_EQ(nextPageRegion.state, MemoryRegionState::RESERVED);
    }
}
} // namespace
} // namespace naru::tests::allocator
