// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cstdint>
#include <cstring>

#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

// STATIC: 0x00B33FDC
Blizzard::Hardware::ProcessorFeatures System_Hardware::s_processorFeatures;

// STATIC: 0x00B33FEC
bool System_Hardware::s_initialized;

// STATIC: 0x00B33FF0
Blizzard::Lock::Atomic System_Hardware::s_initCount;

// OFFSET: 0x00432D90
void System_Hardware::InitProcessorFeatures() {

    uint32_t features = 0;
    s_processorFeatures.m_processorCount = GetProcessorCount();
    if (!s_processorFeatures.m_processorCount) s_processorFeatures.m_processorCount = 1;

    s_processorFeatures.m_coreCount = 1;

    CPUIDInfo info;

    if (ReadCPUID(&info)) {
        if (info.m_leaf1Edx & 0x10) {
            features = 0x1;
        }

        if (info.m_leaf1Edx & 0x800000) {
            features |= 0x2;
        }

        if (info.m_leaf1Edx & 0x2000000) {
            features |= 0x4;
        }

        if (info.m_ext1Edx & 0x80000000) {
            features |= 0x8;
        }

        const char* vendors = "GenuineIntel" "AuthenticAMD" "CyrixInstead" "CentaurHalls";
        const char* vendor = reinterpret_cast<const char*>(info.m_vendor);

        if (!strncmp(vendor, vendors, 12)) {
            s_processorFeatures.m_vendor = 1;

            if (info.m_leaf1Edx & 0x4000000) {
                features |= 0x10;
            }

            if (info.m_maxLeaf >= 4) {
                uint8_t cores = static_cast<uint8_t>(info.m_leaf4Eax >> 26);

                if (cores) {
                    s_processorFeatures.m_features = features | 0x40;
                    s_processorFeatures.m_coreCount = cores + 1;
                    return;
                }
            }
        } else if (!strncmp(vendor, vendors + 12, 12)) {
            s_processorFeatures.m_vendor = 2;

            if (info.m_leaf1Edx & 0x4000000) {
                features |= 0x10;
            }

            uint8_t cores = static_cast<uint8_t>(info.m_ext8Ecx);

            if (cores) {
                s_processorFeatures.m_features = features | 0x40;
                s_processorFeatures.m_coreCount = cores + 1;
                return;
            }
        } else if (!strncmp(vendor, vendors + 24, 12)) {
        } else if (!strncmp(vendor, vendors + 36, 12)) {
        }
    }

    s_processorFeatures.m_features = features;
}
