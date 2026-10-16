set(WOWDECOMP_SLANGC "" CACHE FILEPATH "Path to the locally built Slang shader compiler")
if (NOT EXISTS "${WOWDECOMP_SLANGC}")
    message(FATAL_ERROR "Set WOWDECOMP_SLANGC to slangc; authored Vulkan shaders are compiled from Slang")
endif ()
set(WOWDECOMP_VULKAN_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders")
set(GX_FIXED_SHADER "${WOWDECOMP_ROOT}/Engine/Source/Gx/CGxDeviceVulkan/Shaders/FixedFunction.slang")
set(GX_SLANG_OUTPUTS)
foreach (stage vertex fragment)
    if (stage STREQUAL "vertex")
        set(entry vertexMain)
        set(extension vert)
    else ()
        set(entry pixelMain)
        set(extension frag)
    endif ()
    set(output "${WOWDECOMP_VULKAN_SHADER_DIR}/fixed.${extension}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WOWDECOMP_VULKAN_SHADER_DIR}"
        COMMAND "${WOWDECOMP_SLANGC}" "${GX_FIXED_SHADER}" -entry "${entry}" -stage "${stage}"
            -target spirv -profile spirv_1_3 -matrix-layout-row-major -o "${output}"
        DEPENDS "${GX_FIXED_SHADER}" "${WOWDECOMP_SLANGC}"
        VERBATIM)
    list(APPEND GX_SLANG_OUTPUTS "${output}")
endforeach ()
add_custom_target(GxSlangShaders DEPENDS ${GX_SLANG_OUTPUTS})
