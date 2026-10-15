file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
if (WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Linux/")
endif ()

add_library(Storm STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Storm
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Storm
    PUBLIC
        blast
        bzip2
        zlib
)
