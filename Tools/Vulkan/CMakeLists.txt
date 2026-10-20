cmake_minimum_required(VERSION 3.16)
project(wowdecomp_vulkan_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(WOWDECOMP_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${WOWDECOMP_ROOT}/cmake/MojoShader.cmake")
add_library(GxVulkanShaders STATIC
    "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/Shader.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/TemporalShader.cpp")
target_include_directories(GxVulkanShaders PUBLIC "${WOWDECOMP_ROOT}")
target_link_libraries(GxVulkanShaders PUBLIC GxMojoShader)
add_executable(VulkanShaderTest ShaderTest.cpp)
add_executable(VulkanShaderCorpus ShaderCorpus.cpp)
add_executable(VulkanShaderSource ShaderSource.cpp)
target_link_libraries(VulkanShaderTest PRIVATE GxVulkanShaders)
target_link_libraries(VulkanShaderCorpus PRIVATE GxVulkanShaders)
target_link_libraries(VulkanShaderSource PRIVATE GxVulkanShaders)
find_path(SPIRV_TOOLS_INCLUDE_DIR spirv-tools/libspirv.h)
find_library(SPIRV_TOOLS_LIBRARY NAMES SPIRV-Tools-shared)
if (SPIRV_TOOLS_INCLUDE_DIR AND SPIRV_TOOLS_LIBRARY)
    foreach (target VulkanShaderTest VulkanShaderCorpus)
        target_include_directories(${target} PRIVATE "${SPIRV_TOOLS_INCLUDE_DIR}")
        target_link_libraries(${target} PRIVATE "${SPIRV_TOOLS_LIBRARY}")
        target_compile_definitions(${target} PRIVATE WOWDECOMP_VALIDATE_SPIRV=1)
    endforeach ()
else ()
    message(STATUS "SPIRV-Tools development files not found; module validation unavailable")
endif ()
enable_testing()
add_test(NAME VulkanShader COMMAND VulkanShaderTest)
set_tests_properties(VulkanShader PROPERTIES TIMEOUT 20)
foreach (name TemporalHistory TemporalBackend)
    add_executable(${name}Test ${name}Test.cpp)
    target_include_directories(${name}Test PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_options(${name}Test PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name}Test)
endforeach ()
if (WOWDECOMP_SLANGC)
    include("${WOWDECOMP_ROOT}/cmake/SlangCompiler.cmake")
    target_sources(GxVulkanShaders PRIVATE "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/Slang.cpp")
    target_link_libraries(GxVulkanShaders PUBLIC GxSlangCompiler)
    target_compile_definitions(VulkanShaderTest PRIVATE WOWDECOMP_TEST_SLANG_COMPILER=1)
    target_compile_definitions(VulkanShaderCorpus PRIVATE WOWDECOMP_TEST_SLANG_COMPILER=1)
    include("${WOWDECOMP_ROOT}/cmake/SlangShaders.cmake")
    add_dependencies(VulkanShaderTest GxSlangShaders)
    target_compile_definitions(VulkanShaderTest PRIVATE
        WOWDECOMP_SLANG_TEST_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
    find_package(Vulkan 1.1 QUIET)
    if (Vulkan_FOUND)
        add_executable(FixedFunctionTest FixedFunctionTest.cpp
            "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/ResidentBuffer.cpp")
        target_link_libraries(FixedFunctionTest PRIVATE Vulkan::Vulkan GxVulkanShaders)
        target_compile_definitions(FixedFunctionTest PRIVATE
            WOWDECOMP_SLANG_TEST_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
        add_dependencies(FixedFunctionTest GxSlangShaders)
        add_test(NAME FixedFunctionGpu COMMAND FixedFunctionTest)
        add_test(NAME BufferResidencyGpu COMMAND FixedFunctionTest --residency)
        add_test(NAME WorldTargetGpu COMMAND FixedFunctionTest --world-target)
        add_test(NAME TemporalBackendGpu COMMAND FixedFunctionTest --temporal-backend)
        add_test(NAME TemporalShaderGpu COMMAND FixedFunctionTest --temporal)
        foreach (name TemporalResolve TemporalUpscale)
            add_executable(${name}Test ${name}Test.cpp)
            target_include_directories(${name}Test PRIVATE "${WOWDECOMP_ROOT}")
            target_link_libraries(${name}Test PRIVATE Vulkan::Vulkan)
            target_compile_definitions(${name}Test PRIVATE
                WOWDECOMP_TEMPORAL_SHADER_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
            add_dependencies(${name}Test GxSlangShaders)
            add_test(NAME ${name}Gpu COMMAND ${name}Test)
            set_tests_properties(${name}Gpu PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
                ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
        endforeach ()
        set_tests_properties(FixedFunctionGpu BufferResidencyGpu WorldTargetGpu TemporalBackendGpu TemporalShaderGpu PROPERTIES TIMEOUT 30
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endif ()
endif ()

set(WOWDECOMP_VULKAN_CLIENT_BUILD "" CACHE PATH "Built native Vulkan client archives for production integration fixtures")
if (TARGET FixedFunctionTest AND WOWDECOMP_VULKAN_CLIENT_BUILD)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    add_executable(FfxAlignmentTest FfxAlignmentTest.cpp
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/FFX/FFX.cpp")
    target_include_directories(FfxAlignmentTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(FfxAlignmentTest PRIVATE WOWDECOMP_ARCH_64=1 WOWDECOMP_VULKAN=1
        WOWDECOMP_VULKAN_SHADER_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
    target_compile_options(FfxAlignmentTest PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(FfxAlignmentTest PRIVATE -Wl,--gc-sections)
    set(ffxArchives)
    foreach (library Gx Gxu Os Storm Base Tempest BlizzardCore Images expat jpeg blast zlib bzip2 lzma GxMojoShader)
        list(APPEND ffxArchives "${WOWDECOMP_VULKAN_CLIENT_BUILD}/lib/lib${library}.a")
    endforeach ()
    target_link_libraries(FfxAlignmentTest PRIVATE GxVulkanShaders -Wl,--start-group ${ffxArchives} -Wl,--end-group
        Vulkan::Vulkan PkgConfig::SDL2 curl dl m)
    add_dependencies(FfxAlignmentTest GxSlangShaders)
    add_test(NAME FfxAlignmentGpu COMMAND FfxAlignmentTest)
    add_executable(TemporalLiveTest TemporalLiveTest.cpp
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.cpp")
    target_include_directories(TemporalLiveTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(TemporalLiveTest PRIVATE WOWDECOMP_ARCH_64=1 WOWDECOMP_VULKAN=1
        WOWDECOMP_VULKAN_SHADER_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
    target_link_libraries(TemporalLiveTest PRIVATE GxVulkanShaders -Wl,--start-group ${ffxArchives} -Wl,--end-group
        Vulkan::Vulkan PkgConfig::SDL2 curl dl m)
    add_dependencies(TemporalLiveTest GxSlangShaders)
    add_test(NAME TemporalLiveGpu COMMAND TemporalLiveTest)
    add_test(NAME TemporalLiveEngineeringGpu COMMAND TemporalLiveTest)
    add_test(NAME TemporalUpscaleLiveGpu COMMAND TemporalLiveTest --upscale)
    set_tests_properties(TemporalLiveGpu TemporalUpscaleLiveGpu PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    set_tests_properties(TemporalLiveEngineeringGpu PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~;WOWDECOMP_VULKAN_WORLD_TARGET=1")
    set_tests_properties(FfxAlignmentGpu PROPERTIES TIMEOUT 90 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")

    add_executable(WindowStartupTest WindowStartupTest.cpp WindowConsoleHost.cpp
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/ConsoleDevice.cpp"
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/ConsoleDevice_CVar.cpp"
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/ConsoleDevice_Console.cpp"
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/Posix/Window.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/ConsoleBase/ConsoleVar.cpp")
    target_include_directories(WindowStartupTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(WindowStartupTest PRIVATE WOWDECOMP_ARCH_64=1 WOWDECOMP_VULKAN=1)
    target_compile_options(WindowStartupTest PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(WindowStartupTest PRIVATE -Wl,--gc-sections
        -Wl,--wrap=_Z14CreateGxDevice6EGxApiPFlPvjmlERK9CGxFormat)
    target_link_libraries(WindowStartupTest PRIVATE GxVulkanShaders -Wl,--start-group ${ffxArchives}
        "${WOWDECOMP_VULKAN_CLIENT_BUILD}/lib/libDB.a"
        "${WOWDECOMP_VULKAN_CLIENT_BUILD}/lib/libConsoleBase.a"
        -Wl,--end-group Vulkan::Vulkan PkgConfig::SDL2 curl dl m)
    add_test(NAME WindowStartup COMMAND WindowStartupTest)
    set_tests_properties(WindowStartup PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy" RUN_SERIAL TRUE)

    add_executable(WindowPlacementTest WindowPlacementTest.cpp WindowConsoleHost.cpp
        "${WOWDECOMP_ROOT}/Engine/Source/ConsoleBase/ConsoleVar.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/ConsoleBase/ConsoleCommand.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/Os/SDL/Gui.cpp")
    target_include_directories(WindowPlacementTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(WindowPlacementTest PRIVATE WOWDECOMP_ARCH_64=1)
    target_compile_options(WindowPlacementTest PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(WindowPlacementTest PRIVATE -Wl,--gc-sections)
    target_link_libraries(WindowPlacementTest PRIVATE -Wl,--start-group ${ffxArchives} -Wl,--end-group PkgConfig::SDL2 curl dl m)
    add_test(NAME WindowPlacement COMMAND WindowPlacementTest)
    set_tests_properties(WindowPlacement PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(WindowSettingsTest WindowSettingsTest.cpp
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.cpp"
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/ConsoleDevice_Console.cpp"
        "${WOWDECOMP_ROOT}/WoW/Source/ConsoleDevice/Posix/Window.cpp")
    target_include_directories(WindowSettingsTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(WindowSettingsTest PRIVATE WOWDECOMP_ARCH_64=1 WOWDECOMP_VULKAN=1
        WOWDECOMP_VULKAN_SHADER_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
    target_link_libraries(WindowSettingsTest PRIVATE GxVulkanShaders -Wl,--start-group ${ffxArchives} -Wl,--end-group
        Vulkan::Vulkan PkgConfig::SDL2 curl dl m)
    add_dependencies(WindowSettingsTest GxSlangShaders)
    add_test(NAME WindowSettingsGpu COMMAND WindowSettingsTest)
    set_tests_properties(WindowSettingsGpu PROPERTIES TIMEOUT 60 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")

    add_executable(NativeSceneSettingsTest NativeSceneSettingsTest.cpp WindowConsoleHost.cpp
        "${WOWDECOMP_ROOT}/WoW/Source/WorldClient/WorldParam_CVar.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/ConsoleBase/ConsoleVar.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/ConsoleBase/ConsoleCommand.cpp")
    target_include_directories(NativeSceneSettingsTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(NativeSceneSettingsTest PRIVATE WOWDECOMP_ARCH_64=1 WOWDECOMP_VULKAN=1)
    target_compile_options(NativeSceneSettingsTest PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(NativeSceneSettingsTest PRIVATE -Wl,--gc-sections)
    target_link_libraries(NativeSceneSettingsTest PRIVATE GxVulkanShaders -Wl,--start-group ${ffxArchives} -Wl,--end-group
        Vulkan::Vulkan PkgConfig::SDL2 curl dl m)
    add_test(NAME NativeSceneSettings COMMAND NativeSceneSettingsTest)

    add_executable(NativeVideoOptionsTest NativeVideoOptionsTest.cpp)
    target_include_directories(NativeVideoOptionsTest PRIVATE "${WOWDECOMP_ROOT}")
    target_link_libraries(NativeVideoOptionsTest PRIVATE -Wl,--start-group
        "${WOWDECOMP_VULKAN_CLIENT_BUILD}/lib/liblua.a" ${ffxArchives} -Wl,--end-group PkgConfig::SDL2 curl dl m)
    add_test(NAME NativeVideoOptions COMMAND NativeVideoOptionsTest
        "${CMAKE_CURRENT_SOURCE_DIR}/test_native_video_options.lua"
        "${WOWDECOMP_ROOT}/WoW/Source/Ui/NativeVideoOptions.lua")
endif ()

add_executable(DeviceSelectionTest DeviceSelectionTest.cpp "${WOWDECOMP_ROOT}/Engine/Source/Gx/DeviceSelection.cpp")
target_include_directories(DeviceSelectionTest PRIVATE "${WOWDECOMP_ROOT}")
target_compile_definitions(DeviceSelectionTest PRIVATE WOWDECOMP_VULKAN=1)
target_compile_options(DeviceSelectionTest PRIVATE -UNDEBUG)
add_test(NAME DeviceSelection COMMAND DeviceSelectionTest)
if (TARGET Vulkan::Vulkan)
    add_executable(PresentModeTest PresentModeTest.cpp "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxFormat.cpp")
    target_include_directories(PresentModeTest PRIVATE "${WOWDECOMP_ROOT}")
    target_link_libraries(PresentModeTest PRIVATE Vulkan::Vulkan)
    add_test(NAME PresentMode COMMAND PresentModeTest)
    add_executable(DeviceEnumerationTest DeviceEnumerationTest.cpp "${WOWDECOMP_ROOT}/Engine/Source/Gx/DeviceSelection.cpp")
    target_include_directories(DeviceEnumerationTest PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_definitions(DeviceEnumerationTest PRIVATE WOWDECOMP_VULKAN=1)
    target_compile_options(DeviceEnumerationTest PRIVATE -UNDEBUG)
    target_link_libraries(DeviceEnumerationTest PRIVATE Vulkan::Vulkan)
    add_test(NAME DeviceEnumeration COMMAND DeviceEnumerationTest)
    set_tests_properties(DeviceEnumeration PROPERTIES TIMEOUT 30 LABELS "GPU"
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endif ()
