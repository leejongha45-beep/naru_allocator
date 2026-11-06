import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain
from conan.tools.env import VirtualBuildEnv, VirtualRunEnv


class NaruAllocator(ConanFile):
    name = "naru_allocator"
    version = "0.1.0"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"with_tests": [True, False]}
    default_options = {
        "with_tests": True,
        "gtest/*:shared": False,
        "gtest/*:build_gmock": False,
        "gtest/*:no_main": False,
        "gtest/*:no_exceptions": True,
    }
    exports_sources = "CMakeLists.txt", "cmake/*", "public/*", "private/*", "tests/*"

    def requirements(self):
        self.requires("naru_support/0.1.0", transitive_headers=True, transitive_libs=True)

    def validate(self):
        if str(self.settings.os) not in ("Windows", "Linux"):
            raise ConanInvalidConfiguration(
                "naru_allocator supports Windows and Linux."
            )

    def build_requirements(self):
        if self.options.with_tests and not self.conf.get("tools.build:skip_test", default=False):
            self.test_requires("gtest/1.18.0@moru/stable")

    def layout(self):
        self.folders.generators = "conan"

    def package_id(self):
        self.info.options.rm_safe("with_tests")

    def generate(self):
        CMakeDeps(self).generate()
        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.variables["NARU_ALLOCATOR_BUILD_TESTS"] = (
            bool(self.options.with_tests)
            and not self.conf.get("tools.build:skip_test", default=False)
        )
        toolchain.generate()
        VirtualBuildEnv(self).generate()
        VirtualRunEnv(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if (
            self.options.with_tests
            and not self.conf.get("tools.build:skip_test", default=False)
            and can_run(self)
        ):
            cmake.ctest(cli_args=["--output-on-failure", "--no-tests=error"])

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.libs = ["naru_allocator"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs = [os.path.join("lib", str(self.settings.build_type))]
        self.cpp_info.builddirs = ["lib/cmake/naru_allocator"]
        self.cpp_info.set_property("cmake_find_mode", "none")
