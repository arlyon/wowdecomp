add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/msdfgen" "${CMAKE_CURRENT_BINARY_DIR}/msdfgen")

file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Gxu STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gxu
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Gxu
    PUBLIC
        freetype
        GxuMsdfgen
)

option(WOWDECOMP_FONT_TESTS "Build deterministic production MSDF glyph/atlas tests" OFF)
if (WOWDECOMP_FONT_TESTS)
    enable_testing()
    find_file(WOWDECOMP_FONT_TEST_FACE DejaVuSans.ttf
        PATHS /usr/share/fonts/dejavu-sans-fonts /usr/share/fonts/truetype/dejavu)
    if (NOT WOWDECOMP_FONT_TEST_FACE)
        message(FATAL_ERROR "Set WOWDECOMP_FONT_TEST_FACE to an openly licensed installed outline font")
    endif ()
    add_executable(GxuFontMsdfTest "${CMAKE_CURRENT_SOURCE_DIR}/../../../Tools/Fonts/GxuFontMsdfTest.cpp")
    target_link_libraries(GxuFontMsdfTest PRIVATE Gxu)
    add_test(NAME GxuFontMsdf COMMAND GxuFontMsdfTest "${WOWDECOMP_FONT_TEST_FACE}")
endif ()
if (WOWDECOMP_FONT_TESTS AND WOWDECOMP_VULKAN)
    add_executable(GxuFontNativeTest "${CMAKE_CURRENT_SOURCE_DIR}/../../../Tools/Fonts/GxuFontNativeTest.cpp")
    target_link_libraries(GxuFontNativeTest PRIVATE -Wl,--start-group
        Gx Gxu Os Storm Base Tempest BlizzardCore Images Services Event BLPFile expat jpeg blast zlib bzip2 lzma GxuMsdfgen
        -Wl,--end-group PkgConfig::SDL2 curl dl m)
    add_test(NAME GxuFontNative COMMAND GxuFontNativeTest "${WOWDECOMP_FONT_TEST_FACE}")
    set_tests_properties(GxuFontNative PROPERTIES TIMEOUT 60 LABELS "GPU"
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endif ()
if (WOWDECOMP_FONT_TESTS)
    add_executable(GxuFontFreetypeTest "${CMAKE_CURRENT_SOURCE_DIR}/../../../Tools/Fonts/GxuFontFreetypeTest.cpp")
    target_link_libraries(GxuFontFreetypeTest PRIVATE freetype)
    add_test(NAME GxuFontFreetype COMMAND GxuFontFreetypeTest "${WOWDECOMP_FONT_TEST_FACE}")
endif ()
