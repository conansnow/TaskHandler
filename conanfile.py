from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class TaskHandlerConan(ConanFile):
    name = "taskhandler"
    version = "0.3.0"
    package_type = "library"
    license = "MIT"
    url = "https://github.com/conansnow/TaskHandler"
    homepage = "https://github.com/conansnow/TaskHandler"
    description = "A small single-worker task queue and event handler for C++23"
    topics = ("task-queue", "thread-pool", "event-handler")
    required_conan_version = ">=2.0.14"

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    # The C++ namespace is `conan`; the package name is `taskhandler`.
    exports_sources = ("CMakeLists.txt", "cmake/*", "include/*", "src/*", "LICENSE")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, 23)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["TASKHANDLER_BUILD_TESTS"] = False
        tc.variables["TASKHANDLER_BUILD_EXAMPLES"] = False
        tc.variables["TASKHANDLER_BUILD_BENCHMARKS"] = False
        tc.variables["TASKHANDLER_INSTALL"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # Prefer the installed TaskHandlerConfig.cmake so both exported
        # targets work the same way as a plain cmake --install.
        self.cpp_info.set_property("cmake_file_name", "TaskHandler")
        self.cpp_info.set_property("cmake_target_name", "TaskHandler::task_handler")
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs.append(os.path.join("lib", "cmake", "TaskHandler"))

        compiled_defines = ["TASKHANDLER_COMPILED_LIB"]
        if self.options.shared:
            compiled_defines.append("TASKHANDLER_SHARED_LIB")
        pthread = self.settings.os in ["Linux", "FreeBSD", "Macos", "Android"]

        self.cpp_info.components["task_handler"].set_property(
            "cmake_target_name", "TaskHandler::task_handler")
        self.cpp_info.components["task_handler"].set_property(
            "pkg_config_name", "taskhandler")
        self.cpp_info.components["task_handler"].libs = ["task_handler"]
        self.cpp_info.components["task_handler"].defines = compiled_defines
        if pthread:
            self.cpp_info.components["task_handler"].system_libs = ["pthread"]

        self.cpp_info.components["header_only"].set_property(
            "cmake_target_name", "TaskHandler::header_only")
        self.cpp_info.components["header_only"].set_property(
            "pkg_config_name", "taskhandler-header-only")
        if pthread:
            self.cpp_info.components["header_only"].system_libs = ["pthread"]
