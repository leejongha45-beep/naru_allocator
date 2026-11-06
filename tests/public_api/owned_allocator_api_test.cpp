#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "naru/allocator/owned/owned_allocator.hpp"

#if __has_include("group/group_allocator_impl.hpp") || __has_include("group/memory_config.hpp")
#error "OwnedAllocator must be usable without private allocator headers."
#endif

#if __has_include("owned/owned_allocator_impl.hpp") ||                                             \
                  __has_include("owned/memory_config.hpp") ||                                      \
                                __has_include("platform/memory.hpp") ||                            \
                                              __has_include("platform/memory_types.hpp")
#error "OwnedAllocator must not require private configuration or platform include paths."
#endif

#if __has_include("naru/allocator/owned/memory_config.hpp")
#error "OwnedAllocator configuration must remain private to the class."
#endif

#if __has_include("naru/allocator/platform/memory.hpp") ||                                        \
                  __has_include("naru/allocator/platform/memory_types.hpp")
#error "Platform headers must not be exposed by the public include directory."
#endif

namespace naru::tests::allocator {
namespace {
struct PublicOwnedPayload {
    std::byte bytes[37];
};

TEST(OwnedAllocatorPublicApiTest, CreatesGrowsAndReusesWithoutSubsystem) {
    using Allocator = ::naru::allocator::owned::OwnedAllocator;

    static_assert(std::is_same_v<decltype(std::declval<Allocator&>().oalloc(1)), std::byte*>);
    static_assert(sizeof(Allocator) == 48);
    static_assert(alignof(Allocator) == 8);
    static_assert(!std::is_copy_constructible_v<Allocator>);
    static_assert(!std::is_copy_assignable_v<Allocator>);
    static_assert(std::is_move_constructible_v<Allocator>);
    static_assert(!std::is_move_assignable_v<Allocator>);

    Allocator smallAllocator = Allocator::createVal({.elementSize = sizeof(std::uint64_t)});
    Allocator paddedAllocator = Allocator::createVal({.elementSize = sizeof(PublicOwnedPayload)});

    std::uint64_t* pSmall = reinterpret_cast<std::uint64_t*>(smallAllocator.oalloc(4'096));
    std::construct_at(pSmall, std::uint64_t{42});
    std::uint64_t* pNextSmall = reinterpret_cast<std::uint64_t*>(smallAllocator.oalloc(1));
    EXPECT_EQ(reinterpret_cast<std::byte*>(pNextSmall),
              reinterpret_cast<std::byte*>(pSmall) + 4'096 * 16);
    EXPECT_EQ(*pSmall, 42);

    PublicOwnedPayload* pPadded =
        reinterpret_cast<PublicOwnedPayload*>(paddedAllocator.oalloc(8'193));
    std::construct_at(pPadded);
    pPadded->bytes[36] = std::byte{0xa5};
    PublicOwnedPayload* pNextPadded =
        reinterpret_cast<PublicOwnedPayload*>(paddedAllocator.oalloc(1));
    EXPECT_EQ(reinterpret_cast<std::byte*>(pNextPadded),
              reinterpret_cast<std::byte*>(pPadded) + 8'193 * 48);
    EXPECT_EQ(pPadded->bytes[36], std::byte{0xa5});

    Allocator movedAllocator(std::move(smallAllocator));

    movedAllocator.ofree();
    paddedAllocator.ofree();

    std::uint64_t* pReusedSmall = reinterpret_cast<std::uint64_t*>(movedAllocator.oalloc(8'193));
    PublicOwnedPayload* pReusedPadded =
        reinterpret_cast<PublicOwnedPayload*>(paddedAllocator.oalloc(16'384));
    EXPECT_EQ(pReusedSmall, pSmall);
    EXPECT_EQ(pReusedPadded, pPadded);
    EXPECT_EQ(*pReusedSmall, 42);
    EXPECT_EQ(pReusedPadded->bytes[36], std::byte{0xa5});

    movedAllocator.ofree();
    paddedAllocator.ofree();
}
} // namespace
} // namespace naru::tests::allocator
