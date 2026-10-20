#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/FarResources.h"
#include <algorithm>
#include <cstring>
#include <SDL.h>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
constexpr uint64_t MiB = 1024*1024;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Vertex { float x,y,z; uint32_t colour; float u,v; };
const std::array<Vertex,4> quad{{{-1,-1,0,0xffffffff,0,0},{1,-1,0,0xffffffff,1,0},
    {-1,1,0,0xffffffff,0,1},{1,1,0,0xffffffff,1,1}}};
const uint16_t indices[]{0,1,2,2,1,3};
const uint8_t red[]{219,31,67,255,219,31,67,255,219,31,67,255,219,31,67,255};
const uint8_t green[]{17,193,83,255};
GxFarResourceRequest Request() {
    GxFarResourceRequest r;
    r.vertices=quad.data();r.vertexStride=sizeof(Vertex);r.vertexCount=quad.size();
    r.indices=indices;r.indexCount=6;r.width=r.height=2;
    r.mips={{red,sizeof(red)},{green,sizeof(green)}};r.linearFilter=false;
    return r;
}
bool Same(const GxFarResourceUsage& a, const GxFarResourceUsage& b) {
    return a.sceneBytes==b.sceneBytes && a.farGeometryBytes==b.farGeometryBytes &&
        a.imageBytes==b.imageBytes && a.farImageBytes==b.farImageBytes;
}
std::shared_ptr<GxFarResourceLease> Admit(CGxDevice& gx, const GxFarResourceRequest& r) {
    auto admitted=gx.SceneCreateFarResources(r);
    Check(admitted.result==GxFarResourceResult::Ready && admitted.lease, "FAR admission failed");
    return std::move(admitted.lease);
}
void Bind(CGxDevice& gx, const std::shared_ptr<GxFarResourceLease>& geometry, const std::shared_ptr<GxFarResourceLease>& material) {
    const CGxVertexAttrib attributes[]={{GxVA_Position,4,0,sizeof(Vertex)},{GxVA_Color0,0,12,sizeof(Vertex)},
        {GxVA_TexCoord0,3,16,sizeof(Vertex)}};
    gx.PrimVertexFormat(geometry->VertexBuffer(),attributes,3);
    gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0)|(1u<<GxVA_TexCoord0));
    gx.PrimIndexPtr(geometry->IndexBuffer());
    gx.RsSet(GxRs_Texture0,static_cast<void*>(material->Texture()));
}
void Draw(CGxDevice& gx) {
    CGxBatch batch{};batch.m_primType=GxPrim_Triangles;batch.m_count=6;batch.m_maxIndex=3;
    gx.Draw(&batch,1);
}
void Pixel(CGxDevice& gx, const uint8_t* expected) {
    CiRect rect;rect.minX=160;rect.maxX=161;rect.minY=120;rect.maxY=121;
    TSGrowableArray<CImVector> pixels;gx.DeviceReadPixels(rect,pixels);
    Check(pixels.Count()==1,"production pixel missing");
    const auto p=pixels[0];
    if(p.r!=expected[0] || p.g!=expected[1] || p.b!=expected[2] || p.a!=expected[3])
        std::cerr<<"pixel="<<unsigned(p.r)<<','<<unsigned(p.g)<<','<<unsigned(p.b)<<','<<unsigned(p.a)<<'\n';
    Check(p.r==expected[0] && p.g==expected[1] && p.b==expected[2] && p.a==expected[3],"generic Gx FAR pixel mismatch");
}
void Next(CGxDevice& gx) { gx.ScenePresent();gx.SceneClear(3,CImVector(0,0,0,255)); }
}
int main(int argc, char** argv) {
    try {
        const bool rayCapability = argc == 2 && std::strcmp(argv[1], "--ray-query-capability") == 0;
        Check(argc == 1 || rayCapability, "unknown fixture mode");
        Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        auto owner=std::make_unique<CGxDeviceVulkan>();auto& gx=*owner;
        g_theGxDevicePtr=&gx;CGxDevice& api=gx;
        if (!rayCapability) {
            gx.SceneFarTestAdmissionLimits(256*MiB,192*MiB,32*MiB);
            GxFar::SetFarCpuPressureForTesting(32*MiB);
            Check(gx.CGxDevice::SceneCreateFarResources(Request()).result==GxFarResourceResult::Unsupported,"non-native default manufactured FAR support");
        }
        CGxFormat format;format.m_window=1;format.m_maximize=0;format.m_size={320,240};
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"device creation failed");gx.m_cursorVisible=0;
        if (rayCapability) {
            Check(gx.SceneRayQueryTestDeviceAddress(), "required RT query/device-address/texture-indexing capability unavailable or unenabled");
            gx.DeviceDestroy(); Check(gx.SceneHdrTestValidationClean(), "Vulkan capability diagnostics emitted");
            g_theGxDevicePtr = nullptr; owner.reset(); SDL_Quit();
            std::cout << "PASS original native device optional ray-query feature chain and physically admitted device-address buffer; NOT shadow readiness\n";
            return 0;
        }
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Unavailable,"unrecorded admission accepted");
        for(auto state:{GxRs_Culling,GxRs_DepthTest,GxRs_DepthWrite,GxRs_Fog,GxRs_Lighting})gx.RsSet(state,0);
        gx.RsSet(GxRs_ColorWrite,15);gx.RsSet(GxRs_BlendingMode,0);gx.RsSet(GxRs_ColorOp0,0);gx.RsSet(GxRs_AlphaOp0,0);
        gx.RsSet(GxRs_VertexShader,static_cast<void*>(nullptr));gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
        C44Matrix identity;gx.XformPushAndSet(GxXform_World,identity);gx.XformSetView(identity);gx.XformSetProjection(identity);
        gx.SceneClear(3,CImVector(0,0,0,255));
        const auto initial=api.SceneFarResourceUsage();const auto cpuInitial=GxFar::FarCpuBytes().total;
        const auto invalid=[&](const GxFarResourceRequest& r) {
            Check(api.SceneCreateFarResources(r).result==GxFarResourceResult::Invalid,"invalid FAR accepted");
            Check(Same(initial,api.SceneFarResourceUsage()) && GxFar::FarCpuBytes().total==cpuInitial,"invalid request allocated storage");
        };
        invalid({});auto bad=Request();bad.vertexStride=UINT32_MAX;bad.vertexCount=UINT32_MAX;invalid(bad);
        bad=Request();bad.indices=nullptr;invalid(bad);bad=Request();bad.width=UINT32_MAX;bad.height=UINT32_MAX;invalid(bad);
        bad=Request();bad.mips[0].bytes=15;invalid(bad);bad=Request();bad.mips.push_back({green,4});invalid(bad);
        uint16_t outside[]{0,1,4};bad=Request();bad.indices=outside;bad.indexCount=3;invalid(bad);
        auto metadataPressure=GxFar::ReserveFarCpuBytes(GxFar::FarCpuBudget-128,GxFar::FarCpuClass::Source);
        Check(bool(metadataPressure),"CPU source-share pressure unavailable");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"resource metadata escaped shared CPU FAR16");
        Check(Same(initial,api.SceneFarResourceUsage()),"CPU metadata rejection allocated GPU storage");metadataPressure.reset();
        for(uint32_t boundary=1;boundary<=4;++boundary) {
            gx.SceneFarTestFailAllocation(boundary);
            Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::AllocationFailure,"injected allocation failure not refused");
            Check(Same(initial,api.SceneFarResourceUsage()) && GxFar::FarCpuBytes().total==cpuInitial,"failed allocation leaked reservations");
        }
        auto uploadQuad=quad;
        std::array<uint16_t,6> uploadIndices;std::copy(std::begin(indices),std::end(indices),uploadIndices.begin());
        std::array<uint8_t,sizeof(red)> uploadPixels;std::copy(std::begin(red),std::end(red),uploadPixels.begin());
        auto upload=Request();upload.vertices=uploadQuad.data();upload.indices=uploadIndices.data();upload.mips[0].data=uploadPixels.data();
        auto lease=Admit(api,upload);
        // Upload spans are borrowed only during admission, not captured world jobs.
        uploadQuad.fill({});uploadIndices.fill(0);uploadPixels.fill(0);
        Bind(api,lease,lease);Draw(api);Pixel(api,red);
        auto exaggerated=quad;for(auto& v:exaggerated){v.u*=4096;v.v*=4096;}
        auto r=Request();r.vertices=exaggerated.data();r.width=r.height=0;r.mips.clear();
        auto minified=Admit(api,r);Bind(api,minified,lease);Draw(api);Pixel(api,green);
        const auto cpuPinned=GxFar::FarCpuBytes().resourceMetadata;
        gx.RsSet(GxRs_Texture15,static_cast<void*>(lease->Texture()));
        api.SceneWithdrawFarResources();
        gx.SceneClear(3,CImVector(0,0,0,255));Draw(api);
        const uint8_t black[]{0,0,0,255};Pixel(api,black);
        // Stale inactive custom attributes and unused texture registers must not
        // suppress ordinary near draws after FAR withdrawal.
        auto* nearPool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,sizeof(quad),GxPoolHintBit_Unk3,"FAR near-priority oracle");
        auto* nearBuffer=gx.BufCreate(nearPool,sizeof(Vertex),quad.size(),0);gx.BufData(nearBuffer,quad.data(),sizeof(quad),0);
        const CGxVertexAttrib nearAttributes[]={{GxVA_Position,4,0,sizeof(Vertex)},{GxVA_Color0,0,12,sizeof(Vertex)}};
        gx.PrimVertexFormat(nearBuffer,nearAttributes,2);gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0));
        gx.RsSet(GxRs_Texture0,static_cast<void*>(nullptr));
        CGxBatch nearBatch{};nearBatch.m_primType=GxPrim_TriangleStrip;nearBatch.m_count=4;gx.Draw(&nearBatch,0);
        const uint8_t white[]{255,255,255,255};Pixel(api,white);
        auto updated=quad;for(auto& vertex:updated)vertex.colour=0xff336699;
        gx.BufData(nearBuffer,updated.data(),sizeof(updated),0);gx.Draw(&nearBatch,0);
        const uint8_t changedNear[]{51,102,153,255};Pixel(api,changedNear);
        gx.BufDestroy(nearBuffer);gx.PoolDestroy(nearPool);Next(api);
        Check(!api.SceneFarResourceUsage().farGeometryBytes && !api.SceneFarResourceUsage().farImageBytes,"fenced withdrawal leaked GPU storage");
        Check(GxFar::FarCpuBytes().resourceMetadata==cpuPinned,"withdrawn external wrappers lost CPU charge");
        Check(!lease->VertexBuffer() && !lease->Texture() && lease->Status()==GxFarResourceStatus::Withdrawn,"withdrawal did not invalidate handles");
        lease.reset();minified.reset();Next(api);Check(GxFar::FarCpuBytes().total==cpuInitial,"metadata final release leaked");
        // Exact physical FAR geometry cap, including overlapping material staging.
        std::vector<uint8_t> big(32*MiB,127);
        r={};r.vertices=big.data();r.vertexStride=4;r.vertexCount=big.size()/4;
        auto fullGeometry=Admit(api,r);Check(api.SceneFarResourceUsage().farGeometryBytes<=32*MiB,"geometry32 exceeded");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"geometry32 bypassed by overlap");
        fullGeometry->Withdraw();api.SceneWithdrawFarResources();fullGeometry.reset();
        Check(!api.SceneFarResourceUsage().farGeometryBytes,"unused geometry not reclaimed");
        r={};r.width=4096;r.height=2048;r.mips={{big.data(),big.size()}};
        auto fullImage=Admit(api,r);auto usage=api.SceneFarResourceUsage();
        Check(usage.farImageBytes<=32*MiB && usage.farGeometryBytes<=32*MiB && usage.farGeometryBytes>=big.size(),"material staging overlap not charged");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"live staging overlap bypassed geometry32");
        Next(api);usage=api.SceneFarResourceUsage();Check(!usage.farGeometryBytes && usage.farImageBytes,"completed staging retained or image lost");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"material32 bypassed after staging fence");
        api.SceneWithdrawFarResources();fullImage.reset();Next(api);
        // Real recorded and submitted-unfinished users cannot be reclaimed to
        // satisfy near priority. Retry succeeds only after the actual fence.
        lease=Admit(api,Request());Bind(api,lease,lease);Draw(api);
        const auto recorded=api.SceneFarResourceUsage();
        Check(!gx.SceneFarTestNearCharge(false,256*MiB),"near allocation freed recorded FAR readers");
        Check(Same(recorded,api.SceneFarResourceUsage()),"recorded withdrawal dropped charged bytes");
        gx.SceneFarTestHoldGpu();
        Check(!gx.SceneFarTestNearCharge(false,256*MiB),"near allocation freed unfinished submitted FAR readers");
        Check(Same(recorded,api.SceneFarResourceUsage()),"unfinished fence dropped charged bytes");
        gx.SceneFarTestReleaseGpu();
        Check(gx.SceneFarTestNearCharge(false,256*MiB),"near geometry retry failed after fence");
        Check(api.SceneFarResourceUsage().sceneBytes==256*MiB && !api.SceneFarResourceUsage().farGeometryBytes,"near/FAR physical256 not shared");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"FAR ignored near GPU256 usage");
        gx.SceneFarTestReleaseNear();lease.reset();Next(api);
        lease=Admit(api,Request());const auto imageRecorded=api.SceneFarResourceUsage();
        Check(!gx.SceneFarTestNearCharge(true,192*MiB),"image admission freed recorded image upload");
        Check(Same(imageRecorded,api.SceneFarResourceUsage()),"image withdrawal dropped recorded storage");
        gx.SceneFarTestHoldGpu();
        Check(!gx.SceneFarTestNearCharge(true,192*MiB),"image admission freed unfinished image upload");
        gx.SceneFarTestReleaseGpu();
        Check(gx.SceneFarTestNearCharge(true,192*MiB),"near image retry failed after fence");
        Check(api.SceneFarResourceUsage().imageBytes==192*MiB && !api.SceneFarResourceUsage().farImageBytes,"image/FAR192 not shared");
        Check(api.SceneCreateFarResources(Request()).result==GxFarResourceResult::Budget,"FAR ignored image192 usage");
        gx.SceneFarTestReleaseNear();lease.reset();Next(api);
        // Device destruction expires external leases without leaving a device
        // pointer in their destructor; their wrappers remain CPU-charged.
        lease=Admit(api,Request());auto retained=lease;
        gx.DeviceDestroy();
        Check(lease->Status()==GxFarResourceStatus::DeviceExpired && !lease->VertexBuffer() && !lease->IndexBuffer() && !lease->Texture(),"device reset did not expire lease");
        Check(!api.SceneFarResourceUsage().sceneBytes && !api.SceneFarResourceUsage().imageBytes,"device destruction leaked GPU reservations");
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"device recreation failed");
        gx.SceneClear(3,CImVector(0,0,0,255));auto newGeneration=Admit(api,Request());
        Check(lease->Status()==GxFarResourceStatus::DeviceExpired && newGeneration->Status()==GxFarResourceStatus::Resident,"old lease revived on device generation reset");
        gx.DeviceDestroy();newGeneration.reset();
        Check(gx.SceneHdrTestValidationClean(),"Vulkan diagnostics emitted");
        g_theGxDevicePtr=nullptr;owner.reset(); // external lease now outlives the device object too
        lease.reset();Check(GxFar::FarCpuBytes().resourceMetadata,"retained reset wrapper lost CPU charge");
        Check(retained->Status()==GxFarResourceStatus::DeviceExpired,"expired lease dereferenced destroyed device");retained.reset();
        Check(GxFar::FarCpuBytes().total==cpuInitial,"expired lease leaked CPU metadata");
        SDL_Quit();
        std::cout<<"PASS production generic Gx FAR vertex/index/RGBA8 mip pixels, physical256+image192 mixed admission, FAR32 subshares/staging, failures, recorded/unfenced retirement/retry and device-expired leases\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
