cmake_minimum_required(VERSION 3.13)
project(wowdecomp_posix_tests LANGUAGES CXX)
get_filename_component(WOWDECOMP_ROOT "../.." ABSOLUTE)
include("${WOWDECOMP_ROOT}/cmake/WowPlatform.cmake")
wowdecomp_configure_platform()
if (NOT WOWDECOMP_POSIX)
    message(FATAL_ERROR "This test project exercises the shared POSIX providers")
endif ()
find_package(Threads REQUIRED)
enable_testing()
add_executable(CloudNoiseTest CloudNoiseTest.cpp)
target_compile_features(CloudNoiseTest PRIVATE cxx_std_17)
target_include_directories(CloudNoiseTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME CloudNoise COMMAND CloudNoiseTest)
add_library(PosixServices STATIC
    "${WOWDECOMP_ROOT}/Engine/Source/Os/Posix/TimeManager.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Os/Posix/Tls.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Os/Posix/OsLibrary.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Storm/Source/Posix/SCritSect.cpp")
target_compile_features(PosixServices PUBLIC cxx_std_17)
target_include_directories(PosixServices PUBLIC "${WOWDECOMP_ROOT}")
target_link_libraries(PosixServices PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
add_library(PosixLibraryFixture SHARED LibraryFixture.cpp)
add_executable(PosixServicesTest PosixServicesTest.cpp)
target_link_libraries(PosixServicesTest PRIVATE PosixServices)
add_test(NAME PosixServices COMMAND PosixServicesTest $<TARGET_FILE:PosixLibraryFixture>)
set_tests_properties(PosixServices PROPERTIES TIMEOUT 20)
# Compile the real lifecycle against its declared OS leaves. There is no fake
# scheduling implementation here; integration/runtime checks belong to the OS PR.
add_library(PosixThreadContract OBJECT
    "${WOWDECOMP_ROOT}/Engine/Source/BlizzardCore/Source/System/Thread/Posix/Thread.cpp")
target_compile_features(PosixThreadContract PRIVATE cxx_std_17)
target_include_directories(PosixThreadContract PRIVATE "${WOWDECOMP_ROOT}")
target_link_libraries(PosixThreadContract PRIVATE Threads::Threads)
