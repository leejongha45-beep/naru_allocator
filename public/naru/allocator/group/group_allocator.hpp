#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "naru/support/static_pimpl.hpp"

namespace naru::allocator::group {
class GroupAllocator final
{
    // ==================== singleton ====================
public:
    GroupAllocator(const GroupAllocator&) = delete;
    GroupAllocator& operator=(const GroupAllocator&) = delete;
    GroupAllocator(GroupAllocator&&) = delete;
    GroupAllocator& operator=(GroupAllocator&&) = delete;

    static GroupAllocator& getInstance();

    // ==================== implementation ====================
private:
#ifdef _WIN32
    static constexpr std::size_t IMPL_SIZE = 65'536;
#elifdef __linux__
    static constexpr std::size_t IMPL_SIZE = 55'296;
#endif
    static constexpr std::size_t IMPL_ALIGNMENT = 8;

    struct Impl;
    StaticPimpl<Impl, IMPL_SIZE, IMPL_ALIGNMENT> m_impl;

    GroupAllocator();
    ~GroupAllocator();

    // ==================== group allocator ====================
public:
    static constexpr std::size_t MAX_SUPPORTED_ALIGNMENT = 4'096;

    struct TypeInfo final {
        const std::size_t size;

        template <typename T>
        consteval TypeInfo(std::type_identity<T>) : size(sizeof(T)) {
            static_assert(alignof(T) <= MAX_SUPPORTED_ALIGNMENT,
                          "GroupAllocator does not support type alignment above 4096 bytes.");
        }
    };

    [[nodiscard]] std::byte* galloc(std::uint8_t groupId, TypeInfo typeInfo);
    void greserve(std::uint8_t groupId, std::size_t typeSize, std::size_t segCount);
    void gfree(std::uint8_t groupId, std::size_t typeSize, std::byte* pTargetAddr);
    void gclean(std::uint8_t groupId);
};
} // namespace naru::allocator::group
