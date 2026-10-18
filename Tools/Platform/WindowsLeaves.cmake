get_filename_component(root "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(paths
    Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.cpp
    Engine/Source/BlizzardCore/Source/System/Debug/Win64/Debug.cpp
    Engine/Source/BlizzardCore/Source/System/Hardware/Win64/Hardware.cpp
    Engine/Source/BlizzardCore/Source/System/Time/Win64/Time.cpp
    Engine/Source/BlizzardCore/Source/System/File/Win64/File.cpp
    Engine/Source/BlizzardCore/Source/System/Thread/ThreadPools.cpp
    Engine/Source/BlizzardCore/Source/System/Thread/Win64/ThreadPools.cpp
    Engine/Source/BlizzardCore/Source/System/Log/Log.cpp
    Engine/Source/BlizzardCore/Source/System/Log/Win64/Log.cpp
    Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.cpp
    Engine/Source/BlizzardCore/Source/Packages/iounit/Win64/IOFileUnit.cpp
    Engine/Source/BlizzardCore/Source/Packages/Streaming/Win64/Manifests.cpp
    Engine/Source/BlizzardCore/Source/Packages/Streaming/Win64/Storage.cpp
    Engine/Source/BlizzardCore/Source/Packages/Streaming/Win64/Streaming.cpp
    WoW/Source/WowServices/WowConnection.cpp
    WoW/Source/WowServices/WowConnectionNet.cpp
    WoW/Source/WowServices/Win64/WowConnection.cpp
    WoW/Source/WowServices/Win64/WowConnectionNet.cpp
    WoW/Source/WowServices/Win64/BattlenetLogin.cpp)
execute_process(COMMAND "${GIT_EXECUTABLE}" diff --exit-code main -- ${paths}
    WORKING_DIRECTORY "${root}" RESULT_VARIABLE result OUTPUT_VARIABLE changes ERROR_VARIABLE error)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Windows originals differ from main: ${changes}${error}")
endif ()
