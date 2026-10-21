#include "FarAdtPreparation.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstring>

namespace FarAdt {
struct Preparation::State {
    enum class Phase { Idle, Geometry, HeaderRead, Header, MipRead, Mip, Published };
    PreparationHooks hooks;
    std::shared_ptr<const void> charge, admission;
    ReadyPayload input;
    std::shared_ptr<PreparedTile> output;
    std::shared_ptr<PreparationRead> read;
    std::array<uint8_t,1172> header{};
    MipRequest mip{};
    uint32_t texture=0, sample=0;
    uint8_t paletteIndex=0;
    bool paletteAlpha=false;
    bool cancelled=false,stopping=false,work=false;
    std::atomic<bool> done{false};
    PrepareResult result=PrepareResult::Pending;
    Phase phase=Phase::Idle;
    PreparationDiagnostics census;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    State(PreparationHooks h,std::shared_ptr<const void> c):hooks(std::move(h)),charge(std::move(c)),worker([this]{Run();}) {}
    void Run() {
        for(;;) {
            {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopping||work;});
             if(stopping) return;work=false;}
            // Borrow ONLY: owner retains input/output/read through done, even reset.
            if(phase==Phase::Geometry) result=PrepareGeometry(*input.tile,*output);
            else if(phase==Phase::Header) result=PrepareTextureHeader(header.data(),header.size(),read->fileBytes,
                output->textureCount,output->textures[texture],mip);
            else {
                auto& t=output->textures[texture];
                if(mip.sparse) {
                    if(header[8]==1 && header[9] && !paletteAlpha) {
                        paletteIndex=read->bytes[0];paletteAlpha=true;result=PrepareResult::Pending;
                    } else {
                        result=PrepareTextureSample(header.data(),read->bytes.data(),read->size,t,sample,paletteIndex,
                            output->albedo.data()+output->materialBytes+sample*4);
                        if(result==PrepareResult::Ready) {
                            paletteAlpha=false;
                            if(++sample==t.width*t.height) {t.offset=output->materialBytes;output->materialBytes+=t.bytes;}
                            else result=PrepareResult::Pending;
                        }
                    }
                } else {
                    result=PrepareTextureMip(header.data(),read->bytes.data(),read->size,t,
                        output->albedo.data()+output->materialBytes,MaterialBytes-output->materialBytes);
                    if(result==PrepareResult::Ready) {t.offset=output->materialBytes;output->materialBytes+=t.bytes;}
                }
            }
            done.store(true,std::memory_order_release);
        }
    }
    void Start(Phase p) {
        done.store(false);{std::lock_guard<std::mutex> lock(mutex);phase=p;work=true;}wake.notify_one();
    }
    bool Current() const {return !cancelled && input.ticket && hooks.current(input);}
    void Clear(PrepareResult status) {
        census.state=status;
        if(status==PrepareResult::Cancelled) ++census.cancelled;
        else if(status==PrepareResult::Malformed) ++census.malformed;
        else if(status==PrepareResult::Unsupported) ++census.unsupported;
        else if(status==PrepareResult::ReadError || status==PrepareResult::SourceChanged) ++census.readFailed;
        else if(status==PrepareResult::BudgetDeferred) ++census.budgetDeferred;
        hooks.release(input);output.reset();read.reset();phase=Phase::Idle;cancelled=false;
    }
    void IssueRead() {
        try {
            if(hooks.read(read)) return;
            read->result=PrepareResult::ReadError;
        } catch(const std::bad_alloc&) {read->result=PrepareResult::BudgetDeferred;}
        read->done.store(true);
    }
    void ReadMip() {
        const auto request=mip.sparse?TextureSampleRequest(header.data(),output->textures[texture],mip,sample,paletteAlpha):mip;
        read->offset=request.offset;read->size=request.bytes;
        read->expectedFileBytes=mip.fileBytes;read->expectedHeaderBytes=mip.headerBytes;
        std::memcpy(read->expectedHeader.data(),header.data(),mip.headerBytes);
        read->done.store(false);phase=Phase::MipRead;
        IssueRead();
    }
    void ReadHeader() {
        read->offset=0;read->size=148;read->expectedFileBytes=read->expectedHeaderBytes=0;
        read->done.store(false);phase=Phase::HeaderRead;
        std::memcpy(read->filename.data(),output->textures[texture].name.data(),260);
        IssueRead();
    }
};
Preparation::Preparation(PreparationHooks hooks) {
    // Includes bounded worker-local ids/alpha/recursive patch scratch, async
    // prefix scratch and one live material request/control-block overhead.
    const size_t bytes=sizeof(State)+sizeof(PreparationRead)+16*1024+AdapterBytes;
    auto charge=hooks.reserve(bytes);if(!charge) throw std::bad_alloc();
    state.reset(new State(std::move(hooks),std::move(charge)));
}
Preparation::~Preparation() {
    auto& s=*state;
    {std::lock_guard<std::mutex> lock(s.mutex);s.stopping=true;}
    s.wake.notify_one();s.worker.join();
    // Outstanding read callback owns its scratch+charge, but never State/input.
    s.hooks.release(s.input);
}
void Preparation::Reset() {state->cancelled=true;}
void Preparation::Pump(bool enabled) {
    auto& s=*state;
    if(!enabled) s.cancelled=true;
    if(s.phase==State::Phase::Idle) {
        if(!enabled || s.cancelled) {s.admission.reset();s.cancelled=false;return;}
        if(!s.admission) {
            try {s.admission=s.hooks.reserve(sizeof(PreparedTile)+256);}
            catch(const std::bad_alloc&) {} // same typed backpressure as refused admission
        }
        if(!s.admission) {s.census.state=PrepareResult::BudgetDeferred;++s.census.budgetDeferred;return;}
        auto input=s.hooks.consume();if(!input.tile) return;
        s.input=std::move(input);
        try {
            s.output=std::make_shared<PreparedTile>();s.output->charge=std::move(s.admission);
            s.read=std::make_shared<PreparationRead>();s.read->charge=s.charge;
        } catch(const std::bad_alloc&) {s.Clear(PrepareResult::BudgetDeferred);return;}
        s.output->key=s.input.key;s.output->ticket=s.input.ticket;
        s.output->map=s.input.map;s.output->generation=s.input.generation;
        s.census.state=PrepareResult::Pending;s.texture=0;s.Start(State::Phase::Geometry);return;
    }
    if(!s.Current()) s.cancelled=true;
    const bool reading=s.phase==State::Phase::HeaderRead || s.phase==State::Phase::MipRead;
    const bool working=s.phase==State::Phase::Geometry || s.phase==State::Phase::Header || s.phase==State::Phase::Mip;
    if((reading && !s.read->done.load(std::memory_order_acquire)) || (working && !s.done.load(std::memory_order_acquire))) return;
    if(s.cancelled) {s.Clear(PrepareResult::Cancelled);return;}
    if(s.phase==State::Phase::Published) return;
    if(reading && s.read->result!=PrepareResult::Ready) {s.Clear(s.read->result);return;}
    if(working && s.result!=PrepareResult::Ready && !(s.phase==State::Phase::Mip && s.mip.sparse && s.result==PrepareResult::Pending)) {s.Clear(s.result);return;}
    if(s.phase==State::Phase::Geometry) {
        // No worker references the raw ADT now. Ticket remains, bulky lease is
        // freed here on OWNER, never by a worker's last shared_ptr destructor.
        s.input.tile.reset();s.ReadHeader();
    } else if(s.phase==State::Phase::HeaderRead) {
        std::memcpy(s.header.data()+(s.read->offset==148?148:0),s.read->bytes.data(),s.read->size);
        // Palette metadata is a separate bounded range, never whole BLP read.
        if(s.read->size==148 && s.header[8]==1) {
            s.read->offset=148;s.read->size=1024;s.read->expectedFileBytes=s.read->fileBytes;
            s.read->expectedHeaderBytes=148;std::memcpy(s.read->expectedHeader.data(),s.header.data(),148);
            s.read->done.store(false);
            s.IssueRead();
        } else {s.Start(State::Phase::Header);}
    } else if(s.phase==State::Phase::Header) {
        s.sample=0;s.paletteAlpha=false;s.ReadMip();
    } else if(s.phase==State::Phase::MipRead) {s.Start(State::Phase::Mip);}
    else if(s.phase==State::Phase::Mip) {
        if(s.result==PrepareResult::Pending) {s.ReadMip();return;}
        if(++s.texture==s.output->textureCount) {
            s.read.reset();s.phase=State::Phase::Published;s.census.state=PrepareResult::Ready;++s.census.completed;
        } else s.ReadHeader();
    }
}
std::shared_ptr<const PreparedTile> Preparation::Consume() {
    auto& s=*state;
    if(s.phase!=State::Phase::Published) return {};
    if(!s.Current()) {s.Clear(PrepareResult::Cancelled);return {};}
    auto result=std::move(s.output);
    // Consumer retains ticket in PreparedTile; releasing it is explicit. No raw
    // pointers or leases into DTO survive. Old output stays charged via readers.
    s.input={};s.phase=State::Phase::Idle;return result;
}
const PreparationDiagnostics& Preparation::Census() const {return state->census;}
}
