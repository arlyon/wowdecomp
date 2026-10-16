file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
    # These original Windows translation units have explicit native-extension
    # counterparts. OS leaves are linked separately; never import Win64 sources.
    list(REMOVE_ITEM PRIVATE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/Log/Log.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/System/Thread/ThreadPools.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Packages/iounit/IOFileUnit.cpp")
endif ()

if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Win(32|64)/")
else ()
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Mac/")
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
elseif(APPLE)
    target_link_libraries(BlizzardCore
        PUBLIC
            CURL::libcurl
    )
endif()

if (WOWDECOMP_POSIX)
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET GLOBAL vorbisfile)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET GLOBAL libcurl>=7.85.0)
    target_link_libraries(BlizzardCore PUBLIC PkgConfig::VORBISFILE PkgConfig::CURL bzip2 Threads::Threads)
else ()
    target_include_directories(BlizzardCore
        PRIVATE
            ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/ogg/include
            ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/vorbis/include
    )
endif ()
