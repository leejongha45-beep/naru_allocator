#ifdef __linux__

#include <cerrno>
#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>

#include "memory_region_test_support.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::tests::allocator {
namespace {
namespace memory = ::naru::allocator::platform;

void expectMemoryState(const void* pAddress, MemoryRegionState expectedState) {
    EXPECT_EQ(queryMemoryRegion(pAddress).state, expectedState);
}

void expectUnmapped(const std::byte* pBaseAddr, const memory::VmMemoryInfo& info) {
    for (std::size_t pageIndex = 0; pageIndex < info.reservePageCount; ++pageIndex) {
        unsigned char residency = 0;
        errno = 0;
        const int result = mincore(const_cast<std::byte*>(pBaseAddr + pageIndex * info.pageSize),
                                   info.pageSize, &residency);
        const int error = errno;
        EXPECT_EQ(result, -1) << "pageIndex=" << pageIndex;
        EXPECT_EQ(error, ENOMEM) << "pageIndex=" << pageIndex;
    }
}

TEST(LinuxMemoryTest, ReserveCommitDecommitAndReleaseReturnOnSuccess) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 4,
        .commitPageCount = 2,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    for (std::size_t pageIndex = 0; pageIndex < info.reservePageCount; ++pageIndex) {
        expectMemoryState(pBaseAddr + pageIndex * pageSize, MemoryRegionState::RESERVED);
    }

    memory::commitCommonMemory(pBaseAddr, info);
    for (std::size_t pageIndex = 0; pageIndex < info.reservePageCount; ++pageIndex) {
        const MemoryRegionState expected = pageIndex < info.commitPageCount
                                               ? MemoryRegionState::COMMITTED
                                               : MemoryRegionState::RESERVED;
        expectMemoryState(pBaseAddr + pageIndex * pageSize, expected);
    }
    pBaseAddr[0] = std::byte{0x31};
    pBaseAddr[2 * pageSize - 1] = std::byte{0x52};
    EXPECT_EQ(pBaseAddr[0], std::byte{0x31});
    EXPECT_EQ(pBaseAddr[2 * pageSize - 1], std::byte{0x52});

    memory::decommitCommonMemory(pBaseAddr, info);
    for (std::size_t pageIndex = 0; pageIndex < info.reservePageCount; ++pageIndex) {
        expectMemoryState(pBaseAddr + pageIndex * pageSize, MemoryRegionState::RESERVED);
    }

    memory::releasePostCommitMemory(pBaseAddr, info);
    expectUnmapped(pBaseAddr, info);
}

TEST(LinuxMemoryTest, ReleaseEntireMemoryAcceptsCommittedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    memory::commitCommonMemory(pBaseAddr, info);
    expectMemoryState(pBaseAddr, MemoryRegionState::COMMITTED);

    memory::releaseEntireMemory(pBaseAddr, info);
    expectUnmapped(pBaseAddr, info);
}

TEST(LinuxMemoryTest, PartialCommitDecommitAndRecommitReturnOnSuccess) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo regionInfo{
        .pageSize = pageSize,
        .reservePageCount = 3,
        .commitPageCount = 1,
    };
    const memory::VmMemoryInfo pageInfo{
        .pageSize = pageSize,
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(regionInfo));
    std::byte* pMiddlePage = pBaseAddr + pageSize;

    memory::commitCommonMemory(pMiddlePage, pageInfo);
    pMiddlePage[0] = std::byte{0x31};
    pMiddlePage[pageSize - 1] = std::byte{0x52};
    EXPECT_EQ(pMiddlePage[0], std::byte{0x31});
    EXPECT_EQ(pMiddlePage[pageSize - 1], std::byte{0x52});

    memory::commitCommonMemory(pBaseAddr, pageInfo);
    pBaseAddr[0] = std::byte{0x17};
    memory::decommitCommonMemory(pMiddlePage, pageInfo);
    expectMemoryState(pMiddlePage, MemoryRegionState::RESERVED);
    expectMemoryState(pBaseAddr, MemoryRegionState::COMMITTED);
    expectMemoryState(pBaseAddr + 2 * pageSize, MemoryRegionState::RESERVED);
    memory::commitCommonMemory(pMiddlePage, pageInfo);
    expectMemoryState(pMiddlePage, MemoryRegionState::COMMITTED);
    pMiddlePage[0] = std::byte{0x63};
    EXPECT_EQ(pBaseAddr[0], std::byte{0x17});
    EXPECT_EQ(pMiddlePage[0], std::byte{0x63});

    memory::releaseEntireMemory(pBaseAddr, regionInfo);
    expectUnmapped(pBaseAddr, regionInfo);
}

TEST(LinuxMemoryTest, EntireReleaseAcceptsReservedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 3,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));

    memory::releaseEntireMemory(pBaseAddr, info);

    expectUnmapped(pBaseAddr, info);
}

TEST(LinuxMemoryTest, PostCommitReleaseUnmapsTheEntireDecommittedReservation) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 4,
        .commitPageCount = 2,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    memory::commitCommonMemory(pBaseAddr, info);
    pBaseAddr[0] = std::byte{0x31};
    pBaseAddr[info.commitPageCount * info.pageSize - 1] = std::byte{0x52};
    memory::decommitCommonMemory(pBaseAddr, info);

    memory::releasePostCommitMemory(pBaseAddr, info);

    expectUnmapped(pBaseAddr, info);
}

TEST(LinuxMemoryDeathTest, PostCommitReleaseFailureTerminatesAtTheSystemCallBoundary) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));

    ASSERT_DEATH(memory::releasePostCommitMemory(pBaseAddr + 1, info),
                 "munmap post-commit release failed");

    memory::releasePostCommitMemory(pBaseAddr, info);
}

TEST(LinuxMemoryDeathTest, ReserveFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = (std::numeric_limits<std::size_t>::max)() / pageSize,
    };

    ASSERT_DEATH(static_cast<void>(memory::reserveCommonMemory(info)), "mmap reserve failed");
}

TEST(LinuxMemoryDeathTest, CommitFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 1,
        .commitPageCount = (std::numeric_limits<std::size_t>::max)() / pageSize,
    };

    ASSERT_DEATH(
        {
            void* pBaseAddr = memory::reserveCommonMemory(info);
            memory::commitCommonMemory(pBaseAddr, info);
        },
        "mprotect commit failed");
}

TEST(LinuxMemoryDeathTest, DecommitFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    memory::VmMemoryInfo decommitInfo = info;
    decommitInfo.reservePageCount = (std::numeric_limits<std::size_t>::max)() / pageSize;

    ASSERT_DEATH(
        {
            void* pBaseAddr = memory::reserveCommonMemory(info);
            memory::commitCommonMemory(pBaseAddr, info);
            memory::decommitCommonMemory(pBaseAddr, decommitInfo);
        },
        "mprotect decommit failed");
}

TEST(LinuxMemoryDeathTest, EntireReleaseFailureTerminatesAtTheSystemCallBoundary) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    memory::commitCommonMemory(pBaseAddr, info);

    ASSERT_DEATH(memory::releaseEntireMemory(pBaseAddr + 1, info), "munmap entire release failed");

    memory::releaseEntireMemory(pBaseAddr, info);
}

#ifndef NDEBUG
TEST(LinuxMemoryDeathTest, ReleaseTerminatesWhenItsInternalStateQueryFails) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);

    // Restrict new descriptors only in the death-test child, making /proc/self/maps fail to open.
    ASSERT_DEATH(
        {
            rlimit limit{};
            ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &limit), 0);
            limit.rlim_cur = 0;
            ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &limit), 0);
            memory::releaseEntireMemory(pBaseAddr, info);
        },
        "Opening /proc/self/maps failed");

    memory::releaseEntireMemory(pBaseAddr, info);
}

TEST(LinuxMemoryDeathTest, CommitRejectsAlreadyCommittedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);
    memory::commitCommonMemory(pBaseAddr, info);

    ASSERT_DEATH(memory::commitCommonMemory(pBaseAddr, info), "Check failed");

    ASSERT_EQ(munmap(pBaseAddr, info.pageSize), 0);
}

TEST(LinuxMemoryDeathTest, DecommitRejectsReservedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);

    ASSERT_DEATH(memory::decommitCommonMemory(pBaseAddr, info), "Check failed");

    ASSERT_EQ(munmap(pBaseAddr, info.pageSize), 0);
}

TEST(LinuxMemoryDeathTest, UnmappedPagesCannotBeCommittedOrReleased) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 3,
        .commitPageCount = 1,
    };
    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    std::byte* pMiddlePage = pBaseAddr + info.pageSize;
    ASSERT_EQ(munmap(pMiddlePage, info.pageSize), 0);

    const memory::VmMemoryInfo pageInfo{
        .pageSize = info.pageSize,
        .reservePageCount = 1,
    };
    ASSERT_DEATH(memory::commitCommonMemory(pMiddlePage, info), "Check failed");
    ASSERT_DEATH(memory::releaseEntireMemory(pMiddlePage, pageInfo), "Check failed");

    ASSERT_EQ(munmap(pBaseAddr, info.reservePageCount * info.pageSize), 0);
}

TEST(LinuxMemoryDeathTest, PostCommitReleaseRejectsCommittedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);
    memory::commitCommonMemory(pBaseAddr, info);

    ASSERT_DEATH(memory::releasePostCommitMemory(pBaseAddr, info), "Check failed");

    ASSERT_EQ(munmap(pBaseAddr, info.pageSize), 0);
}

TEST(LinuxMemoryDeathTest, QueryRejectsProtectionOutsideTheAllocatorContract) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);
    ASSERT_EQ(mprotect(pBaseAddr, info.pageSize, PROT_READ), 0);

    ASSERT_DEATH(memory::commitCommonMemory(pBaseAddr, info),
                 "Unexpected allocator memory protection");

    ASSERT_EQ(munmap(pBaseAddr, info.pageSize), 0);
}
#endif
} // namespace
} // namespace naru::tests::allocator

#endif // __linux__
