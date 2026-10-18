#include "Engine/Source/Platform/Platform.h"

static_assert(WOW_PLATFORM_WINDOWS + WOW_PLATFORM_LINUX + WOW_PLATFORM_MACOS == 1);
static_assert(WOW_CPU_X86 + WOW_CPU_X86_64 + WOW_CPU_ARM64 == 1);
static_assert(WOW_COMPILER_MSVC + WOW_COMPILER_CLANG + WOW_COMPILER_GCC == 1);
static_assert(EXPECTED_OS && EXPECTED_CPU && EXPECTED_COMPILER);
static_assert(sizeof(void*) * 8 == EXPECTED_POINTER_BITS);
#if defined(WOWDECOMP_ARCH_64)
static_assert(sizeof(void*) == 8);
#elif defined(WOWDECOMP_ARCH_32)
static_assert(sizeof(void*) == 4);
#else
#error "Missing legacy layout definition."
#endif
int main() { return 0; }
