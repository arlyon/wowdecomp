file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WOWDECOMP_POSIX)
    file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "Posix/*.cpp" "${WOWDECOMP_OS}/*.cpp")
    wowdecomp_filter_platform_sources(PLATFORM_SOURCES ${PLATFORM_SOURCES})
elseif (WOWDECOMP_ARCH_64)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win64/*.cpp")
else ()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win32/*.cpp")
endif ()

add_executable(Wow WIN32 ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})

if (WOWDECOMP_ARCH_64)
    set(WOW_OUTPUT_NAME Wow64)
else ()
    set(WOW_OUTPUT_NAME Wow32)
endif ()

if (WIN32)
    set(WOW_RUNTIME_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/Win)
else ()
    set(WOW_OUTPUT_NAME Wow)
    set(WOW_RUNTIME_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif ()

set_target_properties(Wow PROPERTIES
    OUTPUT_NAME ${WOW_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${WOW_RUNTIME_DIRECTORY}
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(Wow PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${WOW_RUNTIME_DIRECTORY})
endforeach ()

target_include_directories(Wow
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

if (WIN32)
    target_link_options(Wow PRIVATE "/INCREMENTAL:NO" "/ENTRY:start")
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The original module graph has static-library cycles; rescan, don't force
    # extraction of unused objects as --whole-archive would.
    target_link_libraries(Wow PRIVATE PkgConfig::SDL2 "-Wl,--start-group")
endif ()

target_link_libraries(Wow
    PRIVATE
        BLPFile
        Base
        BlizzardCore
        ComSat
        ConsoleBase
        Event
        FFX
        Frame
        FrameScript
        lua
        FrameXML
        Gx
        Gxu
        Images
        Model2
        Os
        Scrn
        Services
        ShaderEffect
        Tempest
        SoundEngine
        SteelseriesMouse
        Storm
        WoWCommon
        Battlenet
        Console
        ConsoleDevice
        DB
        Game
        Glue
        LiquidSystem
        Magic
        Minigame
        Net
        Object
        ObjectAlloc
        ObjectMgrClient
        SoundInterface
        UIUtil
        Ui
        WorldClient
        WowServices
        WowSvcs
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Wow PRIVATE "-Wl,--end-group")
endif ()
