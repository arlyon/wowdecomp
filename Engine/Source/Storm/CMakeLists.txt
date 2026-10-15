if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "Source/Linux/*.cpp")
    list(APPEND PRIVATE_SOURCES Source/SStr.cpp Source/SOption.cpp)
else ()
    file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/Linux/")
endif ()

add_library(Storm STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Storm
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Storm PUBLIC Threads::Threads)
else ()
    target_link_libraries(Storm
        PUBLIC
            blast
            bzip2
            zlib
    )
endif ()
