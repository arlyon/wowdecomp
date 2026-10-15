#ifndef ENGINE_SOURCE_STORM_H_COMPILER_H
#define ENGINE_SOURCE_STORM_H_COMPILER_H

#if defined(_WIN32)
#define STORMAPI __stdcall
#define STORM_FASTCALL __fastcall
#define STORM_ALLOCA(size) _alloca(size)
#define STORM_VSNPRINTF _vsnprintf
#else
#define STORMAPI
#define STORM_FASTCALL
#define STORM_ALLOCA(size) __builtin_alloca(size)
#define STORM_VSNPRINTF std::vsnprintf
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
