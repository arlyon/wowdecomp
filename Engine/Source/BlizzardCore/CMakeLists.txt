file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
    list(REMOVE_ITEM PRIVATE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/Log/Log.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/Thread/ThreadPools.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Packages/iounit/IOFileUnit.cpp")
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

target_include_directories(BlizzardCore
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/ogg/include
        ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/vorbis/include
)
