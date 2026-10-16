if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "Source/Linux/*.cpp")
    list(APPEND PRIVATE_SOURCES Source/SStr.cpp Source/SOption.cpp Source/soprintf.cpp Source/MD5.cpp Source/SComp.cpp
        Source/SArc4.cpp Source/SCrc.cpp Source/SCmd.cpp Source/SBig.cpp
        Source/RSA.cpp Source/Sha1.cpp Source/SSignature.cpp Source/SUni.cpp Source/SRgn.cpp)
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
    find_package(Threads REQUIRED)
    if (NOT TARGET PkgConfig::SDL2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2>=2.0.22)
    endif ()
    target_link_libraries(Storm PUBLIC Threads::Threads PRIVATE PkgConfig::SDL2 blast bzip2 zlib)
else ()
    target_link_libraries(Storm
        PUBLIC
            blast
            bzip2
            zlib
    )
endif ()
