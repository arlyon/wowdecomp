#include "WoW/Source/WorldClient/FarAdtPreparation.h"
#include "Engine/Source/Gx/FarResources.h"
#include "Engine/Source/Services/AsyncFile.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
#include <map>
#include <set>
using namespace FarAdt;
using namespace GxFar;
namespace {
void Put(uint8_t* p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8));}
std::array<uint8_t,1172> Header() {
    std::array<uint8_t,1172> h{};Put(h.data(),0x32504c42);Put(h.data()+4,1);
    h[8]=2;h[11]=1;Put(h.data()+12,4);Put(h.data()+16,4);Put(h.data()+20,148);Put(h.data()+84,8);return h;
}
std::shared_ptr<Tile> Synthetic() {
    auto t=std::make_shared<Tile>();t->encoded=std::make_unique<uint8_t[]>(8192);std::memset(t->encoded.get(),0,8192);
    t->encodedBytes=8192;t->textureCount=1;t->textureNamesOffset=0;t->textureNamesBytes=6;
    std::memcpy(t->encoded.get(),"a.blp",6);
    for(unsigned ci=0;ci<256;++ci) {
        auto& c=t->chunks[ci];c.layerCount=1;c.layers[0]={0,0,0,0};
        c.position={100.f-float(ci/16)*TileSize/16,200.f-float(ci%16)*TileSize/16,10};
        for(unsigned r=0;r<9;++r) {
            for(unsigned col=0;col<9;++col) c.heights[r*17+col]=float(r+col);
            if(r<8) for(unsigned col=0;col<8;++col) c.heights[r*17+9+col]=float(r+col)+1;
        }
        for(unsigned i=0;i<145;++i)c.normals[i*3+2]=127;
    }
    return t;
}
void Geometry() {
    auto t=Synthetic();auto p=std::make_unique<PreparedTile>();assert(PrepareGeometry(*t,*p)==PrepareResult::Ready);
    assert(p->indexCount<=p->indices.size());assert(p->textureCount==1);assert(p->vertices[0].position[2]==10);
    assert(p->vertices[17].position[0]<p->vertices[0].position[0]);assert(p->vertices[1].position[1]<200);
    assert(p->vertices[9].halfColumn==1 && p->vertices[9].halfRow==1);
    assert(p->vertices[0].normal[2]==1 && p->vertices[0].shading[0]==127);
    for(auto& c:p->chunks) {assert(c.coarseError==0);assert(c.indexCount[0]==768 && c.indexCount[1]==384);}
    for(unsigned ci=0;ci<255;++ci) {
        auto& c=p->chunks[ci];
        for(unsigned lod=0;lod<5;++lod) for(unsigned k=c.firstIndex[lod];k<c.firstIndex[lod]+c.indexCount[lod];k+=3) {
            assert(p->indices[k]>=ci*145 && p->indices[k]<ci*145+145);
            assert(p->indices[k+1]>=ci*145 && p->indices[k+1]<ci*145+145);
            assert(p->indices[k+2]>=ci*145 && p->indices[k+2]<ci*145+145);
            auto& a=p->vertices[p->indices[k]];auto& b=p->vertices[p->indices[k+1]];auto& d=p->vertices[p->indices[k+2]];
            assert((b.position[0]-a.position[0])*(d.position[1]-a.position[1])-(b.position[1]-a.position[1])*(d.position[0]-a.position[0])>0);
        }
        if(ci%16!=15) for(unsigned i=0;i<9;++i) {
            auto& a=p->vertices[ci*145+c.edges[1][i]];auto& b=p->vertices[(ci+1)*145+p->chunks[ci+1].edges[3][i]];
            assert(std::abs(a.position[0]-b.position[0])<0.001 && std::abs(a.position[1]-b.position[1])<0.001);
        }
    }
    assert(p->chunks[0].indexCount[2]<p->chunks[0].indexCount[1]);
    assert(p->chunks[0].indexCount[3]<p->chunks[0].indexCount[2]);
    assert(p->chunks[0].indexCount[4]<p->chunks[0].indexCount[3]);
    for(uint16_t mask:{uint16_t(0),uint16_t(1),uint16_t(0x5a5a),uint16_t(0xffff)}) {
        t->chunks[0].holes=mask;assert(PrepareGeometry(*t,*p)==PrepareResult::Ready);
        std::set<std::pair<uint16_t,uint16_t>> reference;
        for(unsigned lod=0;lod<5;++lod) {
            std::map<std::pair<uint16_t,uint16_t>,unsigned> edges;double area=0;
            auto& c=p->chunks[0];
            for(unsigned i=c.firstIndex[lod];i<c.firstIndex[lod]+c.indexCount[lod];i+=3) {
                auto a=p->indices[i],b=p->indices[i+1],d=p->indices[i+2];
                for(auto e:{std::make_pair(a,b),std::make_pair(b,d),std::make_pair(d,a)}) {if(e.first>e.second)std::swap(e.first,e.second);++edges[e];}
                const auto& va=p->vertices[a];const auto& vb=p->vertices[b];const auto& vd=p->vertices[d];
                area+=((vb.halfRow-va.halfRow)*int(vd.halfColumn-va.halfColumn)-(vb.halfColumn-va.halfColumn)*int(vd.halfRow-va.halfRow))/8.;
            }
            unsigned holes=0;for(unsigned i=0;i<16;++i)holes+=(mask>>i)&1;
            assert(std::abs(area-(64-4*holes))<0.0001);
            std::set<std::pair<uint16_t,uint16_t>> boundary;
            for(auto e:edges){assert(e.second<=2);if(e.second==1)boundary.insert(e.first);}
            if(!lod)reference=boundary;else assert(reference==boundary);
        }
    }
    t->chunks[0].holes=1;t->chunks[0].heights[9]+=5;
    assert(PrepareGeometry(*t,*p)==PrepareResult::Ready);assert(p->chunks[0].indexCount[0]==720 && p->chunks[0].indexCount[1]==360);
    // Centre inside hole is not error/coverage. Other authored centre is measured.
    assert(p->chunks[0].coarseError==0);t->chunks[0].heights[11]+=3;
    auto& c=t->chunks[0];c.layerCount=2;c.layers[1]={0,0x100,0,0};c.alphaOffset=1024;c.alphaBytes=2048;
    std::memset(t->encoded.get()+1024,0x21,2048);c.vertexShadingOffset=4096;c.vertexShadingBytes=580;
    t->encoded[4096]=1;t->encoded[4097]=2;t->encoded[4098]=3;t->encoded[4099]=255;
    c.layers[0].flags=0x4c5;c.layers[0].effectId=9;
    t->textureFlagsOffset=6000;t->textureFlagsBytes=4;Put(t->encoded.get()+6000,7);
    c.shadowOffset=5000;c.shadowBytes=512;t->encoded[5000]=0x80;
    assert(PrepareGeometry(*t,*p)==PrepareResult::Ready);assert(p->chunks[0].coarseError==3);
    assert(p->blend[BlendOffset(0,0,0)]==17 && p->blend[BlendOffset(0,1,0)]==34 && p->vertices[0].shading[0]==3);
    assert(p->blend[BlendOffset(0,0,0)+3]==255 && p->blend[BlendOffset(0,7,0)+3]==0 && p->blend[BlendOffset(0,8,0)+3]==255);
    for(unsigned ci=0;ci<256;++ci) for(unsigned y=0;y<64;++y) {
        assert(!std::memcmp(p->blend.data()+BlendOffset(ci,0,y)-4,p->blend.data()+BlendOffset(ci,0,y),4));
        assert(!std::memcmp(p->blend.data()+BlendOffset(ci,63,y)+4,p->blend.data()+BlendOffset(ci,63,y),4));
    }
    assert(sizeof(PreparedTile)+256<PreparedLimit);
    assert(p->chunks[0].layers[0].flags==0x4c5 && p->chunks[0].layers[0].effectId==9 && p->textures[0].flags==7);
    c.alphaBytes=1;assert(PrepareGeometry(*t,*p)==PrepareResult::Malformed);
    t->mapFlags=4;c.layers[1].flags=0x300;c.alphaBytes=66;
    for(unsigned i=0;i<33;++i) {t->encoded[1024+i*2]=uint8_t(128+(i==32?32:127));t->encoded[1024+i*2+1]=77;}
    assert(PrepareGeometry(*t,*p)==PrepareResult::Ready && p->blend[BlendOffset(0,0,0)]==77 && p->blend[BlendOffset(0,63,63)]==77);
    --c.alphaBytes;assert(PrepareGeometry(*t,*p)==PrepareResult::Malformed);
    c.layers[1].flags=0x100;c.alphaBytes=4096;std::memset(t->encoded.get()+1024,80,4096);t->encoded[1024+63]=90;
    assert(PrepareGeometry(*t,*p)==PrepareResult::Ready && p->blend[BlendOffset(0,63,0)]==90 && p->blend[BlendOffset(0,62,0)]==80);
    auto offset=LayerUvOffset(0x40,{64,0});assert(std::abs(offset[0]-TileSize/128)<0.0001);
    assert(LayerUvOffset(0,{64,0})[0]==0);
}
void Materials() {
    auto h=Header();PreparedTexture t;MipRequest r;
    assert(PrepareTextureHeader(h.data(),148,156,1,t,r)==PrepareResult::Ready);assert(t.encoding==AlbedoEncoding::BC1 && r.offset==148);
    uint8_t mip[8]={0,248,0,0,0,0,0,0},out[64]{};
    assert(PrepareTextureMip(h.data(),mip,8,t,out,sizeof(out))==PrepareResult::Ready && !std::memcmp(mip,out,8));
    assert(PrepareTextureMip(h.data(),mip,7,t,out,sizeof(out))==PrepareResult::Malformed);
    h[8]=1;h[9]=4;Put(h.data()+20,1172);Put(h.data()+84,24);h[148]=7;h[149]=9;h[150]=11;
    uint8_t pal[24]{};std::memset(pal+16,0xf1,8);
    assert(PrepareTextureHeader(h.data(),h.size(),1196,1,t,r)==PrepareResult::Ready);
    assert(PrepareTextureMip(h.data(),pal,sizeof(pal),t,out,sizeof(out))==PrepareResult::Ready);
    assert(out[0]==11 && out[1]==9 && out[2]==7 && out[3]==17 && out[7]==255);
    h[8]=7;assert(PrepareTextureHeader(h.data(),h.size(),1196,1,t,r)==PrepareResult::Unsupported);
    h=Header();Put(h.data()+12,4096);Put(h.data()+16,4096);Put(h.data()+20,148);Put(h.data()+84,8388608);
    Put(h.data()+20+6*4,8388756);Put(h.data()+84+6*4,2048);
    assert(PrepareTextureHeader(h.data(),148,8388756+2048,1,t,r)==PrepareResult::Ready && t.mip==6);
    assert(PrepareTextureHeader(h.data(),148,156,1,t,r)==PrepareResult::Malformed);
    h=Header();h[11]=0;Put(h.data()+12,4096);Put(h.data()+16,4096);Put(h.data()+84,8388608);
    assert(PrepareTextureHeader(h.data(),148,8388756,1,t,r)==PrepareResult::Ready && r.sparse && t.reduced);
    auto sample=TextureSampleRequest(h.data(),t,r,0,false);assert(sample.bytes==8 && sample.offset>=148);
    uint8_t red[8]={0,248,0,0,0,0,0,0};
    assert(PrepareTextureSample(h.data(),red,8,t,0,0,out)==PrepareResult::Ready && out[0]==255 && out[1]==0 && out[3]==255);
}
struct Reader {
    uint32_t size=10000000,calls=0,maxRead=0;bool fail=false,shortRead=false;
    uint32_t Size(){return size;}
    bool Read(uint32_t,void* out,uint32_t n,uint32_t& got) {++calls;maxRead=std::max(maxRead,n);got=shortRead?n-1:n;if(fail)return false;std::memset(out,0,n);return true;}
};
void Ranges() {
    CAsyncObject o{};Reader r;uint8_t b[148]{};o.m_buffer=b;o.m_size=148;
    AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::InvalidRange && !r.calls);
    o.m_filenameMode=AsyncFilenameMode::Range;AsyncFileReadFilenameBytes(o,r);assert(o.m_readSucceeded && o.m_bytesRead==148 && r.maxRead==148);
    o.m_readOffset=UINT64_MAX;AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::InvalidRange);
    o.m_readOffset=r.size;AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::EndOfFile);
    o.m_size=0;AsyncFileReadFilenameBytes(o,r);assert(o.m_readSucceeded && o.m_fileBytes==r.size);
    o.m_readOffset=0;o.m_size=148;o.m_expectedFileBytes=r.size+1;AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::SourceChanged);
    o.m_expectedFileBytes=0;o.m_expectedPrefix=b;o.m_expectedPrefixBytes=148;b[0]=1;
    AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::SourceChanged);
    b[0]=0;AsyncFileReadFilenameBytes(o,r);assert(o.m_readSucceeded);
    o.m_expectedPrefixBytes=0;r.shortRead=true;AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::ReadError);
    r.shortRead=false;r.size=148;o.m_filenameMode=AsyncFilenameMode::WholeFile;AsyncFileReadFilenameBytes(o,r);assert(o.m_readSucceeded);
    r.size=0;AsyncFileReadFilenameBytes(o,r);assert(!o.m_readSucceeded);o.m_filenameMode=AsyncFilenameMode::Range;AsyncFileReadFilenameBytes(o,r);assert(o.m_filenameResult==AsyncFilenameResult::EndOfFile);
    o.m_size=0;AsyncFileReadFilenameBytes(o,r);assert(o.m_readSucceeded);
}
void AsyncFlow() {
    const auto owner=std::this_thread::get_id();size_t bytes=0,peak=0;uint64_t next=0;bool valid=true,hold=false;
    int mode=0;uint32_t readRequests=0;
    std::shared_ptr<PreparationRead> pending;auto source=Synthetic();std::weak_ptr<const Tile> raw;
    PreparationHooks h;
    h.reserve=[&](size_t n)->std::shared_ptr<const void>{
        if(n>PreparedLimit || bytes>PreparedLimit-n)return {};
        bytes+=n;peak=std::max(peak,bytes);return {new char,[&,n](const void* p){assert(std::this_thread::get_id()==owner);bytes-=n;delete static_cast<const char*>(p);}};
    };
    h.consume=[&]{if(!source)return ReadyPayload{};ReadyPayload p{{uint8_t(next%64),0},++next,std::move(source),7,9};raw=p.tile;return p;};
    h.current=[&](const ReadyPayload& p){return valid && p.map==7 && p.generation==9;};
    h.release=[](ReadyPayload& p){p={};};
    h.read=[&](const std::shared_ptr<PreparationRead>& q){
        if(hold){pending=q;return true;}
        ++readRequests;auto header=Header();q->fileBytes=156;
        if(mode==1) {header[8]=1;header[9]=4;Put(header.data()+20,1172);Put(header.data()+84,24);header[150]=230;q->fileBytes=1196;}
        if(mode==2) {header[11]=0;Put(header.data()+12,4096);Put(header.data()+16,4096);Put(header.data()+84,8388608);q->fileBytes=8388756;}
        if(q->offset==0)std::memcpy(q->bytes.data(),header.data(),148);
        else if(mode==1 && q->offset==148) {assert(q->size==1024);std::memcpy(q->bytes.data(),header.data()+148,1024);}
        else if(mode==1) {assert(q->size==24 && q->expectedHeaderBytes==1172);std::memset(q->bytes.data(),0,16);std::memset(q->bytes.data()+16,0xff,8);}
        else if(mode==2) {assert(q->size==8);uint8_t red[8]={0,248,0,0,0,0,0,0};std::memcpy(q->bytes.data(),red,8);}
        else {assert(q->expectedHeaderBytes==148 && q->offset==148 && q->size==8);std::memset(q->bytes.data(),0x19,8);}
        q->result=mode==3?PrepareResult::SourceChanged:PrepareResult::Ready;q->done.store(true);return true;
    };
    {
        Preparation prep(h);
        for(unsigned tile=0;tile<9;++tile) {
            if(tile)source=Synthetic();
            std::shared_ptr<const PreparedTile> out;
            for(unsigned frame=0;frame<200000 && !out;++frame){prep.Pump();out=prep.Consume();std::this_thread::yield();}
            assert(out && out->map==7 && out->generation==9 && out->ticket==tile+1 && out->materialBytes==8);
            assert(raw.expired());assert(out->albedo[0]==0x19);assert(out->chunks[0].layerCount==1);
            auto pinned=out;out.reset();prep.Pump();assert(prep.Census().state==PrepareResult::BudgetDeferred);pinned.reset();
        }
        for(mode=1;mode<=2;++mode) {
            source=Synthetic();readRequests=0;std::shared_ptr<const PreparedTile> out;
            for(unsigned frame=0;frame<500000 && !out;++frame) {prep.Pump();out=prep.Consume();std::this_thread::yield();}
            assert(out && raw.expired());
            if(mode==1)assert(out->albedo[0]==230 && out->albedo[3]==255 && readRequests==3);
            else assert(out->textures[0].reduced && out->albedo[0]==255 && out->albedo[3]==255 && readRequests==4097);
        }
        mode=3;source=Synthetic();
        for(unsigned frame=0;frame<200000 && !prep.Census().readFailed;++frame){prep.Pump();std::this_thread::yield();}
        assert(prep.Census().readFailed==1 && !prep.Consume());
        mode=0;source=Synthetic();source->chunks[0].layerCount=0;
        for(unsigned frame=0;frame<200000 && !prep.Census().malformed;++frame){prep.Pump();std::this_thread::yield();}
        assert(prep.Census().malformed==1 && !prep.Consume());
        source=Synthetic();hold=true;
        for(unsigned frame=0;frame<200000 && !pending;++frame){prep.Pump();std::this_thread::yield();}
        assert(pending);valid=false;prep.Reset();prep.Pump();assert(!prep.Consume());
        // Pending/cancelled bytes remain charged until owner sees callback.
        assert(bytes>sizeof(PreparedTile));pending->result=PrepareResult::Ready;pending->done.store(true);prep.Pump();pending.reset();
        assert(prep.Census().cancelled==1);assert(!prep.Consume());prep.Pump(false);
    }
    assert(bytes==0 && peak<=PreparedLimit);
    std::printf("prepared-size=%zu peak=%zu nine completed, cancellation owner-release PASS\n",sizeof(PreparedTile),peak);
}
void Real(const char* path,uint32_t flags,const std::map<std::string,std::string>& catalog) {
    std::ifstream f(path,std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
    auto source=std::make_shared<Tile>();source->encoded=std::make_unique<uint8_t[]>(b.size());
    std::memcpy(source->encoded.get(),b.data(),b.size());source->encodedBytes=b.size();source->mapFlags=flags;
    source->charge=ReserveFarCpuBytes(Producer::ReservationBytes(),FarCpuClass::Source);assert(source->charge);
    assert(Decode(*source)==DecodeResult::Ready);
    if(catalog.empty()) {
        auto p=std::make_unique<PreparedTile>();assert(PrepareGeometry(*source,*p)==PrepareResult::Ready);
        std::printf("real geometry %s indices=%u textures=%u\n",path,p->indexCount,p->textureCount);return;
    }
    PreparationHooks h;uint32_t largestRead=0,requests=0;
    h.reserve=[](size_t n){return ReserveFarCpuBytes(n,FarCpuClass::Prepared);};
    h.consume=[&]{if(!source)return ReadyPayload{};return ReadyPayload{{32,31},1,std::move(source),4,5};};
    h.current=[](const ReadyPayload& p){return p.map==4 && p.generation==5;};h.release=[](ReadyPayload& p){p={};};
    h.read=[&](const std::shared_ptr<PreparationRead>& r) {
        auto found=catalog.find(r->filename.data());if(found==catalog.end())return false;
        std::ifstream file(found->second,std::ios::binary|std::ios::ate);if(!file)return false;
        r->fileBytes=uint32_t(file.tellg());r->result=PrepareResult::ReadError;
        std::array<uint8_t,1172> prefix{};
        if(r->expectedHeaderBytes) {file.seekg(0);file.read(reinterpret_cast<char*>(prefix.data()),r->expectedHeaderBytes);}
        if((r->expectedFileBytes && r->expectedFileBytes!=r->fileBytes) ||
           (r->expectedHeaderBytes && std::memcmp(prefix.data(),r->expectedHeader.data(),r->expectedHeaderBytes))) r->result=PrepareResult::SourceChanged;
        else if(r->offset<=r->fileBytes && r->size<=r->fileBytes-r->offset) {
            file.seekg(r->offset);file.read(reinterpret_cast<char*>(r->bytes.data()),r->size);
            if(file.gcount()==r->size)r->result=PrepareResult::Ready;
        }
        largestRead=std::max(largestRead,r->size);++requests;r->done.store(true);return true;
    };
    {
        Preparation prep(h);std::shared_ptr<const PreparedTile> out;
        for(unsigned frame=0;frame<500000 && !out;++frame) {prep.Pump();out=prep.Consume();std::this_thread::yield();}
        assert(out && out->materialBytes && FarCpuBytes().source==0);
        assert(out->materialBytes<=MaterialBytes && FarCpuBytes().prepared<=PreparedLimit && FarCpuBytes().total<=PartitionLimit);
        std::printf("real prepared %s indices=%u textures=%u albedo=%u requests=%u largestRead=%u lod4-first=%u\n",path,out->indexCount,out->textureCount,out->materialBytes,requests,largestRead,out->chunks[0].indexCount[4]);
    }
    assert(FarCpuBytes().total==0);
}
}
int main(int argc,char** argv) {
    Geometry();Materials();Ranges();AsyncFlow();
    // Optional private catalog + real ADT pairs, all read-only fixture I/O.
    std::map<std::string,std::string> catalog;int first=1;
    if(argc>2 && std::strcmp(argv[1],"--catalog")==0) {
        std::ifstream f(argv[2]);std::string line;
        while(std::getline(f,line)) {auto tab=line.find('\t');assert(tab!=std::string::npos);catalog[line.substr(0,tab)]=line.substr(tab+1);}
        first=3;
    }
    for(int i=first;i+1<argc;i+=2)Real(argv[i],std::strtoul(argv[i+1],nullptr,0),catalog);
}
