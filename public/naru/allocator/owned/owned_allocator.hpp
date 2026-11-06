#pragma once
#include <cstddef>

#include "naru/support/static_pimpl.hpp"

namespace naru::allocator::owned {

class OwnedAllocator final
{
public:
    // ==================== lifecycle ====================
    OwnedAllocator() = delete;
    ~OwnedAllocator();
    OwnedAllocator(const OwnedAllocator&) = delete;
    OwnedAllocator& operator=(const OwnedAllocator&) = delete;
    OwnedAllocator(OwnedAllocator&& other);
    OwnedAllocator& operator=(OwnedAllocator&&) = delete;

    // ==================== creation info ====================
    struct CreateInfo {
        std::size_t elementSize = 0;
    };

    // ==================== creation ====================
    [[nodiscard]] static OwnedAllocator createVal(const CreateInfo& info);

    // ==================== allocation ====================
    [[nodiscard]] std::byte* oalloc(std::size_t allocCount);

    // ==================== reset ====================
    void ofree();

    std::size_t getAllocSizeUnit() const;
    std::size_t getMemorySize() const;

private:
    // ==================== initialization ====================
    struct InitInfo;
    explicit OwnedAllocator(const InitInfo& info);

    // ==================== implementation ====================
    static constexpr std::size_t IMPL_SIZE = 48;
    static constexpr std::size_t IMPL_ALIGNMENT = 8;

    struct Impl;
    StaticPimpl<Impl, IMPL_SIZE, IMPL_ALIGNMENT> m_impl;
};

} // namespace naru::allocator::owned
