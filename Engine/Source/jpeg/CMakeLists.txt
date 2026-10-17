file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.c" "*.cpp")

set_source_files_properties(${PRIVATE_SOURCES} PROPERTIES LANGUAGE CXX)

add_library(jpeg STATIC
    ${PRIVATE_SOURCES}
)

if (NOT MSVC)
    # Keep the existing C++ linkage of this legacy C library; its sources use
    # register, which was removed from C++17.
    set_property(TARGET jpeg PROPERTY CXX_STANDARD 14)
endif ()

target_include_directories(jpeg
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)
