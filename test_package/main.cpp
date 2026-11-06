#include <cstddef>
#include <cstdint>
#include <memory>
#include <naru/allocator.hpp>
#include <new>
#include <type_traits>

static_assert(sizeof(void*) == 8);
static_assert(__cplusplus >= 202302L);
#ifdef __cpp_exceptions
#error "The installed package must propagate the no-exceptions policy."
#elifdef __EXCEPTIONS
#error "The installed package must propagate the no-exceptions policy."
#elifdef _CPPUNWIND
#error "The installed package must propagate the no-exceptions policy."
#endif

namespace {
struct alignas(16) Payload {
    std::uint64_t value = 42;
};
} // namespace

int main() {
    constexpr std::uint8_t groupId = 0;
    naru::allocator::group::GroupAllocator& allocator =
        naru::allocator::group::GroupAllocator::getInstance();
    Payload* pPayload =
        new (static_cast<void*>(allocator.galloc(groupId, std::type_identity<Payload>{}))) Payload;
    const bool valid = pPayload != nullptr && pPayload->value == 42;
    std::destroy_at(pPayload);
    allocator.gfree(groupId, sizeof(*pPayload), reinterpret_cast<std::byte*>(pPayload));
    allocator.gclean(groupId);
    return valid ? 0 : 1;
}
