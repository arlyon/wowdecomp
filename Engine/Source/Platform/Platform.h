#ifndef ENGINE_SOURCE_PLATFORM_PLATFORM_H
#define ENGINE_SOURCE_PLATFORM_PLATFORM_H

// Compiler target macros remain correct for standalone compilation and cross builds.
#define WOW_PLATFORM_WINDOWS 0
#define WOW_PLATFORM_LINUX 0
#define WOW_PLATFORM_MACOS 0
#if defined(_WIN32)
#undef WOW_PLATFORM_WINDOWS
#define WOW_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#error "Only desktop macOS is supported."
#endif
#undef WOW_PLATFORM_MACOS
#define WOW_PLATFORM_MACOS 1
#elif defined(__linux__)
#undef WOW_PLATFORM_LINUX
#define WOW_PLATFORM_LINUX 1
#else
#error "Unsupported target OS."
#endif
#define WOW_PLATFORM_POSIX (WOW_PLATFORM_LINUX || WOW_PLATFORM_MACOS)

#define WOW_CPU_X86 0
#define WOW_CPU_X86_64 0
#define WOW_CPU_ARM64 0
#if defined(_M_X64) || defined(__x86_64__)
#undef WOW_CPU_X86_64
#define WOW_CPU_X86_64 1
#elif defined(_M_IX86) || defined(__i386__)
#undef WOW_CPU_X86
#define WOW_CPU_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#undef WOW_CPU_ARM64
#define WOW_CPU_ARM64 1
#else
#error "Unsupported target CPU."
#endif
#define WOW_CPU_X86_FAMILY (WOW_CPU_X86 || WOW_CPU_X86_64)

// Compiler family is independent of the OS and MSVC-compatible ABI (_MSC_VER).
#define WOW_COMPILER_CLANG 0
#define WOW_COMPILER_MSVC 0
#define WOW_COMPILER_GCC 0
#if defined(__clang__)
#undef WOW_COMPILER_CLANG
#define WOW_COMPILER_CLANG 1
#elif defined(_MSC_VER)
#undef WOW_COMPILER_MSVC
#define WOW_COMPILER_MSVC 1
#elif defined(__GNUC__)
#undef WOW_COMPILER_GCC
#define WOW_COMPILER_GCC 1
#else
#error "Unsupported compiler."
#endif

#endif
