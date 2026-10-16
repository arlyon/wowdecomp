file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
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

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            d3d9.lib
            nvapi
            opengl32.lib
    )
endif ()
