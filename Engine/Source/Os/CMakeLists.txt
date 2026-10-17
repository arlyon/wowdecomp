if (WOWDECOMP_POSIX)
    file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "Native/*.cpp" "Posix/*.cpp" "SDL/*.cpp" "${WOWDECOMP_OS}/*.cpp")
    list(APPEND PRIVATE_SOURCES Time.cpp Processor.cpp Path.cpp)
else ()
    file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
endif ()
wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})

add_library(Os STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Os
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WOWDECOMP_POSIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2>=2.0.22)
    target_link_libraries(Os PRIVATE Threads::Threads ${CMAKE_DL_LIBS} PkgConfig::SDL2 Storm BlizzardCore)
endif ()

if(WIN32)
    target_link_libraries(Os
        PUBLIC
            imm32
            wininet
            ws2_32
            dinput8
            dxguid
            version
    )
endif()
