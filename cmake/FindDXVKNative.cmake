set(DXVK_NATIVE_ROOT "" CACHE PATH "DXVK Native installation prefix")
find_path(DXVKNative_INCLUDE_DIR d3d9.h
    HINTS "${DXVK_NATIVE_ROOT}/include/dxvk")
find_library(DXVKNative_D3D9_LIBRARY NAMES dxvk_d3d9
    HINTS "${DXVK_NATIVE_ROOT}/lib" "${DXVK_NATIVE_ROOT}/lib64")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DXVKNative DEFAULT_MSG
    DXVKNative_INCLUDE_DIR DXVKNative_D3D9_LIBRARY)

if (DXVKNative_FOUND AND NOT TARGET DXVKNative::D3D9)
    add_library(DXVKNative::D3D9 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(DXVKNative::D3D9 PROPERTIES
        IMPORTED_LOCATION "${DXVKNative_D3D9_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXVKNative_INCLUDE_DIR}"
    )
endif ()
mark_as_advanced(DXVKNative_INCLUDE_DIR DXVKNative_D3D9_LIBRARY)
