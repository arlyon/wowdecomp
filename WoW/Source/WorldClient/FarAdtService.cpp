#include "FarAdtService.h"
#include "FarAdtRender.h"
#include "MapLowDetail.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Gx/FarResources.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include <cstdio>
#include <memory>
#include <system_error>

namespace {
static_assert(FarAdt::PartitionLimit == GxFar::FarCpuBudget);
static_assert(FarAdt::PreparedLimit == GxFar::FarPreparedCpuBudget);
static_assert(FarAdt::NearHeadroom == GxFar::FarNearHeadroom);
std::unique_ptr<FarAdt::Producer> producer;
std::unique_ptr<FarAdt::Preparation> preparation;
FarAdt::Diagnostics admissionCensus;
struct Read { std::shared_ptr<FarAdt::Job> job; CAsyncObject* object = nullptr; };
// Reservation slack covers the read adapter, async request, ledger lease and
// shared_ptr control blocks without relying on vector spare capacities.
static_assert(sizeof(Read)+sizeof(CAsyncObject)+128 <= FarAdt::AdapterBytes);
void Complete(void* arg) {
    std::unique_ptr<Read> read(static_cast<Read*>(arg));
    auto& job = *read->job;
    job.bytesRead = read->object->m_bytesRead;
    job.readSucceeded = read->object->m_readSucceeded;
    AsyncFileReadDestroyObject(read->object);
    job.readDone.store(true);
}
bool StartRead(const std::shared_ptr<FarAdt::Job>& job) {
    char filename[260];
    const int n = std::snprintf(filename, sizeof(filename), "%s\\%s_%u_%u.adt", CMap::mapPath, CMap::mapName, job->key.x, job->key.y);
    if (n < 0 || size_t(n) >= sizeof(filename)) return false;
    auto read = std::make_unique<Read>(); read->job = job;
    read->object = AsyncFileReadAllocObject();
    if (!read->object) return false;
    read->object->m_buffer = job->tile->encoded.get();
    read->object->m_size = FarAdt::EncodedLimit;
    read->object->m_userArg = read.get();
    read->object->m_userPostloadCallback = reinterpret_cast<void*>(&Complete);
    auto* object = read->object;
    read.release();
    AsyncFileReadFilename(object, filename, 0);
    return true;
}
std::shared_ptr<const void> Reserve(size_t bytes, bool prepared) {
    try {return GxFar::ReserveFarCpuBytes(bytes,prepared?GxFar::FarCpuClass::Prepared:GxFar::FarCpuClass::Source);}
    catch(const std::bad_alloc&) {return {}; }
}
struct MaterialRead { std::shared_ptr<FarAdt::PreparationRead> read; CAsyncObject* object=nullptr; };
static_assert(sizeof(MaterialRead)+sizeof(CAsyncObject)+128<=FarAdt::AdapterBytes);
void MaterialComplete(void* arg) {
    std::unique_ptr<MaterialRead> request(static_cast<MaterialRead*>(arg));
    auto& r=*request->read;auto* o=request->object;
    r.fileBytes=o->m_fileBytes;
    r.result=o->m_readSucceeded?FarAdt::PrepareResult::Ready:
        o->m_filenameResult==AsyncFilenameResult::SourceChanged?FarAdt::PrepareResult::SourceChanged:FarAdt::PrepareResult::ReadError;
    AsyncFileReadDestroyObject(o);r.done.store(true,std::memory_order_release);
}
bool ReadMaterial(const std::shared_ptr<FarAdt::PreparationRead>& read) {
    if(read->size>read->bytes.size()) return false;
    auto r=std::make_unique<MaterialRead>();r->read=read;r->object=AsyncFileReadAllocObject();
    if(!r->object) return false;
    auto* o=r->object;o->m_buffer=read->bytes.data();o->m_size=read->size;
    o->m_userArg=r.get();o->m_userPostloadCallback=reinterpret_cast<void*>(&MaterialComplete);r.release();
    AsyncFileReadFilenameRange(o,read->filename.data(),0,read->offset,read->expectedFileBytes,
        read->expectedHeader.data(),read->expectedHeaderBytes);
    return true;
}

}
void FarAdtPump() {
    const float distance = CWorldParam::cvar_landscapeDistance ? CWorldParam::cvar_landscapeDistance->m_floatValue : 0.f;
    if (!producer && distance <= 0) return;
    if(!g_theGxDevicePtr || !g_theGxDevicePtr->SceneFarResourcesSupported()) {FarAdtRenderReset();return;}
    if(distance<=World::s_farClip*2) FarAdtRenderReset();
    if (!producer) {
        FarAdt::Hooks hooks;
        hooks.farBytes = [] { const auto bytes=GxFar::FarCpuBytes();return bytes.source+bytes.prepared; };
        // Live near texture ownership is uncapped. Its old 32MiB threshold
        // cannot describe pressure; FAR still has its own reservation ledger.
        hooks.nearPressure = [] { return false; };
        hooks.reserve = [](size_t bytes) { return Reserve(bytes,false); };
        hooks.exists = [](FarAdt::Key key) {
            const auto index=size_t(key.y)*64+key.x;
            return (CMap::m_areaInfo[index].m_flags&1) && CMap::s_lowDetail.GetCensus().admittedIds.test(index);
        };
        hooks.read = StartRead;
        try { producer = std::make_unique<FarAdt::Producer>(std::move(hooks)); }
        catch (const std::bad_alloc&) { ++admissionCensus.budgetDeferred; return; }
        catch (const std::system_error&) { ++admissionCensus.unsupported; return; }
    }
    if(!preparation) {
        FarAdt::PreparationHooks hooks;
        hooks.reserve=[](size_t bytes) { return Reserve(bytes,true); };
        hooks.consume=[] {
            // Bounded scan, at most one transfer/request per pump.
            for(unsigned y=0;y<64;++y) for(unsigned x=0;x<64;++x) {
                auto p=producer->ConsumeReady({uint8_t(x),uint8_t(y)});if(p.tile) return p;
            }
            return FarAdt::ReadyPayload{};
        };
        hooks.current=[](const FarAdt::ReadyPayload& p) { return producer && producer->Current(p); };
        hooks.release=[](FarAdt::ReadyPayload& p) { if(producer) producer->Release(p); };
        hooks.read=ReadMaterial;
        try { preparation=std::make_unique<FarAdt::Preparation>(std::move(hooks)); }
        catch(const std::bad_alloc&) { ++admissionCensus.budgetDeferred;return; }
        catch(const std::system_error&) { ++admissionCensus.unsupported;return; }
    }
    // Preparation admission precedes producer reads: otherwise three raw
    // payloads can occupy FAR16 and prevent their own transformation forever.
    preparation->Pump(distance>World::s_farClip*2);
    // This range does not modify near actors, WMO, sounds, collision, shared
    // bucket tables, render pools or the normal area loading/wait policy.
    producer->Pump({World::s_currentWorldPos.x, World::s_currentWorldPos.y,
                    World::s_farClip*2, distance, uint64_t(uint32_t(s_mapId)), CMap::header.m_flags});
    FarAdtRenderMaintain();
}
void FarAdtReset() { FarAdtRenderReset(); if(preparation) preparation->Reset(); if (producer) producer->Reset(); }
const FarAdt::Producer* FarAdtGetProducer() { return producer.get(); }
FarAdt::ReadyPayload FarAdtConsumeReady(FarAdt::Key key) {
    return producer ? producer->ConsumeReady(key) : FarAdt::ReadyPayload{};
}
void FarAdtRelease(FarAdt::ReadyPayload& payload) {
    if (producer) producer->Release(payload);
    else { payload.tile.reset(); payload.ticket = 0; }
}
const FarAdt::Diagnostics& FarAdtCensus() { return producer ? producer->Census() : admissionCensus; }

std::shared_ptr<const FarAdt::PreparedTile> FarAdtConsumePrepared() {
    return preparation?preparation->Consume():nullptr;
}
bool FarAdtPreparedCurrent(const FarAdt::PreparedTile& tile) {
    FarAdt::ReadyPayload p{tile.key,tile.ticket,{},tile.map,tile.generation};
    return producer && producer->Current(p);
}
void FarAdtReleasePrepared(std::shared_ptr<const FarAdt::PreparedTile>& tile) {
    if(tile && producer) {FarAdt::ReadyPayload p{tile->key,tile->ticket,{},tile->map,tile->generation};producer->Release(p);}
    tile.reset();
}
const FarAdt::PreparationDiagnostics& FarAdtPreparationCensus() {
    static const FarAdt::PreparationDiagnostics empty;
    return preparation?preparation->Census():empty;
}
