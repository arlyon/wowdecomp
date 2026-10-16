option(WOWDECOMP_VULKAN "Build the native Vulkan backend alongside D3D9/DXVK" OFF)
file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
if (NOT WOWDECOMP_VULKAN)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDeviceVulkan/")
elseif (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Native Vulkan client bring-up currently targets the Linux port")
endif ()

wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Mac/")
    list(REMOVE_ITEM PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/CGxDeviceD3d/CGxDeviceD3d.cpp")
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d9Ex|OpenGL)/")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake")
    find_package(DXVKNative REQUIRED)
    if (NOT TARGET PkgConfig::SDL2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2>=2.0.22)
    endif ()
endif ()

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WOWDECOMP_POSIX)
    target_link_libraries(Gx PUBLIC DXVKNative::D3D9 PRIVATE PkgConfig::SDL2)
endif ()

if (WOWDECOMP_VULKAN)
    get_filename_component(WOWDECOMP_ROOT "../../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    find_package(Vulkan 1.1 REQUIRED)
    include("${WOWDECOMP_ROOT}/cmake/MojoShader.cmake")
    include("${WOWDECOMP_ROOT}/cmake/SlangCompiler.cmake")
    include("${WOWDECOMP_ROOT}/cmake/SlangShaders.cmake")
    target_link_libraries(Gx PRIVATE Vulkan::Vulkan GxMojoShader GxSlangCompiler)
    target_compile_definitions(Gx PUBLIC WOWDECOMP_VULKAN=1)
    target_compile_definitions(Gx PRIVATE WOWDECOMP_VULKAN_SHADER_DIR="${WOWDECOMP_VULKAN_SHADER_DIR}")
    add_dependencies(Gx GxSlangShaders)
endif ()

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            d3d9.lib
            nvapi
            opengl32.lib
    )
endif ()
