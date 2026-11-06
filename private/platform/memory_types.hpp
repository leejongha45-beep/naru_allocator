#pragma once

#include <cstddef>

namespace naru::allocator::platform {
#ifdef _WIN32
struct wVirtualMemoryInfo {
    std::size_t pageSize = 0;
    std::size_t reservePageCount = 0;
    std::size_t commitPageCount = 0;
};
using VmMemoryInfo = wVirtualMemoryInfo;
#elifdef __linux__
struct lVirtualMemoryInfo {
    std::size_t pageSize = 0;
    std::size_t reservePageCount = 0;
    std::size_t commitPageCount = 0;
};
using VmMemoryInfo = lVirtualMemoryInfo;
#endif
} // namespace naru::allocator::platform
