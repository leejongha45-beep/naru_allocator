add_executable(naru_allocator_tests
        owned_allocator_test.cpp
        group_allocator_test.cpp
        group_allocator_impl_test.cpp
        memory_segment_list_test.cpp
        memory_segment_list_vector_test.cpp
        memory_segment_list_storage_test.cpp
        sorted_free_segment_vector_test.cpp
        windows_memory_test.cpp
        linux_memory_test.cpp
        allocator_test_support.hpp
        memory_region_test_support.cpp
        memory_region_test_support.hpp
)

# Inspect private state only in the allocator implementation test translation units.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set_property(SOURCE owned_allocator_test.cpp group_allocator_impl_test.cpp APPEND PROPERTY
            COMPILE_OPTIONS /clang:-fno-access-control)
else ()
    set_property(SOURCE owned_allocator_test.cpp group_allocator_impl_test.cpp APPEND PROPERTY
            COMPILE_OPTIONS -fno-access-control)
endif ()

naru_apply_cpp_options(naru_allocator_tests)
target_include_directories(naru_allocator_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/private"
)
target_link_libraries(naru_allocator_tests PRIVATE
        naru::allocator
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(naru_allocator_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "allocator" TIMEOUT 60
)
