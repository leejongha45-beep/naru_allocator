#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "memory_region_test_support.hpp"
// Compile the implementation here to inspect its cpp-local Impl.
// Public API tests and consumers separately link the production archive.
#include "naru/allocator/owned/owned_allocator.hpp"
#include "naru/support/log.hpp"
#include "naru/support/static_pimpl.hpp"
#include "owned/owned_allocator.cpp"
#include "platform/memory.hpp"

namespace naru::tests::allocator {
namespace {
namespace owned = ::naru::allocator::owned;
namespace memory = ::naru::allocator::platform;

static_assert(!std::is_copy_constructible_v<owned::OwnedAllocator::Impl>);
static_assert(!std::is_copy_assignable_v<owned::OwnedAllocator::Impl>);
static_assert(std::is_move_constructible_v<owned::OwnedAllocator::Impl>);
static_assert(!std::is_move_assignable_v<owned::OwnedAllocator::Impl>);

constexpr std::size_t reserveBytes = owned::OwnedAllocator::Impl::OWNED_RESERVE_SIZE;
constexpr std::size_t commitBytes = owned::OwnedAllocator::Impl::OWNED_COMMIT_SIZE;
constexpr std::size_t unitBytes = owned::OwnedAllocator::Impl::OWNED_ALLOC_SIZE_UNIT;
constexpr int capacityFailureExitCode = 86;

template <std::size_t Size, std::size_t ExpectedStride>
struct TestElement {
    std::byte bytes[Size];
    static constexpr std::size_t expectedStride = ExpectedStride;
};

void expectRegionSpan(const std::byte* pStart, std::size_t size, MemoryRegionState state) {
    std::size_t offset = 0;
    while (offset < size) {
        const MemoryRegion region = queryMemoryRegion(pStart + offset);
        ASSERT_EQ(region.state, state) << "offset=" << offset;
        const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(region.pBaseAddr);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pStart + offset);
        ASSERT_GE(address, regionStart);
        ASSERT_LT(address - regionStart, region.size);
        const std::size_t remainingRegion = region.size - (address - regionStart);
        offset += std::min(remainingRegion, size - offset);
    }
}

std::byte pageMarker(std::size_t offset, std::size_t pageSize) {
    return static_cast<std::byte>((offset / pageSize) % 251 + 1);
}

void expectPageMarkers(std::byte* pBase, std::size_t start, std::size_t size,
                       std::size_t pageSize) {
    for (std::size_t offset = start; offset < start + size; offset += pageSize) {
        volatile std::byte* pPage = pBase + offset;
        const std::byte first = pPage[0];
        const std::byte last = pPage[pageSize - 1];
        EXPECT_EQ(first, pageMarker(offset, pageSize)) << "offset=" << offset;
        EXPECT_EQ(last, std::byte{0xa5}) << "offset=" << offset;
    }
}

void writeAndReadPages(std::byte* pBase, std::size_t start, std::size_t size,
                       std::size_t pageSize) {
    ASSERT_EQ(start % pageSize, 0);
    ASSERT_EQ(size % pageSize, 0);
    for (std::size_t offset = start; offset < start + size; offset += pageSize) {
        volatile std::byte* pPage = pBase + offset;
        pPage[0] = pageMarker(offset, pageSize);
        pPage[pageSize - 1] = std::byte{0xa5};
    }
    expectPageMarkers(pBase, start, size, pageSize);
}

void expectBytes(const std::byte* pBase, std::size_t size, std::byte expected) {
    const volatile std::byte* pObserved = pBase;
    for (std::size_t offset = 0; offset < size; ++offset) {
        const std::byte actual = pObserved[offset];
        if (actual != expected) {
            FAIL() << "offset=" << offset << " expected=" << std::to_integer<int>(expected)
                   << " actual=" << std::to_integer<int>(actual);
        }
    }
}

template <typename T>
class OwnedAllocatorFixture : public ::testing::Test
{
protected:
    using Allocator = owned::OwnedAllocator;
    Allocator allocator = Allocator::createVal({.elementSize = sizeof(T)});
    Allocator::Impl& impl = *allocator.m_impl;

    void expectAllocation(std::size_t count) {
        const std::byte* pExpected = impl.m_pBaseAddr + impl.m_currentOffset;
        std::byte* pAllocation = allocator.oalloc(count);
        ASSERT_EQ(pAllocation, pExpected);
    }

    void expectState(std::size_t offset, std::size_t committed) {
        ASSERT_NE(impl.m_pBaseAddr, nullptr);
        ASSERT_EQ(impl.m_pageSize, memory::getCommonPageSize());
        ASSERT_EQ(impl.m_reservedSize, reserveBytes);
        ASSERT_EQ(impl.m_allocSize, T::expectedStride);
        ASSERT_EQ(impl.m_currentOffset, offset);
        ASSERT_EQ(impl.m_committedSize, committed);
        ASSERT_LE(offset, committed);
        ASSERT_LE(committed, reserveBytes);
        ASSERT_EQ(committed % impl.m_pageSize, 0);
        ASSERT_NO_FATAL_FAILURE(
            expectRegionSpan(impl.m_pBaseAddr, committed, MemoryRegionState::COMMITTED));
        ASSERT_NO_FATAL_FAILURE(expectRegionSpan(
            impl.m_pBaseAddr + committed, reserveBytes - committed, MemoryRegionState::RESERVED));
    }

    void writeAndReadCommittedPages() {
        writeAndReadPages(impl.m_pBaseAddr, 0, impl.m_committedSize, impl.m_pageSize);
    }

    void fillAndVerifyCommittedBytes(std::byte value) {
        std::memset(impl.m_pBaseAddr, std::to_integer<int>(value), impl.m_committedSize);
        ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, impl.m_committedSize, value));
    }

    void expectResetHeap(std::byte* pExpectedBase, std::size_t expectedCommitted) {
        ASSERT_EQ(impl.m_pBaseAddr, pExpectedBase);
        ASSERT_NO_FATAL_FAILURE(expectState(0, expectedCommitted));
    }
};

template <typename T>
class OwnedAllocatorCreateTest : public OwnedAllocatorFixture<T>
{
};

using ElementSizes = ::testing::Types<TestElement<1, 16>, TestElement<16, 16>, TestElement<18, 32>,
                                      TestElement<32, 32>, TestElement<33, 48>>;
TYPED_TEST_SUITE(OwnedAllocatorCreateTest, ElementSizes);

TYPED_TEST(OwnedAllocatorCreateTest, InitialMappingAndPaddedStride) {
    ASSERT_NO_FATAL_FAILURE(this->expectState(0, commitBytes));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(this->impl.m_pBaseAddr) % this->impl.m_pageSize, 0);
    ASSERT_NO_FATAL_FAILURE(this->expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(this->expectState(TypeParam::expectedStride, commitBytes));
    ASSERT_NO_FATAL_FAILURE(this->writeAndReadCommittedPages());
}

using OwnedAllocatorRuntimeTest = OwnedAllocatorFixture<TestElement<16, 16>>;
using OwnedAllocatorPaddedTest = OwnedAllocatorFixture<TestElement<33, 48>>;
using OwnedAllocatorDeathTest = OwnedAllocatorFixture<TestElement<16, 16>>;
using OwnedAllocatorFreeTest = OwnedAllocatorFixture<TestElement<16, 16>>;
using OwnedAllocatorPaddedFreeTest = OwnedAllocatorFixture<TestElement<33, 48>>;
using OwnedAllocatorMoveTest = OwnedAllocatorFixture<TestElement<33, 48>>;

void expectEmptyAllocator(const owned::OwnedAllocator& allocator) {
    const owned::OwnedAllocator::Impl& impl = *allocator.m_impl;
    EXPECT_EQ(impl.m_pBaseAddr, nullptr);
    EXPECT_EQ(impl.m_currentOffset, 0);
    EXPECT_EQ(impl.m_pageSize, 0);
    EXPECT_EQ(impl.m_reservedSize, 0);
    EXPECT_EQ(impl.m_committedSize, 0);
    EXPECT_EQ(impl.m_allocSize, 0);
}

TEST_F(OwnedAllocatorMoveTest, MoveConstructionPreservesHeapAndClearsSource) {
    constexpr std::size_t initialOffset = commitBytes / 48 * 48;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / 48));
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    const std::size_t originalPageSize = impl.m_pageSize;
    pOriginalBase[0] = std::byte{0xa5};

    Allocator moved(std::move(allocator));
    Allocator::Impl& movedImpl = *moved.m_impl;
    expectEmptyAllocator(allocator);
    EXPECT_EQ(movedImpl.m_pBaseAddr, pOriginalBase);
    EXPECT_EQ(movedImpl.m_currentOffset, initialOffset);
    EXPECT_EQ(movedImpl.m_pageSize, originalPageSize);
    EXPECT_EQ(movedImpl.m_reservedSize, reserveBytes);
    EXPECT_EQ(movedImpl.m_committedSize, commitBytes);
    EXPECT_EQ(movedImpl.m_allocSize, 48);
    EXPECT_EQ(moved.oalloc(1), pOriginalBase + initialOffset);
    EXPECT_EQ(movedImpl.m_committedSize, 2 * commitBytes);
    EXPECT_EQ(pOriginalBase[0], std::byte{0xa5});

    Allocator owner(std::move(moved));
    Allocator::Impl& ownerImpl = *owner.m_impl;
    expectEmptyAllocator(moved);
    EXPECT_EQ(ownerImpl.m_pBaseAddr, pOriginalBase);
    EXPECT_EQ(ownerImpl.m_currentOffset, initialOffset + 48);
    EXPECT_EQ(ownerImpl.m_pageSize, originalPageSize);
    EXPECT_EQ(ownerImpl.m_reservedSize, reserveBytes);
    EXPECT_EQ(ownerImpl.m_committedSize, 2 * commitBytes);
    EXPECT_EQ(ownerImpl.m_allocSize, 48);
    EXPECT_EQ(owner.oalloc(1), pOriginalBase + initialOffset + 48);
    EXPECT_EQ(pOriginalBase[0], std::byte{0xa5});

    owner.ofree();
    EXPECT_EQ(ownerImpl.m_currentOffset, 0);
    EXPECT_EQ(ownerImpl.m_pBaseAddr, pOriginalBase);
    EXPECT_EQ(ownerImpl.m_committedSize, 2 * commitBytes);
    EXPECT_EQ(pOriginalBase[0], std::byte{0xa5});
    EXPECT_EQ(owner.oalloc(1), pOriginalBase);
    EXPECT_EQ(pOriginalBase[0], std::byte{0xa5});
}

void expectReleasedHeap(std::uintptr_t baseAddress, std::size_t committedSize) {
    const std::size_t offsets[] = {0, committedSize - 1, committedSize, reserveBytes / 2,
                                   reserveBytes - 1};
    for (const std::size_t offset : offsets) {
        if (offset >= reserveBytes) {
            continue;
        }

        const void* pAddress = reinterpret_cast<const void*>(baseAddress + offset);
        EXPECT_EQ(queryMemoryRegion(pAddress).state, MemoryRegionState::FREE)
            << "offset=" << offset;
    }
}

void expectDestructionReleasesHeap(std::size_t allocationCount) {
    using Allocator = owned::OwnedAllocator;
    std::uintptr_t baseAddress = 0;
    std::size_t committedSize = 0;
    {
        Allocator allocator = Allocator::createVal({.elementSize = unitBytes});
        if (allocationCount != 0) {
            static_cast<void>(allocator.oalloc(allocationCount));
        }

        const Allocator::Impl& impl = *allocator.m_impl;
        baseAddress = reinterpret_cast<std::uintptr_t>(impl.m_pBaseAddr);
        committedSize = impl.m_committedSize;
        ASSERT_NO_FATAL_FAILURE(
            expectRegionSpan(impl.m_pBaseAddr, committedSize, MemoryRegionState::COMMITTED));
        ASSERT_NO_FATAL_FAILURE(expectRegionSpan(impl.m_pBaseAddr + committedSize,
                                                 reserveBytes - committedSize,
                                                 MemoryRegionState::RESERVED));
    }
    expectReleasedHeap(baseAddress, committedSize);
}

TEST(OwnedAllocatorDestructionTest, ReleasesInitialCommitAndReservedTail) {
    expectDestructionReleasesHeap(0);
}

TEST(OwnedAllocatorDestructionTest, ReleasesGrownCommitAndReservedTail) {
    expectDestructionReleasesHeap(commitBytes / unitBytes + 1);
}

TEST(OwnedAllocatorDestructionTest, ReleasesFullyCommittedReservation) {
    expectDestructionReleasesHeap(reserveBytes / unitBytes);
}

TEST(OwnedAllocatorDestructionTest, MovedFromDestructionPreservesHeapUntilOwnerIsDestroyed) {
    using Allocator = owned::OwnedAllocator;
    std::uintptr_t baseAddress = 0;
    {
        std::optional<Allocator> owner;
        std::byte* pBase = nullptr;
        {
            Allocator source = Allocator::createVal({.elementSize = unitBytes});
            pBase = source.oalloc(1);
            pBase[0] = std::byte{0xa5};
            baseAddress = reinterpret_cast<std::uintptr_t>(pBase);
            owner.emplace(std::move(source));
            expectEmptyAllocator(source);
        }

        ASSERT_EQ(queryMemoryRegion(pBase).state, MemoryRegionState::COMMITTED);
        EXPECT_EQ(pBase[0], std::byte{0xa5});
        EXPECT_EQ(owner->oalloc(1), pBase + unitBytes);
    }
    expectReleasedHeap(baseAddress, commitBytes);
}

TEST_F(OwnedAllocatorRuntimeTest, AllocatesWithinInitialCommitWithoutGrowing) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(unitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes - 2));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes - unitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, ExactInitialCommitBoundaryDoesNotGrow) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, OneUnitAcrossInitialCommitBoundaryGrowsByOneBlock) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes - 1));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes - unitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectAllocation(2));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes + unitBytes, 2 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, RequestLargerThanInitialCommitUsesExistingSlack) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes + 1));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes + unitBytes, 2 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, ExactShortfallMultipleDoesNotCommitExtraBlock) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectAllocation(2 * commitBytes / unitBytes - 1));
    ASSERT_NO_FATAL_FAILURE(expectState(2 * commitBytes, 2 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, MultiBlockRequestUsesPartialCommittedSpace) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(3));
    ASSERT_NO_FATAL_FAILURE(expectAllocation(2 * commitBytes / unitBytes + 1));
    ASSERT_NO_FATAL_FAILURE(expectState(2 * commitBytes + 4 * unitBytes, 3 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, PreservesExistingDataWhileGrowing) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());

    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes + unitBytes, 2 * commitBytes));
    expectPageMarkers(impl.m_pBaseAddr, 0, commitBytes, impl.m_pageSize);
    ASSERT_NO_FATAL_FAILURE(
        writeAndReadPages(impl.m_pBaseAddr, commitBytes, commitBytes, impl.m_pageSize));
}

TEST_F(OwnedAllocatorRuntimeTest, SingleRequestFillsEntireReservation) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(reserveBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes, reserveBytes));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorRuntimeTest, EveryCommitBoundaryThroughReservationEnd) {
    for (std::size_t offset = 0; offset < reserveBytes; offset += commitBytes) {
        SCOPED_TRACE(offset);
        ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes));
        ASSERT_NO_FATAL_FAILURE(expectState(offset + commitBytes, offset + commitBytes));
        ASSERT_NO_FATAL_FAILURE(
            writeAndReadPages(impl.m_pBaseAddr, offset, commitBytes, impl.m_pageSize));
    }
    expectPageMarkers(impl.m_pBaseAddr, 0, reserveBytes, impl.m_pageSize);
}

TEST_F(OwnedAllocatorPaddedTest, FillsLastWholeElementAndCommitsFinalPage) {
    constexpr std::size_t elementCount = reserveBytes / 48;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(elementCount));
    ASSERT_NO_FATAL_FAILURE(expectState(elementCount * 48, reserveBytes));
    EXPECT_EQ(reserveBytes - impl.m_currentOffset, unitBytes);
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorFreeTest, FreeBeforeAnyAllocationPreservesInitialCommitAndAllowsReuse) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    ASSERT_NO_FATAL_FAILURE(expectState(0, commitBytes));
    ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(std::byte{0xa5}));

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0xa5}));

    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(unitBytes, commitBytes));
    EXPECT_EQ(impl.m_pBaseAddr, pOriginalBase);
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0xa5}));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorFreeTest, FreePreservesUsedBytesAndUnusedCommittedTail) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(unitBytes, commitBytes));
    ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(std::byte{0x7d}));

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0x7d}));

    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes, commitBytes));
    EXPECT_EQ(impl.m_pBaseAddr, pOriginalBase);
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0x7d}));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorFreeTest, FreeAfterGrowthAllowsReuseAndFurtherCommit) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(commitBytes / unitBytes + 1));
    ASSERT_NO_FATAL_FAILURE(expectState(commitBytes + unitBytes, 2 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(std::byte{0x3c}));

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, 2 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, 2 * commitBytes, std::byte{0x3c}));

    ASSERT_NO_FATAL_FAILURE(expectAllocation(2 * commitBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(2 * commitBytes, 2 * commitBytes));
    EXPECT_EQ(impl.m_pBaseAddr, pOriginalBase);
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, 2 * commitBytes, std::byte{0x3c}));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());

    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(2 * commitBytes + unitBytes, 3 * commitBytes));
    expectPageMarkers(impl.m_pBaseAddr, 0, 2 * commitBytes, impl.m_pageSize);
    ASSERT_NO_FATAL_FAILURE(
        writeAndReadPages(impl.m_pBaseAddr, 2 * commitBytes, commitBytes, impl.m_pageSize));
}

TEST_F(OwnedAllocatorFreeTest, FreePreservesEveryByteOfFullReservationAndAllowsFullReallocation) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(reserveBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes, reserveBytes));
    ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(std::byte{0xa5}));

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, reserveBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, reserveBytes, std::byte{0xa5}));

    ASSERT_NO_FATAL_FAILURE(expectAllocation(reserveBytes / unitBytes));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes, reserveBytes));
    EXPECT_EQ(impl.m_pBaseAddr, pOriginalBase);
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, reserveBytes, std::byte{0xa5}));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

TEST_F(OwnedAllocatorFreeTest, RepeatedFreeAndReallocateRetainsHeapAndPreservesEveryByte) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    constexpr std::size_t counts[] = {1, commitBytes / unitBytes + 1,
                                      3 * commitBytes / unitBytes + 1, 1};
    constexpr std::size_t committed[] = {commitBytes, 2 * commitBytes, 4 * commitBytes,
                                         4 * commitBytes};

    for (std::size_t cycle = 0; cycle < 4; ++cycle) {
        SCOPED_TRACE(cycle);
        ASSERT_NO_FATAL_FAILURE(expectAllocation(counts[cycle]));
        ASSERT_NO_FATAL_FAILURE(expectState(counts[cycle] * unitBytes, committed[cycle]));
        ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(static_cast<std::byte>(cycle + 1)));

        allocator.ofree();
        ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, committed[cycle]));
        ASSERT_NO_FATAL_FAILURE(
            expectBytes(impl.m_pBaseAddr, committed[cycle], static_cast<std::byte>(cycle + 1)));
    }

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, 4 * commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, 4 * commitBytes, std::byte{4}));
}

TEST_F(OwnedAllocatorPaddedFreeTest, FreePreservesPaddingAndAllowsPaddedReallocation) {
    std::byte* pOriginalBase = impl.m_pBaseAddr;
    ASSERT_NO_FATAL_FAILURE(expectAllocation(3));
    ASSERT_NO_FATAL_FAILURE(expectState(3 * 48, commitBytes));
    ASSERT_NO_FATAL_FAILURE(fillAndVerifyCommittedBytes(std::byte{0x36}));

    allocator.ofree();
    ASSERT_NO_FATAL_FAILURE(expectResetHeap(pOriginalBase, commitBytes));
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0x36}));

    ASSERT_NO_FATAL_FAILURE(expectAllocation(5));
    ASSERT_NO_FATAL_FAILURE(expectState(5 * 48, commitBytes));
    EXPECT_EQ(impl.m_pBaseAddr, pOriginalBase);
    ASSERT_NO_FATAL_FAILURE(expectBytes(impl.m_pBaseAddr, commitBytes, std::byte{0x36}));
    ASSERT_NO_FATAL_FAILURE(writeAndReadCommittedPages());
}

void exitOnCapacityFailure() {
    std::_Exit(capacityFailureExitCode);
}

TEST_F(OwnedAllocatorDeathTest, FinalSlotSucceedsThenFollowingRequestTerminates) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(reserveBytes / unitBytes - 1));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes - unitBytes, reserveBytes));
    ASSERT_NO_FATAL_FAILURE(expectAllocation(1));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes, reserveBytes));

    ASSERT_EXIT(
        {
            setFatalExitHandler(&exitOnCapacityFailure);
            static_cast<void>(allocator.oalloc(1));
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(capacityFailureExitCode),
        "FATAL.*Owned allocator reservation exhausted: .*requested_bytes=16 remaining_bytes=0");
}

TEST_F(OwnedAllocatorDeathTest, RequestCrossingReservationEndTerminates) {
    ASSERT_NO_FATAL_FAILURE(expectAllocation(reserveBytes / unitBytes - 1));
    ASSERT_NO_FATAL_FAILURE(expectState(reserveBytes - unitBytes, reserveBytes));

    ASSERT_EXIT(
        {
            setFatalExitHandler(&exitOnCapacityFailure);
            static_cast<void>(allocator.oalloc(2));
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(capacityFailureExitCode),
        "FATAL.*Owned allocator reservation exhausted: .*requested_bytes=32 remaining_bytes=16");
}

#ifndef NDEBUG
TEST_F(OwnedAllocatorDeathTest, AllocationByteCountOverflowTerminates) {
    constexpr std::size_t count = std::size_t{1} << 60;

    ASSERT_DEATH(
        { static_cast<void>(allocator.oalloc(count)); },
        "Check failed: expression=allocCount <= m_reservedSize / m_allocSize");
}

#endif
} // namespace
} // namespace naru::tests::allocator
