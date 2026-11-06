cmake_minimum_required(VERSION 4.4)
project(naru_allocator LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "naru_allocator supports Windows and Linux.")
endif ()

if (NOT TARGET naru::support)
    find_package(naru_support CONFIG REQUIRED)
endif ()

add_library(naru_allocator STATIC
        public/naru/allocator.hpp
        public/naru/allocator/owned/owned_allocator.hpp
        public/naru/allocator/group/group_allocator.hpp
        private/owned/owned_allocator.cpp
        private/group/group_allocator.cpp
        private/group/memory_config.hpp
        private/group/memory_segment_list.hpp
        private/group/memory_segment_list.cpp
        private/group/memory_segment_list_vector.hpp
        private/group/memory_segment_list_vector.cpp
        private/group/memory_segment_list_storage.hpp
        private/group/memory_segment_list_storage.cpp
        private/group/sorted_free_segment_vector.hpp
        private/group/sorted_free_segment_vector.cpp
        private/platform/memory.hpp
        private/platform/memory_types.hpp
        private/platform/windows/windows_memory.cpp
        private/platform/linux/linux_memory.cpp
)
add_library(naru::allocator ALIAS naru_allocator)

set_target_properties(naru_allocator PROPERTIES EXPORT_NAME allocator)
target_include_directories(naru_allocator
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/public>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/private"
)
target_link_libraries(naru_allocator PUBLIC naru::support)
naru_apply_cpp_options(naru_allocator)

install(TARGETS naru_allocator
        EXPORT naru_allocatorTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
)
install(DIRECTORY public/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.hpp"
)
install(EXPORT naru_allocatorTargets
        NAMESPACE naru::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/naru_allocator"
)
configure_package_config_file(
        cmake/naru_allocatorConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/naru_allocatorConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/naru_allocator"
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/naru_allocatorConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/naru_allocator"
)

option(NARU_ALLOCATOR_BUILD_TESTS "Build allocator tests" ${PROJECT_IS_TOP_LEVEL})
if (NARU_ALLOCATOR_BUILD_TESTS)
    enable_testing()
    if (NOT TARGET GTest::gtest_main)
        include(cmake/NaruGoogleTest.cmake)
    endif ()
    add_subdirectory(tests)
endif ()
