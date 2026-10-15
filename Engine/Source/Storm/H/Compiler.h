#ifndef ENGINE_SOURCE_STORM_H_COMPILER_H
#define ENGINE_SOURCE_STORM_H_COMPILER_H

#if defined(_WIN32)
#define STORMAPI __stdcall
#else
#define STORMAPI
#endif

// Expand to the original MSVC expression without adding a helper call or
// shifting the __LINE__ values embedded in stpl.h's allocator diagnostics.
#if defined(_MSC_VER)
#define STORM_TYPE_NAME(type) typeid(type).raw_name()
#define STORM_ENUM32_MAX 0xFFFFFFFFFFFFFFFF
#else
#define STORM_TYPE_NAME(type) typeid(type).name()
#define STORM_ENUM32_MAX 0xFFFFFFFF
#endif

#endif
