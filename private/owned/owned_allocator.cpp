#include "naru/allocator/owned/owned_allocator.hpp"

#include <cstddef>
#include <utility>

#include "naru/support/log.hpp"
#include "naru/support/static_pimpl.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::allocator::owned {

struct OwnedAllocator::InitInfo {
    std::byte* pBaseAddr = nullptr;
    std::size_t pageSize = 0;
    std::size_t reserveSize = 0;
    std::size_t commitSize = 0;
    std::size_t allocUnit = 0;
};

struct OwnedAllocator::Impl {
public:
    // ==================== lifecycle ====================
    Impl() = delete;
    ~Impl() {
        if (m_pBaseAddr == nullptr) {
            return;
        }

        NARU_CHECK(m_pageSize != 0);
        NARU_CHECK(m_reservedSize != 0);
        NARU_CHECK(m_reservedSize % m_pageSize == 0);

        const platform::VmMemoryInfo vmInfo{
            .pageSize = m_pageSize,
            .reservePageCount = m_reservedSize / m_pageSize,
            .commitPageCount = 0,
        };
        platform::releaseEntireMemory(m_pBaseAddr, vmInfo);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&& other)
        : m_pBaseAddr(other.m_pBaseAddr), m_currentOffset(other.m_currentOffset),
          m_pageSize(other.m_pageSize), m_reservedSize(other.m_reservedSize),
          m_committedSize(other.m_committedSize), m_allocSize(other.m_allocSize) {
        other.m_pBaseAddr = nullptr;
        other.m_currentOffset = 0;
        other.m_pageSize = 0;
        other.m_reservedSize = 0;
        other.m_committedSize = 0;
        other.m_allocSize = 0;
    }
    Impl& operator=(Impl&&) = delete;

    // ==================== creation ====================
    [[nodiscard]] static OwnedAllocator createVal(const CreateInfo& createInfo) {
        const std::size_t elementSize = createInfo.elementSize;
        NARU_CHECK(elementSize != 0);
        const std::size_t pageSize = platform::getCommonPageSize();
        NARU_CHECK(pageSize != 0);
        const std::size_t reserveCount = OWNED_RESERVE_SIZE / pageSize;
        const std::size_t commitCount = (OWNED_COMMIT_SIZE + pageSize - 1) / pageSize;

        const platform::VmMemoryInfo vmInfo{
            .pageSize = pageSize,
            .reservePageCount = reserveCount,
            .commitPageCount = commitCount,
        };

        std::byte* pBaseAddr = static_cast<std::byte*>(platform::reserveCommonMemory(vmInfo));
        platform::commitCommonMemory(pBaseAddr, vmInfo);

        const std::size_t allocUnitCount =
            (elementSize + OWNED_ALLOC_SIZE_UNIT - 1) / OWNED_ALLOC_SIZE_UNIT;
        const std::size_t allocUnit = allocUnitCount * OWNED_ALLOC_SIZE_UNIT;

        const InitInfo info{
            .pBaseAddr = pBaseAddr,
            .pageSize = pageSize,
            .reserveSize = reserveCount * pageSize,
            .commitSize = commitCount * pageSize,
            .allocUnit = allocUnit,
        };
        NARU_LOG(DEBUG,
                  "Owned allocator created: element_bytes={} heap_base={} "
                  "reserved_bytes={} committed_bytes={}",
                  elementSize, static_cast<void*>(info.pBaseAddr), info.reserveSize,
                  info.commitSize);

        return OwnedAllocator(info);
    }

    // ==================== allocation ====================
    [[nodiscard]] std::byte* oalloc(std::size_t allocCount) {
        NARU_CHECK(allocCount != 0);
        NARU_CHECK(m_pBaseAddr != nullptr);
        NARU_CHECK(m_allocSize % OWNED_ALLOC_SIZE_UNIT == 0);
        NARU_CHECK(allocCount <= m_reservedSize / m_allocSize);
        NARU_CHECK(m_reservedSize >= m_committedSize);

        if (m_reservedSize - m_currentOffset < allocCount * m_allocSize) {
            NARU_LOG(FATAL,
                      "Owned allocator reservation exhausted: heap_base={} alloc_count={} "
                      "alloc_unit_bytes={} requested_bytes={} remaining_bytes={} "
                      "reserved_bytes={} committed_bytes={} current_offset={}",
                      static_cast<void*>(m_pBaseAddr), allocCount, m_allocSize,
                      allocCount * m_allocSize, m_reservedSize - m_currentOffset, m_reservedSize,
                      m_committedSize, m_currentOffset);
            fatalExit();
        }

        if (m_committedSize - m_currentOffset < m_allocSize * allocCount) {
            const std::size_t requiredCommitBytes =
                allocCount * m_allocSize - (m_committedSize - m_currentOffset);
            const std::size_t commitBlockCount =
                (requiredCommitBytes + OWNED_COMMIT_SIZE - 1) / OWNED_COMMIT_SIZE;
            const std::size_t commitPageCount = commitBlockCount * OWNED_COMMIT_SIZE / m_pageSize;
            platform::commitCommonMemory(m_pBaseAddr + m_committedSize,
                                         platform::VmMemoryInfo{
                                             .pageSize = m_pageSize,
                                             .reservePageCount = 0,
                                             .commitPageCount = commitPageCount,
                                         });
            m_committedSize += m_pageSize * commitPageCount;
        }

        std::byte* pAllocation = m_pBaseAddr + m_currentOffset;
        m_currentOffset = m_currentOffset + m_allocSize * allocCount;
        return pAllocation;
    }

    // ==================== reset ====================
    void ofree() {
        NARU_CHECK(m_pBaseAddr != nullptr);

        m_currentOffset = 0;
    }

    // ==================== initialization ====================
    explicit Impl(const InitInfo& info)
        : m_pBaseAddr(info.pBaseAddr), m_pageSize(info.pageSize), m_reservedSize(info.reserveSize),
          m_committedSize(info.commitSize), m_allocSize(info.allocUnit) {
    }

    // ==================== configuration ====================
    static constexpr std::size_t OWNED_RESERVE_SIZE = 268'435'456; // 256MiB
    static constexpr std::size_t OWNED_COMMIT_SIZE = 65'536;       // 64KiB
    static constexpr std::size_t OWNED_ALLOC_SIZE_UNIT = 16;       // 16B

    // ==================== state ====================
    std::byte* m_pBaseAddr = nullptr;
    std::size_t m_currentOffset = 0;
    std::size_t m_pageSize = 0;
    std::size_t m_reservedSize = 0;
    std::size_t m_committedSize = 0;
    std::size_t m_allocSize = 0;
};

// ==================== public API ====================
OwnedAllocator::OwnedAllocator(const InitInfo& info) : m_impl(info) {
}

OwnedAllocator::~OwnedAllocator() = default;

OwnedAllocator::OwnedAllocator(OwnedAllocator&& other) : m_impl(std::move(*other.m_impl)) {
}

OwnedAllocator OwnedAllocator::createVal(const CreateInfo& info) {
    return Impl::createVal(info);
}

std::byte* OwnedAllocator::oalloc(std::size_t allocCount) {
    Impl& impl = *m_impl;
    return impl.oalloc(allocCount);
}

void OwnedAllocator::ofree() {
    Impl& impl = *m_impl;
    impl.ofree();
}

std::size_t OwnedAllocator::getAllocSizeUnit() const {
    const Impl& impl = *m_impl;

    return impl.m_allocSize;
}

std::size_t OwnedAllocator::getMemorySize() const {
    const Impl& impl = *m_impl;

    return impl.m_reservedSize;
}

} // namespace naru::allocator::owned
