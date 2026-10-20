file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/UILCD/Windows/")
endif ()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/NativeVideoOptions.lua")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/NativeVideoOptions.lua" NATIVE_VIDEO_OPTIONS_SOURCE)
configure_file(NativeVideoOptionsSource.h.in "${CMAKE_CURRENT_BINARY_DIR}/NativeVideoOptionsSource.h" @ONLY)

add_library(Ui STATIC
    ${PRIVATE_SOURCES}
)
target_include_directories(Ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_include_directories(Ui
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WIN32)
    target_link_libraries(Ui
        PRIVATE
            lgLcd
    )
endif ()
