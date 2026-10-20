#include "Engine/Source/Gx/FarResources.h"
#include <iostream>
#include <stdexcept>
namespace { void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }
int main() {
    try {
        using namespace GxFar;
        {
            auto resident=ReserveFarCpuBytes(17ull*1024*1024,FarCpuClass::ResourceMetadata);
            Check(bool(resident),"resident metadata still has a staging-pool ceiling");
            auto upload=ReserveFarCpuBytes(4ull*1024*1024,FarCpuClass::Source);
            Check(bool(upload),"resident metadata starved the next FAR staging job");
        }
        SetFarCpuPressureForTesting(32ull*1024*1024);
        const auto before = FarCpuLiveBytesForTesting();
        Check(!ReserveFarCpuBytes(0, FarCpuClass::Source), "zero reservation accepted");
        Check(!ReserveFarCpuBytes(SIZE_MAX, FarCpuClass::Source), "overflow reservation accepted");
        Check(!ReserveFarCpuBytes(1, static_cast<FarCpuClass>(255)), "invalid class accepted");
        auto prepared = ReserveFarCpuBytes(FarPreparedCpuBudget-128, FarCpuClass::Prepared);
        Check(bool(prepared), "prepared share unavailable");
        Check(!ReserveFarCpuBytes(256, FarCpuClass::Prepared), "prepared subshare exceeded");
        auto source = ReserveFarCpuBytes(7*1024*1024, FarCpuClass::Source);
        auto metadata = ReserveFarCpuBytes(1024, FarCpuClass::ResourceMetadata);
        Check(source && metadata, "shared classes unavailable");
        auto census = FarCpuBytes();
        Check(census.prepared && census.source && census.resourceMetadata && census.total == census.prepared+census.source+census.resourceMetadata,
            "class census differs from total");
        Check(FarCpuLiveBytesForTesting()-before == census.total, "duplicated/missing underlying CPU charge");
        Check(!ReserveFarCpuBytes(2*1024*1024, FarCpuClass::Source), "combined FAR16 exceeded");
        auto pinned = source; source.reset();
        Check(FarCpuBytes().source == census.source, "retained reader lost charge");
        pinned.reset(); prepared.reset(); metadata.reset();
        Check(!FarCpuBytes().total && FarCpuLiveBytesForTesting() == before, "final release leaked bytes");
        SetFarCpuPressureForTesting(32ull*1024*1024,4ull*1024*1024);
        auto far = ReserveFarCpuBytes(15*1024*1024-128, FarCpuClass::Source);
        Check(bool(far), "near headroom boundary unavailable");
        Check(!ReserveFarCpuBytes(1024, FarCpuClass::ResourceMetadata), "13MiB near headroom consumed");
        Check(FarCpuLiveBytesForTesting() <= 32*1024*1024-FarNearHeadroom, "near headroom not preserved");
        far.reset(); SetFarCpuPressureForTesting(32ull*1024*1024);
        Check(FarCpuLiveBytesForTesting() == before, "headroom reservations leaked");
        std::cout << "PASS shared FAR16/Prepared8/CPU32 non-evicting13MiB headroom and retained class leases\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
