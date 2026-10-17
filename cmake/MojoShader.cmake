# The shader frontend is deliberately isolated from MojoShader's renderer glue.
# Obtain the reviewed revision locally; configuration never installs or fetches code.
set(WOWDECOMP_MOJOSHADER_SOURCE "" CACHE PATH "MojoShader source checkout (ad5dff84830c2863c841f4b1f4e3df78c705b383)")
if (NOT EXISTS "${WOWDECOMP_MOJOSHADER_SOURCE}/mojoshader.h")
    message(FATAL_ERROR "Set WOWDECOMP_MOJOSHADER_SOURCE to a MojoShader checkout at ad5dff84830c2863c841f4b1f4e3df78c705b383")
endif ()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WOWDECOMP_MOJOSHADER_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE MOJOSHADER_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE MOJOSHADER_REVISION_RESULT)
if (NOT MOJOSHADER_REVISION_RESULT EQUAL 0 OR NOT MOJOSHADER_REVISION STREQUAL "ad5dff84830c2863c841f4b1f4e3df78c705b383")
    message(FATAL_ERROR "MojoShader must use the reviewed revision ad5dff84830c2863c841f4b1f4e3df78c705b383")
endif ()
set(MOJOSHADER_FRONTEND_SOURCES mojoshader.c mojoshader_common.c
    profiles/mojoshader_profile_common.c profiles/mojoshader_profile_spirv.c
    profiles/mojoshader_profile_hlsl.c)
list(TRANSFORM MOJOSHADER_FRONTEND_SOURCES PREPEND "${WOWDECOMP_MOJOSHADER_SOURCE}/")
add_library(GxMojoShader STATIC ${MOJOSHADER_FRONTEND_SOURCES})
target_include_directories(GxMojoShader PUBLIC "${WOWDECOMP_MOJOSHADER_SOURCE}")
target_compile_definitions(GxMojoShader PUBLIC MOJOSHADER_NO_VERSION_INCLUDE)
target_compile_definitions(GxMojoShader PRIVATE
    SUPPORT_PROFILE_D3D=0 SUPPORT_PROFILE_BYTECODE=0 SUPPORT_PROFILE_HLSL=1
    SUPPORT_PROFILE_GLSL=0 SUPPORT_PROFILE_GLSL120=0 SUPPORT_PROFILE_GLSLES=0
    SUPPORT_PROFILE_GLSLES3=0 SUPPORT_PROFILE_ARB1=0 SUPPORT_PROFILE_ARB1_NV=0
    SUPPORT_PROFILE_METAL=0 SUPPORT_PROFILE_GLSPIRV=0 SUPPORT_PROFILE_SPIRV=1)
if (UNIX)
    target_link_libraries(GxMojoShader PUBLIC m)
endif ()
