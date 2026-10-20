cmake_minimum_required(VERSION 3.13)
project(wowdecomp_portability_tests LANGUAGES C CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "These OS portability tests exercise the Linux implementations.")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(WOWDECOMP_ROOT "../.." ABSOLUTE)
include("${WOWDECOMP_ROOT}/cmake/WowPlatform.cmake")
wowdecomp_configure_platform()
find_package(Threads REQUIRED)
enable_testing()

add_executable(RayQueryCapabilityTest RayQueryCapabilityTest.cpp)
target_include_directories(RayQueryCapabilityTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME RayQueryCapability COMMAND RayQueryCapabilityTest)

add_executable(M2LightingSourceTest M2LightingSourceTest.cpp
    "${WOWDECOMP_ROOT}/Engine/Source/Model2/M2Light.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Model2/M2Model.cpp")
target_include_directories(M2LightingSourceTest PRIVATE "${WOWDECOMP_ROOT}")
target_compile_options(M2LightingSourceTest PRIVATE -ffunction-sections -fdata-sections)
target_compile_definitions(M2LightingSourceTest PRIVATE __stdcall= __fastcall=)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(M2LightingSourceTest PRIVATE WOWDECOMP_ARCH_64=1)
else ()
    target_compile_definitions(M2LightingSourceTest PRIVATE WOWDECOMP_ARCH_32=1)
endif ()
target_link_options(M2LightingSourceTest PRIVATE -Wl,--gc-sections)
target_link_libraries(M2LightingSourceTest PRIVATE Tempest BlizzardCore Storm BlizzardCore Threads::Threads)
add_test(NAME M2LightingSource COMMAND M2LightingSourceTest)

add_executable(AssetInventoryTest AssetInventoryTest.cpp)
target_include_directories(AssetInventoryTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME AssetInventory COMMAND AssetInventoryTest)

add_executable(ShadowContractsTest ShadowContractsTest.cpp)
target_include_directories(ShadowContractsTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME ShadowContracts COMMAND ShadowContractsTest)
add_test(NAME ShadowContractsClosedCompile COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DROOT=${WOWDECOMP_ROOT}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/ShadowContractsCompile.cpp"
    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/shadow-contract-compile"
    -P "${WOWDECOMP_ROOT}/cmake/CheckShadowContracts.cmake")

add_executable(CoordinateTest CoordinateTest.cpp "${WOWDECOMP_ROOT}/Engine/Source/Base/Coordinate.cpp")
target_include_directories(CoordinateTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME Coordinate COMMAND CoordinateTest)

foreach (name FarAdtProducer FarAdtPreparation)
    add_executable(${name}Test ${name}Test.cpp
        "${WOWDECOMP_ROOT}/WoW/Source/WorldClient/FarAdtProducer.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/FarResources.cpp")
    if (name STREQUAL "FarAdtPreparation")
        target_sources(${name}Test PRIVATE
            "${WOWDECOMP_ROOT}/WoW/Source/WorldClient/FarAdtPrepared.cpp"
            "${WOWDECOMP_ROOT}/WoW/Source/WorldClient/FarAdtPreparation.cpp")
    endif ()
    target_include_directories(${name}Test PRIVATE "${WOWDECOMP_ROOT}")
    target_compile_options(${name}Test PRIVATE -UNDEBUG)
    target_compile_definitions(${name}Test PRIVATE WOWDECOMP_FAR_TESTING=1)
    if (NOT MSVC)
        target_compile_definitions(${name}Test PRIVATE __stdcall= __fastcall=)
    endif ()
    target_link_libraries(${name}Test PRIVATE Threads::Threads)
    add_test(NAME ${name} COMMAND ${name}Test)
endforeach ()
add_executable(FarTerrainTest FarTerrainTest.cpp)
target_include_directories(FarTerrainTest PRIVATE "${WOWDECOMP_ROOT}")
target_compile_options(FarTerrainTest PRIVATE -UNDEBUG)
target_link_libraries(FarTerrainTest PRIVATE Tempest)
add_test(NAME FarTerrain COMMAND FarTerrainTest)

add_executable(NightDarknessTest NightDarknessTest.cpp)
target_include_directories(NightDarknessTest PRIVATE "${WOWDECOMP_ROOT}")
add_test(NAME NightDarkness COMMAND NightDarknessTest)
add_executable(TimePreviewTest TimePreviewTest.cpp
    "${WOWDECOMP_ROOT}/WoW/Source/Game/GameTime.cpp"
    "${WOWDECOMP_ROOT}/WoW/Common/WowTime.cpp")
target_include_directories(TimePreviewTest PRIVATE "${WOWDECOMP_ROOT}")
target_compile_options(TimePreviewTest PRIVATE -ffunction-sections -fdata-sections)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(TimePreviewTest PRIVATE __stdcall=)
endif ()
target_link_options(TimePreviewTest PRIVATE -Wl,--gc-sections)
target_link_libraries(TimePreviewTest PRIVATE Storm)
add_test(NAME TimePreview COMMAND TimePreviewTest)

add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Os" Os)
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Tempest" Tempest)
# Production modules normally inherit the repository root as CMAKE_SOURCE_DIR.
# This independent test project supplies it without changing the client build.
target_include_directories(Os PUBLIC "${WOWDECOMP_ROOT}")
target_include_directories(Tempest PUBLIC "${WOWDECOMP_ROOT}")
add_library(StormLocks STATIC "${WOWDECOMP_ROOT}/Engine/Source/Storm/Source/Posix/SCritSect.cpp")
target_include_directories(StormLocks PUBLIC "${WOWDECOMP_ROOT}")
target_link_libraries(StormLocks PUBLIC Threads::Threads)
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Storm" Storm)
target_include_directories(Storm PUBLIC "${WOWDECOMP_ROOT}")
add_executable(StormSyncTest StormSyncTest.cpp)
target_link_libraries(StormSyncTest PRIVATE Storm Threads::Threads)
add_test(NAME StormSync COMMAND StormSyncTest)

add_executable(RootLayoutTest RootLayoutTest.cpp
    "${WOWDECOMP_ROOT}/Engine/Source/Base/CDataAllocator.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Base/Coordinate.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Base/Status.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Frame/CLayoutFrame.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Frame/CFramePoint.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/FrameXML/LoadXML.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/FrameXML/XMLTree.cpp")
target_compile_definitions(RootLayoutTest PRIVATE WOWDECOMP_ARCH_64=1)
target_compile_options(RootLayoutTest PRIVATE -ffunction-sections -fdata-sections)
target_link_options(RootLayoutTest PRIVATE -Wl,--gc-sections)
target_link_libraries(RootLayoutTest PRIVATE Storm Tempest expat)
add_test(NAME RootLayout COMMAND RootLayoutTest)
set_tests_properties(StormSync PROPERTIES TIMEOUT 20)
add_executable(StormThreadTest StormThreadTest.cpp)
target_link_libraries(StormThreadTest PRIVATE Storm Threads::Threads)
target_link_options(StormThreadTest PRIVATE "-Wl,--wrap=pthread_create")
add_test(NAME StormThread COMMAND StormThreadTest)
set_tests_properties(StormThread PROPERTIES TIMEOUT 20)
add_subdirectory("${WOWDECOMP_ROOT}/ThirdParty" ThirdParty EXCLUDE_FROM_ALL)
# Exercise bundled FreeType without requiring the optional MSDF renderer.
find_file(WOWDECOMP_FONT_TEST_FACE DejaVuSans.ttf
    PATHS /usr/share/fonts/dejavu-sans-fonts /usr/share/fonts/truetype/dejavu)
if (NOT WOWDECOMP_FONT_TEST_FACE)
    message(FATAL_ERROR "Set WOWDECOMP_FONT_TEST_FACE to an installed DejaVuSans.ttf for the composite-glyph regression")
endif ()
add_executable(GxuFontFreetypeTest "${WOWDECOMP_ROOT}/Tools/Fonts/GxuFontFreetypeTest.cpp")
target_link_libraries(GxuFontFreetypeTest PRIVATE freetype)
add_test(NAME GxuFontFreetype COMMAND GxuFontFreetypeTest "${WOWDECOMP_FONT_TEST_FACE}")
set_tests_properties(GxuFontFreetype PROPERTIES TIMEOUT 30)
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/BlizzardCore" BlizzardCore)
target_include_directories(BlizzardCore PUBLIC "${WOWDECOMP_ROOT}")
add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/SoundEngine" SoundEngine)
target_include_directories(SoundEngine PRIVATE "${WOWDECOMP_ROOT}")
add_executable(NativeSoundTest NativeSoundTest.cpp)
target_include_directories(NativeSoundTest PRIVATE
    "${WOWDECOMP_ROOT}/Engine/Source/SoundEngine/fmodex/sdl/src")
target_link_libraries(NativeSoundTest PRIVATE fmodex)
add_test(NAME NativeSound COMMAND NativeSoundTest)
set_tests_properties(NativeSound PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 20)
add_test(NAME NativeSoundUnavailable COMMAND NativeSoundTest --unavailable)
set_tests_properties(NativeSoundUnavailable PROPERTIES
    ENVIRONMENT "SDL_AUDIODRIVER=wowdecomp-invalid-driver" TIMEOUT 20)
add_executable(BlizzardPrimitivesTest BlizzardPrimitivesTest.cpp)
target_link_libraries(BlizzardPrimitivesTest PRIVATE BlizzardCore Threads::Threads)
add_test(NAME BlizzardPrimitives COMMAND BlizzardPrimitivesTest)
set_tests_properties(BlizzardPrimitives PROPERTIES TIMEOUT 20)
add_executable(IOUnitTest IOUnitTest.cpp)
target_compile_definitions(IOUnitTest PRIVATE WOWDECOMP_ARCH_64=1)
target_link_libraries(IOUnitTest PRIVATE BlizzardCore Storm BlizzardCore)
target_link_options(IOUnitTest PRIVATE
    "-Wl,--wrap=_ZN8Blizzard4File12SetLastErrorEi"
    "-Wl,--wrap=_ZN11System_File6Native16GetFileAlignmentEPKcRj"
    "-Wl,--wrap=_ZN8Blizzard6Memory8AllocateEj")
add_test(NAME IOUnit COMMAND IOUnitTest)
set_tests_properties(IOUnit PROPERTIES TIMEOUT 30)
add_executable(FileBackendTest FileBackendTest.cpp)
target_link_libraries(FileBackendTest PRIVATE BlizzardCore Storm)
target_link_options(FileBackendTest PRIVATE "-Wl,--wrap=_ZN8Blizzard4File12SetLastErrorEi")
add_test(NAME FileBackend COMMAND FileBackendTest)
set_tests_properties(FileBackend PROPERTIES TIMEOUT 20)
add_executable(RendererAssetInventory "${WOWDECOMP_ROOT}/Tools/Vulkan/RendererAssetInventory.cpp")
target_include_directories(RendererAssetInventory PRIVATE "${WOWDECOMP_ROOT}")
target_link_libraries(RendererAssetInventory PRIVATE BlizzardCore Storm BlizzardCore Threads::Threads)
add_executable(RendererAssetInventoryTest RendererAssetInventoryTest.cpp)
target_include_directories(RendererAssetInventoryTest PRIVATE "${WOWDECOMP_ROOT}")
target_link_libraries(RendererAssetInventoryTest PRIVATE BlizzardCore Storm BlizzardCore Threads::Threads)
add_dependencies(RendererAssetInventoryTest RendererAssetInventory)
add_test(NAME RendererAssetInventory COMMAND RendererAssetInventoryTest "$<TARGET_FILE:RendererAssetInventory>")

add_executable(MpqReadTest MpqReadTest.cpp)
target_compile_definitions(MpqReadTest PRIVATE WOWDECOMP_ARCH_64=1)
target_link_libraries(MpqReadTest PRIVATE BlizzardCore Storm BlizzardCore Threads::Threads)
add_test(NAME MpqRead COMMAND MpqReadTest)
set_tests_properties(MpqRead PROPERTIES TIMEOUT 20)
add_executable(BlizzardTlsTest BlizzardTlsTest.cpp)
target_link_libraries(BlizzardTlsTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME BlizzardTls COMMAND BlizzardTlsTest)
set_tests_properties(BlizzardTls PROPERTIES TIMEOUT 20)
add_executable(BlizzardHardwareTest BlizzardHardwareTest.cpp)
target_link_libraries(BlizzardHardwareTest PRIVATE BlizzardCore Threads::Threads)
target_link_options(BlizzardHardwareTest PRIVATE "-Wl,--wrap=_ZN15System_Hardware9ReadCPUIDEPNS_9CPUIDInfoE")
add_test(NAME BlizzardHardware COMMAND BlizzardHardwareTest)
set_tests_properties(BlizzardHardware PROPERTIES TIMEOUT 20)
add_executable(BlizzardThreadTest BlizzardThreadTest.cpp)
target_link_libraries(BlizzardThreadTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME BlizzardThread COMMAND BlizzardThreadTest)
set_tests_properties(BlizzardThread PROPERTIES TIMEOUT 20)
add_executable(BlizzardTimeTest BlizzardTimeTest.cpp)
target_link_libraries(BlizzardTimeTest PRIVATE BlizzardCore Threads::Threads)
add_test(NAME BlizzardTime COMMAND BlizzardTimeTest)
set_tests_properties(BlizzardTime PROPERTIES TIMEOUT 20)
add_executable(BlizzardAssertTest BlizzardAssertTest.cpp)
target_link_libraries(BlizzardAssertTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME BlizzardAssert COMMAND BlizzardAssertTest "${CMAKE_CURRENT_BINARY_DIR}/assert-fixtures")
set_tests_properties(BlizzardAssert PROPERTIES TIMEOUT 40)
add_executable(BlizzardLogTest BlizzardLogTest.cpp)
target_link_libraries(BlizzardLogTest PRIVATE BlizzardCore Storm Threads::Threads)
target_link_options(BlizzardLogTest PRIVATE "-Wl,--wrap=pthread_create")
add_test(NAME BlizzardLog COMMAND BlizzardLogTest)
set_tests_properties(BlizzardLog PROPERTIES TIMEOUT 25)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_executable(LegacyUrlTest LegacyUrlTest.cpp)
target_link_libraries(LegacyUrlTest PRIVATE Os BlizzardCore Storm Threads::Threads)
add_test(NAME LegacyUrl COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/blizzard_url_fixture.py $<TARGET_FILE:LegacyUrlTest>)
set_tests_properties(LegacyUrl PROPERTIES TIMEOUT 40)
add_executable(BlizzardUrlTest BlizzardUrlTest.cpp)
target_link_libraries(BlizzardUrlTest PRIVATE BlizzardCore Storm Threads::Threads)
target_link_options(BlizzardUrlTest PRIVATE
    "-Wl,--wrap=curl_multi_info_read"
    "-Wl,--wrap=_ZN8Blizzard6Thread14CreateDetachedEPFjPvES1_PKc")
add_test(NAME BlizzardUrl COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/blizzard_url_fixture.py $<TARGET_FILE:BlizzardUrlTest>)
set_tests_properties(BlizzardUrl PROPERTIES TIMEOUT 40)
add_executable(StreamingStorageRangesTest StreamingStorageRangesTest.cpp)
target_link_libraries(StreamingStorageRangesTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME StreamingStorageRanges COMMAND StreamingStorageRangesTest)
set_tests_properties(StreamingStorageRanges PROPERTIES TIMEOUT 20)
add_executable(StreamingQueuesTest StreamingQueuesTest.cpp)
target_compile_definitions(StreamingQueuesTest PRIVATE WOWDECOMP_ARCH_64=1)
target_link_libraries(StreamingQueuesTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME StreamingQueues COMMAND StreamingQueuesTest)
set_tests_properties(StreamingQueues PROPERTIES TIMEOUT 20)
add_executable(LegacyNetAddressTest LegacyNetAddressTest.cpp)
target_link_libraries(LegacyNetAddressTest PRIVATE Os Storm)
add_test(NAME LegacyNetAddress COMMAND LegacyNetAddressTest)
set_tests_properties(LegacyNetAddress PROPERTIES TIMEOUT 10)
add_executable(LegacySystemTest LegacySystemTest.cpp)
target_link_libraries(LegacySystemTest PRIVATE Os)
target_link_options(LegacySystemTest PRIVATE
    -Wl,--wrap=sysconf,--wrap=sysinfo,--wrap=gethostname,--wrap=uname,--wrap=getpwuid_r,--wrap=readlink)
add_test(NAME LegacySystem COMMAND LegacySystemTest)
set_tests_properties(LegacySystem PROPERTIES TIMEOUT 20)
add_executable(LegacyOsFileTest LegacyOsFileTest.cpp)
target_link_libraries(LegacyOsFileTest PRIVATE Os Storm)
target_link_options(LegacyOsFileTest PRIVATE "-Wl,--wrap=fdopendir")
add_test(NAME LegacyOsFile COMMAND LegacyOsFileTest)
set_tests_properties(LegacyOsFile PROPERTIES TIMEOUT 20)
add_executable(NativeVersionTest NativeVersionTest.cpp
    "${WOWDECOMP_ROOT}/WoW/Source/WowSvcs/WowSvcsClient/ChecksumExecutables.cpp")
target_link_libraries(NativeVersionTest PRIVATE Os BlizzardCore Storm)
target_link_options(NativeVersionTest PRIVATE
    "-Wl,--wrap=_Z26SErrDisplayAppFatalMessagePKcS0_i"
    "-Wl,--wrap=_ZN4SHA16FinishERNS_6DigestE"
    "-Wl,--wrap=_Z10OsReadFileP9HOSFILE__PvjPj"
    "-Wl,--wrap=_Z13OsGetFileSizeP9HOSFILE__"
    "-Wl,--wrap=_ZN8Blizzard4File12SetLastErrorEi")
add_test(NAME NativeVersion COMMAND NativeVersionTest)
set_tests_properties(NativeVersion PROPERTIES TIMEOUT 20)
add_executable(WowSocketTest WowSocketTest.cpp
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/Linux/Socket.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/Posix/Connection.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/Posix/ConnectionNet.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/Posix/Callbacks.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/Posix/Workers.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/WowServices/WDataStore.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Base/CDataStore.cpp"
    "${WOWDECOMP_ROOT}/Engine/Source/Storm/Source/SArc4.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/ObjectAlloc/ObjectAlloc.cpp")
target_compile_definitions(WowSocketTest PRIVATE WOWDECOMP_ARCH_64=1)
target_compile_options(WowSocketTest PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(WowSocketTest PRIVATE Storm BlizzardCore Os Threads::Threads)
target_link_options(WowSocketTest PRIVATE -Wl,--gc-sections
    -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=poll -Wl,--wrap=pthread_create)
add_test(NAME WowSocket COMMAND WowSocketTest)
set_tests_properties(WowSocket PROPERTIES TIMEOUT 35)
add_executable(NativeStartupTest NativeStartupTest.cpp
    "${WOWDECOMP_ROOT}/WoW/Source/Native/Arch/X86/FloatingPoint.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/Linux/Startup.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/Posix/BotCheck.cpp")
target_link_libraries(NativeStartupTest PRIVATE Storm)
add_test(NAME NativeStartup COMMAND NativeStartupTest)
set_tests_properties(NativeStartup PROPERTIES TIMEOUT 20)

add_executable(NativeUnavailableTest NativeUnavailableTest.cpp
    "${WOWDECOMP_ROOT}/WoW/Source/Battlenet/Battlenet.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/Battlenet/BSN/BSNCpp.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/Battlenet/Unavailable/Library.cpp"
    "${WOWDECOMP_ROOT}/WoW/Source/Glue/Unavailable/PatchDownloadGlue.cpp")
target_link_libraries(NativeUnavailableTest PRIVATE Storm)
add_test(NAME NativeUnavailable COMMAND NativeUnavailableTest)
set_tests_properties(NativeUnavailable PROPERTIES TIMEOUT 10)
add_executable(LocalOnlyStreamingTest LocalOnlyStreamingTest.cpp)
target_link_libraries(LocalOnlyStreamingTest PRIVATE BlizzardCore Storm Threads::Threads)
add_test(NAME LocalOnlyStreaming COMMAND LocalOnlyStreamingTest)
set_tests_properties(LocalOnlyStreaming PROPERTIES TIMEOUT 20)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(BlizzardCore PRIVATE WOWDECOMP_ARCH_64=1)
else ()
    target_compile_definitions(BlizzardCore PRIVATE WOWDECOMP_ARCH_32=1)
endif ()

list(APPEND CMAKE_MODULE_PATH "${WOWDECOMP_ROOT}/cmake")
find_package(DXVKNative QUIET)
if (DXVKNative_FOUND)
    add_subdirectory("${WOWDECOMP_ROOT}/Engine/Source/Gx" Gx)
    target_include_directories(Gx PUBLIC "${WOWDECOMP_ROOT}")
else ()
    message(STATUS "DXVK Native not found; only the CPU portability tests will be built")
endif ()

# Exercise the real public selection API without linking or initializing GPUs.
# Linux section GC isolates these functions, like the socket contract tests above.
foreach (backend dxvk vulkan both)
    add_executable(GxBackend-${backend} GxBackendTest.cpp
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/Gx.cpp"
        "${WOWDECOMP_ROOT}/Engine/Source/Gx/DeviceSelection.cpp")
    target_include_directories(GxBackend-${backend} PRIVATE "${WOWDECOMP_ROOT}")
    target_link_libraries(GxBackend-${backend} PRIVATE Storm)
    target_compile_options(GxBackend-${backend} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(GxBackend-${backend} PRIVATE -Wl,--gc-sections)
    if (NOT backend STREQUAL "vulkan")
        target_compile_definitions(GxBackend-${backend} PRIVATE WOWDECOMP_DXVK=1)
    endif ()
    if (NOT backend STREQUAL "dxvk")
        target_compile_definitions(GxBackend-${backend} PRIVATE WOWDECOMP_VULKAN=1)
    endif ()
    add_test(NAME GxBackend-${backend} COMMAND GxBackend-${backend})
endforeach ()

add_library(NativeLibraryFixture SHARED LibraryFixture.cpp)

add_executable(CDataRecyclerTest CDataRecyclerTest.cpp
    "${WOWDECOMP_ROOT}/Engine/Source/Base/CDataRecycler.cpp")
target_link_libraries(CDataRecyclerTest PRIVATE Storm Threads::Threads)
target_compile_options(CDataRecyclerTest PRIVATE -O3 -fstrict-aliasing)
add_test(NAME CDataRecycler COMMAND CDataRecyclerTest)
set_tests_properties(CDataRecycler PROPERTIES TIMEOUT 20)
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

add_executable(OrderedPrintTest OrderedPrintTest.cpp)
target_link_libraries(OrderedPrintTest PRIVATE Storm)
add_test(NAME OrderedPrint COMMAND OrderedPrintTest)
set_tests_properties(OrderedPrint PROPERTIES TIMEOUT 20)

add_executable(StormRuntimeTest StormRuntimeTest.cpp)
target_link_libraries(StormRuntimeTest PRIVATE Storm)
add_test(NAME StormRuntime COMMAND StormRuntimeTest)
set_tests_properties(StormRuntime PROPERTIES TIMEOUT 20)

add_executable(StormDiagnosticsTest StormDiagnosticsTest.cpp)
target_link_libraries(StormDiagnosticsTest PRIVATE Storm Threads::Threads)
add_test(NAME StormDiagnostics COMMAND StormDiagnosticsTest)
set_tests_properties(StormDiagnostics PROPERTIES TIMEOUT 45)

add_executable(LogDirectoryTest LogDirectoryTest.cpp)
target_link_libraries(LogDirectoryTest PRIVATE Os Storm Threads::Threads)
add_test(NAME LogDirectory COMMAND LogDirectoryTest)
set_tests_properties(LogDirectory PROPERTIES TIMEOUT 20)

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
add_executable(SdlServicesTest SdlServicesTest.cpp)
target_link_libraries(SdlServicesTest PRIVATE Os PkgConfig::SDL2 Threads::Threads)
find_program(SDL_SERVICES_BWRAP bwrap)
if (NOT SDL_SERVICES_BWRAP)
    message(FATAL_ERROR "SdlServicesTest requires bwrap device isolation")
endif ()
add_test(NAME SdlServices COMMAND "${SDL_SERVICES_BWRAP}"
    --ro-bind / / --dev /dev --tmpfs /sys --unshare-net -- $<TARGET_FILE:SdlServicesTest>)
set_tests_properties(SdlServices PROPERTIES TIMEOUT 20)

set(WOWDECOMP_MSVC_BASE "" CACHE STRING "Optional Git baseline for MSVC-macro preprocessing checks (not binary matching)")
if (WOWDECOMP_MSVC_BASE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME MsvcPreprocessing COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_msvc_preprocessing.py --base ${WOWDECOMP_MSVC_BASE})
    set_tests_properties(MsvcPreprocessing PROPERTIES TIMEOUT 60)
endif ()
