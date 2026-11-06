#pragma once
#include <cstddef>
#include <limits>

namespace naru::allocator::group::config {
// ==================== identifiers ====================
inline constexpr std::size_t INVALID_PAGE_INDEX = (std::numeric_limits<std::size_t>::max)();

// ==================== group heap ====================
inline constexpr std::size_t GROUP_RESERVE_SIZE = 1'073'741'824; // 1GiB
inline constexpr std::size_t COMMIT_SIZE = 33'554'432;           // 32MiB
inline constexpr std::size_t SEGMENT_SIZE_UNIT = 16;             // 16B

// ==================== metadata heap ====================
inline constexpr std::size_t METADATA_RESERVE_SIZE = 268'435'456;          // 256MiB
inline constexpr std::size_t METADATA_LIST_COMMIT_SIZE = 4'096;            // 4KiB
inline constexpr std::size_t METADATA_SORTED_FREE_SEG_COMMIT_SIZE = 4'096; // 4KiB
inline constexpr std::size_t METADATA_NODE_COMMIT_SIZE = 65'536;           // 64KiB
inline constexpr std::size_t METADATA_NODE_SLOT_SIZE = 24;                 // 24B
inline constexpr std::size_t METADATA_LIST_SLOT_SIZE = 80;                 // 80B
} // namespace naru::allocator::group::config
