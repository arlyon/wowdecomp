#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

// ARM64 does not implement the x86 CPUID leaves. Do not advertise x86 SIMD
// capabilities merely because selected algorithms have NEON equivalents.
bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    if (info) *info = {};
    return false;
}
