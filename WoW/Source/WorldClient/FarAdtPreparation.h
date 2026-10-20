#ifndef WOW_WORLDCLIENT_FARADTPREPARATION_H
#define WOW_WORLDCLIENT_FARADTPREPARATION_H
#include "FarAdtPrepared.h"
#include <functional>
namespace FarAdt {
struct PreparationRead {
    std::shared_ptr<const void> charge;
    std::array<char,260> filename{};
    std::array<uint8_t,16384> bytes{};
    std::array<uint8_t,1172> expectedHeader{};
    uint32_t size=0, fileBytes=0, expectedFileBytes=0, expectedHeaderBytes=0;
    uint64_t offset=0;
    PrepareResult result=PrepareResult::ReadError;
    std::atomic<bool> done{false};
};
struct PreparationHooks {
    // Same owner-thread FAR16/CPU32 ledger, additionally bounded prepared8.
    std::function<std::shared_ptr<const void>(size_t)> reserve;
    std::function<ReadyPayload()> consume;
    std::function<bool(const ReadyPayload&)> current;
    std::function<void(ReadyPayload&)> release;
    std::function<bool(const std::shared_ptr<PreparationRead>&)> read;
};
struct PreparationDiagnostics {
    PrepareResult state=PrepareResult::Pending;
    uint64_t completed=0,cancelled=0,malformed=0,unsupported=0,readFailed=0,budgetDeferred=0;
};
class Preparation {
public:
    explicit Preparation(PreparationHooks);
    ~Preparation(); // owner thread; joins pure CPU only, never waits for file I/O
    void Pump(bool enabled = true); // one transition / read request per call; never waits
    void Reset();
    std::shared_ptr<const PreparedTile> Consume(); // owner thread, validates freshness
    const PreparationDiagnostics& Census() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
