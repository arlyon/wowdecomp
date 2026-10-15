#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_HARDWARE_HARDWARE_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_HARDWARE_HARDWARE_H

#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

namespace Blizzard {
namespace Hardware {

struct ProcessorFeatures {
    /* 0000 */ uint32_t m_vendor;
    /* 0004 */ uint32_t m_features;
    /* 0008 */ uint32_t m_coreCount;
    /* 000C */ uint32_t m_processorCount;
};

uint32_t GetProcessorFeatures();

}
}

namespace System_Hardware {

struct CPUIDInfo {
    /* 0000 */ uint32_t m_vendor[3];
    /* 000C */ uint32_t m_maxLeaf;
    /* 0010 */ uint32_t m_leaf1Ebx;
    /* 0014 */ uint32_t m_leaf1Edx;
    /* 0018 */ uint32_t m_leaf4Eax;
    /* 001C */ uint32_t m_maxExtLeaf;
    /* 0020 */ uint32_t m_ext1Ecx;
    /* 0024 */ uint32_t m_ext1Edx;
    /* 0028 */ uint32_t m_ext4Eax;
    /* 002C */ uint32_t m_ext8Ecx;
};

extern Blizzard::Hardware::ProcessorFeatures s_processorFeatures;
extern bool s_initialized;
extern Blizzard::Lock::Atomic s_initCount;

void InitProcessorFeatures();
bool ReadCPUID(CPUIDInfo* info);
#if !defined(_WIN32)
uint32_t GetProcessorCount();
#endif

}

#endif
