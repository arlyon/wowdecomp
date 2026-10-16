file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Unavailable/")
else ()
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Win(32|64)/")
endif ()

add_library(SteelseriesMouse STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(SteelseriesMouse
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WIN32)
    target_link_libraries(SteelseriesMouse
        PUBLIC
            hid
            setupapi
    )
endif ()
