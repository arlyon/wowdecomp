#include "WoW/Source/WorldClient/FarAdtProducer.h"
#include "Engine/Source/Gx/FarResources.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
using namespace FarAdt;
using namespace GxFar;
namespace {
void Put(std::vector<uint8_t>& v, size_t at, uint32_t n) { for (unsigned i=0;i<4;++i) v[at+i]=uint8_t(n>>(8*i)); }
size_t Record(std::vector<uint8_t>& v, uint32_t token, size_t bytes) {
    size_t at=v.size(); v.resize(at+bytes+8); Put(v,at,token); Put(v,at+4,uint32_t(bytes)); return at;
}
// Production ADT v18 wire layout: MVER/MHDR/MCIN/MTEX/MTXF +256 MCNK,
// 145 staggered heights, signed authored MCNR, MCLY and packed MCAL.
std::vector<uint8_t> Fixture(bool art=false) {
    std::vector<uint8_t> v;
    size_t ver=Record(v,0x4d564552,4); Put(v,ver+8,18);
    size_t hdr=Record(v,0x4d484452,64), base=hdr+8;
    size_t idx=Record(v,0x4d43494e,4096); Put(v,base+4,uint32_t(idx-base));
    const char names[]="terrain\\rock.blp\0terrain\\grass.blp\0";
    size_t tex=Record(v,0x4d544558,sizeof(names)-1); std::memcpy(v.data()+tex+8,names,sizeof(names)-1); Put(v,base+8,uint32_t(tex-base));
    size_t flags=Record(v,0x4d545846,8); Put(v,base+44,uint32_t(flags-base)); Put(v,flags+12,0x1);
    for(unsigned i=0;i<256;++i) {
        size_t chunk=Record(v,0x4d434e4b,128), c=chunk+8;
        Put(v,c+4,i%16); Put(v,c+8,i/16); Put(v,c+12,2); Put(v,c+52,123); Put(v,c+60,i==0?0xffff:1);
        float z=47.5f; uint32_t bits; std::memcpy(&bits,&z,4); Put(v,c+112,bits);
        size_t height=Record(v,0x4d435654,580); Put(v,c+20,uint32_t(height-chunk));
        for(unsigned j=0;j<145;++j) {float f=float(i)+j*.25f; std::memcpy(&bits,&f,4); Put(v,height+8+j*4,bits);}
        size_t normal=Record(v,0x4d434e52,448); Put(v,c+24,uint32_t(normal-chunk));
        v[normal+8]=uint8_t(-127); v[normal+9]=64; v[normal+10]=12;
        size_t layer=Record(v,0x4d434c59,32); Put(v,c+28,uint32_t(layer-chunk));
        Put(v,layer+8+4,0xc0); Put(v,layer+8+16,1); Put(v,layer+8+20,0x100|0x40|0x800); Put(v,layer+8+28,73);
        size_t alpha=Record(v,0x4d43414c,2048); Put(v,c+36,uint32_t(alpha-chunk)); Put(v,c+40,2056);
        std::memset(v.data()+alpha+8,0x3c,2048);
        if(art && i==0) {
            size_t shading=Record(v,0x4d434356,145*4);Put(v,c+116,uint32_t(shading-chunk));
            for(unsigned j=0;j<145;++j)Put(v,shading+8+j*4,0xff3264a5u+j);
            size_t shadow=Record(v,0x4d435348,512);Put(v,c+44,uint32_t(shadow-chunk));Put(v,c+48,512);
            v[shadow+8]=0x81;v[shadow+519]=0x40;
        }
        Put(v,chunk+4,uint32_t(v.size()-chunk-8)); Put(v,idx+8+i*16,uint32_t(chunk)); Put(v,idx+12+i*16,uint32_t(v.size()-chunk));
    }
    return v;
}
Tile TileFrom(const std::vector<uint8_t>& v) {
    Tile t; t.encoded.reset(new uint8_t[v.size()]); std::memcpy(t.encoded.get(),v.data(),v.size()); t.encodedBytes=v.size(); return t;
}
void DecodeTests(const std::vector<uint8_t>& bytes) {
    auto t=TileFrom(bytes); assert(Decode(t)==DecodeResult::Ready);
    assert(t.chunks[0].holes==0xffff && t.chunks[1].holes==1);
    assert(t.chunks[255].heights[144]==291.f && t.chunks[0].position[2]==47.5f);
    assert(t.chunks[0].normals[0]==-127 && t.chunks[0].normals[1]==64);
    assert(t.chunks[0].layers[1].flags==(0x100|0x40|0x800));
    assert(t.chunks[0].layers[1].effectId==73 && t.textureCount==2 && t.textureFlagsBytes==8);
    auto broken=bytes; Put(broken,4,0xffffffff); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,8,19); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Unsupported);
    const size_t first=12+72+4096+8+(sizeof("terrain\\rock.blp\0terrain\\grass.blp\0")-1)+8+16;
    assert(first<bytes.size());
    // Resolve the actual first chunk from MCIN, not a guessed packing offset.
    auto get=[](const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
    size_t chunk=get(bytes.data()+12+72+8), c=chunk+8;
    broken=bytes; Put(broken,c+12,5); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,c+4,16); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,c+20,0xfffffff0); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,12+72+4,4095); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,c+36,0); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    // Compressed alpha with a zero-length RLE command must terminate as invalid.
    const size_t layer=chunk+get(bytes.data()+c+28), alpha=chunk+get(bytes.data()+c+36);
    // Real Northrend assets use zero MCAL record lengths; MCNK sizeAlpha is
    // authoritative, but neither a short nor out-of-chunk span is admissible.
    broken=bytes;Put(broken,alpha+4,0);t=TileFrom(broken);
    assert(Decode(t)==DecodeResult::Ready && t.chunks[0].alphaBytes==2048);
    for(uint32_t size:{0u,7u,8u,0xffffffffu}) {
        broken=bytes;Put(broken,alpha+4,0);Put(broken,c+40,size);t=TileFrom(broken);
        assert(Decode(t)==DecodeResult::Malformed);
    }
    broken=bytes;Put(broken,alpha+4,2047);t=TileFrom(broken);assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,layer+28,0x300); broken[alpha+8]=0; t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,layer+32,4096); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,layer+28,0x300);
    for (unsigned j=0;j<33;++j) {broken[alpha+8+j*2]=uint8_t(128+(j==32?32:127));broken[alpha+9+j*2]=uint8_t(j);}
    t=TileFrom(broken); assert(Decode(t)==DecodeResult::Ready);
    const size_t height=chunk+get(bytes.data()+c+20);
    broken=bytes; Put(broken,height+8,0x7fc00000); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    broken=bytes; Put(broken,layer+24,99); t=TileFrom(broken); assert(Decode(t)==DecodeResult::Malformed);
    for(size_t n: {size_t(0),size_t(11),size_t(90),bytes.size()-1}) {t=TileFrom(bytes); t.encodedBytes=n; assert(Decode(t)==DecodeResult::Malformed);}
    t=TileFrom(bytes); t.mapFlags=4; assert(Decode(t)==DecodeResult::Malformed); // 8-bit alpha needs4096
    t=TileFrom(bytes);assert(Decode(t)==DecodeResult::Ready);
    assert(!t.chunks[0].vertexShadingBytes && !t.chunks[0].shadowBytes);
    auto art=Fixture(true);t=TileFrom(art);assert(Decode(t)==DecodeResult::Ready);
    const auto& authored=t.chunks[0];
    assert(authored.vertexShadingBytes==580 && authored.shadowBytes==512);
    assert(get(t.encoded.get()+authored.vertexShadingOffset)==0xff3264a5u);
    assert(t.encoded[authored.shadowOffset]==0x81 && t.encoded[authored.shadowOffset+511]==0x40);
    const auto shadingOffset=authored.vertexShadingOffset, shadowOffset=authored.shadowOffset;
    for(auto record: {shadingOffset,shadowOffset}) {
        for(uint32_t size: {0u,511u,581u,0xffffffffu}) {
            broken=art;Put(broken,record-4,size);t=TileFrom(broken);assert(Decode(t)==DecodeResult::Malformed);
        }
    }
    broken=art;Put(broken,c+116,0xfffffff0);t=TileFrom(broken);assert(Decode(t)==DecodeResult::Malformed);
    broken=art;Put(broken,c+44,0xfffffff0);t=TileFrom(broken);assert(Decode(t)==DecodeResult::Malformed);
    // Northrend's absent MCSH retains an alias to the zero-length MCAL record.
    broken=bytes;Put(broken,c+44,uint32_t(alpha-chunk));Put(broken,alpha+4,0);
    t=TileFrom(broken);assert(Decode(t)==DecodeResult::Ready && !t.chunks[0].shadowBytes);
}
void ProducerTests(const std::vector<uint8_t>& fixture) {
    size_t farBytes=0;
    struct Lease {size_t* bytes; size_t n; std::shared_ptr<const void> parent; ~Lease(){*bytes-=n;}};
    std::vector<std::shared_ptr<Job>> reads;
    bool pressure=false;
    Hooks hooks;
    hooks.reserve=[&](size_t n)->std::shared_ptr<const void>{
        auto parent=ReserveFarCpuBytes(n,FarCpuClass::Source); if(!parent)return {};
        auto lease=std::make_shared<Lease>(); lease->bytes=&farBytes;lease->n=n;lease->parent=std::move(parent);farBytes+=n;return lease;
    };
    hooks.farBytes=[&]{return farBytes;}; hooks.nearPressure=[&]{return pressure;};
    hooks.exists=[](Key k){return k.y==32 && k.x>=37 && k.x<=45;};
    hooks.read=[&](const std::shared_ptr<Job>& j){reads.push_back(j);return true;};
    Producer producer(hooks);
    const size_t serviceBytes=farBytes;
    assert(serviceBytes>0);
    View view{0,0,1600,10000,1,0};
    assert(Requested({37,32},view)); assert(Distance({37,32},view)>2133);
    assert(!Requested({64,32},view)); assert(!Requested({32,32},view));
    View edge{17066.666f,17066.666f,0,4000,1,0}; assert(Requested({3,3},edge));
    // External/near ownership is not a ray snapshot and is never entrusted to FAR.
    auto pinnedNear=std::make_shared<const uint32_t>(0xff336699);
    SetFarCpuPressureForTesting(32ull*1024*1024,20ull*1024*1024);
    producer.Pump(view); assert(reads.empty() && producer.Census().budgetDeferred==1);
    assert(pinnedNear && *pinnedNear==0xff336699); // refusal never evicts external owners
    assert(producer.Census().requested==9); // no nearest-four candidate truncation
    SetFarCpuPressureForTesting(32ull*1024*1024); producer.Pump(view); assert(reads.size()==1);
    producer.Pump(view); assert(reads.size()==2); producer.Pump(view); assert(reads.size()==2);
    auto complete=[&](size_t i,bool good){auto& j=*reads[i];if(good)std::memcpy(j.tile->encoded.get(),fixture.data(),fixture.size()); j.bytesRead=fixture.size();j.readSucceeded=good;j.readDone.store(true);};
    complete(0,true);
    for(unsigned i=0;i<200 && producer.Census().ready==0;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));producer.Pump(view);}
    assert(producer.Census().ready==1 && producer.Census().farthestReady>2133);
    auto retained=producer.Ready(reads[0]->key); assert(retained);
    reads[0].reset();
    pressure=true; producer.Pump(view);
    assert(producer.Census().ready==0 && producer.Census().retired>0);
    pressure=false;
    producer.Reset(); assert(farBytes>=Producer::ReservationBytes()*2); // retained+cancelled I/O
    complete(1,true);
    View classic=view;classic.farDistance=0;
    for(unsigned i=0;i<200 && producer.Census().pending;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));producer.Pump(classic);}
    // A third read may have been issued after first publication; retire it too.
    for(size_t i=2;i<reads.size();++i) if(reads[i]&&!reads[i]->readDone.load())complete(i,false);
    for(unsigned i=0;i<200 && producer.Census().pending;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));producer.Pump(classic);}
    assert(producer.Census().ready==0 && producer.Census().pending==0);
    reads.clear(); assert(farBytes==serviceBytes+Producer::ReservationBytes()); retained.reset(); assert(farBytes==serviceBytes);
    pressure=true;producer.Pump(view);assert(producer.Census().pending==0);
    pressure=false;producer.Pump(view);assert(reads.size()==1);
    View reversal=view; reversal.y=3000; producer.Pump(reversal);
    assert(producer.Census().retired>=1); // teleport/reversal invalidates without waiting
    // Pressure prevents new requests while a cancelled read still owns bytes.
    pressure=true; View changed=view;changed.map=2;changed.x=8000;producer.Pump(changed);assert(producer.Census().retired>=1);
    for (size_t i=0;i<reads.size();++i) if (!reads[i]->readDone.load()) complete(i,false);
    pressure=true;
    for(unsigned i=0;i<200 && producer.Census().pending;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));producer.Pump(classic);}
    reads.clear();assert(farBytes==serviceBytes);
    pinnedNear.reset();
    std::cout<<"fixture bytes="<<fixture.size()<<" reservation="<<Producer::ReservationBytes()<<" service="<<serviceBytes<<" farthest requested="<<Distance({45,32},view)<<"\n";
}
void ConsumeReadyTests(const std::vector<uint8_t>& fixture) {
    size_t bytes=0;
    struct Lease { size_t* total; size_t bytes; ~Lease(){*total-=bytes;} };
    Hooks hooks;
    hooks.reserve=[&](size_t n)->std::shared_ptr<const void>{
        if(n>PartitionLimit-bytes)return {};
        auto lease=std::make_shared<Lease>();lease->total=&bytes;lease->bytes=n;bytes+=n;return lease;
    };
    hooks.farBytes=[&]{return bytes;};hooks.nearPressure=[]{return false;};
    hooks.exists=[](Key key){return key.y==32 && key.x>=37 && key.x<=45;};
    hooks.read=[&](const std::shared_ptr<Job>& job){
        std::memcpy(job->tile->encoded.get(),fixture.data(),fixture.size());
        job->bytesRead=fixture.size();job->readSucceeded=true;job->readDone.store(true);return true;
    };
    Producer producer(hooks);
    const auto serviceBytes=bytes;
    View view{0,0,1600,10000,1,0};
    std::array<ReadyPayload,9> payloads{};
    unsigned consumed=0;
    for(unsigned frame=0;frame<2000 && consumed<9;++frame) {
        producer.Pump(view);
        for(unsigned x=37;x<=45;++x) if(!payloads[x-37].ticket) {
            auto payload=producer.ConsumeReady({uint8_t(x),32});
            if(!payload.tile)continue;
            assert(payload.tile->chunks[255].heights[144]==291.f);
            assert(payload.map==view.map && producer.Current(payload));
            auto wrongMap=payload;wrongMap.map++;assert(!producer.Current(wrongMap));
            auto wrongGeneration=payload;wrongGeneration.generation++;assert(!producer.Current(wrongGeneration));
            payload.tile.reset(); // uploaded/prepared consumer releases bulky DTO, retains ticket
            payloads[x-37]=std::move(payload);++consumed;
        }
        assert(bytes<=PartitionLimit);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(consumed==9 && producer.Census().transferred==9 && producer.Census().consumed==9);
    assert(producer.Census().issued==9 && bytes==serviceBytes); // progresses beyond three payloads
    for(unsigned frame=0;frame<130;++frame)producer.Pump(view);
    assert(producer.Census().issued==9); // no repeated reads of already transferred keys
    auto stale=payloads[0];
    producer.Release(payloads[0]);assert(producer.Census().transferred==8);
    ReadyPayload replacement;
    for(unsigned frame=0;frame<2000 && !replacement.tile;++frame) {
        producer.Pump(view);replacement=producer.ConsumeReady({37,32});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(replacement.tile && replacement.ticket!=stale.ticket);
    assert(!producer.Current(stale) && producer.Current(replacement));
    producer.Release(stale);assert(producer.Census().transferred==9);
    const size_t pinned=bytes;
    producer.Reset();assert(producer.Census().transferred==0 && bytes==pinned);
    assert(!producer.Current(replacement));
    // Old-generation release cannot decrement/reset a new transfer; copies of
    // the tile remain charged until their last owner releases them.
    auto retained=replacement.tile;
    producer.Release(replacement);assert(bytes==pinned);
    retained.reset();assert(bytes==serviceBytes);
    for(auto& payload:payloads)producer.Release(payload);
    assert(producer.Census().transferred==0);
    assert(!producer.ConsumeReady({64,32}).tile);
}
}
int main(int argc,char** argv) {
    SetFarCpuPressureForTesting(32ull*1024*1024);
    auto fixture=Fixture();DecodeTests(fixture);ProducerTests(fixture);ConsumeReadyTests(fixture);
    assert(FarCpuLiveBytesForTesting()==0);
    // Optional local assets are never embedded in the executable or repository.
    // Each pair is an ADT filename and its source WDT flags.
    if((argc-1)%2)return 2;
    for(int i=1;i<argc;i+=2) {
        std::ifstream input(argv[i],std::ios::binary|std::ios::ate);
        if(!input)return 3;
        const auto size=input.tellg();
        if(size<=0 || size>std::streamoff(EncodedLimit))return 4;
        Tile tile;tile.encodedBytes=size;tile.encoded.reset(new uint8_t[tile.encodedBytes]);
        input.seekg(0);input.read(reinterpret_cast<char*>(tile.encoded.get()),tile.encodedBytes);
        if(!input)return 5;
        tile.mapFlags=uint32_t(std::stoul(argv[i+1]));
        if(Decode(tile)!=DecodeResult::Ready)return 6;
        unsigned colours=0,shadows=0;
        for(const auto& chunk:tile.chunks){colours+=chunk.vertexShadingBytes!=0;shadows+=chunk.shadowBytes!=0;}
        std::cout<<"real ADT ready bytes="<<tile.encodedBytes<<" MCCV="<<colours<<" MCSH="<<shadows<<" flags="<<tile.mapFlags<<"\n";
    }
}
