file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/(Win32|Win64)/")
    # This file contains shared throttling logic, despite its original location.
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND PRIVATE_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/File/Win64/File.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/Thread/Win64/ThreadPools.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Packages/Streaming/Win64/Manifests.cpp"
        )
    endif ()
else ()
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Linux/")
endif ()

add_library(BlizzardCore STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(BlizzardCore
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(BlizzardCore
    PUBLIC
        blast
        lzma
        zlib
)

if(WIN32)
    target_link_libraries(BlizzardCore
        PUBLIC
            wininet
    )
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET GLOBAL vorbisfile)
    target_link_libraries(BlizzardCore PUBLIC PkgConfig::VORBISFILE bzip2 Threads::Threads)
else ()
    target_include_directories(BlizzardCore
        PRIVATE
            ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/ogg/include
            ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/vorbis/include
    )
endif ()
