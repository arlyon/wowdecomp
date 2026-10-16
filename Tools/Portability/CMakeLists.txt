# Opt-in CPU regression tests, built against the client's real shared libraries.
# Keep the Linux OS-provider harness out of this macOS integration branch.
if(NOT WOWDECOMP_POSIX)
    message(FATAL_ERROR "These backend-selection tests use POSIX environment APIs")
endif()

# Each selection test compiles Gx.cpp for a different renderer combination,
# independently of the enclosing client's enabled backend.
get_directory_property(test_definitions COMPILE_DEFINITIONS)
list(FILTER test_definitions EXCLUDE REGEX "^WOWDECOMP_(VULKAN|DXVK)(=|$)")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "${test_definitions}")

add_executable(CoordinateTest CoordinateTest.cpp
    "${PROJECT_SOURCE_DIR}/Engine/Source/Base/Coordinate.cpp")
target_include_directories(CoordinateTest PRIVATE "${PROJECT_SOURCE_DIR}")
add_test(NAME Coordinate COMMAND CoordinateTest)

add_executable(RootLayoutTest RootLayoutTest.cpp
    "${PROJECT_SOURCE_DIR}/Engine/Source/Base/CDataAllocator.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/Base/Coordinate.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/Base/Status.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/Frame/CLayoutFrame.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/Frame/CFramePoint.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/FrameXML/LoadXML.cpp"
    "${PROJECT_SOURCE_DIR}/Engine/Source/FrameXML/XMLTree.cpp")
target_include_directories(RootLayoutTest PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(RootLayoutTest PRIVATE Storm Tempest BlizzardCore expat)
add_test(NAME RootLayout COMMAND RootLayoutTest)

foreach(backend dxvk vulkan both)
    add_executable(GxBackend-${backend} GxBackendTest.cpp
        "${PROJECT_SOURCE_DIR}/Engine/Source/Gx/Gx.cpp")
    target_include_directories(GxBackend-${backend} PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(GxBackend-${backend} PRIVATE Storm Tempest BlizzardCore)
    if(NOT backend STREQUAL "vulkan")
        target_compile_definitions(GxBackend-${backend} PRIVATE WOWDECOMP_DXVK=1)
    endif()
    if(NOT backend STREQUAL "dxvk")
        target_compile_definitions(GxBackend-${backend} PRIVATE WOWDECOMP_VULKAN=1)
    endif()
    add_test(NAME GxBackend-${backend} COMMAND GxBackend-${backend})
endforeach()

# Isolate the exercised CPU paths from unrelated renderer/XML entry points.
foreach(target RootLayoutTest GxBackend-dxvk GxBackend-vulkan GxBackend-both)
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(${target} PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(${target} PRIVATE -Wl,--gc-sections)
    endif()
endforeach()
