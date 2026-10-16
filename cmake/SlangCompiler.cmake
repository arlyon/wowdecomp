set(WOWDECOMP_SLANG_SOURCE "" CACHE PATH "Slang source checkout for the local compiler headers")
set(WOWDECOMP_SLANGC "" CACHE FILEPATH "Path to the locally built Slang shader compiler")
if (NOT EXISTS "${WOWDECOMP_SLANG_SOURCE}/include/slang.h" OR NOT EXISTS "${WOWDECOMP_SLANGC}")
    message(FATAL_ERROR "Set WOWDECOMP_SLANG_SOURCE and WOWDECOMP_SLANGC to the local Slang v2026.4.2 source/build")
endif ()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WOWDECOMP_SLANG_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE SLANG_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE SLANG_REVISION_RESULT)
if (NOT SLANG_REVISION_RESULT EQUAL 0 OR NOT SLANG_REVISION STREQUAL "5cde9ea5ff3b6c657978de54d2241a86cb3528b1")
    message(FATAL_ERROR "Slang must use the reviewed v2026.4.2 revision 5cde9ea5ff3b6c657978de54d2241a86cb3528b1")
endif ()
get_filename_component(SLANG_BIN_DIRECTORY "${WOWDECOMP_SLANGC}" DIRECTORY)
find_library(WOWDECOMP_SLANG_LIBRARY NAMES slang-compiler
    HINTS "${SLANG_BIN_DIRECTORY}/../lib" NO_DEFAULT_PATH)
if (NOT WOWDECOMP_SLANG_LIBRARY)
    message(FATAL_ERROR "Cannot find the Slang compiler library alongside slangc")
endif ()
add_library(GxSlangCompiler SHARED IMPORTED GLOBAL)
set_target_properties(GxSlangCompiler PROPERTIES
    IMPORTED_LOCATION "${WOWDECOMP_SLANG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WOWDECOMP_SLANG_SOURCE}/include")
