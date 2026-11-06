#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <new>
#include <string>
#include <type_traits>

#include "allocator_test_support.hpp"
// Compile the implementation in this test translation unit to inspect its cpp-local Impl.
// Public API tests and consumers separately link the production archive.
#include "group/group_allocator.cpp"
#include "group/memory_segment_list.hpp"
#include "group/memory_segment_list_storage.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "naru/allocator/group/group_allocator.hpp"
#include "naru/support/return_code.hpp"
#include "naru/support/static_pimpl.hpp"

namespace naru::tests::allocator {
namespace {
static_assert(!std::is_default_constructible_v<alloc::group::MemorySegList>);
static_assert(!std::is_copy_constructible_v<alloc::group::GroupAllocator::Impl>);
static_assert(!std::is_copy_assignable_v<alloc::group::GroupAllocator::Impl>);
static_assert(!std::is_move_constructible_v<alloc::group::GroupAllocator::Impl>);
static_assert(!std::is_move_assignable_v<alloc::group::GroupAllocator::Impl>);

void expectUnallocatedGroup(alloc::group::MemorySegListStorage& storage, std::uint8_t groupId) {
    EXPECT_FALSE(storage.isExistGroup(groupId));
    EXPECT_EQ(storage.getGroupBaseHeapAddr(groupId), nullptr);
    EXPECT_EQ(storage.getPageSize(groupId), 0);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 0);
    EXPECT_EQ(storage.getLastReservedPageIndex(groupId), config::INVALID_PAGE_INDEX);
    EXPECT_EQ(storage.getLastCommittedPageIndex(groupId), config::INVALID_PAGE_INDEX);

    alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    EXPECT_EQ(usedLists.m_pVectorHeapBaseAddr, nullptr);
    EXPECT_EQ(freeLists.m_pVectorHeapBaseAddr, nullptr);
    EXPECT_EQ(usedLists.getListCount(), 0);
    EXPECT_EQ(freeLists.getListCount(), 0);
    EXPECT_EQ(usedLists.m_pageSize, 0);
    EXPECT_EQ(freeLists.m_pageSize, 0);
    EXPECT_EQ(usedLists.m_lastCommittedPageIndex, 0);
    EXPECT_EQ(freeLists.m_lastCommittedPageIndex, 0);
    EXPECT_EQ(usedLists.m_pSortedFreeSegVector, nullptr);
    EXPECT_EQ(freeLists.m_pSortedFreeSegVector, nullptr);
}

void expectAlignedSegments(alloc::group::MemorySegListVector& lists) {
    for (std::size_t index = 0; index < lists.getListCount(); ++index) {
        alloc::group::MemorySegList& list = lists[index];
        const std::size_t segSize = list.getSegSize();
        ASSERT_GT(segSize, 0);
        EXPECT_EQ(segSize % config::SEGMENT_SIZE_UNIT, 0);
        for (alloc::group::MemorySegList::Iterator segment = list.begin(); segment != list.end();
             ++segment) {
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(segment->pBaseAddr) % segSize, 0);
        }
    }
}

TEST(GroupAllocatorFactoryTest, StorageCreatesInlineArraysWithoutAllocatingGroupHeaps) {
    alloc::group::MemorySegListStorage storage = alloc::group::MemorySegListStorage::createVal();
    static_assert(std::is_same_v<decltype(storage.m_sameGroupUsedLists),
                                 std::array<alloc::group::MemorySegListVector, 256>>);
    static_assert(std::is_same_v<decltype(storage.m_sameGroupFreeLists),
                                 std::array<alloc::group::MemorySegListVector, 256>>);

    const std::uintptr_t storageBegin = reinterpret_cast<std::uintptr_t>(&storage);
    const std::uintptr_t storageEnd = storageBegin + sizeof(storage);
    for (std::size_t index = 0; index < storage.m_groupsData.size(); ++index) {
        SCOPED_TRACE(index);
        const std::uint8_t groupId = static_cast<std::uint8_t>(index);
        expectUnallocatedGroup(storage, groupId);

        const std::uintptr_t usedAddress =
            reinterpret_cast<std::uintptr_t>(&storage.getSameGroupUsedLists(groupId));
        const std::uintptr_t freeAddress =
            reinterpret_cast<std::uintptr_t>(&storage.getSameGroupFreeLists(groupId));
        EXPECT_GE(usedAddress, storageBegin);
        EXPECT_LE(usedAddress + sizeof(alloc::group::MemorySegListVector), storageEnd);
        EXPECT_GE(freeAddress, storageBegin);
        EXPECT_LE(freeAddress + sizeof(alloc::group::MemorySegListVector), storageEnd);
    }
}

TEST(GroupAllocatorFactoryTest, FirstAllocationInitializesOnlyTheRequestedGroup) {
    alloc::group::GroupAllocator allocator{};
    alloc::group::GroupAllocator::Impl& group = *allocator.m_impl;
    alloc::group::MemorySegListStorage& storage = group.m_segListStorage;
    constexpr std::uint8_t groupId = 255;

    std::byte* pMemory = allocator.galloc(groupId, std::type_identity<std::uint64_t>{});
    ASSERT_NE(pMemory, nullptr);
    pMemory[0] = std::byte{0x5a};
    EXPECT_EQ(pMemory[0], std::byte{0x5a});
    EXPECT_TRUE(storage.isExistGroup(groupId));
    EXPECT_NE(storage.getGroupBaseHeapAddr(groupId), nullptr);
    EXPECT_NE(storage.getSameGroupUsedLists(groupId).m_pVectorHeapBaseAddr, nullptr);
    EXPECT_NE(storage.getSameGroupFreeLists(groupId).m_pVectorHeapBaseAddr, nullptr);
    EXPECT_NE(storage.getSameGroupFreeLists(groupId).m_pSortedFreeSegVector, nullptr);

    for (std::size_t index = 0; index < storage.m_groupsData.size(); ++index) {
        if (index != groupId) {
            SCOPED_TRACE(index);
            expectUnallocatedGroup(storage, static_cast<std::uint8_t>(index));
        }
    }

    allocator.gfree(groupId, sizeof(std::uint64_t), pMemory);
    std::byte* pReused = allocator.galloc(groupId, std::type_identity<std::uint64_t>{});
    EXPECT_EQ(pReused, pMemory);
    allocator.gfree(groupId, sizeof(std::uint64_t), pReused);
}

class GroupAllocatorImplTest : public ::testing::Test
{
protected:
    void SetUp() override {
        storage.setGroupBaseAddr(groupId, heap.data());
        storage.setPageSize(groupId, heap.size());
        storage.setLastReservedPageIndex(groupId, 0);
        storage.setLastCommittedPageIndex(groupId, 0);
        alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
        alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
        new (static_cast<void*>(&freeLists)) alloc::group::MemorySegListVector(
            alloc::group::MemorySegListVector::createVal({.isFree = true}));
        new (static_cast<void*>(&usedLists)) alloc::group::MemorySegListVector(
            alloc::group::MemorySegListVector::createVal({.isFree = false}));
        storage.setGroupUsedState(groupId, true);
    }

    void TearDown() override {
        storage.getSameGroupUsedLists(groupId).destroy();
        storage.getSameGroupFreeLists(groupId).destroy();
        storage.reset();
    }

    std::byte* setHeapBaseResidue(std::size_t period, std::size_t residue) {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(heap.data());
        const std::size_t offset = (period + residue - address % period) % period;
        std::byte* const pBase = heap.data() + offset;
        storage.setGroupBaseAddr(groupId, pBase);
        storage.setPageSize(groupId, heap.size() - offset);
        return pBase;
    }

    void seedFreeSegment(alloc::group::GroupAllocator::TypeInfo typeInfo, std::size_t offset) {
        storage.setCurrentOffset(groupId, offset);
        std::byte* pResult = allocator.galloc(allocatorGroupId, typeInfo);
        ASSERT_NE(pResult, nullptr);
        ASSERT_EQ(pResult, storage.getGroupBaseHeapAddr(groupId) + offset);
        allocator.gfree(allocatorGroupId, typeInfo.size, pResult);
    }

    static constexpr std::uint8_t groupId = 7;
    static constexpr std::uint8_t allocatorGroupId = groupId;
    alignas(64) std::array<std::byte, 4096> heap{};
    alloc::group::GroupAllocator allocator{};
    alloc::group::GroupAllocator::Impl& group = *allocator.m_impl;
    alloc::group::MemorySegListStorage& storage = group.m_segListStorage;
};

TEST_F(GroupAllocatorImplTest, AcquireBatchReturnsFirstAddress) {
    EX_TYPE<alloc::group::MemorySegList::Segment> result =
        storage.acquireAlignedHeapRange(groupId, 32, 3);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().pBaseAddr, heap.data());
    EXPECT_EQ(storage.getCurrentOffset(groupId), 96);
}

TEST_F(GroupAllocatorImplTest, ExactFitSucceedsAndFollowingRequestFails) {
    EX_TYPE<alloc::group::MemorySegList::Segment> result =
        storage.acquireAlignedHeapRange(groupId, 16, heap.size() / 16);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().pBaseAddr, heap.data());
    EXPECT_EQ(storage.getCurrentOffset(groupId), heap.size());

    EX_TYPE<alloc::group::MemorySegList::Segment> overflow =
        storage.acquireAlignedHeapRange(groupId, 16);
    ASSERT_FALSE(overflow.hasValue());
    EXPECT_EQ(overflow.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);
    EXPECT_EQ(storage.getCurrentOffset(groupId), heap.size());
}

struct AcquireFailureCase {
    std::size_t offset;
    std::size_t size;
    std::size_t count;
};

class GroupAllocatorImplCapacityTest : public GroupAllocatorImplTest,
                                       public ::testing::WithParamInterface<AcquireFailureCase>
{
};

TEST_P(GroupAllocatorImplCapacityTest, FailedAcquirePreservesState) {
    const AcquireFailureCase input = GetParam();
    storage.setCurrentOffset(groupId, input.offset);
    heap.fill(std::byte{0x5a});
    const std::array<std::byte, 4096> originalBytes = heap;

    EX_TYPE<alloc::group::MemorySegList::Segment> result =
        storage.acquireAlignedHeapRange(groupId, input.size, input.count);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);
    EXPECT_EQ(storage.getCurrentOffset(groupId), input.offset);
    EXPECT_EQ(storage.getLastCommittedPageIndex(groupId), 0);
    EXPECT_EQ(heap, originalBytes);
    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
    EXPECT_TRUE(freeLists.isEmpty());
    EXPECT_TRUE(usedLists.isEmpty());
}

INSTANTIATE_TEST_SUITE_P(
    BoundaryRequests, GroupAllocatorImplCapacityTest,
    ::testing::Values(AcquireFailureCase{0, 16, 257}, AcquireFailureCase{16, 32, 128},
                      AcquireFailureCase{4080, 48, 1}, AcquireFailureCase{4080, 8192, 1},
                      AcquireFailureCase{0, 16, (std::numeric_limits<std::size_t>::max)()}));

TEST_F(GroupAllocatorImplTest, PaddingShortagePreservesStateAndDeterminesCommitPages) {
    std::byte* const pBase = setHeapBaseResidue(192, 64);
    storage.setPageSize(groupId, 64);
    storage.setLastCommittedPageIndex(groupId, 2);
    storage.setLastReservedPageIndex(groupId, 3);
    std::byte* const pLive = allocator.galloc(groupId, std::type_identity<std::byte[16]>{});
    ASSERT_EQ(pLive, pBase);
    std::fill_n(pLive, 16, std::byte{0x5a});
    allocator.greserve(groupId, 16, 2);
    ASSERT_EQ(storage.getCurrentOffset(groupId), 48);

    // 144B remain committed. The request needs 80B of padding plus 96B of storage.
    const EX_TYPE<alloc::group::MemorySegList::Segment> result =
        storage.acquireAlignedHeapRange(groupId, 96);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ErrorCode::MEMORY_INSUFFICIENT_COMMITTED_SPACE);
    EXPECT_EQ(storage.calcRequiredCommitPageCount(groupId, 96), 1);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 48);
    EXPECT_EQ(storage.getLastCommittedPageIndex(groupId), 2);

    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
    ASSERT_EQ(freeLists.getListCount(), 1);
    ASSERT_EQ(usedLists.getListCount(), 1);
    ASSERT_NO_FATAL_FAILURE(expectSegments(freeLists[0], {pBase + 16, pBase + 32}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(usedLists[0], {pLive}));
    EXPECT_TRUE(std::all_of(pLive, pLive + 16,
                            [](std::byte value) { return value == std::byte{0x5a}; }));
}

TEST_F(GroupAllocatorImplTest, FirstAllocationAligns48ByteSegmentFromMisalignedBase) {
    std::byte* const pBase = setHeapBaseResidue(192, 64);
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(pBase) % 48, 16);

    std::byte* const pResult = allocator.galloc(groupId, std::type_identity<std::byte[33]>{});
    EXPECT_EQ(pResult, pBase + 32);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 80);

    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    EX_REF<alloc::group::MemorySegList> padding = freeLists.find(32);
    ASSERT_TRUE(padding.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(padding.value(), {pBase}));
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(freeLists));
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(storage.getSameGroupUsedLists(groupId)));
}

TEST_F(GroupAllocatorImplTest, AllocateRegistersExactlyOneUsedSegment) {
    std::byte* pResult = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
    ASSERT_NE(pResult, nullptr);
    EXPECT_EQ(pResult, heap.data());
    EXPECT_EQ(storage.getCurrentOffset(groupId), 16);

    EX_REF<alloc::group::MemorySegList> used = storage.getSameGroupUsedLists(groupId).find(16);
    EX_REF<alloc::group::MemorySegList> free = storage.getSameGroupFreeLists(groupId).find(16);
    ASSERT_TRUE(used.hasValue());
    ASSERT_TRUE(free.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(used.value(), {heap.data()}));
    EXPECT_TRUE(free.value().isEmpty());
}

TEST_F(GroupAllocatorImplTest, ExactSizeFreeSegmentIsReusedWithoutGrowingHeap) {
    ASSERT_NO_FATAL_FAILURE(seedFreeSegment(std::type_identity<std::byte[16]>{}, 0));
    storage.setCurrentOffset(groupId, 16);

    std::byte* pResult = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});

    ASSERT_NE(pResult, nullptr);
    EXPECT_EQ(pResult, heap.data());
    EXPECT_EQ(storage.getCurrentOffset(groupId), 16);
    EX_REF<alloc::group::MemorySegList> free = storage.getSameGroupFreeLists(groupId).find(16);
    ASSERT_TRUE(free.hasValue());
    EXPECT_TRUE(free.value().isEmpty());
}

TEST_F(GroupAllocatorImplTest, RepeatedFreeAndAllocateReuseEachListsNodeSlot) {
    std::byte* pFirst = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
    std::byte* pSecond = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
    ASSERT_NE(pFirst, nullptr);
    ASSERT_NE(pSecond, nullptr);
    std::fill_n(pFirst, 16, std::byte{0x3c});

    EX_REF<alloc::group::MemorySegList> used = storage.getSameGroupUsedLists(groupId).find(16);
    EX_REF<alloc::group::MemorySegList> free = storage.getSameGroupFreeLists(groupId).find(16);
    ASSERT_TRUE(used.hasValue());
    ASSERT_TRUE(free.hasValue());
    EX_TYPE<alloc::group::MemorySegList::Iterator> usedNode = used.value().find(pSecond);
    ASSERT_TRUE(usedNode.hasValue());
    const alloc::group::MemorySegList::Segment* pUsedSlot = &*usedNode.value();

    allocator.gfree(allocatorGroupId, 16, pSecond);
    ASSERT_NO_FATAL_FAILURE(expectSegments(free.value(), {pSecond}));
    const alloc::group::MemorySegList::Segment* pFreeSlot = &*free.value().begin();
    EXPECT_NE(pUsedSlot, pFreeSlot);

    for (std::size_t index = 0; index < 64; ++index) {
        SCOPED_TRACE(index);
        std::byte* pResult =
            allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
        ASSERT_NE(pResult, nullptr);
        EXPECT_EQ(pResult, pSecond);
        EXPECT_TRUE(free.value().isEmpty());

        EX_TYPE<alloc::group::MemorySegList::Iterator> reused = used.value().find(pResult);
        ASSERT_TRUE(reused.hasValue());
        EXPECT_EQ(&*reused.value(), pUsedSlot);
        ASSERT_NO_FATAL_FAILURE(expectSegments(used.value(), {pFirst, pResult}));

        allocator.gfree(allocatorGroupId, 16, pResult);
        ASSERT_NO_FATAL_FAILURE(expectSegments(used.value(), {pFirst}));
        ASSERT_NO_FATAL_FAILURE(expectSegments(free.value(), {pSecond}));
        EXPECT_EQ(&*free.value().begin(), pFreeSlot);
    }

    EXPECT_EQ(storage.getCurrentOffset(groupId), 32);
    EXPECT_TRUE(std::all_of(heap.begin(), heap.begin() + 16,
                            [](std::byte value) { return value == std::byte{0x3c}; }));
}

TEST_F(GroupAllocatorImplTest, ExactSizeListTakesPriorityOverLargerList) {
    std::byte* pLarge = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[64]>{});
    std::byte* pSmallSeg = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
    ASSERT_NE(pLarge, nullptr);
    ASSERT_NE(pSmallSeg, nullptr);
    allocator.gfree(allocatorGroupId, 64, pLarge);
    allocator.gfree(allocatorGroupId, 16, pSmallSeg);

    std::byte* pResult = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});

    ASSERT_NE(pResult, nullptr);
    EXPECT_EQ(pResult, heap.data() + 64);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 80);
    EX_REF<alloc::group::MemorySegList> larger = storage.getSameGroupFreeLists(groupId).find(64);
    ASSERT_TRUE(larger.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(larger.value(), {heap.data()}));
}

TEST_F(GroupAllocatorImplTest, NonPowerOfTwoMultipleSplitsIntoDistinctSegments) {
    std::byte* const pBase = setHeapBaseResidue(192, 0);
    ASSERT_NO_FATAL_FAILURE(seedFreeSegment(std::type_identity<std::byte[48]>{}, 0));
    storage.setCurrentOffset(groupId, 48);

    for (std::size_t index = 0; index < 3; ++index) {
        std::byte* pResult =
            allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
        ASSERT_NE(pResult, nullptr);
        EXPECT_EQ(pResult, pBase + index * 16);
        EXPECT_EQ(storage.getCurrentOffset(groupId), 48);
    }

    EX_REF<alloc::group::MemorySegList> source = storage.getSameGroupFreeLists(groupId).find(48);
    ASSERT_TRUE(source.hasValue());
    EXPECT_TRUE(source.value().isEmpty());

    std::byte* pNext = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
    ASSERT_NE(pNext, nullptr);
    EXPECT_EQ(pNext, pBase + 48);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 64);
}

TEST_F(GroupAllocatorImplTest, NonMultipleFreeSegmentIsPreserved) {
    std::byte* const pBase = setHeapBaseResidue(192, 0);
    ASSERT_NO_FATAL_FAILURE(seedFreeSegment(std::type_identity<std::byte[48]>{}, 0));
    storage.setCurrentOffset(groupId, 48);

    std::byte* pResult = allocator.galloc(allocatorGroupId, std::type_identity<std::byte[32]>{});

    ASSERT_NE(pResult, nullptr);
    EXPECT_EQ(pResult, pBase + 64);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 96);
    EX_REF<alloc::group::MemorySegList> original = storage.getSameGroupFreeLists(groupId).find(48);
    ASSERT_TRUE(original.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(original.value(), {pBase}));
    EX_REF<alloc::group::MemorySegList> padding = storage.getSameGroupFreeLists(groupId).find(16);
    ASSERT_TRUE(padding.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(padding.value(), {pBase + 48}));
}

TEST_F(GroupAllocatorImplTest, FrontPaddingIsFullyRegisteredAndReusable) {
    // 960 is divisible by 64, 80 and 96, so the residue fixes both padding choices.
    std::byte* const pBase = setHeapBaseResidue(960, 64);
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(pBase) % 96, 64);
    for (std::size_t offset = 0; offset < 48; offset += 16) {
        std::byte* const pPrefix =
            allocator.galloc(groupId, std::type_identity<std::byte[16]>{});
        ASSERT_EQ(pPrefix, pBase + offset);
        std::fill_n(pPrefix, 16, std::byte{0x3c});
    }

    allocator.greserve(groupId, 81, 2);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 320);

    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    EXPECT_EQ(freeLists.getListCount(), 3);
    EX_REF<alloc::group::MemorySegList> sixteen = freeLists.find(16);
    EX_REF<alloc::group::MemorySegList> sixtyFour = freeLists.find(64);
    EX_REF<alloc::group::MemorySegList> ninetySix = freeLists.find(96);
    ASSERT_TRUE(sixteen.hasValue());
    ASSERT_TRUE(sixtyFour.hasValue());
    ASSERT_TRUE(ninetySix.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(sixteen.value(), {pBase + 48}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(sixtyFour.value(), {pBase + 64}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(ninetySix.value(), {pBase + 128, pBase + 224}));
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(freeLists));

    EXPECT_EQ(allocator.galloc(groupId, std::type_identity<std::byte[16]>{}), pBase + 48);
    EXPECT_EQ(allocator.galloc(groupId, std::type_identity<std::byte[64]>{}), pBase + 64);
    EXPECT_EQ(allocator.galloc(groupId, std::type_identity<std::byte[81]>{}), pBase + 128);
    EXPECT_EQ(allocator.galloc(groupId, std::type_identity<std::byte[81]>{}), pBase + 224);
    EXPECT_EQ(storage.getCurrentOffset(groupId), 320);
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(storage.getSameGroupUsedLists(groupId)));
    EXPECT_TRUE(std::all_of(pBase, pBase + 48,
                            [](std::byte value) { return value == std::byte{0x3c}; }));
}

TEST_F(GroupAllocatorImplTest, GcleanRepartitionsFreeSegmentsAndPreservesAlignment) {
    std::byte* const pBase = setHeapBaseResidue(192, 64);
    using Payload = std::byte[48];
    std::byte* const pLive = allocator.galloc(groupId, std::type_identity<Payload>{});
    ASSERT_EQ(pLive, pBase + 32);
    std::fill_n(pLive, sizeof(Payload), std::byte{0x5a});

    allocator.greserve(groupId, 16, 1);
    std::byte* const pReleased =
        allocator.galloc(groupId, std::type_identity<std::byte[16]>{});
    ASSERT_EQ(pReleased, pBase + 80);
    allocator.greserve(groupId, 32, 1);
    allocator.greserve(groupId, 16, 1);
    allocator.gfree(groupId, 16, pReleased);
    ASSERT_EQ(storage.getCurrentOffset(groupId), 144);

    allocator.gclean(groupId);

    alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
    alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(freeLists));
    ASSERT_NO_FATAL_FAILURE(expectAlignedSegments(usedLists));

    // The live 48B segment separates the first padding from the 64B free run.
    ASSERT_EQ(freeLists.getListCount(), 3);
    EX_REF<alloc::group::MemorySegList> padding = freeLists.find(32);
    EX_REF<alloc::group::MemorySegList> merged = freeLists.find(48);
    EX_REF<alloc::group::MemorySegList> remainder = freeLists.find(16);
    EX_REF<alloc::group::MemorySegList> live = usedLists.find(48);
    ASSERT_TRUE(padding.hasValue());
    ASSERT_TRUE(merged.hasValue());
    ASSERT_TRUE(remainder.hasValue());
    ASSERT_TRUE(live.hasValue());
    ASSERT_NO_FATAL_FAILURE(expectSegments(padding.value(), {pBase}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(merged.value(), {pBase + 80}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(remainder.value(), {pBase + 128}));
    ASSERT_NO_FATAL_FAILURE(expectSegments(live.value(), {pLive}));

    std::byte* const pReused = allocator.galloc(groupId, std::type_identity<Payload>{});
    ASSERT_EQ(pReused, pBase + 80);
    std::fill_n(pReused, sizeof(Payload), std::byte{0x33});
    EXPECT_TRUE(std::all_of(pLive, pLive + sizeof(Payload),
                            [](std::byte value) { return value == std::byte{0x5a}; }));
    EXPECT_EQ(storage.getCurrentOffset(groupId), 144);
}

TEST_F(GroupAllocatorImplTest, GallocRejectsExhaustedReservation) {
    storage.setCurrentOffset(groupId, heap.size());

    ASSERT_DEATH(
        {
            std::byte* pResult =
                allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
            (void)pResult;
        },
        "Group heap reservation exhausted");
}

TEST_F(GroupAllocatorImplTest, RepeatedAllocationsPreserveEarlierPayloads) {
    std::array<std::byte*, 128> addresses{};
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        std::byte* pResult =
            allocator.galloc(allocatorGroupId, std::type_identity<std::byte[16]>{});
        ASSERT_NE(pResult, nullptr);
        addresses[index] = pResult;
        ASSERT_EQ(addresses[index], heap.data() + index * 16);
        std::fill_n(addresses[index], 16, static_cast<std::byte>(index));
    }

    for (std::size_t index = 0; index < addresses.size(); ++index) {
        for (std::size_t byte = 0; byte < 16; ++byte) {
            EXPECT_EQ(addresses[index][byte], static_cast<std::byte>(index));
        }
    }
    EXPECT_EQ(storage.getCurrentOffset(groupId), addresses.size() * 16);
}

} // namespace
} // namespace naru::tests::allocator
