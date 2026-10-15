cmake_minimum_required(VERSION 3.13)
project(wowdecomp_portability_tests LANGUAGES CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "These OS portability tests exercise the Linux implementations.")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(WOWDECOMP_ROOT "../.." ABSOLUTE)
find_package(Threads REQUIRED)
enable_testing()

add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Os" Os)
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Tempest" Tempest)
# Production modules normally inherit the repository root as CMAKE_SOURCE_DIR.
# This independent test project supplies it without changing the client build.
target_include_directories(Os PUBLIC "${WOWDECOMP_ROOT}")
target_include_directories(Tempest PUBLIC "${WOWDECOMP_ROOT}")
add_library(StormLocks STATIC "${WOWDECOMP_ROOT}/Engine/Source/Storm/Source/Linux/SCritSect.cpp")
target_include_directories(StormLocks PUBLIC "${WOWDECOMP_ROOT}")
target_link_libraries(StormLocks PUBLIC Threads::Threads)
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Storm" Storm)
target_include_directories(Storm PUBLIC "${WOWDECOMP_ROOT}")

add_library(NativeLibraryFixture SHARED LibraryFixture.cpp)

add_executable(CompilerHelpersTest CompilerHelpersTest.cpp)
target_link_libraries(CompilerHelpersTest PRIVATE Tempest)
add_test(NAME CompilerHelpers COMMAND CompilerHelpersTest)

add_executable(ProcessorTest ProcessorTest.cpp)
target_link_libraries(ProcessorTest PRIVATE Os Threads::Threads)
add_test(NAME Processor COMMAND ProcessorTest)
set_tests_properties(Processor PROPERTIES TIMEOUT 20)

add_executable(StormStringCrtTest StormStringCrtTest.cpp)
target_link_libraries(StormStringCrtTest PRIVATE Storm Threads::Threads)
add_test(NAME StormStringCrt COMMAND StormStringCrtTest)
set_tests_properties(StormStringCrt PROPERTIES TIMEOUT 20)

add_executable(OsServicesTest OsServicesTest.cpp)
target_link_libraries(OsServicesTest PRIVATE Os Threads::Threads)
add_test(NAME OsServices COMMAND OsServicesTest $<TARGET_FILE:NativeLibraryFixture>)

add_executable(SecureRandomTest SecureRandomTest.cpp)
target_link_libraries(SecureRandomTest PRIVATE Os)
target_link_options(SecureRandomTest PRIVATE "-Wl,--wrap=getrandom")
add_test(NAME SecureRandom COMMAND SecureRandomTest)

add_executable(StormLocksTest StormLocksTest.cpp)
target_link_libraries(StormLocksTest PRIVATE StormLocks)
add_test(NAME StormLocks COMMAND StormLocksTest)
add_executable(SdlInputTest SdlInputTest.cpp)
target_link_libraries(SdlInputTest PRIVATE Os PkgConfig::SDL2 Threads::Threads)
target_link_options(SdlInputTest PRIVATE "-Wl,--wrap=SDL_SetWindowResizable")
add_test(NAME SdlInput COMMAND SdlInputTest)
set_tests_properties(CompilerHelpers OsServices SecureRandom StormLocks SdlInput PROPERTIES TIMEOUT 20)

set(WOWDECOMP_MSVC_BASE "" CACHE STRING "Optional Git baseline for MSVC-macro preprocessing checks (not binary matching)")
if (WOWDECOMP_MSVC_BASE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME MsvcPreprocessing COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_msvc_preprocessing.py --base ${WOWDECOMP_MSVC_BASE})
    set_tests_properties(MsvcPreprocessing PROPERTIES TIMEOUT 60)
endif ()
