# Pinned source-only msdfgen core. No extensions, downloads, or system packages.
file(GLOB MSDFGEN_CORE_SOURCES CONFIGURE_DEPENDS "core/*.cpp")
add_library(GxuMsdfgen STATIC ${MSDFGEN_CORE_SOURCES})
target_include_directories(GxuMsdfgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(GxuMsdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
target_compile_features(GxuMsdfgen PUBLIC cxx_std_11)
