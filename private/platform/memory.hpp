#pragma once

#include <cstddef>

#include "memory_types.hpp"

namespace naru::allocator::platform {
// OS failures terminate here. Returning means the operation succeeded.
[[nodiscard]] std::size_t getCommonPageSize();
[[nodiscard]] void* reserveCommonMemory(const VmMemoryInfo& pageInfo);
void commitCommonMemory(void* pStartAddr, const VmMemoryInfo& pageInfo);
void decommitCommonMemory(void* pPageAddr, const VmMemoryInfo& pageInfo);

/**
 * @brief Releases an entire reservation after decommit.
 * @param pPageAddr Base address returned by reserveCommonMemory().
 * @param pageInfo Windows: unused and may be omitted.
 *                 Linux: provide the original pageSize and reservePageCount.
 */
void releasePostCommitMemory(void* pPageAddr, const VmMemoryInfo& pageInfo = {});

/**
 * @brief Releases an entire reservation, including any committed pages.
 * @param pPageAddr Base address returned by reserveCommonMemory().
 * @param pageInfo Windows: unused and may be omitted.
 *                 Linux: provide the original pageSize and reservePageCount.
 */
void releaseEntireMemory(void* pPageAddr, const VmMemoryInfo& pageInfo = {});
} // namespace naru::allocator::platform
