file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_filter_platform_sources(PRIVATE_SOURCES ${PRIVATE_SOURCES})
if (WOWDECOMP_POSIX)
    list(REMOVE_ITEM PRIVATE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/WowConnection.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/WowConnectionNet.cpp")
endif ()

add_library(WowServices STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WowServices
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
