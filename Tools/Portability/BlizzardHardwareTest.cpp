#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

namespace {
System_Hardware::CPUIDInfo fixture{};
bool synthetic = true, present = true;
unsigned failures;
void Check(bool result, const char* message) {
    if (!result) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void Vendor(const char* name) { std::memcpy(fixture.m_vendor, name, 12); }
void Decode() {
    System_Hardware::s_processorFeatures = {};
    System_Hardware::InitProcessorFeatures();
}
}

// Only the CPU query is replaced for deterministic legacy-decoder fixtures.
// The final phase switches back to the actual native CPUID implementation.
extern "C" bool __real__ZN15System_Hardware9ReadCPUIDEPNS_9CPUIDInfoE(System_Hardware::CPUIDInfo*);
extern "C" bool __wrap__ZN15System_Hardware9ReadCPUIDEPNS_9CPUIDInfoE(System_Hardware::CPUIDInfo* info) {
    if (!synthetic) return __real__ZN15System_Hardware9ReadCPUIDEPNS_9CPUIDInfoE(info);
    *info = fixture;
    return present;
}

int main() {
    fixture.m_maxLeaf = 4;
    fixture.m_leaf1Edx = 0x10 | 0x800000 | 0x2000000 | 0x4000000;
    fixture.m_ext1Edx = 0x80000000;
    fixture.m_leaf4Eax = 3u << 26;
    Vendor("GenuineIntel");
    Decode();
    Check(System_Hardware::s_processorFeatures.m_vendor == 1 &&
          System_Hardware::s_processorFeatures.m_features == 0x5F &&
          System_Hardware::s_processorFeatures.m_coreCount == 4, "shared Intel decoding preserves all legacy feature bits and package-core count");
    Vendor("AuthenticAMD");
    fixture.m_ext8Ecx = 7;
    Decode();
    Check(System_Hardware::s_processorFeatures.m_vendor == 2 &&
          System_Hardware::s_processorFeatures.m_features == 0x5F &&
          System_Hardware::s_processorFeatures.m_coreCount == 8, "shared AMD decoding uses extended CPUID core count");
    Vendor("UnknownBrand");
    Decode();
    Check(!System_Hardware::s_processorFeatures.m_vendor &&
          System_Hardware::s_processorFeatures.m_features == 0xF &&
          System_Hardware::s_processorFeatures.m_coreCount == 1, "unknown vendors retain conservative legacy behavior");
    present = false;
    Decode();
    Check(!System_Hardware::s_processorFeatures.m_features &&
          System_Hardware::s_processorFeatures.m_coreCount == 1, "unavailable CPUID does not fabricate features");
    present = true;
    struct BitCase { uint32_t leaf, extended, expected; };
    for (const auto& bit : {BitCase{0x10, 0, 1}, BitCase{0x800000, 0, 2},
                           BitCase{0x2000000, 0, 4}, BitCase{0, 0x80000000, 8},
                           BitCase{0x4000000, 0, 0x10}}) {
        fixture = {};
        Vendor("GenuineIntel");
        fixture.m_leaf1Edx = bit.leaf;
        fixture.m_ext1Edx = bit.extended;
        Decode();
        Check(System_Hardware::s_processorFeatures.m_features == bit.expected,
              "individual CPUID bits map to the original Blizzard feature mask");
    }
    synthetic = false;
    System_Hardware::CPUIDInfo actual{};
    Check(System_Hardware::ReadCPUID(&actual) && actual.m_maxLeaf >= 1, "real native CPUID query");
    Check(!System_Hardware::ReadCPUID(nullptr) && errno == EINVAL, "null native query output is rejected");
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    Check(processors > 0 && System_Hardware::GetProcessorCount() == static_cast<uint32_t>(processors), "native processor count matches OS online count");
    // Reset fixture results before racing the genuine one-time initializer.
    System_Hardware::s_processorFeatures = {};
    std::atomic<unsigned> ready{}, inconsistent{};
    std::vector<std::thread> threads;
    std::vector<uint32_t> results(16);
    for (unsigned i = 0; i < results.size(); ++i) threads.emplace_back([&, i] {
        ++ready;
        while (ready != results.size()) std::this_thread::yield();
        results[i] = Blizzard::Hardware::GetProcessorFeatures();
        for (unsigned j = 0; j < 100; ++j)
            if (Blizzard::Hardware::GetProcessorFeatures() != results[i]) ++inconsistent;
    });
    for (auto& thread : threads) thread.join();
    for (auto result : results) Check(result == results.front(), "concurrent initialization publishes the same feature mask");
    Check(!inconsistent && System_Hardware::s_initialized &&
          System_Hardware::s_processorFeatures.m_processorCount == static_cast<uint32_t>(processors), "one-time publication includes the complete processor record");
    if (!failures) std::puts("PASS: shared CPU feature decoding, real CPUID, OS processor count and concurrent publication");
    return failures ? 1 : 0;
}
