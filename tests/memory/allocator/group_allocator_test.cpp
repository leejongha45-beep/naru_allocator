#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "allocator_test_support.hpp"
#include "group/memory_config.hpp"
#include "group/sorted_free_segment_vector.hpp"
#include "memory_region_test_support.hpp"
#include "naru/allocator/group/group_allocator.hpp"
#include "platform/memory.hpp"

namespace naru::tests::allocator {
namespace {
using GroupId = std::uint8_t;

template <std::size_t Size, std::size_t Alignment = 16>
struct alignas(Alignment) Payload {
    std::byte bytes[Size];
};

struct alignas(16) CountedObject {
    inline static std::size_t constructionCount = 0;
    inline static std::size_t destructionCount = 0;
    std::uint64_t value = 0x123456789abcdef0ULL;

    CountedObject() {
        ++constructionCount;
    }

    ~CountedObject() {
        ++destructionCount;
    }
};

struct alignas(16) ReentrantObject {
    inline static GroupId groupId = 0;
    inline static std::atomic<std::size_t> constructionCount{0};
    inline static std::atomic<std::size_t> destructionCount{0};
    Payload<16>* pChild = nullptr;

    ReentrantObject() {
        pChild = new (static_cast<void*>(alloc::group::GroupAllocator::getInstance().galloc(
            groupId, std::type_identity<Payload<16>>{}))) Payload<16>;
        std::fill_n(pChild->bytes, 16, std::byte{0x5a});
        constructionCount.fetch_add(1, std::memory_order_relaxed);
    }

    ~ReentrantObject() {
        std::destroy_at(pChild);
        alloc::group::GroupAllocator::getInstance().gfree(groupId, sizeof(*pChild),
                                                          reinterpret_cast<std::byte*>(pChild));
        destructionCount.fetch_add(1, std::memory_order_relaxed);
    }
};

std::uintptr_t addressOf(const void* pAddress) {
    return reinterpret_cast<std::uintptr_t>(pAddress);
}

std::uintptr_t roundUpToMultiple(std::uintptr_t value, std::size_t multiple) {
    return value + (multiple - value % multiple) % multiple;
}

class GroupAllocatorTest : public ::testing::Test
{
protected:
    void SetUp() override {
        ASSERT_LT(nextGroupId, 254);
        groupId = static_cast<GroupId>(nextGroupId);
        ++nextGroupId;
        CountedObject::constructionCount = 0;
        CountedObject::destructionCount = 0;
    }

    void expectReusedUnits(std::vector<std::uintptr_t> expectedAddresses) {
        std::vector<std::uintptr_t> actualAddresses;
        for (std::size_t index = 0; index < expectedAddresses.size(); ++index) {
            Payload<16>* pObject = new (static_cast<void*>(
                allocator.galloc(groupId, std::type_identity<Payload<16>>{}))) Payload<16>;
            ASSERT_NE(pObject, nullptr);
            EXPECT_EQ(addressOf(pObject) % alignof(Payload<16>), 0);
            actualAddresses.push_back(addressOf(pObject));
        }
        std::ranges::sort(actualAddresses);
        std::ranges::sort(expectedAddresses);
        EXPECT_EQ(actualAddresses, expectedAddresses);
    }

    inline static std::uint16_t nextGroupId = 1;
    GroupId groupId = 0;
    alloc::group::GroupAllocator& allocator = alloc::group::GroupAllocator::getInstance();
};

TEST_F(GroupAllocatorTest, GallocReturnsStorageWithoutConstructingTheObject) {
    std::byte* pMemory = allocator.galloc(groupId, std::type_identity<CountedObject>{});

    ASSERT_NE(pMemory, nullptr);
    EXPECT_EQ(CountedObject::constructionCount, 0);
    EXPECT_EQ(addressOf(pMemory) % alignof(CountedObject), 0);
    EXPECT_EQ(queryCommittedHeap(pMemory).state, MemoryRegionState::COMMITTED);

    allocator.gfree(groupId, sizeof(CountedObject), pMemory);
    EXPECT_EQ(CountedObject::destructionCount, 0);
}

TEST_F(GroupAllocatorTest, MixedSizesAreAlignedAndDoNotOverlap) {
    Payload<17, 1>* pSmallObject =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<17, 1>>{})))
            Payload<17, 1>;
    Payload<48>* pMedium =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<48>>{})))
            Payload<48>;
    Payload<64, 64>* pAligned =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<64, 64>>{})))
            Payload<64, 64>;
    ASSERT_NE(pSmallObject, nullptr);
    ASSERT_NE(pMedium, nullptr);
    ASSERT_NE(pAligned, nullptr);

    EXPECT_EQ(addressOf(pSmallObject) % 32, 0);
    EXPECT_EQ(addressOf(pMedium) % sizeof(*pMedium), 0);
    EXPECT_EQ(addressOf(pAligned) % sizeof(*pAligned), 0);
    EXPECT_LE(addressOf(pSmallObject) + 32, addressOf(pMedium));
    EXPECT_LE(addressOf(pMedium) + sizeof(*pMedium), addressOf(pAligned));
    std::fill_n(pSmallObject->bytes, sizeof(pSmallObject->bytes), std::byte{0x11});
    std::fill_n(pMedium->bytes, sizeof(pMedium->bytes), std::byte{0x22});
    std::fill_n(pAligned->bytes, sizeof(pAligned->bytes), std::byte{0x33});
    for (std::byte value : pSmallObject->bytes) {
        EXPECT_EQ(value, std::byte{0x11});
    }
    for (std::byte value : pMedium->bytes) {
        EXPECT_EQ(value, std::byte{0x22});
    }
}

TEST_F(GroupAllocatorTest, ManyAllocationsPreserveEarlierObjects) {
    std::array<Payload<16>*, 128> objects{};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objects[index] =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(objects[index], nullptr);
        if (index != 0) {
            EXPECT_EQ(addressOf(objects[index]), addressOf(objects[0]) + index * 16);
        }
        std::fill_n(objects[index]->bytes, 16, static_cast<std::byte>(index));
    }
    for (std::size_t index = 0; index < objects.size(); ++index) {
        for (std::byte value : objects[index]->bytes) {
            EXPECT_EQ(value, static_cast<std::byte>(index));
        }
    }
}

TEST_F(GroupAllocatorTest, GreserveProvidesSegmentsWithoutConstructingObjects) {
    allocator.greserve(groupId, sizeof(CountedObject), 5);
    EXPECT_EQ(CountedObject::constructionCount, 0);
    Payload<32>* pAfterReserved =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<32>>{})))
            Payload<32>;
    ASSERT_NE(pAfterReserved, nullptr);

    CountedObject* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<CountedObject>{})))
            CountedObject;
    ASSERT_NE(pFirst, nullptr);
    EXPECT_EQ(static_cast<void*>(pFirst), queryCommittedHeap(pFirst).pBaseAddr);
    EXPECT_GE(addressOf(pAfterReserved), addressOf(pFirst) + 5 * 16);
    for (std::size_t index = 1; index < 5; ++index) {
        CountedObject* pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<CountedObject>{})))
                CountedObject;
        ASSERT_NE(pObject, nullptr);
        EXPECT_EQ(addressOf(pObject), addressOf(pFirst) + index * 16);
        EXPECT_EQ(pObject->value, pFirst->value);
    }
    EXPECT_EQ(CountedObject::constructionCount, 5);
}

TEST_F(GroupAllocatorTest, RepeatedGreserveAppendsTheRequestedCount) {
    allocator.greserve(groupId, sizeof(Payload<16>), 2);
    allocator.greserve(groupId, sizeof(Payload<16>), 3);
    Payload<48>* pMarker =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<48>>{})))
            Payload<48>;
    ASSERT_NE(pMarker, nullptr);

    Payload<16>* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pFirst, nullptr);
    EXPECT_EQ(static_cast<void*>(pFirst), queryCommittedHeap(pFirst).pBaseAddr);
    const std::uintptr_t reservedEnd = addressOf(pFirst) + 5 * 16;
    const std::uintptr_t markerAddress = roundUpToMultiple(reservedEnd, sizeof(*pMarker));
    EXPECT_EQ(addressOf(pMarker), markerAddress);
    for (std::size_t index = 1; index < 5; ++index) {
        Payload<16>* pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObject, nullptr);
        EXPECT_EQ(addressOf(pObject), addressOf(pFirst) + index * 16);
    }
    Payload<16>* pAfterReserved =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    const std::uintptr_t nextAddress =
        markerAddress > reservedEnd ? reservedEnd : markerAddress + sizeof(*pMarker);
    EXPECT_EQ(addressOf(pAfterReserved), nextAddress);
}

TEST_F(GroupAllocatorTest, LargerReservedSegmentCanServeSmallerAllocations) {
    allocator.greserve(groupId, sizeof(Payload<48>), 1);

    Payload<16>* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pFirst, nullptr);
    const MemoryRegion region = queryCommittedHeap(pFirst);
    EXPECT_EQ(static_cast<void*>(pFirst), region.pBaseAddr);
    const std::uintptr_t heapBase = addressOf(region.pBaseAddr);
    const std::uintptr_t reservedEnd = roundUpToMultiple(heapBase, 48) + 48;
    const std::size_t reservedUnitCount = (reservedEnd - heapBase) / 16;
    for (std::size_t index = 1; index <= reservedUnitCount; ++index) {
        Payload<16>* pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObject, nullptr);
        EXPECT_EQ(addressOf(pObject), addressOf(pFirst) + index * 16);
    }
}

TEST_F(GroupAllocatorTest, TaskAndMaximumGroupsUseSeparateReservations) {
    Payload<16>* pTaskGroup =
        new (static_cast<void*>(allocator.galloc(0, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pMaximumGroup = new (static_cast<void*>(allocator.galloc(
        static_cast<GroupId>(255), std::type_identity<Payload<16>>{}))) Payload<16>;
    ASSERT_NE(pTaskGroup, nullptr);
    ASSERT_NE(pMaximumGroup, nullptr);
    const MemoryRegion taskRegion = queryCommittedHeap(pTaskGroup);
    const MemoryRegion maximumRegion = queryCommittedHeap(pMaximumGroup);

    EXPECT_NE(taskRegion.pBaseAddr, maximumRegion.pBaseAddr);
    EXPECT_EQ(taskRegion.state, MemoryRegionState::COMMITTED);
    EXPECT_EQ(maximumRegion.state, MemoryRegionState::COMMITTED);
    pTaskGroup->bytes[0] = std::byte{0x11};
    pMaximumGroup->bytes[0] = std::byte{0x22};
    EXPECT_EQ(pTaskGroup->bytes[0], std::byte{0x11});
    EXPECT_EQ(pMaximumGroup->bytes[0], std::byte{0x22});
}

TEST_F(GroupAllocatorTest, SmallShortageCommitsAnotherDefaultBlock) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    allocator.greserve(groupId, pageSize, config::COMMIT_SIZE / pageSize);
    Payload<16>* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pFirst, nullptr);
    pFirst->bytes[0] = std::byte{0x31};
    pFirst->bytes[15] = std::byte{0x32};
    const MemoryRegion initialRegion = queryCommittedHeap(pFirst);
    ASSERT_EQ(initialRegion.size, config::COMMIT_SIZE);

    // A page-size Free segment cannot be split into 48B pieces.
    Payload<48>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<48>>{})))
            Payload<48>;
    ASSERT_NE(pNext, nullptr);
    pNext->bytes[0] = std::byte{0x33};
    const std::uintptr_t initialEnd = addressOf(initialRegion.pBaseAddr) + initialRegion.size;
    EXPECT_EQ(addressOf(pNext), roundUpToMultiple(initialEnd, sizeof(*pNext)));
    EXPECT_EQ(queryCommittedHeap(pFirst).size, config::COMMIT_SIZE * 2);
    EXPECT_EQ(pFirst->bytes[0], std::byte{0x31});
    EXPECT_EQ(pFirst->bytes[15], std::byte{0x32});
}

TEST_F(GroupAllocatorTest, LargeObjectCommitsTheEntireShortageInOneRetry) {
    constexpr std::size_t size = config::COMMIT_SIZE * 2 + 16;
    Payload<size>* pObject =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<size>>{})))
            Payload<size>;
    ASSERT_NE(pObject, nullptr);
    pObject->bytes[0] = std::byte{0x41};
    pObject->bytes[size - 1] = std::byte{0x42};

    const MemoryRegion region = queryCommittedHeap(pObject);
    EXPECT_EQ(region.state, MemoryRegionState::COMMITTED);
    const std::uintptr_t heapBase = addressOf(region.pBaseAddr);
    const std::uintptr_t expectedAddress = roundUpToMultiple(heapBase, size);
    EXPECT_EQ(addressOf(pObject), expectedAddress);
    EXPECT_EQ(region.size, roundUpToMultiple(expectedAddress - heapBase + size,
                                             alloc::platform::getCommonPageSize()));
    EXPECT_EQ(pObject->bytes[0], std::byte{0x41});
    EXPECT_EQ(pObject->bytes[size - 1], std::byte{0x42});
}

TEST_F(GroupAllocatorTest, LargeBatchCommitsSpaceForEveryReservedSegment) {
    using LargeObject = Payload<config::COMMIT_SIZE>;
    allocator.greserve(groupId, sizeof(LargeObject), 3);

    LargeObject* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<LargeObject>{})))
            LargeObject;
    ASSERT_NE(pFirst, nullptr);
    const MemoryRegion region = queryCommittedHeap(pFirst);
    const std::uintptr_t heapBase = addressOf(region.pBaseAddr);
    const std::uintptr_t expectedAddress = roundUpToMultiple(heapBase, sizeof(LargeObject));
    EXPECT_EQ(addressOf(pFirst), expectedAddress);
    EXPECT_EQ(region.size, expectedAddress - heapBase + 3 * sizeof(LargeObject));
    pFirst->bytes[0] = std::byte{0x51};
    for (std::size_t index = 1; index < 3; ++index) {
        LargeObject* pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<LargeObject>{})))
                LargeObject;
        ASSERT_NE(pObject, nullptr);
        EXPECT_EQ(addressOf(pObject), addressOf(pFirst) + index * sizeof(LargeObject));
        pObject->bytes[sizeof(LargeObject) - 1] = static_cast<std::byte>(index);
        EXPECT_EQ(pObject->bytes[sizeof(LargeObject) - 1], static_cast<std::byte>(index));
    }
    EXPECT_EQ(pFirst->bytes[0], std::byte{0x51});
}

TEST_F(GroupAllocatorTest, BatchCommitIncludesTheFirstSegmentsFrontPadding) {
    using Prefix = Payload<config::COMMIT_SIZE>;
    using BatchObject = Payload<config::COMMIT_SIZE + 16>;
    Prefix* pPrefix =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Prefix>{}))) Prefix;
    ASSERT_NE(pPrefix, nullptr);
    pPrefix->bytes[sizeof(Prefix) - 1] = std::byte{0x61};

    std::uintptr_t cursor = addressOf(pPrefix) + sizeof(Prefix);
    if (cursor % sizeof(BatchObject) == 0) {
        allocator.greserve(groupId, 16, 1);
        cursor += 16;
    }
    const std::uintptr_t expectedAddress = roundUpToMultiple(cursor, sizeof(BatchObject));
    allocator.greserve(groupId, sizeof(BatchObject), 2);
    BatchObject* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<BatchObject>{})))
            BatchObject;
    BatchObject* pSecond =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<BatchObject>{})))
            BatchObject;
    ASSERT_NE(pFirst, nullptr);
    ASSERT_NE(pSecond, nullptr);
    EXPECT_EQ(addressOf(pFirst), expectedAddress);
    EXPECT_EQ(addressOf(pSecond), expectedAddress + sizeof(BatchObject));
    const MemoryRegion region = queryCommittedHeap(pPrefix);
    const std::uintptr_t heapBase = addressOf(region.pBaseAddr);
    const std::size_t expectedEndOffset = expectedAddress - heapBase + 2 * sizeof(BatchObject);
    EXPECT_EQ(region.size, roundUpToMultiple(expectedEndOffset, alloc::platform::getCommonPageSize()));
    pSecond->bytes[sizeof(BatchObject) - 1] = std::byte{0x62};
    EXPECT_EQ(pPrefix->bytes[sizeof(Prefix) - 1], std::byte{0x61});
    EXPECT_EQ(pSecond->bytes[sizeof(BatchObject) - 1], std::byte{0x62});
}

TEST_F(GroupAllocatorTest, CallerDestroysTheObjectBeforeGfreeAndReuse) {
    CountedObject* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<CountedObject>{})))
            CountedObject;
    ASSERT_NE(pFirst, nullptr);
    std::destroy_at(pFirst);
    allocator.gfree(groupId, sizeof(*pFirst), reinterpret_cast<std::byte*>(pFirst));
    EXPECT_EQ(CountedObject::destructionCount, 1);

    CountedObject* pReused =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<CountedObject>{})))
            CountedObject;
    ASSERT_NE(pReused, nullptr);
    EXPECT_EQ(pReused, pFirst);
    EXPECT_EQ(CountedObject::constructionCount, 2);
    std::destroy_at(pReused);
    allocator.gfree(groupId, sizeof(*pReused), reinterpret_cast<std::byte*>(pReused));
    EXPECT_EQ(CountedObject::destructionCount, 2);
}

TEST_F(GroupAllocatorTest, GfreeReusesAddressAndPreservesLiveNeighbors) {
    Payload<16>* pLeft =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pMiddle =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pRight =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pLeft, nullptr);
    ASSERT_NE(pMiddle, nullptr);
    ASSERT_NE(pRight, nullptr);
    std::fill_n(pLeft->bytes, 16, std::byte{0x11});
    std::fill_n(pRight->bytes, 16, std::byte{0x33});

    std::destroy_at(pMiddle);
    allocator.gfree(groupId, sizeof(*pMiddle), reinterpret_cast<std::byte*>(pMiddle));
    Payload<16>* pReused =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_EQ(pReused, pMiddle);
    std::fill_n(pReused->bytes, 16, std::byte{0x22});

    for (std::byte value : pLeft->bytes) {
        EXPECT_EQ(value, std::byte{0x11});
    }
    for (std::byte value : pRight->bytes) {
        EXPECT_EQ(value, std::byte{0x33});
    }
}

TEST_F(GroupAllocatorTest, GcleanMergesAcrossDifferentSizeLists) {
    Payload<64>* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<64>>{})))
            Payload<64>;
    Payload<32>* pSecond =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<32>>{})))
            Payload<32>;
    Payload<16>* pThird =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pFourth =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_EQ(addressOf(pSecond), addressOf(pFirst) + 64);
    ASSERT_EQ(addressOf(pThird), addressOf(pFirst) + 96);
    ASSERT_EQ(addressOf(pFourth), addressOf(pFirst) + 112);
    const std::uintptr_t firstAddress = addressOf(pFirst);

    std::destroy_at(pFourth);
    allocator.gfree(groupId, sizeof(*pFourth), reinterpret_cast<std::byte*>(pFourth));
    std::destroy_at(pFirst);
    allocator.gfree(groupId, sizeof(*pFirst), reinterpret_cast<std::byte*>(pFirst));
    std::destroy_at(pThird);
    allocator.gfree(groupId, sizeof(*pThird), reinterpret_cast<std::byte*>(pThird));
    std::destroy_at(pSecond);
    allocator.gfree(groupId, sizeof(*pSecond), reinterpret_cast<std::byte*>(pSecond));
    allocator.gclean(groupId);

    Payload<128>* pMerged =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<128>>{})))
            Payload<128>;
    EXPECT_EQ(addressOf(pMerged), firstAddress);
    EXPECT_EQ(addressOf(pMerged) % sizeof(*pMerged), 0);
    Payload<16>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pNext), firstAddress + 128);
}

TEST_F(GroupAllocatorTest, GcleanKeepsFreeSegmentsAcrossLiveGapsSeparate) {
    std::array<Payload<16>*, 4> pObjects{};
    for (std::size_t index = 0; index < pObjects.size(); ++index) {
        pObjects[index] =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObjects[index], nullptr);
        std::fill_n(pObjects[index]->bytes, 16, static_cast<std::byte>(index + 1));
    }
    const std::uintptr_t firstAddress = addressOf(pObjects[0]);
    const std::uintptr_t thirdAddress = addressOf(pObjects[2]);
    std::destroy_at(pObjects[2]);
    allocator.gfree(groupId, sizeof(*pObjects[2]), reinterpret_cast<std::byte*>(pObjects[2]));
    std::destroy_at(pObjects[0]);
    allocator.gfree(groupId, sizeof(*pObjects[0]), reinterpret_cast<std::byte*>(pObjects[0]));
    allocator.gclean(groupId);

    Payload<32>* pLarger =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<32>>{})))
            Payload<32>;
    EXPECT_EQ(addressOf(pLarger), firstAddress + 64);
    std::fill_n(pLarger->bytes, 32, std::byte{0x55});
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits({firstAddress, thirdAddress}));
    for (std::byte value : pObjects[1]->bytes) {
        EXPECT_EQ(value, std::byte{0x02});
    }
    for (std::byte value : pObjects[3]->bytes) {
        EXPECT_EQ(value, std::byte{0x04});
    }
}

TEST_F(GroupAllocatorTest, GcleanProcessesTheFinalContiguousRun) {
    std::array<Payload<16>*, 8> pObjects{};
    for (std::size_t index = 0; index < pObjects.size(); ++index) {
        pObjects[index] =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObjects[index], nullptr);
        std::fill_n(pObjects[index]->bytes, 16, static_cast<std::byte>(index + 1));
    }
    const std::uintptr_t firstAddress = addressOf(pObjects[0]);
    const std::array<std::size_t, 5> freeOrder{7, 0, 5, 4, 6};
    for (std::size_t index : freeOrder) {
        std::destroy_at(pObjects[index]);
        allocator.gfree(groupId, sizeof(*pObjects[index]),
                        reinterpret_cast<std::byte*>(pObjects[index]));
    }
    allocator.gclean(groupId);

    Payload<64>* pLastRun =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<64>>{})))
            Payload<64>;
    EXPECT_EQ(addressOf(pLastRun), firstAddress + 64);
    std::fill_n(pLastRun->bytes, 64, std::byte{0x66});
    Payload<16>* pFirstRun =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pFirstRun), firstAddress);
    for (std::size_t index = 1; index < 4; ++index) {
        for (std::byte value : pObjects[index]->bytes) {
            EXPECT_EQ(value, static_cast<std::byte>(index + 1));
        }
    }
}

struct CleanupRunCase {
    std::size_t prefixCount;
    std::size_t freeCount;
};

class GroupAllocatorCleanupRunTest : public GroupAllocatorTest,
                                     public ::testing::WithParamInterface<CleanupRunCase>
{
};

TEST_P(GroupAllocatorCleanupRunTest, ReusesEveryFreedUnitExactlyOnce) {
    const CleanupRunCase input = GetParam();
    std::vector<Payload<16>*> pPrefixes(input.prefixCount);
    for (Payload<16>*& pPrefix : pPrefixes) {
        pPrefix =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pPrefix, nullptr);
        std::fill_n(pPrefix->bytes, 16, std::byte{0x5a});
    }

    std::vector<Payload<16>*> pObjects(input.freeCount);
    std::vector<std::uintptr_t> expectedAddresses;
    for (Payload<16>*& pObject : pObjects) {
        pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObject, nullptr);
        expectedAddresses.push_back(addressOf(pObject));
    }
    const std::uintptr_t nextAddress = expectedAddresses.back() + 16;
    for (std::size_t index = pObjects.size(); index != 0; --index) {
        std::destroy_at(pObjects[index - 1]);
        allocator.gfree(groupId, sizeof(*pObjects[index - 1]),
                        reinterpret_cast<std::byte*>(pObjects[index - 1]));
    }

    allocator.gclean(groupId);
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(expectedAddresses));
    Payload<16>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pNext), nextAddress);
    for (const Payload<16>* pPrefix : pPrefixes) {
        for (std::byte value : pPrefix->bytes) {
            EXPECT_EQ(value, std::byte{0x5a});
        }
    }
}

INSTANTIATE_TEST_SUITE_P(FreeRangeBoundaries, GroupAllocatorCleanupRunTest,
                         ::testing::Values(CleanupRunCase{0, 1}, CleanupRunCase{1, 3},
                                           CleanupRunCase{1, 7}, CleanupRunCase{3, 11},
                                           CleanupRunCase{5, 17}, CleanupRunCase{7, 31}));

TEST_F(GroupAllocatorTest, GcleanRepeatedWithoutChangesKeepsCoverage) {
    std::array<Payload<16>*, 8> pObjects{};
    for (Payload<16>*& pObject : pObjects) {
        pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObject, nullptr);
    }
    const std::uintptr_t firstAddress = addressOf(pObjects[0]);
    const std::array<std::size_t, 8> freeOrder{3, 7, 0, 5, 1, 6, 2, 4};
    for (std::size_t index : freeOrder) {
        std::destroy_at(pObjects[index]);
        allocator.gfree(groupId, sizeof(*pObjects[index]),
                        reinterpret_cast<std::byte*>(pObjects[index]));
    }
    for (std::size_t count = 0; count < 3; ++count) {
        allocator.gclean(groupId);
    }

    Payload<128>* pMerged =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<128>>{})))
            Payload<128>;
    EXPECT_EQ(addressOf(pMerged), firstAddress);
    Payload<16>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pNext), firstAddress + 128);
}

TEST_F(GroupAllocatorTest, GcleanGrowsSortedStorageAndPreservesAllSegments) {
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::size_t initialBytes =
        (config::METADATA_SORTED_FREE_SEG_COMMIT_SIZE + pageSize - 1) / pageSize * pageSize;
    const std::size_t segmentCount =
        initialBytes / sizeof(alloc::group::SortedFreeSegVector::Segment) + 1;
    std::vector<Payload<16>*> pObjects(segmentCount);
    std::vector<std::uintptr_t> expectedAddresses;
    for (Payload<16>*& pObject : pObjects) {
        pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObject, nullptr);
        expectedAddresses.push_back(addressOf(pObject));
    }
    Payload<16>* pGuard =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pGuard, nullptr);
    std::fill_n(pGuard->bytes, 16, std::byte{0x7c});
    for (std::size_t index = pObjects.size(); index != 0; --index) {
        std::destroy_at(pObjects[index - 1]);
        allocator.gfree(groupId, sizeof(*pObjects[index - 1]),
                        reinterpret_cast<std::byte*>(pObjects[index - 1]));
    }

    allocator.gclean(groupId);
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(expectedAddresses));
    Payload<16>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pNext), addressOf(pGuard) + 16);
    for (std::byte value : pGuard->bytes) {
        EXPECT_EQ(value, std::byte{0x7c});
    }
}

TEST_F(GroupAllocatorTest, GcleanOnlyChangesTheRequestedGroup) {
    ASSERT_LT(nextGroupId, 254);
    const GroupId otherGroupId = static_cast<GroupId>(nextGroupId);
    ++nextGroupId;
    Payload<16>* pFirst =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pSecond =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pOtherFree =
        new (static_cast<void*>(allocator.galloc(otherGroupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    Payload<16>* pOtherLive =
        new (static_cast<void*>(allocator.galloc(otherGroupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pOtherLive, nullptr);
    std::fill_n(pOtherLive->bytes, 16, std::byte{0x6d});
    const std::uintptr_t firstAddress = addressOf(pFirst);
    const std::uintptr_t otherAddress = addressOf(pOtherFree);
    std::destroy_at(pSecond);
    allocator.gfree(groupId, sizeof(*pSecond), reinterpret_cast<std::byte*>(pSecond));
    std::destroy_at(pFirst);
    allocator.gfree(groupId, sizeof(*pFirst), reinterpret_cast<std::byte*>(pFirst));
    std::destroy_at(pOtherFree);
    allocator.gfree(otherGroupId, sizeof(*pOtherFree), reinterpret_cast<std::byte*>(pOtherFree));

    allocator.gclean(groupId);
    Payload<32>* pMerged =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<32>>{})))
            Payload<32>;
    EXPECT_EQ(addressOf(pMerged), firstAddress);
    Payload<16>* pOtherReused =
        new (static_cast<void*>(allocator.galloc(otherGroupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pOtherReused), otherAddress);
    for (std::byte value : pOtherLive->bytes) {
        EXPECT_EQ(value, std::byte{0x6d});
    }
}

TEST_F(GroupAllocatorTest, ShuffledFreeCleanAllocateCyclesPreserveState) {
    std::array<Payload<16>*, 64> pObjects{};
    std::array<std::size_t, 64> freeOrder{};
    std::iota(freeOrder.begin(), freeOrder.end(), std::size_t{0});
    std::mt19937 randomEngine(0x4d4f5255U);
    for (std::size_t index = 0; index < pObjects.size(); ++index) {
        pObjects[index] =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
                Payload<16>;
        ASSERT_NE(pObjects[index], nullptr);
        std::fill_n(pObjects[index]->bytes, 16, static_cast<std::byte>(index + 1));
    }

    for (std::size_t cycle = 0; cycle < 16; ++cycle) {
        SCOPED_TRACE(cycle);
        std::shuffle(freeOrder.begin(), freeOrder.end(), randomEngine);
        std::vector<std::uintptr_t> freedAddresses;
        for (std::size_t count = 0; count < pObjects.size() / 2; ++count) {
            const std::size_t index = freeOrder[count];
            freedAddresses.push_back(addressOf(pObjects[index]));
            std::destroy_at(pObjects[index]);
            allocator.gfree(groupId, sizeof(*pObjects[index]),
                            reinterpret_cast<std::byte*>(pObjects[index]));
            pObjects[index] = nullptr;
        }

        allocator.gclean(groupId);
        std::vector<std::uintptr_t> reusedAddresses;
        for (std::size_t count = 0; count < pObjects.size() / 2; ++count) {
            const std::size_t index = freeOrder[count];
            pObjects[index] = new (static_cast<void*>(
                allocator.galloc(groupId, std::type_identity<Payload<16>>{}))) Payload<16>;
            ASSERT_NE(pObjects[index], nullptr);
            reusedAddresses.push_back(addressOf(pObjects[index]));
            std::fill_n(pObjects[index]->bytes, 16, static_cast<std::byte>(index + 1));
        }
        std::ranges::sort(freedAddresses);
        std::ranges::sort(reusedAddresses);
        EXPECT_EQ(reusedAddresses, freedAddresses);
        for (std::size_t index = 0; index < pObjects.size(); ++index) {
            for (std::byte value : pObjects[index]->bytes) {
                EXPECT_EQ(value, static_cast<std::byte>(index + 1));
            }
        }
    }
}

TEST_F(GroupAllocatorTest, ConcurrentFirstUseAndCrossThreadFreesPreserveEverySegment) {
    constexpr std::size_t workerCount = 8;
    constexpr std::size_t objectsPerWorker = 384;
    std::array<std::array<Payload<16>*, objectsPerWorker>, workerCount> pObjects{};
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            for (Payload<16>*& pObject : pObjects[workerIndex]) {
                pObject = new (static_cast<void*>(
                    allocator.galloc(groupId, std::type_identity<Payload<16>>{}))) Payload<16>;
                std::fill_n(pObject->bytes, 16, static_cast<std::byte>(workerIndex + 1));
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    const std::uintptr_t heapBase = addressOf(queryCommittedHeap(pObjects[0][0]).pBaseAddr);
    std::vector<std::uintptr_t> addresses;
    addresses.reserve(workerCount * objectsPerWorker);
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        SCOPED_TRACE(workerIndex);
        const std::byte expected = static_cast<std::byte>(workerIndex + 1);
        for (Payload<16>* pObject : pObjects[workerIndex]) {
            const std::uintptr_t address = addressOf(pObject);
            EXPECT_EQ(address % alignof(Payload<16>), 0);
            EXPECT_GE(address, heapBase);
            EXPECT_LT(address, heapBase + config::GROUP_RESERVE_SIZE);
            EXPECT_TRUE(std::ranges::all_of(
                pObject->bytes, [expected](std::byte value) { return value == expected; }));
            addresses.push_back(address);
        }
    }
    std::ranges::sort(addresses);
    for (std::size_t index = 1; index < addresses.size(); ++index) {
        EXPECT_GE(addresses[index], addresses[index - 1] + sizeof(Payload<16>));
    }

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            const std::size_t ownerIndex = (workerIndex + 1) % workerCount;
            for (Payload<16>* pObject : pObjects[ownerIndex]) {
                std::destroy_at(pObject);
                allocator.gfree(groupId, sizeof(*pObject), reinterpret_cast<std::byte*>(pObject));
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(addresses));
}

TEST_F(GroupAllocatorTest, ConcurrentGreservePreservesTheTotalReservedCount) {
    constexpr std::size_t workerCount = 8;
    constexpr std::size_t segmentsPerWorker = 256;
    constexpr std::size_t totalSegments = workerCount * segmentsPerWorker;
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));

    for (std::jthread& worker : workers) {
        worker = std::jthread([&] {
            start.arrive_and_wait();
            allocator.greserve(groupId, sizeof(Payload<16>), segmentsPerWorker);
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    Payload<32>* pMarker =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<32>>{})))
            Payload<32>;
    const std::uintptr_t heapBase = addressOf(queryCommittedHeap(pMarker).pBaseAddr);
    EXPECT_EQ(addressOf(pMarker), heapBase + totalSegments * 16);
    std::fill_n(pMarker->bytes, 32, std::byte{0x6b});

    std::vector<std::uintptr_t> expectedAddresses;
    expectedAddresses.reserve(totalSegments);
    for (std::size_t index = 0; index < totalSegments; ++index) {
        expectedAddresses.push_back(heapBase + index * 16);
    }
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(expectedAddresses));
    EXPECT_TRUE(std::ranges::all_of(pMarker->bytes,
                                    [](std::byte value) { return value == std::byte{0x6b}; }));
    std::destroy_at(pMarker);
    allocator.gfree(groupId, sizeof(*pMarker), reinterpret_cast<std::byte*>(pMarker));
}

TEST_F(GroupAllocatorTest,
       ConcurrentGcleanGreserveAndAllocationPreserveLiveObjectsAndFreeCoverage) {
    constexpr std::size_t allocationWorkerCount = 4;
    constexpr std::size_t reserveWorkerCount = 2;
    constexpr std::size_t cleanWorkerCount = 2;
    constexpr std::size_t workerCount =
        allocationWorkerCount + reserveWorkerCount + cleanWorkerCount;
    constexpr std::size_t batchSize = 32;
    constexpr std::size_t reserveBatchSize = 8;
    constexpr std::size_t roundCount = 16;
    constexpr std::size_t initialFreeCount = 256;
    constexpr std::size_t totalFreeCount =
        initialFreeCount + reserveWorkerCount * roundCount * 2 * reserveBatchSize;

    Payload<64>* pSentinel =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<64>>{})))
            Payload<64>;
    std::fill_n(pSentinel->bytes, 64, std::byte{0x7c});
    allocator.greserve(groupId, sizeof(Payload<16>), initialFreeCount);

    std::array<std::array<Payload<16>*, batchSize>, allocationWorkerCount> pObjects{};
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> phase(static_cast<std::ptrdiff_t>(workerCount));
    bool uniqueRanges = true;
    bool preservedPayloads = true;

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            for (std::size_t round = 0; round < roundCount; ++round) {
                phase.arrive_and_wait();
                if (workerIndex < allocationWorkerCount) {
                    for (Payload<16>*& pObject : pObjects[workerIndex]) {
                        pObject = new (static_cast<void*>(allocator.galloc(
                            groupId, std::type_identity<Payload<16>>{}))) Payload<16>;
                        std::fill_n(pObject->bytes, 16,
                                    static_cast<std::byte>(workerIndex + round + 1));
                    }
                } else if (workerIndex < allocationWorkerCount + reserveWorkerCount) {
                    allocator.greserve(groupId, sizeof(Payload<16>), reserveBatchSize);
                } else {
                    allocator.gclean(groupId);
                }
                phase.arrive_and_wait();

                if (workerIndex == 0) {
                    std::array<std::uintptr_t, allocationWorkerCount * batchSize> addresses{};
                    std::size_t addressIndex = 0;
                    for (std::size_t ownerIndex = 0; ownerIndex < allocationWorkerCount;
                         ++ownerIndex) {
                        const std::byte expected = static_cast<std::byte>(ownerIndex + round + 1);
                        for (Payload<16>* pObject : pObjects[ownerIndex]) {
                            addresses[addressIndex++] = addressOf(pObject);
                            for (std::byte value : pObject->bytes) {
                                if (value != expected) {
                                    preservedPayloads = false;
                                }
                            }
                        }
                    }
                    std::ranges::sort(addresses);
                    for (std::size_t index = 1; index < addresses.size(); ++index) {
                        if (addresses[index] < addresses[index - 1] + 16) {
                            uniqueRanges = false;
                        }
                    }
                }
                phase.arrive_and_wait();

                if (workerIndex < allocationWorkerCount) {
                    const std::size_t ownerIndex = (workerIndex + 1) % allocationWorkerCount;
                    for (Payload<16>* pObject : pObjects[ownerIndex]) {
                        std::destroy_at(pObject);
                        allocator.gfree(groupId, sizeof(*pObject),
                                        reinterpret_cast<std::byte*>(pObject));
                    }
                } else if (workerIndex < allocationWorkerCount + reserveWorkerCount) {
                    allocator.greserve(groupId, sizeof(Payload<16>), reserveBatchSize);
                } else {
                    allocator.gclean(groupId);
                }
                phase.arrive_and_wait();
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    EXPECT_TRUE(uniqueRanges);
    EXPECT_TRUE(preservedPayloads);
    EXPECT_TRUE(std::ranges::all_of(pSentinel->bytes,
                                    [](std::byte value) { return value == std::byte{0x7c}; }));

    allocator.gclean(groupId);
    std::vector<std::uintptr_t> expectedAddresses;
    expectedAddresses.reserve(totalFreeCount);
    const std::uintptr_t freeBase = addressOf(pSentinel) + sizeof(*pSentinel);
    for (std::size_t index = 0; index < totalFreeCount; ++index) {
        expectedAddresses.push_back(freeBase + index * 16);
    }
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(expectedAddresses));
    std::destroy_at(pSentinel);
    allocator.gfree(groupId, sizeof(*pSentinel), reinterpret_cast<std::byte*>(pSentinel));
}

TEST_F(GroupAllocatorTest, ConcurrentGallocGfreeGreserveAndGcleanWithoutPhases) {
    struct alignas(16) QueuedPayload {
        std::uint64_t sequence = 0;
        std::uint64_t ownerIndex = 0;
    };

    constexpr std::size_t pairCount = 2;
    constexpr std::size_t reserveWorkerCount = 2;
    constexpr std::size_t cleanWorkerCount = 2;
    constexpr std::size_t workerCount = pairCount * 2 + reserveWorkerCount + cleanWorkerCount;
    constexpr std::size_t slotCount = 32;
    constexpr std::size_t prefilledCount = slotCount / 2;
    constexpr std::size_t allocationsPerWorker = 1024;
    constexpr std::size_t maintenanceIterations = 128;
    constexpr std::size_t reserveBatchSize = 8;
    constexpr std::size_t initialFreeCount = 256;
    constexpr std::size_t totalFreeCount =
        initialFreeCount + reserveWorkerCount * maintenanceIterations * reserveBatchSize;
    static_assert(sizeof(QueuedPayload) == config::SEGMENT_SIZE_UNIT);

    Payload<64>* pSentinel =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<64>>{})))
            Payload<64>;
    std::fill_n(pSentinel->bytes, 64, std::byte{0x7c});
    allocator.greserve(groupId, sizeof(QueuedPayload), initialFreeCount);
    const std::uintptr_t freeBase = addressOf(pSentinel) + sizeof(*pSentinel);

    std::array<std::array<std::atomic<QueuedPayload*>, slotCount>, pairCount> pSlots{};
    for (std::size_t pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        for (std::size_t sequence = 0; sequence < prefilledCount; ++sequence) {
            QueuedPayload* pObject = new (static_cast<void*>(
                allocator.galloc(groupId, std::type_identity<QueuedPayload>{}))) QueuedPayload;
            pObject->sequence = sequence;
            pObject->ownerIndex = pairIndex;
            pSlots[pairIndex][sequence].store(pObject, std::memory_order_relaxed);
        }
    }

    std::array<std::jthread, workerCount> workers{};
    std::array<std::size_t, workerCount> completedOperations{};
    std::array<bool, pairCount> preservedPayloads{true, true};
    std::array<bool, pairCount> validAddresses{true, true};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            if (workerIndex < pairCount) {
                for (std::size_t sequence = prefilledCount;
                     sequence < prefilledCount + allocationsPerWorker; ++sequence) {
                    std::atomic<QueuedPayload*>& pSlot = pSlots[workerIndex][sequence % slotCount];
                    QueuedPayload* pOccupied = pSlot.load(std::memory_order_acquire);
                    while (pOccupied != nullptr) {
                        pSlot.wait(pOccupied, std::memory_order_acquire);
                        pOccupied = pSlot.load(std::memory_order_acquire);
                    }

                    QueuedPayload* pObject = new (static_cast<void*>(allocator.galloc(
                        groupId, std::type_identity<QueuedPayload>{}))) QueuedPayload;
                    const std::uintptr_t address = addressOf(pObject);
                    if (address % alignof(QueuedPayload) != 0 || address < freeBase ||
                        address >= freeBase + totalFreeCount * sizeof(*pObject)) {
                        validAddresses[workerIndex] = false;
                    }
                    pObject->sequence = sequence;
                    pObject->ownerIndex = workerIndex;
                    pSlot.store(pObject, std::memory_order_release);
                    pSlot.notify_one();
                    ++completedOperations[workerIndex];
                }
            } else if (workerIndex < pairCount * 2) {
                const std::size_t pairIndex = workerIndex - pairCount;
                for (std::size_t sequence = 0; sequence < prefilledCount + allocationsPerWorker;
                     ++sequence) {
                    std::atomic<QueuedPayload*>& pSlot = pSlots[pairIndex][sequence % slotCount];
                    QueuedPayload* pObject = pSlot.load(std::memory_order_acquire);
                    while (pObject == nullptr) {
                        pSlot.wait(nullptr, std::memory_order_acquire);
                        pObject = pSlot.load(std::memory_order_acquire);
                    }

                    if (pObject->sequence != sequence || pObject->ownerIndex != pairIndex) {
                        preservedPayloads[pairIndex] = false;
                    }
                    std::destroy_at(pObject);
                    allocator.gfree(groupId, sizeof(*pObject),
                                    reinterpret_cast<std::byte*>(pObject));
                    pSlot.store(nullptr, std::memory_order_release);
                    pSlot.notify_one();
                    ++completedOperations[workerIndex];
                }
            } else if (workerIndex < pairCount * 2 + reserveWorkerCount) {
                for (std::size_t iteration = 0; iteration < maintenanceIterations; ++iteration) {
                    allocator.greserve(groupId, sizeof(QueuedPayload), reserveBatchSize);
                    ++completedOperations[workerIndex];
                    std::this_thread::yield();
                }
            } else {
                for (std::size_t iteration = 0; iteration < maintenanceIterations; ++iteration) {
                    allocator.gclean(groupId);
                    ++completedOperations[workerIndex];
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    for (std::size_t pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        EXPECT_EQ(completedOperations[pairIndex], allocationsPerWorker);
        EXPECT_EQ(completedOperations[pairIndex + pairCount],
                  prefilledCount + allocationsPerWorker);
        EXPECT_TRUE(preservedPayloads[pairIndex]);
        EXPECT_TRUE(validAddresses[pairIndex]);
        for (const std::atomic<QueuedPayload*>& pSlot : pSlots[pairIndex]) {
            EXPECT_EQ(pSlot.load(std::memory_order_relaxed), nullptr);
        }
    }
    for (std::size_t workerIndex = pairCount * 2; workerIndex < workerCount; ++workerIndex) {
        EXPECT_EQ(completedOperations[workerIndex], maintenanceIterations);
    }
    EXPECT_TRUE(std::ranges::all_of(pSentinel->bytes,
                                    [](std::byte value) { return value == std::byte{0x7c}; }));

    allocator.gclean(groupId);
    std::vector<std::uintptr_t> expectedAddresses;
    expectedAddresses.reserve(totalFreeCount);
    for (std::size_t index = 0; index < totalFreeCount; ++index) {
        expectedAddresses.push_back(freeBase + index * sizeof(QueuedPayload));
    }
    ASSERT_NO_FATAL_FAILURE(expectReusedUnits(expectedAddresses));
    Payload<16>* pNext =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    EXPECT_EQ(addressOf(pNext), freeBase + totalFreeCount * sizeof(QueuedPayload));
    std::destroy_at(pNext);
    allocator.gfree(groupId, sizeof(*pNext), reinterpret_cast<std::byte*>(pNext));
    std::destroy_at(pSentinel);
    allocator.gfree(groupId, sizeof(*pSentinel), reinterpret_cast<std::byte*>(pSentinel));
}

TEST_F(GroupAllocatorTest, ConcurrentAllocationsCommitAdditionalCorePages) {
    using LargePayload = Payload<config::COMMIT_SIZE / 8>;
    constexpr std::size_t workerCount = 4;
    constexpr std::size_t objectsPerWorker = 3;
    std::array<std::array<LargePayload*, objectsPerWorker>, workerCount> pObjects{};
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> phase(static_cast<std::ptrdiff_t>(workerCount));

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            for (LargePayload*& pObject : pObjects[workerIndex]) {
                phase.arrive_and_wait();
                pObject = new (static_cast<void*>(
                    allocator.galloc(groupId, std::type_identity<LargePayload>{}))) LargePayload;
                pObject->bytes[0] = static_cast<std::byte>(workerIndex + 1);
                pObject->bytes[sizeof(LargePayload) - 1] = static_cast<std::byte>(workerIndex + 1);
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    const MemoryRegion region = queryCommittedHeap(pObjects[0][0]);
    const std::uintptr_t firstAlignedAddress =
        roundUpToMultiple(addressOf(region.pBaseAddr), sizeof(LargePayload));
    EXPECT_EQ(region.size, config::COMMIT_SIZE * 2);
    std::vector<std::uintptr_t> addresses;
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        for (LargePayload* pObject : pObjects[workerIndex]) {
            EXPECT_EQ(pObject->bytes[0], static_cast<std::byte>(workerIndex + 1));
            EXPECT_EQ(pObject->bytes[sizeof(LargePayload) - 1],
                      static_cast<std::byte>(workerIndex + 1));
            EXPECT_EQ(queryCommittedHeap(pObject).pBaseAddr, region.pBaseAddr);
            addresses.push_back(addressOf(pObject));
        }
    }
    std::ranges::sort(addresses);
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        EXPECT_EQ(addresses[index], firstAlignedAddress + index * sizeof(LargePayload));
    }
    for (const std::array<LargePayload*, objectsPerWorker>& workerObjects : pObjects) {
        for (LargePayload* pObject : workerObjects) {
            std::destroy_at(pObject);
            allocator.gfree(groupId, sizeof(*pObject), reinterpret_cast<std::byte*>(pObject));
        }
    }
}

TEST_F(GroupAllocatorTest, ConcurrentDifferentGroupsKeepSeparateHeaps) {
    constexpr std::size_t groupCount = 4;
    constexpr std::size_t workerCount = groupCount * 2;
    constexpr std::size_t objectsPerWorker = 64;
    std::array<GroupId, groupCount> groupIds{};
    groupIds[0] = groupId;
    for (std::size_t index = 1; index < groupCount; ++index) {
        ASSERT_LT(nextGroupId, 254);
        groupIds[index] = static_cast<GroupId>(nextGroupId++);
    }

    std::array<std::array<Payload<64>*, objectsPerWorker>, workerCount> pObjects{};
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            const GroupId workerGroupId = groupIds[workerIndex % groupCount];
            for (Payload<64>*& pObject : pObjects[workerIndex]) {
                pObject = new (static_cast<void*>(allocator.galloc(
                    workerGroupId, std::type_identity<Payload<64>>{}))) Payload<64>;
                std::fill_n(pObject->bytes, 64, static_cast<std::byte>(workerIndex + 1));
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    std::array<const void*, groupCount> pHeapBases{};
    for (std::size_t index = 0; index < groupCount; ++index) {
        pHeapBases[index] = queryCommittedHeap(pObjects[index][0]).pBaseAddr;
        for (std::size_t other = 0; other < index; ++other) {
            EXPECT_NE(pHeapBases[index], pHeapBases[other]);
        }
    }
    std::vector<std::uintptr_t> addresses;
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        const std::byte expected = static_cast<std::byte>(workerIndex + 1);
        for (Payload<64>* pObject : pObjects[workerIndex]) {
            EXPECT_EQ(queryCommittedHeap(pObject).pBaseAddr, pHeapBases[workerIndex % groupCount]);
            EXPECT_TRUE(std::ranges::all_of(
                pObject->bytes, [expected](std::byte value) { return value == expected; }));
            addresses.push_back(addressOf(pObject));
        }
    }
    std::ranges::sort(addresses);
    for (std::size_t index = 1; index < addresses.size(); ++index) {
        EXPECT_GE(addresses[index], addresses[index - 1] + sizeof(Payload<64>));
    }
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        for (Payload<64>* pObject : pObjects[workerIndex]) {
            std::destroy_at(pObject);
            allocator.gfree(groupIds[workerIndex % groupCount], sizeof(*pObject),
                            reinterpret_cast<std::byte*>(pObject));
        }
    }
}

TEST_F(GroupAllocatorTest, ConcurrentConstructorsAndDestructorsCanReenterTheSameGroup) {
    constexpr std::size_t workerCount = 8;
    constexpr std::size_t iterations = 64;
    ReentrantObject::groupId = groupId;
    ReentrantObject::constructionCount.store(0, std::memory_order_relaxed);
    ReentrantObject::destructionCount.store(0, std::memory_order_relaxed);
    std::array<bool, workerCount> preservedPayloads{};
    preservedPayloads.fill(true);
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                ReentrantObject* pObject = new (static_cast<void*>(allocator.galloc(
                    groupId, std::type_identity<ReentrantObject>{}))) ReentrantObject;
                for (std::byte value : pObject->pChild->bytes) {
                    if (value != std::byte{0x5a}) {
                        preservedPayloads[workerIndex] = false;
                    }
                }
                std::destroy_at(pObject);
                allocator.gfree(groupId, sizeof(*pObject), reinterpret_cast<std::byte*>(pObject));
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(ReentrantObject::constructionCount.load(std::memory_order_relaxed),
              workerCount * iterations);
    EXPECT_EQ(ReentrantObject::destructionCount.load(std::memory_order_relaxed),
              workerCount * iterations);
    for (bool preserved : preservedPayloads) {
        EXPECT_TRUE(preserved);
    }
}

#ifndef NDEBUG
TEST_F(GroupAllocatorTest, GcleanRejectsAnUnregisteredGroup) {
    ASSERT_DEATH(allocator.gclean(groupId), "isExistGroup");
}

TEST_F(GroupAllocatorTest, GcleanRejectsAGroupWithoutFreeSegments) {
    Payload<16>* pObject =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload<16>>{})))
            Payload<16>;
    ASSERT_NE(pObject, nullptr);
    ASSERT_DEATH(allocator.gclean(groupId), "sortedSegCount > 0");
}
#endif

} // namespace
} // namespace naru::tests::allocator
