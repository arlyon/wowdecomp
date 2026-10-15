#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/Linux/Processor.h"

namespace {
unsigned failures;
void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void CheckDecoder() {
    auto result = OsLinuxDecodeProcessorFeatures(nullptr);
    Check(result.features == 0x80000000 && result.vendor == 4, "failed query has no advertised CPU features");
    CpuIdInfo info{};
    result = OsLinuxDecodeProcessorFeatures(&info);
    Check(result.features == 0x80000000 && result.vendor == 0, "unknown vendor remains distinct from failed query");
    const uint32_t sourceBits[] = {0x10, 0x800000, 0x2000000, 0x4000000};
    const uint32_t clientBits[] = {1, 2, 4, 0x10};
    for (unsigned i = 0; i < 4; ++i) {
        info.m_standard1d = sourceBits[i];
        Check(OsLinuxDecodeProcessorFeatures(&info).features == (0x80000000 | clientBits[i]), "legacy standard feature mapping");
    }
    info.m_standard1d = 0;
    info.m_extended1d = 0x80000000;
    Check(OsLinuxDecodeProcessorFeatures(&info).features == 0x80000008, "legacy 3DNow feature mapping");
    info = {};
    std::memcpy(info.m_vendor, "GenuineIntel", 12);
    info.m_standard4a = 3u << 26;
    result = OsLinuxDecodeProcessorFeatures(&info);
    Check(result.vendor == 1 && result.features == 0x80000000, "Intel cache leaf must be supported before decoding core count");
    info.m_maxStandard = 4;
    Check(OsLinuxDecodeProcessorFeatures(&info).features == 0x80000040, "Intel multicore marker");
    info.m_standard4a = 0;
    Check(OsLinuxDecodeProcessorFeatures(&info).features == 0x80000000, "Intel single core");
    std::memcpy(info.m_vendor, "AuthenticAMD", 12);
    info.m_extended8c = 7;
    result = OsLinuxDecodeProcessorFeatures(&info);
    Check(result.vendor == 2 && result.features == 0x80000040, "AMD vendor and multicore marker");
    info.m_extended8c = 0x100;
    Check(OsLinuxDecodeProcessorFeatures(&info).features == 0x80000000, "AMD core count uses only the low eight bits");
}

void CheckHardwareAndCache() {
    Check(!IOsGetProcessorFeatures(nullptr) && errno == EINVAL, "null CPUID destination is rejected");
    CpuIdInfo info{};
    const bool available = IOsGetProcessorFeatures(&info) != 0;
#if defined(__i386__) || defined(__x86_64__)
    uint32_t a, b, c, d;
    const bool expectedAvailable = __get_cpuid(0, &a, &b, &c, &d) != 0;
    Check(available == expectedAvailable, "native CPUID availability");
    if (available) {
        const uint32_t vendor[] = {b, d, c};
        Check(info.m_maxStandard == a && std::memcmp(info.m_vendor, vendor, 12) == 0, "CPUID vendor register order and standard leaf range");
        if (a >= 1) {
            __cpuid(1, a, b, c, d);
            // The APIC ID in EBX can change if the scheduler moves this thread.
            Check(info.m_standard1d == d && (info.m_standard1b & 0xFFFF) == (b & 0xFFFF), "CPUID standard feature fields");
        }
        Check(info.m_maxExtended == __get_cpuid_max(0x80000000, nullptr), "CPUID extended leaf range");
        if (info.m_maxExtended >= 0x80000004) {
            for (uint32_t i = 0; i < 3; ++i) {
                __cpuid(0x80000002 + i, a, b, c, d);
                const uint32_t brand[] = {a, b, c, d};
                Check(std::memcmp(info.m_brand + 16 * i, brand, 16) == 0, "CPUID brand leaf packing");
            }
        }
    }
#else
    Check(!available && errno == ENOTSUP, "non-x86 hardware does not advertise x86 capabilities");
#endif
    const auto expected = OsLinuxDecodeProcessorFeatures(available ? &info : nullptr);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> consistent{true};
    std::array<std::thread, 8> readers;
    for (auto& reader : readers) {
        reader = std::thread([&] {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = 0; i < 1000; ++i) {
                int32_t vendor = -1;
                if (OsGetProcessorFeaturesEx(vendor) != expected.features || vendor != expected.vendor ||
                    OsGetProcessorFeatures() != expected.features) consistent = false;
            }
        });
    }
    while (ready.load() != readers.size()) std::this_thread::yield();
    start = true;
    for (auto& reader : readers) reader.join();
    Check(consistent, "concurrent first-use cache publishes complete feature and vendor results");
}
}

int main() {
    CheckDecoder();
    CheckHardwareAndCache();
    if (!failures) std::puts("PASS: native CPUID, legacy feature decoding and concurrent cache");
    return failures ? 1 : 0;
}
