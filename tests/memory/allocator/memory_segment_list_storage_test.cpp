#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <new>
#include <string>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#elifdef __linux__
#include <cerrno>
#include <sys/mman.h>
#endif

#include "allocator_test_support.hpp"
#include "group/memory_config.hpp"
#include "group/memory_segment_list.hpp"
#include "group/memory_segment_list_storage.hpp"
#include "group/memory_segment_list_vector.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::tests::allocator {
namespace {
static_assert(!std::is_default_constructible_v<alloc::group::MemorySegListStorage>);
static_assert(!std::is_copy_constructible_v<alloc::group::MemorySegListStorage>);
static_assert(!std::is_copy_assignable_v<alloc::group::MemorySegListStorage>);
static_assert(!std::is_move_constructible_v<alloc::group::MemorySegListStorage>);
static_assert(!std::is_move_assignable_v<alloc::group::MemorySegListStorage>);
static_assert(std::is_same_v<decltype(alloc::group::MemorySegListStorage::createVal()),
                             alloc::group::MemorySegListStorage>);

void expectReleased(const void* pAddress) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION region{};
    ASSERT_EQ(VirtualQuery(pAddress, &region, sizeof(region)), sizeof(region));
    EXPECT_EQ(region.State, MEM_FREE);
#elifdef __linux__
    const std::size_t pageSize = alloc::platform::getCommonPageSize();
    const std::uintptr_t pageAddress =
        reinterpret_cast<std::uintptr_t>(pAddress) / pageSize * pageSize;
    unsigned char residency = 0;
    const int result = mincore(reinterpret_cast<void*>(pageAddress), pageSize, &residency);
    const int error = errno;
    EXPECT_EQ(result, -1);
    EXPECT_EQ(error, ENOMEM);
#endif
}

class MemorySegmentListStorageTest : public ::testing::Test
{
protected:
    static void createGroup(alloc::group::MemorySegListStorage& targetStorage,
                            std::uint8_t groupId) {
        const std::size_t pageSize = alloc::platform::getCommonPageSize();
        const alloc::platform::VmMemoryInfo info{
            .pageSize = pageSize,
            .reservePageCount = 2,
            .commitPageCount = 1,
        };
        std::byte* pBaseAddr = static_cast<std::byte*>(alloc::platform::reserveCommonMemory(info));
        alloc::platform::commitCommonMemory(pBaseAddr, info);

        targetStorage.setGroupBaseAddr(groupId, pBaseAddr);
        targetStorage.setPageSize(groupId, pageSize);
        targetStorage.setLastReservedPageIndex(groupId, 1);
        targetStorage.setLastCommittedPageIndex(groupId, 0);

        alloc::group::MemorySegListVector& usedLists = targetStorage.getSameGroupUsedLists(groupId);
        alloc::group::MemorySegListVector& freeLists = targetStorage.getSameGroupFreeLists(groupId);
        new (static_cast<void*>(&usedLists)) alloc::group::MemorySegListVector(
            alloc::group::MemorySegListVector::createVal({.isFree = false}));
        new (static_cast<void*>(&freeLists)) alloc::group::MemorySegListVector(
            alloc::group::MemorySegListVector::createVal({.isFree = true}));
        targetStorage.setGroupUsedState(groupId, true);
    }

    alloc::group::MemorySegListStorage storage = alloc::group::MemorySegListStorage::createVal();
};

TEST_F(MemorySegmentListStorageTest, DestroySkipsUninitializedGroups) {
    storage.destroy();
    EXPECT_FALSE(storage.isExistGroup(0));
    EXPECT_FALSE(storage.isExistGroup(255));
}

TEST_F(MemorySegmentListStorageTest, DestroyHandlesGroupWithoutSizeLists) {
    constexpr std::uint8_t groupId = 7;
    ASSERT_NO_FATAL_FAILURE(createGroup(storage, groupId));

    std::byte* pCoreHeap = storage.getGroupBaseHeapAddr(groupId);
    const std::array<const void*, 3> heapAddresses{
        pCoreHeap,
        pCoreHeap + storage.getPageSize(groupId),
        &storage.getSameGroupFreeLists(groupId).getSortedFreeSegVector(),
    };

    storage.destroy();

    for (const void* pAddress : heapAddresses) {
        ASSERT_NO_FATAL_FAILURE(expectReleased(pAddress));
    }
    EXPECT_FALSE(storage.isExistGroup(groupId));
}

TEST_F(MemorySegmentListStorageTest, DestroyReleasesAllOwnedHeapsAcrossGroups) {
    const std::array<std::uint8_t, 2> groupIds{0, 255};
    const std::array<std::size_t, 2> sizes{16, 32};
    std::array<const void*, 18> heapAddresses{};
    std::size_t addressIndex = 0;

    for (std::uint8_t groupId : groupIds) {
        SCOPED_TRACE(groupId);
        ASSERT_NO_FATAL_FAILURE(createGroup(storage, groupId));
        std::byte* pCoreHeap = storage.getGroupBaseHeapAddr(groupId);
        alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
        alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);

        heapAddresses[addressIndex++] = pCoreHeap;
        heapAddresses[addressIndex++] = pCoreHeap + storage.getPageSize(groupId);
        heapAddresses[addressIndex++] = &freeLists.getSortedFreeSegVector();

        std::size_t offset = 0;
        for (std::size_t size : sizes) {
            ASSERT_TRUE(usedLists.emplaceBack(groupId, size).hasValue());
            ASSERT_TRUE(freeLists.emplaceBack(groupId, size).hasValue());
            alloc::group::MemorySegList& usedList = usedLists.back();
            alloc::group::MemorySegList& freeList = freeLists.back();
            ASSERT_TRUE(usedList.emplaceBack({pCoreHeap + offset}).hasValue());
            ASSERT_TRUE(freeList.emplaceBack({pCoreHeap + offset + 64}).hasValue());
            heapAddresses[addressIndex++] = &*usedList.begin();
            heapAddresses[addressIndex++] = &*freeList.begin();
            offset += 128;
        }

        heapAddresses[addressIndex++] = &usedLists[0];
        heapAddresses[addressIndex++] = &freeLists[0];
        storage.setCurrentOffset(groupId, offset);
    }
    ASSERT_EQ(addressIndex, heapAddresses.size());

    storage.destroy();

    for (const void* pAddress : heapAddresses) {
        SCOPED_TRACE(pAddress);
        ASSERT_NO_FATAL_FAILURE(expectReleased(pAddress));
    }
    for (std::uint8_t groupId : groupIds) {
        SCOPED_TRACE(groupId);
        EXPECT_FALSE(storage.isExistGroup(groupId));
        EXPECT_EQ(storage.getGroupBaseHeapAddr(groupId), nullptr);
        EXPECT_EQ(storage.getPageSize(groupId), 0);
        EXPECT_EQ(storage.getCurrentOffset(groupId), 0);
        EXPECT_EQ(storage.getLastReservedPageIndex(groupId), config::INVALID_PAGE_INDEX);
        EXPECT_EQ(storage.getLastCommittedPageIndex(groupId), config::INVALID_PAGE_INDEX);
        alloc::group::MemorySegListVector& usedLists = storage.getSameGroupUsedLists(groupId);
        alloc::group::MemorySegListVector& freeLists = storage.getSameGroupFreeLists(groupId);
        EXPECT_TRUE(usedLists.isEmpty());
        EXPECT_TRUE(freeLists.isEmpty());
        EXPECT_FALSE(freeLists.isFreeLists());
    }

    storage.destroy();
}

TEST_F(MemorySegmentListStorageTest, DestructorReleasesAllOwnedHeaps) {
    std::array<const void*, 7> heapAddresses{};
    {
        alloc::group::MemorySegListStorage scopedStorage =
            alloc::group::MemorySegListStorage::createVal();
        constexpr std::uint8_t groupId = 7;
        ASSERT_NO_FATAL_FAILURE(createGroup(scopedStorage, groupId));
        std::byte* pCoreHeap = scopedStorage.getGroupBaseHeapAddr(groupId);
        alloc::group::MemorySegListVector& usedLists = scopedStorage.getSameGroupUsedLists(groupId);
        alloc::group::MemorySegListVector& freeLists = scopedStorage.getSameGroupFreeLists(groupId);
        ASSERT_TRUE(usedLists.emplaceBack(groupId, 16).hasValue());
        ASSERT_TRUE(freeLists.emplaceBack(groupId, 16).hasValue());
        alloc::group::MemorySegList& usedList = usedLists[0];
        alloc::group::MemorySegList& freeList = freeLists[0];
        ASSERT_TRUE(usedList.emplaceBack({pCoreHeap}).hasValue());
        ASSERT_TRUE(freeList.emplaceBack({pCoreHeap + 16}).hasValue());

        heapAddresses = {
            pCoreHeap,
            pCoreHeap + scopedStorage.getPageSize(groupId),
            &usedList,
            &freeList,
            &*usedList.begin(),
            &*freeList.begin(),
            &freeLists.getSortedFreeSegVector(),
        };
    }

    for (const void* pAddress : heapAddresses) {
        SCOPED_TRACE(pAddress);
        ASSERT_NO_FATAL_FAILURE(expectReleased(pAddress));
    }
}
} // namespace
} // namespace naru::tests::allocator
