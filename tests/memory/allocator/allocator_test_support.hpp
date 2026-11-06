#pragma once

#include <cstddef>
#include <gtest/gtest.h>
#include <initializer_list>

#include "group/memory_config.hpp"
#include "group/memory_segment_list.hpp"

namespace naru::tests::allocator {
namespace alloc = ::naru::allocator;
namespace config = ::naru::allocator::group::config;

inline void expectSegments(alloc::group::MemorySegList& list,
                           std::initializer_list<std::byte*> expectedAddresses) {
    auto it = list.begin();
    for (std::byte* pExpected : expectedAddresses) {
        ASSERT_NE(it, list.end());
        EXPECT_EQ(it->pBaseAddr, pExpected);
        ++it;
    }
    EXPECT_EQ(it, list.end());
    EXPECT_EQ(list.isEmpty(), expectedAddresses.size() == 0);
}
} // namespace naru::tests::allocator
