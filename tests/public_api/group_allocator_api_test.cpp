#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>

#include "naru/allocator/group/group_allocator.hpp"

#if __has_include("group/group_allocator_impl.hpp") ||                                             \
                  __has_include(                                                                   \
                      "group/memory_segment_list_storage.hpp") ||                                  \
                      __has_include(                                                               \
                          "group/memory_segment_list.hpp") ||                                      \
                          __has_include(                                                           \
                              "platform/memory.hpp") ||                                            \
                              __has_include(                                                       \
                                  "platform/memory_types.hpp") ||                                  \
                                  __has_include("memory/allocator/group_allocator.hpp") ||         \
                                                __has_include("memory/memory.hpp")
#error "Allocator implementation headers must not be exposed to consumers."
#endif

#ifdef NARU_TRY
#error "Allocator public headers must not include the private retry dependency."
#endif

namespace naru::tests::allocator {
namespace {
using GroupAllocator = ::naru::allocator::group::GroupAllocator;
using GroupId = std::uint8_t;

static_assert(!std::is_default_constructible_v<GroupAllocator::TypeInfo>);
static_assert(!std::is_constructible_v<GroupAllocator::TypeInfo, std::size_t>);
static_assert(std::is_same_v<decltype(&GroupAllocator::galloc),
                             std::byte* (GroupAllocator::*)(GroupId, GroupAllocator::TypeInfo)>);
static_assert(std::is_same_v<decltype(&GroupAllocator::greserve),
                             void (GroupAllocator::*)(GroupId, std::size_t, std::size_t)>);
static_assert(std::is_same_v<decltype(&GroupAllocator::gfree),
                             void (GroupAllocator::*)(GroupId, std::size_t, std::byte*)>);
static_assert(std::is_same_v<decltype(&GroupAllocator::gclean), void (GroupAllocator::*)(GroupId)>);

struct alignas(GroupAllocator::MAX_SUPPORTED_ALIGNMENT) PageAlignedPayload {
    std::byte bytes[GroupAllocator::MAX_SUPPORTED_ALIGNMENT * 2];
};

constexpr GroupAllocator::TypeInfo pageTypeInfo(std::type_identity<PageAlignedPayload>{});
static_assert(pageTypeInfo.size == sizeof(PageAlignedPayload));
static_assert(pageTypeInfo.size > GroupAllocator::MAX_SUPPORTED_ALIGNMENT);

TEST(AllocatorTypeInfoTest, SupportedAlignmentLimitAllocatesAndReusesLargerObject) {
    constexpr GroupId groupId = static_cast<GroupId>(2);
    GroupAllocator& allocator = GroupAllocator::getInstance();
    std::byte* pStorage = allocator.galloc(groupId, pageTypeInfo);
    ASSERT_NE(pStorage, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pStorage) % alignof(PageAlignedPayload), 0);

    PageAlignedPayload* pObject =
        std::construct_at(reinterpret_cast<PageAlignedPayload*>(pStorage));
    pObject->bytes[0] = std::byte{0x12};
    pObject->bytes[sizeof(PageAlignedPayload) - 1] = std::byte{0x34};
    EXPECT_EQ(pObject->bytes[0], std::byte{0x12});
    EXPECT_EQ(pObject->bytes[sizeof(PageAlignedPayload) - 1], std::byte{0x34});

    std::destroy_at(pObject);
    allocator.gfree(groupId, pageTypeInfo.size, pStorage);

    std::byte* pReused = allocator.galloc(groupId, std::type_identity<PageAlignedPayload>{});
    EXPECT_EQ(pReused, pStorage);
    allocator.gfree(groupId, pageTypeInfo.size, pReused);
}

TEST(AllocatorPublicApiConcurrencyTest, ConcurrentFirstAccessUsesSameInstance) {
    using GroupAllocator = ::naru::allocator::group::GroupAllocator;
    constexpr std::size_t workerCount = 8;
    constexpr GroupId groupId = static_cast<GroupId>(3);
    std::array<GroupAllocator*, workerCount> pAllocators{};
    std::array<std::uint64_t*, workerCount> pValues{};
    std::array<std::jthread, workerCount> workers{};
    std::barrier<> start(static_cast<std::ptrdiff_t>(workerCount));

    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            start.arrive_and_wait();
            GroupAllocator& allocator = GroupAllocator::getInstance();
            pAllocators[workerIndex] = &allocator;
            pValues[workerIndex] = new (static_cast<void*>(
                allocator.galloc(groupId, std::type_identity<std::uint64_t>{}))) std::uint64_t;
            *pValues[workerIndex] = workerIndex + 1;
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    ASSERT_NE(pAllocators[0], nullptr);
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        EXPECT_EQ(pAllocators[workerIndex], pAllocators[0]);
        ASSERT_NE(pValues[workerIndex], nullptr);
        EXPECT_EQ(*pValues[workerIndex], workerIndex + 1);
        for (std::size_t otherIndex = 0; otherIndex < workerIndex; ++otherIndex) {
            EXPECT_NE(pValues[workerIndex], pValues[otherIndex]);
        }
    }
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        std::destroy_at(pValues[workerIndex]);
        pAllocators[workerIndex]->gfree(groupId, sizeof(*pValues[workerIndex]),
                                        reinterpret_cast<std::byte*>(pValues[workerIndex]));
    }
}

struct alignas(16) PublicPayload {
    inline static std::size_t constructionCount = 0;
    inline static std::size_t destructionCount = 0;
    std::uint64_t value = 42;

    PublicPayload() {
        ++constructionCount;
    }

    ~PublicPayload() {
        ++destructionCount;
    }
};

class AllocatorPublicApiTest : public ::testing::Test
{
protected:
    void SetUp() override {
        PublicPayload::constructionCount = 0;
        PublicPayload::destructionCount = 0;
    }

    ::naru::allocator::group::GroupAllocator& allocator =
        ::naru::allocator::group::GroupAllocator::getInstance();
};

TEST_F(AllocatorPublicApiTest, CallerConstructsDestroysAndReusesWithoutInitialization) {
    constexpr GroupId groupId = static_cast<GroupId>(1);
    PublicPayload* pObject =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<PublicPayload>{})))
            PublicPayload;
    ASSERT_NE(pObject, nullptr);
    EXPECT_EQ(pObject->value, 42);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pObject) % alignof(PublicPayload), 0);
    EXPECT_EQ(PublicPayload::constructionCount, 1);

    std::destroy_at(pObject);
    allocator.gfree(groupId, sizeof(*pObject), reinterpret_cast<std::byte*>(pObject));
    EXPECT_EQ(PublicPayload::destructionCount, 1);

    PublicPayload* pReused =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<PublicPayload>{})))
            PublicPayload;
    EXPECT_EQ(pReused, pObject);
    std::destroy_at(pReused);
    allocator.gfree(groupId, sizeof(*pReused), reinterpret_cast<std::byte*>(pReused));
    EXPECT_EQ(PublicPayload::constructionCount, 2);
    EXPECT_EQ(PublicPayload::destructionCount, 2);
}

TEST_F(AllocatorPublicApiTest, ReservesAndCleansTaskGroupWithoutInitialization) {
    constexpr GroupId groupId = 0;
    allocator.greserve(groupId, sizeof(PublicPayload), 3);
    EXPECT_EQ(PublicPayload::constructionCount, 0);

    std::array<PublicPayload*, 3> pObjects{};
    for (PublicPayload*& pObject : pObjects) {
        pObject =
            new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<PublicPayload>{})))
                PublicPayload;
        ASSERT_NE(pObject, nullptr);
    }
    EXPECT_NE(pObjects[0], pObjects[1]);
    EXPECT_NE(pObjects[1], pObjects[2]);
    EXPECT_NE(pObjects[0], pObjects[2]);

    for (PublicPayload* pObject : pObjects) {
        std::destroy_at(pObject);
        allocator.gfree(groupId, sizeof(*pObject), reinterpret_cast<std::byte*>(pObject));
    }
    allocator.gclean(groupId);
    EXPECT_EQ(PublicPayload::destructionCount, 3);

    PublicPayload* pReused =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<PublicPayload>{})))
            PublicPayload;
    EXPECT_TRUE(pReused == pObjects[0] || pReused == pObjects[1] || pReused == pObjects[2]);
    std::destroy_at(pReused);
    allocator.gfree(groupId, sizeof(*pReused), reinterpret_cast<std::byte*>(pReused));
    EXPECT_EQ(PublicPayload::constructionCount, 4);
    EXPECT_EQ(PublicPayload::destructionCount, 4);
}
} // namespace
} // namespace naru::tests::allocator
