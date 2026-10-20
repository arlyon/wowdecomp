#include "FarResources.h"
#include <limits>
#include <new>

namespace GxFar {
namespace {
FarCpuCensus farCpu;
#ifdef WOWDECOMP_FAR_TESTING
size_t testLimit = 0, testExternalBytes = 0;
#else
constexpr size_t testLimit = 0, testExternalBytes = 0;
#endif
size_t& ClassBytes(FarCpuClass classification) {
    switch (classification) {
        case FarCpuClass::Source: return farCpu.source;
        case FarCpuClass::Prepared: return farCpu.prepared;
        case FarCpuClass::ResourceMetadata: return farCpu.resourceMetadata;
    }
    std::terminate();
}
struct CpuCharge {
    size_t bytes;
    FarCpuClass classification;
    CpuCharge(size_t n, FarCpuClass c) : bytes(n), classification(c) {
        farCpu.total += n; ClassBytes(c) += n;
    }
    ~CpuCharge() { farCpu.total -= bytes; ClassBytes(classification) -= bytes; }
};
}
FarCpuCensus FarCpuBytes() { return farCpu; }
std::shared_ptr<const void> ReserveFarCpuBytes(size_t bytes, FarCpuClass classification) {
    if (classification != FarCpuClass::Source && classification != FarCpuClass::Prepared && classification != FarCpuClass::ResourceMetadata) return {};
    constexpr size_t overhead = sizeof(CpuCharge) + 4 * sizeof(void*);
    if (!bytes || bytes > std::numeric_limits<size_t>::max() - overhead) return {};
    const size_t charge = bytes + overhead;
    if (charge > std::numeric_limits<size_t>::max() - farCpu.total) return {};
    const bool staging = classification != FarCpuClass::ResourceMetadata || testLimit;
    const size_t used = testLimit ? farCpu.total : farCpu.source + farCpu.prepared;
    if (staging && (charge > FarCpuBudget || used > FarCpuBudget - charge)) return {};
    if (testLimit && (FarNearHeadroom > testLimit || testExternalBytes > testLimit - FarNearHeadroom ||
        farCpu.total > testLimit - FarNearHeadroom - testExternalBytes ||
        charge > testLimit - FarNearHeadroom - testExternalBytes - farCpu.total)) return {};
    if (classification == FarCpuClass::Prepared && (charge > FarPreparedCpuBudget || farCpu.prepared > FarPreparedCpuBudget - charge)) return {};
    try { return std::make_shared<CpuCharge>(charge, classification); }
    catch (const std::bad_alloc&) { return {}; }
}
#ifdef WOWDECOMP_FAR_TESTING
void SetFarCpuPressureForTesting(size_t limit, size_t externalBytes) { testLimit = limit; testExternalBytes = externalBytes; }
size_t FarCpuLiveBytesForTesting() {
    return testExternalBytes > std::numeric_limits<size_t>::max() - farCpu.total ? std::numeric_limits<size_t>::max() : farCpu.total + testExternalBytes;
}
#endif
}
