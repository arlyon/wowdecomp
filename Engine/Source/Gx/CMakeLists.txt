file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
    list(REMOVE_ITEM PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/CGxDeviceD3d/CGxDeviceD3d.cpp")
endif ()

if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|OpenGL)/")
endif ()

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Gx
    PRIVATE
        d3d9.lib
)

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            nvapi
            opengl32.lib
    )
endif ()
