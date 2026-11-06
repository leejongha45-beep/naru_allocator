#ifdef _WIN32

#include <cstddef>
#include <limits>
#include <windows.h>

#include "naru/support/log.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::allocator::platform {
namespace {
enum class MemoryState { FREE, RESERVED, COMMITTED };

[[nodiscard, maybe_unused]] MemoryState queryMemoryState(const void* pPageAddr) {
    MEMORY_BASIC_INFORMATION memInfo{};
    if (VirtualQuery(pPageAddr, &memInfo, sizeof(memInfo)) == 0) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL, "VirtualQuery failed: base={} os_error={}", pPageAddr, error);
        fatalExit();
    }

    switch (memInfo.State) {
    case MEM_RESERVE:
        return MemoryState::RESERVED;
    case MEM_COMMIT:
        return MemoryState::COMMITTED;
    default:
        return MemoryState::FREE;
    }
}
} // namespace

std::size_t getCommonPageSize() {
    SYSTEM_INFO sSysInfo;
    GetSystemInfo(&sSysInfo);

    return sSysInfo.dwPageSize;
}

void* reserveCommonMemory(const VmMemoryInfo& pageInfo) {
    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pageInfo.reservePageCount != 0);
    NARU_CHECK(pageInfo.reservePageCount <= maxSize / pageInfo.pageSize);

    std::size_t reserveSize = pageInfo.reservePageCount * pageInfo.pageSize;
    void* pBaseAddr = VirtualAlloc(nullptr, reserveSize, MEM_RESERVE, PAGE_NOACCESS);
    if (pBaseAddr == nullptr) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL, "VirtualAlloc reserve failed: requested_bytes={} os_error={}", reserveSize,
                  error);
        fatalExit();
    }

    return pBaseAddr;
}

void commitCommonMemory(void* pStartAddr, const VmMemoryInfo& pageInfo) {
    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pStartAddr != nullptr);
    NARU_CHECK(pageInfo.commitPageCount != 0);
    NARU_CHECK(pageInfo.commitPageCount <= maxSize / pageInfo.pageSize);
    NARU_CHECK(queryMemoryState(pStartAddr) == MemoryState::RESERVED);

    const std::size_t commitSize = pageInfo.commitPageCount * pageInfo.pageSize;
    void* pCommitted = VirtualAlloc(pStartAddr, commitSize, MEM_COMMIT, PAGE_READWRITE);
    if (pCommitted == nullptr) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL,
                  "VirtualAlloc commit failed: base={} requested_bytes={} "
                  "os_error={}",
                  pStartAddr, commitSize, error);
        fatalExit();
    }
}

void decommitCommonMemory(void* pPageAddr, const VmMemoryInfo& pageInfo) {
    std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pPageAddr != nullptr);
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pageInfo.reservePageCount != 0);
    NARU_CHECK(pageInfo.reservePageCount <= maxSize / pageInfo.pageSize);
    NARU_CHECK(queryMemoryState(pPageAddr) == MemoryState::COMMITTED);

    std::size_t decommitSize = pageInfo.reservePageCount * pageInfo.pageSize;
    bool decommitResult = VirtualFree(pPageAddr, decommitSize, MEM_DECOMMIT);
    if (decommitResult == false) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL,
                  "VirtualFree decommit failed: base={} requested_bytes={} "
                  "os_error={}",
                  pPageAddr, decommitSize, error);
        fatalExit();
    }
}

void releasePostCommitMemory(void* pPageAddr, const VmMemoryInfo&) {
    NARU_CHECK(pPageAddr != nullptr);
    NARU_CHECK(queryMemoryState(pPageAddr) != MemoryState::FREE &&
                queryMemoryState(pPageAddr) != MemoryState::COMMITTED);

    bool releaseResult = VirtualFree(pPageAddr, 0, MEM_RELEASE);
    if (releaseResult == false) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL, "VirtualFree release failed: base={} os_error={}", pPageAddr, error);
        fatalExit();
    }
}

void releaseEntireMemory(void* pPageAddr, const VmMemoryInfo&) {
    NARU_CHECK(pPageAddr != nullptr);
    NARU_CHECK(queryMemoryState(pPageAddr) != MemoryState::FREE);

    bool releaseResult = VirtualFree(pPageAddr, 0, MEM_RELEASE);
    if (releaseResult == false) {
        const DWORD error = GetLastError();
        NARU_LOG(FATAL, "VirtualFree release failed: base={} os_error={}", pPageAddr, error);
        fatalExit();
    }
}

} // namespace naru::allocator::platform

#endif
