#ifdef __linux__

#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

#include "naru/support/log.hpp"
#include "platform/memory.hpp"
#include "platform/memory_types.hpp"

namespace naru::allocator::platform {
namespace {
enum class MemoryState { FREE, RESERVED, COMMITTED };

[[nodiscard, maybe_unused]] MemoryState queryMemoryState(const void* pPageAddr) {
    std::FILE* pMaps = std::fopen("/proc/self/maps", "r");
    if (pMaps == nullptr) {
        const int error = errno;
        NARU_LOG(FATAL, "Opening /proc/self/maps failed: os_error={}", error);
        fatalExit();
    }

    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(pPageAddr);
    char* pLine = nullptr;
    std::size_t lineCapacity = 0;
    MemoryState memoryState = MemoryState::FREE;

    while (true) {
        if (getline(&pLine, &lineCapacity, pMaps) == -1) {
            if (std::feof(pMaps) == 0) {
                const int error = errno;
                NARU_LOG(FATAL, "Reading /proc/self/maps failed: os_error={}", error);
                fatalExit();
            }
            break;
        }

        std::uintptr_t regionStart = 0;
        std::uintptr_t regionEnd = 0;
        char permissions[5]{};
        if (std::sscanf(pLine, "%" SCNxPTR "-%" SCNxPTR " %4s", &regionStart, &regionEnd,
                        permissions) != 3) {
            NARU_LOG(FATAL, "Parsing /proc/self/maps failed: base={}", pPageAddr);
            fatalExit();
        }

        if (addr < regionStart || addr >= regionEnd) {
            continue;
        }

        // Allocator contract: PROT_NONE is reserved; read/write is committed.
        if (permissions[0] == '-' && permissions[1] == '-' && permissions[2] == '-') {
            memoryState = MemoryState::RESERVED;
        } else if (permissions[0] == 'r' && permissions[1] == 'w' && permissions[2] == '-') {
            memoryState = MemoryState::COMMITTED;
        } else {
            NARU_LOG(FATAL, "Unexpected allocator memory protection: base={} permissions={}",
                      pPageAddr, permissions);
            fatalExit();
        }
        break;
    }

    std::free(pLine);
    if (std::fclose(pMaps) != 0) {
        const int error = errno;
        NARU_LOG(FATAL, "Closing /proc/self/maps failed: os_error={}", error);
        fatalExit();
    }
    return memoryState;
}
} // namespace

std::size_t getCommonPageSize() {
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize == -1) {
        const int error = errno;
        NARU_LOG(FATAL, "sysconf page size query failed: os_error={}", error);
        fatalExit();
    }

    return static_cast<std::size_t>(pageSize);
}

void* reserveCommonMemory(const VmMemoryInfo& pageInfo) {
    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pageInfo.reservePageCount != 0);
    NARU_CHECK(pageInfo.reservePageCount <= maxSize / pageInfo.pageSize);

    std::size_t reserveSize = pageInfo.reservePageCount * pageInfo.pageSize;
    void* pBaseAddr = mmap(nullptr, reserveSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pBaseAddr == MAP_FAILED) {
        const int error = errno;
        NARU_LOG(FATAL, "mmap reserve failed: requested_bytes={} os_error={}", reserveSize, error);
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
    if (mprotect(pStartAddr, commitSize, PROT_READ | PROT_WRITE) == -1) {
        const int error = errno;
        NARU_LOG(FATAL, "mprotect commit failed: base={} requested_bytes={} os_error={}",
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

    const std::size_t decommitSize = pageInfo.reservePageCount * pageInfo.pageSize;
    if (mprotect(pPageAddr, decommitSize, PROT_NONE) == -1) {
        const int error = errno;
        NARU_LOG(FATAL, "mprotect decommit failed: base={} requested_bytes={} os_error={}",
                  pPageAddr, decommitSize, error);
        fatalExit();
    }
}

void releasePostCommitMemory(void* pPageAddr, const VmMemoryInfo& pageInfo) {
    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pPageAddr != nullptr);
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pageInfo.reservePageCount != 0);
    NARU_CHECK(pageInfo.reservePageCount <= maxSize / pageInfo.pageSize);
    NARU_CHECK(queryMemoryState(pPageAddr) == MemoryState::RESERVED);

    const std::size_t releaseSize = pageInfo.reservePageCount * pageInfo.pageSize;
    if (munmap(pPageAddr, releaseSize) == -1) {
        const int error = errno;
        NARU_LOG(FATAL,
                  "munmap post-commit release failed: base={} requested_bytes={} os_error={}",
                  pPageAddr, releaseSize, error);
        fatalExit();
    }
}

void releaseEntireMemory(void* pPageAddr, const VmMemoryInfo& pageInfo) {
    constexpr std::size_t maxSize = (std::numeric_limits<std::size_t>::max)();
    NARU_CHECK(pPageAddr != nullptr);
    NARU_CHECK(pageInfo.pageSize != 0);
    NARU_CHECK(pageInfo.reservePageCount != 0);
    NARU_CHECK(pageInfo.reservePageCount <= maxSize / pageInfo.pageSize);
    NARU_CHECK(queryMemoryState(pPageAddr) != MemoryState::FREE);

    const std::size_t releaseSize = pageInfo.reservePageCount * pageInfo.pageSize;
    if (munmap(pPageAddr, releaseSize) == -1) {
        const int error = errno;
        NARU_LOG(FATAL, "munmap entire release failed: base={} requested_bytes={} os_error={}",
                  pPageAddr, releaseSize, error);
        fatalExit();
    }
}

} // namespace naru::allocator::platform

#endif // __linux__
