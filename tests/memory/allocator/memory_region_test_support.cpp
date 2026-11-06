#include "memory_region_test_support.hpp"

#include <cstdint>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#elifdef __linux__
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#endif

namespace naru::tests::allocator {
MemoryRegion queryMemoryRegion(const void* pAddress) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pAddress, &info, sizeof(info)) != sizeof(info)) {
        ADD_FAILURE() << "VirtualQuery failed: " << GetLastError();
        return {};
    }

    MemoryRegionState state = MemoryRegionState::OTHER;
    switch (info.State) {
    case MEM_FREE:
        state = MemoryRegionState::FREE;
        break;
    case MEM_RESERVE:
        state = MemoryRegionState::RESERVED;
        break;
    case MEM_COMMIT:
        state = MemoryRegionState::COMMITTED;
        break;
    }
    return {info.BaseAddress, info.RegionSize, state};
#elifdef __linux__
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        ADD_FAILURE() << "Cannot open /proc/self/maps";
        return {};
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pAddress);
    std::string line;
    while (std::getline(maps, line)) {
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, permissions) !=
                3 ||
            start >= end) {
            ADD_FAILURE() << "Invalid /proc/self/maps entry: " << line;
            return {};
        }
        if (address < start || address >= end) {
            continue;
        }

        MemoryRegionState state = MemoryRegionState::OTHER;
        const std::string protection(permissions, 3);
        if (protection == "---") {
            state = MemoryRegionState::RESERVED;
        } else if (protection == "rw-") {
            state = MemoryRegionState::COMMITTED;
        }
        return {reinterpret_cast<const void*>(start), end - start, state};
    }
    if (!maps.eof()) {
        ADD_FAILURE() << "Cannot read /proc/self/maps";
        return {};
    }
    return {nullptr, 0, MemoryRegionState::FREE};
#else
#error Unsupported allocator test platform
#endif
}

MemoryRegion queryCommittedHeap(const void* pAddress) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pAddress, &info, sizeof(info)) != sizeof(info)) {
        ADD_FAILURE() << "VirtualQuery failed: " << GetLastError();
        return {};
    }
    EXPECT_EQ(info.State, MEM_COMMIT);
    const MemoryRegion region = queryMemoryRegion(info.AllocationBase);
#else
    const MemoryRegion region = queryMemoryRegion(pAddress);
#endif
    EXPECT_EQ(region.state, MemoryRegionState::COMMITTED);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(region.pBaseAddr);
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pAddress);
    EXPECT_GE(address, base);
    EXPECT_LT(address - base, region.size);
    return region;
}
} // namespace naru::tests::allocator
