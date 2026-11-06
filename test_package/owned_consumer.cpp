#include <cstddef>
#include <memory>
#include <naru/allocator/owned/owned_allocator.hpp>
#include <type_traits>
#include <utility>

static_assert(sizeof(void*) == 8);
static_assert(__cplusplus >= 202302L);
#ifdef __cpp_exceptions
#error "The installed package must propagate the no-exceptions policy."
#elifdef __EXCEPTIONS
#error "The installed package must propagate the no-exceptions policy."
#elifdef _CPPUNWIND
#error "The installed package must propagate the no-exceptions policy."
#endif
#if __has_include("group/group_allocator_impl.hpp") || __has_include("group/memory_config.hpp")
#error "The installed package must not expose private allocator headers."
#endif

#if __has_include("owned/owned_allocator_impl.hpp") ||                                             \
                  __has_include("owned/memory_config.hpp") ||                                      \
                                __has_include("platform/memory.hpp") ||                            \
                                              __has_include("platform/memory_types.hpp")
#error "The installed package must not expose private configuration or platform include paths."
#endif

#if __has_include("naru/allocator/owned/memory_config.hpp")
#error "OwnedAllocator configuration must remain private to the class."
#endif

#if __has_include("naru/allocator/platform/memory.hpp") ||                                        \
                  __has_include("naru/allocator/platform/memory_types.hpp")
#error "The installed package must not expose platform headers."
#endif

namespace {
struct Payload {
    std::byte bytes[37];
};
} // namespace

int main() {
    using Allocator = naru::allocator::owned::OwnedAllocator;
    static_assert(std::is_same_v<decltype(std::declval<Allocator&>().oalloc(1)), std::byte*>);
    static_assert(sizeof(Allocator) == 48);
    static_assert(alignof(Allocator) == 8);
    static_assert(!std::is_copy_constructible_v<Allocator>);
    static_assert(!std::is_copy_assignable_v<Allocator>);
    static_assert(std::is_move_constructible_v<Allocator>);
    static_assert(!std::is_move_assignable_v<Allocator>);

    Allocator allocator = Allocator::createVal({.elementSize = sizeof(Payload)});
    Payload* pFirst = reinterpret_cast<Payload*>(allocator.oalloc(8'193));
    std::construct_at(pFirst);
    pFirst->bytes[36] = std::byte{0xa5};

    Payload* pNext = reinterpret_cast<Payload*>(allocator.oalloc(1));
    if (reinterpret_cast<std::byte*>(pNext) != reinterpret_cast<std::byte*>(pFirst) + 8'193 * 48 ||
        pFirst->bytes[36] != std::byte{0xa5}) {
        return 1;
    }

    Allocator movedAllocator(std::move(allocator));

    movedAllocator.ofree();
    Payload* pReused = reinterpret_cast<Payload*>(movedAllocator.oalloc(16'384));
    if (pReused != pFirst || pReused->bytes[36] != std::byte{0xa5}) {
        return 2;
    }

    movedAllocator.ofree();
    return 0;
}
