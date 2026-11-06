add_executable(naru_allocator_api_tests
        group_allocator_api_test.cpp
        owned_allocator_api_test.cpp
)

naru_apply_cpp_options(naru_allocator_api_tests)
target_link_libraries(naru_allocator_api_tests PRIVATE
        naru::allocator
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(naru_allocator_api_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "allocator;public_api" TIMEOUT 60
)
