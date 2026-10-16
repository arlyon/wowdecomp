cmake_minimum_required(VERSION 3.13)

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "In-source builds not allowed.
        Please make a new directory (called a build directory) and run CMake from there.
        You may need to remove CMakeCache.txt."
    )
endif ()

project(wowdecomp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/Engine/Source/Platform/Compat")
endif ()

include(cmake/WowPlatform.cmake)
wowdecomp_configure_platform()
if (NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "This branch implements the Windows and macOS clients; use linux for Linux.")
endif ()

# The decompiled code relies on MSVC semantics: no type-based alias analysis (it type-puns through
# reinterpret_cast, e.g. expanding MPQ block tables in place) and wrapping signed arithmetic. Clang/GCC
# optimize both as undefined behavior, which silently miscompiles such code.
if (NOT MSVC)
    add_compile_options(-fno-strict-aliasing -fwrapv)
endif ()

# Native macOS client: SDL2 (window, input, audio device), the system OpenGL framework and libcurl replace
# Win32/WGL/DirectInput/WinINet. Platform sources live in Mac/ directories next to the Win32/Win64 ones.
if (WOWDECOMP_POSIX)
    find_package(Threads REQUIRED)
endif ()

if (APPLE)
    find_package(SDL2 CONFIG REQUIRED)
    find_package(CURL REQUIRED)
    find_package(OpenGL REQUIRED)
endif ()

option(WOWDECOMP_VULKAN "Build the native Vulkan backend alongside D3D9/DXVK" OFF)
if (WOWDECOMP_VULKAN)
    if (CMAKE_VERSION VERSION_LESS 3.16)
        message(FATAL_ERROR "The native Vulkan build requires CMake 3.16 or newer")
    endif ()
    add_compile_definitions(WOWDECOMP_VULKAN=1)
endif ()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (WIN32)
    add_compile_definitions(
        WIN32
        _WINDOWS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else ()
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

# Off by default: the client checks MPQ file MD5s (signaturefile) and GlueXML/FrameXML/AddOn .toc.sig files like the
# original. ON accepts unsigned or modified game data and interface files.
option(WOWDECOMP_DISABLE_SIGNATURE_CHECKS "Skip MPQ file signature and .toc signature checks" OFF)
if (WOWDECOMP_DISABLE_SIGNATURE_CHECKS)
    add_compile_definitions(DISABLE_SIGNATURE_CHECKS=1)
endif ()

# Third-party code is linked, not decompiled, for now.
if (EXISTS ${CMAKE_SOURCE_DIR}/ThirdParty/CMakeLists.txt)
    add_subdirectory(ThirdParty)
endif ()

add_subdirectory(Engine/Source/BLPFile)
add_subdirectory(Engine/Source/Base)
add_subdirectory(Engine/Source/BlizzardCore)
add_subdirectory(Engine/Source/ComSat)
add_subdirectory(Engine/Source/ConsoleBase)
add_subdirectory(Engine/Source/Event)
add_subdirectory(Engine/Source/FFX)
add_subdirectory(Engine/Source/Frame)
add_subdirectory(Engine/Source/FrameScript)
add_subdirectory(Engine/Source/FrameXML)
add_subdirectory(Engine/Source/Gx)
add_subdirectory(Engine/Source/Gxu)
add_subdirectory(Engine/Source/Images)
add_subdirectory(Engine/Source/jpeg)
add_subdirectory(Engine/Source/lua-5.1)
add_subdirectory(Engine/Source/Model2)
add_subdirectory(Engine/Source/Os)
add_subdirectory(Engine/Source/pcre)
add_subdirectory(Engine/Source/Scrn)
add_subdirectory(Engine/Source/Services)
add_subdirectory(Engine/Source/ShaderEffect)
add_subdirectory(Engine/Source/SoundEngine)
add_subdirectory(Engine/Source/SteelseriesMouse)
add_subdirectory(Engine/Source/Storm)
add_subdirectory(Engine/Source/Tempest)
add_subdirectory(WoW/Common)
add_subdirectory(WoW/Source/Battlenet)
add_subdirectory(WoW/Source/Console)
add_subdirectory(WoW/Source/ConsoleDevice)
add_subdirectory(WoW/Source/DB)
add_subdirectory(WoW/Source/Game)
add_subdirectory(WoW/Source/Glue)
add_subdirectory(WoW/Source/Magic)
add_subdirectory(WoW/Source/LiquidSystem)
add_subdirectory(WoW/Source/Minigame)
add_subdirectory(WoW/Source/Net)
add_subdirectory(WoW/Source/Object)
add_subdirectory(WoW/Source/ObjectAlloc)
add_subdirectory(WoW/Source/ObjectMgrClient)
add_subdirectory(WoW/Source/SoundInterface)
add_subdirectory(WoW/Source/UIUtil)
add_subdirectory(WoW/Source/Ui)
add_subdirectory(WoW/Source/WorldClient)
add_subdirectory(WoW/Source/WowServices)
add_subdirectory(WoW/Source/WowSvcs)
add_subdirectory(WoW/Source)
if (WIN32)
    add_subdirectory(Tools/WowClient)
endif ()
if (WOWDECOMP_ARCH_64)
    add_subdirectory(Tools/XvidDecoder)
endif ()

if (WIN32)
    foreach (target Wow WowClient MapViewer M2Viewer GxTest MpqTest BlpTest DbcTest AssetCheck)
        if (TARGET ${target})
            target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/waystone.rc)
        endif ()
    endforeach ()
endif ()
