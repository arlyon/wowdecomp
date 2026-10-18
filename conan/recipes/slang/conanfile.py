import os

from conan import ConanFile
from conan.tools.files import copy
from conan.tools.scm import Git

# Reviewed revision pinned by cmake/SlangCompiler.cmake. Keep the two in sync.
REVISION = "5cde9ea5ff3b6c657978de54d2241a86cb3528b1"

SUBMODULES = [
    "external/lua",
    "external/miniz",
    "external/lz4",
    "external/unordered_dense",
    "external/spirv-tools",
    "external/spirv-headers",
    "external/glslang",
    "external/vulkan",
]

# Mirrors SLANG_CMAKE_FLAGS in the Makefile: only slangc + the GLSLang module
# are built, everything optional is disabled.
CMAKE_OPTIONS = {
    "CMAKE_BUILD_TYPE": "Release",
    "SLANG_ENABLE_PREBUILT_BINARIES": "OFF",
    "SLANG_SLANG_LLVM_FLAVOR": "DISABLE",
    "SLANG_ENABLE_SLANG_GLSLANG": "ON",
    "SLANG_ENABLE_GFX": "OFF",
    "SLANG_ENABLE_SLANG_RHI": "OFF",
    "SLANG_ENABLE_EXAMPLES": "OFF",
    "SLANG_ENABLE_TESTS": "OFF",
    "SLANG_ENABLE_DXIL": "OFF",
    "SLANG_ENABLE_CUDA": "OFF",
    "SLANG_ENABLE_OPTIX": "OFF",
    "SLANG_ENABLE_AFTERMATH": "OFF",
    "SLANG_ENABLE_NVAPI": "OFF",
    "SLANG_ENABLE_SLANGD": "OFF",
    "SLANG_ENABLE_SLANGRT": "OFF",
}


class SlangConan(ConanFile):
    name = "slang"
    version = "2026.4.2"
    description = ("Slang compiler pinned for wowdecomp. Ships the source checkout "
                   "(for headers + revision check) and a locally built slangc.")
    settings = "os", "arch", "compiler", "build_type"

    def source(self):
        git = Git(self, folder=os.path.join(self.source_folder, "checkout"))
        git.clone(url="https://github.com/shader-slang/slang.git", target=".")
        git.checkout(REVISION)
        self.run("git submodule update --init --recursive -- " + " ".join(SUBMODULES),
                 cwd=git.folder)

    def build(self):
        src = os.path.join(self.source_folder, "checkout")
        options = " ".join(f"-D{k}={v}" for k, v in CMAKE_OPTIONS.items())
        self.run(f'cmake -S "{src}" -B "{self.build_folder}" -G Ninja {options}')
        self.run(f'cmake --build "{self.build_folder}" --target slangc slang-glslang '
                 f'--parallel {os.cpu_count() or 1}')

    def package(self):
        src = os.path.join(self.source_folder, "checkout")
        release = os.path.join(self.build_folder, "Release")
        # Full checkout, .git included: SlangCompiler.cmake checks rev-parse HEAD
        # and reads include/slang.h from this directory.
        copy(self, "*", src=src, dst=os.path.join(self.package_folder, "source"))
        # slangc + libslang-compiler.so. slangc has a relative RUNPATH
        # ($ORIGIN/../lib), so bin/ + lib/ keeps it runnable out of the cache.
        copy(self, "*", src=os.path.join(release, "bin"),
             dst=os.path.join(self.package_folder, "bin"))
        copy(self, "*", src=os.path.join(release, "lib"),
             dst=os.path.join(self.package_folder, "lib"))
