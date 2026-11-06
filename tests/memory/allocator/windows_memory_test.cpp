#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <windows.h>

#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::tests::allocator {
namespace {
namespace memory = ::naru::allocator::platform;

void expectMemoryState(const void* pAddress, DWORD expectedState) {
    MEMORY_BASIC_INFORMATION region{};
    ASSERT_EQ(VirtualQuery(pAddress, &region, sizeof(region)), sizeof(region));
    EXPECT_EQ(region.State, expectedState);
}

TEST(WindowsMemoryTest, ReserveCommitDecommitAndReleaseReturnOnSuccess) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 4,
        .commitPageCount = 2,
    };

    std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
    expectMemoryState(pBaseAddr, MEM_RESERVE);

    memory::commitCommonMemory(pBaseAddr, info);
    expectMemoryState(pBaseAddr, MEM_COMMIT);
    expectMemoryState(pBaseAddr + 2 * pageSize, MEM_RESERVE);
    pBaseAddr[0] = std::byte{0x31};
    pBaseAddr[2 * pageSize - 1] = std::byte{0x52};
    EXPECT_EQ(pBaseAddr[0], std::byte{0x31});
    EXPECT_EQ(pBaseAddr[2 * pageSize - 1], std::byte{0x52});

    memory::decommitCommonMemory(pBaseAddr, info);
    expectMemoryState(pBaseAddr, MEM_RESERVE);

    memory::releasePostCommitMemory(pBaseAddr);
    expectMemoryState(pBaseAddr, MEM_FREE);
}

TEST(WindowsMemoryTest, ReleaseEntireMemoryAcceptsCommittedMemory) {
    const memory::VmMemoryInfo info{
        .pageSize = memory::getCommonPageSize(),
        .reservePageCount = 1,
        .commitPageCount = 1,
    };
    void* pBaseAddr = memory::reserveCommonMemory(info);
    memory::commitCommonMemory(pBaseAddr, info);

    memory::releaseEntireMemory(pBaseAddr);
    expectMemoryState(pBaseAddr, MEM_FREE);
}

TEST(WindowsMemoryDeathTest, InvalidAddressReleaseTerminatesAtTheSystemCallBoundary) {
    void* pInvalidAddr = reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)());

#ifndef NDEBUG
    ASSERT_DEATH(memory::releaseEntireMemory(pInvalidAddr), "VirtualQuery failed");
#else
    ASSERT_DEATH(memory::releaseEntireMemory(pInvalidAddr), "VirtualFree release failed");
#endif
}

TEST(WindowsMemoryDeathTest, ReserveFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = (std::numeric_limits<std::size_t>::max)() / pageSize,
    };

    ASSERT_DEATH(static_cast<void>(memory::reserveCommonMemory(info)),
                 "VirtualAlloc reserve failed");
}

TEST(WindowsMemoryDeathTest, CommitFailureTerminatesAtTheSystemCallBoundary) {
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
        "VirtualAlloc commit failed");
}

TEST(WindowsMemoryDeathTest, DecommitFailureTerminatesAtTheSystemCallBoundary) {
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
        "VirtualFree decommit failed");
}

TEST(WindowsMemoryDeathTest, PostCommitReleaseFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 2,
    };

    ASSERT_DEATH(
        {
            std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
            memory::releasePostCommitMemory(pBaseAddr + pageSize, info);
        },
        "VirtualFree release failed");
}

TEST(WindowsMemoryDeathTest, EntireReleaseFailureTerminatesAtTheSystemCallBoundary) {
    const std::size_t pageSize = memory::getCommonPageSize();
    const memory::VmMemoryInfo info{
        .pageSize = pageSize,
        .reservePageCount = 2,
    };

    ASSERT_DEATH(
        {
            std::byte* pBaseAddr = static_cast<std::byte*>(memory::reserveCommonMemory(info));
            memory::releaseEntireMemory(pBaseAddr + pageSize);
        },
        "VirtualFree release failed");
}
} // namespace
} // namespace naru::tests::allocator

#endif
