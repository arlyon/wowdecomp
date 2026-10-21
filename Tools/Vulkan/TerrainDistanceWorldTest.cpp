// Synchronous production world acceptance against external read-only archives.
// No account, saved client configuration, synthetic terrain or world executor.
#include "Engine/Source/Base/Base.h"
#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/Frame/CSimpleTexture.h"
#include "Engine/Source/FrameXML/LoadXML.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/GxuLight.h"
#include "Engine/Source/Gxu/GxuFont.h"
#include "Engine/Source/Gxu/GxuFontFont.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "Engine/Source/Model2/M2Cache.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Scrn/Layer.h"
#include "Engine/Source/Scrn/Scrn.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Services/SysMessage.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
// Client.h declares the production no-argument entry point. Keep its real
// bootstrap declarations without conflicting with this fixture's explicit CLI.
#define main WowProductionEntryDeclaration
#include "WoW/Source/Client.h"
#undef main
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/Magic/MagicClient/SpellVisuals.h"
#include "WoW/Source/Net/NetClient/NetClient.h"
#include "WoW/Source/ObjectAlloc/HeapUsage.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/Ui/CGWorldStateInfo.h"
#include "WoW/Source/UIUtil/InputControl.h"
#include "WoW/Source/Game/GameClient/WorldText.h"
#include "WoW/Source/Game/GameClient/WTOBJECT.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/WorldDisplayEpoch.h"
#include "WoW/Source/WorldClient/FarAdtService.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/CDetailDoodadData.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/WorldClient/shadow.h"
#include <SDL.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
unsigned Width=640, Height=480;
struct State {
    std::filesystem::path assets;
    std::vector<SArchive*> archives;
    std::unique_ptr<CGxDeviceVulkan> device;
    std::unique_ptr<CSimpleTop> top;
    std::unique_ptr<ObjectMgr> objects;
    CGWorldFrame* frame=nullptr;
    bool northrend=false, world=false, models=false, spells=false, textures=false, suppressPrepared=false;
    unsigned ticks=0, phase=0, resetFrames=0, networkAttempts=0;
    int result=1;
    std::chrono::steady_clock::time_point started;
    std::vector<CImVector> detailedColour;
    std::vector<float> detailedDepth;
    bool rasterFeatures=false, shadowVisibility=false, terrainShadows=false, terrainFallback=false, terrainPressure=false, ownsFonts=false;
    std::filesystem::path receiverFixtureRoot;
    std::string previousShaderRoot;
    bool hadShaderRoot=false;
    bool independentWorld=false,independentRT=false,independentVsync=false,inputs=false,worldText=false;
    bool detailInstancing=false;uint64_t detailDraws=0;
    std::vector<CImVector> detailOnlyColour;std::vector<float> detailOnlyDepth;
    CGxFont* font=nullptr;
    CGxString* label=nullptr;
    CGxStringBatch* labelBatch=nullptr;
    unsigned featureFrame=0;
    std::vector<unsigned> uiOpaque;
    std::vector<CImVector> rasterBaseline,shadowPrevious;
    std::vector<double> means;
    CImVector nightAmbient, nightDiffuse;
    C3Vector nightDirection;
} test;
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void Stage(const char* name) { std::fprintf(stderr,"Terrain stage=%s map=%s\n",name,test.northrend?"Northrend":"Azeroth"); }
void Set(CVar* variable,const char* value) {
    Check(variable,"production CVar missing");variable->Set(value,true,false,false,false);
}
void MountAssets() {
    SFile::Initialize();
    const auto scratch=std::filesystem::current_path().string();
    SFile::SetBasePath(scratch.c_str()); // Never make external Data the writable base.
    struct Archive {const char* name;int priority;};
    const Archive archives[]={{"common.MPQ",0},{"common-2.MPQ",10},{"expansion.MPQ",20},{"lichking.MPQ",30},
        {"enUS/locale-enUS.MPQ",40},{"enUS/speech-enUS.MPQ",40},
        {"enUS/expansion-locale-enUS.MPQ",50},{"enUS/expansion-speech-enUS.MPQ",50},
        {"enUS/lichking-locale-enUS.MPQ",60},{"enUS/lichking-speech-enUS.MPQ",60},
        {"patch.MPQ",100},{"enUS/patch-enUS.MPQ",110},{"patch-2.MPQ",200},{"enUS/patch-enUS-2.MPQ",210},
        {"patch-3.MPQ",300},{"enUS/patch-enUS-3.MPQ",310},{"patch-W.mpq",400}};
    for(const auto& entry:archives) {
        const auto path=test.assets/entry.name;
        if(!std::filesystem::exists(path)) {Check(std::strcmp(entry.name,"patch-W.mpq")==0,"required archive missing");continue;}
        SArchive* archive=nullptr;
        Check(SFile::OpenArchive(path.c_str(),entry.priority,0xC00,&archive)!=0,"read-only archive mount failed");
        test.archives.push_back(archive);
    }
}
struct RasterCase {
    const char* name;
    const char* time;
    const char* darkness;
    const char* scale;
    const char* aa;
    const char* hdr;
    const char* exposure;
    bool msdf=true;
};
const RasterCase rasterCases[]={
    {"classic-noon","12","100","1","0","0","0"},
    {"classic-font","12","100","1","0","0","0",false},
    {"midnight-stock","0","0","1","0","0","0"},
    {"midnight-darker","0","100","1","0","0","0"},
    {"dusk","18","100","1","0","0","0"},
    {"server-time","-1","100","1","0","0","0"},
    {"half-scene-native-ui","12","100","0.5","0","0","0"},
    {"experimental-aa","12","100","1","1","0","0"},
    {"float-scene","12","100","1","0","1","0"},
    {"exposure-plus-one","12","100","1","0","1","1"},
    {"combined-half-aa-float","12","100","0.5","1","1","0"},
    {"features-off-roundtrip","12","100","1","0","0","0"}
};
void SelectRasterCase() {
    const auto& c=rasterCases[test.phase];
    Set(CWorldParam::cvar_localTimePreview,c.time);Set(CWorldParam::cvar_nightDarkness,c.darkness);
    Set(CWorldParam::cvar_nativeSceneScale,c.scale);Set(CWorldParam::cvar_nativeAA,c.aa);
    Set(CWorldParam::cvar_nativeHDR,c.hdr);Set(CWorldParam::cvar_nativeHDRExposure,c.exposure);
    GxuFontSetMsdfEnabled(c.msdf);test.featureFrame=0;Stage(c.name);
}
void SaveRasterCapture(const RasterCase& c,TSGrowableArray<CImVector>& pixels) {
    auto* surface=SDL_CreateRGBSurfaceFrom(&pixels[0],Width,Height,32,Width*4,
        0x00ff0000,0x0000ff00,0x000000ff,0xff000000);
    Check(surface,"capture surface failed");
    const std::string path=std::string("goldshire-")+c.name+".bmp";
    const int saved=SDL_SaveBMP(surface,path.c_str());SDL_FreeSurface(surface);Check(saved==0,"private raster capture failed");
}
void RasterTick() {
    if(++test.featureFrame<(test.phase?24u:120u))return;
    auto& gx=*test.device;const auto& c=rasterCases[test.phase];
    CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;
    TSGrowableArray<CImVector> before,after;gx.DeviceReadPixels(rect,before);
    Check(before.Count()==Width*Height,"raster world readback incomplete");
    double mean=0;for(unsigned i=0;i<before.Count();++i)mean+=double(before[i].r)+before[i].g+before[i].b;
    mean/=3*before.Count();Check(mean>1 && mean<254,"production raster world is empty or saturated");
    const auto completed=gx.SceneWorldTestCompletedFrame();
    Check(bool(completed[0])==bool(std::atoi(c.hdr)),"production requested float scene did not complete");
    Check(bool(completed[1])==bool(std::atoi(c.aa)),"production requested AA did not resolve");
    if(std::atof(c.scale)!=1)Check(completed[2]==Width/2 && completed[3]==Height/2,"actual scene was not independently half-scaled");
    Check(!gx.SceneHdrActive(),"float scene remained active across native UI boundary");
    if(!test.phase) {
        TSGrowableArray<float> depth;gx.DeviceReadDepths(rect,depth);unsigned covered=0;
        Check(depth.Count()==Width*Height,"raster depth readback incomplete");
        for(unsigned i=0;i<depth.Count();++i)covered+=std::isfinite(depth[i]) && depth[i]>0 && depth[i]<1;
        Check(covered>Width*Height/5,"Goldshire camera did not render substantial actual geometry");
        test.rasterBaseline.assign(&before[0],&before[0]+before.Count());
    }
    const auto* info=DayNight::GetInfo();
    if(test.phase==2) {test.nightAmbient=info->m_light0.m_ambient;test.nightDiffuse=info->m_light0.m_diffuse;test.nightDirection=info->m_light0.m_dir;}
    if(test.phase==3) {
        const auto a=info->m_light0.m_ambient;
        Check(unsigned(a.r)+a.g+a.b<unsigned(test.nightAmbient.r)+test.nightAmbient.g+test.nightAmbient.b,"actual midnight ambient did not darken");
        Check(!std::memcmp(&test.nightDiffuse,&info->m_light0.m_diffuse,sizeof(CImVector)) &&
            (test.nightDirection-info->m_light0.m_dir).SquaredMag()<1e-10f,"night darkness changed actual directional light");
        Check(mean<test.means[2],"ambient-only darkness did not darken the actual world");
    }
    if(test.phase==9)Check(mean>test.means[8]+2,"manual exposure did not meaningfully brighten actual world");
    if(test.phase==11) {
        double difference=0;for(unsigned i=0;i<before.Count();++i) {
            const auto a=before[i],b=test.rasterBaseline[i];
            difference+=std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b));
        }
        difference/=3*before.Count();
        std::fprintf(stderr,"Raster feature-off roundtrip mean absolute RGB difference=%g (live animation allowed)\n",difference);
        Check(difference<5,"feature-off roundtrip changed production raster appearance beyond animation tolerance");
    }
    GxuFontRenderBatch(test.labelBatch);gx.DeviceReadPixels(rect,after);
    Check(after.Count()==before.Count(),"native UI readback incomplete");
    unsigned visible=0;
    for(unsigned i=0;i<after.Count();++i) {
        const auto a=after[i],b=before[i];
        if(a.r==255 && a.g==255 && a.b==255 && (b.r<230 || b.g<230 || b.b<230)) {
            ++visible;if(!test.phase)test.uiOpaque.push_back(i);
        }
    }
    SaveRasterCapture(c,after);
    unsigned changed=0;for(unsigned i=0;i<after.Count();++i)changed+=std::memcmp(&before[i],&after[i],sizeof(CImVector))!=0;
    std::fprintf(stderr,"Raster native overlay: msdf=%u visibleOpaque=%u changedPixels=%u viewport=(%g,%g)\n",test.font->m_isMsdf,visible,changed,
        double(gx.m_viewport.m_x.m_h),double(gx.m_viewport.m_y.m_h));
    Check(visible>30,"actual archive font failed to produce native opaque text");
    Check(test.font->m_isMsdf==c.msdf,"actual archive font ignored Classic/MSDF selection");
    if(c.msdf)for(auto i:test.uiOpaque)Check(after[i].r==255 && after[i].g==255 && after[i].b==255,"world feature changed native opaque glyph pixels");
    test.means.push_back(mean);
    std::fprintf(stderr,"Raster case=%s meanRGB=%g uiOpaque=%u time=%g ambient=(%u,%u,%u) hdr=%u\n",
        c.name,mean,visible,double(info->m_dayProgression),info->m_light0.m_ambient.r,info->m_light0.m_ambient.g,info->m_light0.m_ambient.b,unsigned(completed[0]));
    if(++test.phase==sizeof(rasterCases)/sizeof(*rasterCases)) {
        Check(!test.networkAttempts,"raster fixture attempted network");test.result=0;EventPostClose();return;
    }
    SelectRasterCase();
}
void TerrainTick() {
    auto& gx=*test.device;const auto& census=CMap::s_lowDetail.GetCensus();
    if(test.ticks%15==0) std::fprintf(stderr,"Terrain phase=%u tick=%u setting=%g horizon=%g admitted=%u prepared=%u chunks=%u min=%g ready=%u\n",
        test.phase,test.ticks,double(census.setting),double(census.horizonFar),census.admitted,census.preparedTiles,
        census.preparedChunks,double(census.minPreparedDistance),FarAdtCensus().ready);
    if(test.phase==3) {
        if(++test.resetFrames<3)return;
        const auto usage=gx.SceneFarResourceUsage();
        Check(!census.preparedTiles && !usage.farGeometryBytes && !usage.farImageBytes,"Classic reset retained drawable FAR GPU resources");
        Check(!FarAdtCensus().transferred,"Classic reset retained producer tickets");
        Check(!test.networkAttempts,"production fixture attempted network");
        test.result=0;
        EventPostClose();return;
    }
    CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;
    if(test.phase==0) {
        if(test.ticks<20 || !census.wdlRead)return;
        TSGrowableArray<float> depth;gx.DeviceReadDepths(rect,depth);
        Check(depth.Count()==Width*Height,"baseline depth readback incomplete");
        unsigned covered=0;for(unsigned i=0;i<depth.Count();++i)if(std::isfinite(depth[i]) && depth[i]>0 && depth[i]<1)++covered;
        Check(!covered,"4000-unit horizon rendered ground over 4000 units below camera");
        Set(CWorldParam::cvar_landscapeDistance,"10000");test.phase=1;Stage("10000 awaiting real prepared ADTs");return;
    }
    if(test.phase==1) {
        if(!census.preparedTriangles || census.minPreparedDistance<=4000)return;
        TSGrowableArray<CImVector> colour;TSGrowableArray<float> depth;
        gx.DeviceReadPixels(rect,colour);gx.DeviceReadDepths(rect,depth);
        Check(colour.Count()==Width*Height && depth.Count()==Width*Height,"prepared world readback incomplete");
        test.detailedColour.assign(&colour[0],&colour[0]+colour.Count());
        test.detailedDepth.assign(&depth[0],&depth[0]+depth.Count());
        test.suppressPrepared=true;test.phase=2;Stage("same-view coarse control");return;
    }
    Check(!census.preparedTiles,"coarse control still selected prepared geometry");
    TSGrowableArray<CImVector> colour;TSGrowableArray<float> depth;gx.DeviceReadPixels(rect,colour);gx.DeviceReadDepths(rect,depth);
    Check(colour.Count()==Width*Height && depth.Count()==Width*Height,"coarse readback incomplete");
    unsigned covered=0,changed=0;
    for(unsigned i=0;i<depth.Count();++i) {
        const auto d=test.detailedDepth[i];if(!std::isfinite(d) || d<=0 || d>=1)continue;
        ++covered;const auto a=test.detailedColour[i],b=colour[i];
        if(std::abs(d-depth[i])>1e-7f || std::abs(int(a.r)-int(b.r))>1 || std::abs(int(a.g)-int(b.g))>1 || std::abs(int(a.b)-int(b.b))>1)++changed;
    }
    std::fprintf(stderr,"Terrain actual covered=%u prepared-versus-coarse changed=%u\n",covered,changed);
    Check(covered>Width*Height/20 && changed>=16,"real prepared ADTs did not change distant colour/depth");
    Check(gx.SceneFarResourceUsage().farGeometryBytes>0,"unload control had no live geometry");
    CMap::Unload();
    Check(!FarAdtCensus().transferred && !FarAdtCensus().ready && !FarAdtCensus().resident,"map unload retained live tickets/payloads");
    Set(CWorldParam::cvar_landscapeDistance,"0");test.suppressPrepared=false;test.phase=3;
    const auto* map=g_mapDB.GetRecord(test.northrend?571:0);auto position=test.frame->m_camera->m_position;
    CWorld::LoadMap(map->m_directory,&position,map->m_id,1);Stage("map reload and Classic retirement");
}
void CheckCompletedWorldReplay(TSGrowableArray<CImVector>& expected) {
    auto& gx=*test.device;
    GxWorld::WorldFrameDescription frame;
    frame.scene=reinterpret_cast<uintptr_t>(test.frame->m_camera);frame.width=Width;frame.height=Height;
    frame.viewport={0,0,Width,Height};
    const auto& view=gx.m_xforms[GxXform_View];
    static_assert(sizeof(frame.view)==sizeof(C44Matrix));
    std::memcpy(frame.view.data(),&view.m_mtx[view.m_level],sizeof(C44Matrix));
    std::memcpy(frame.projection.data(),&gx.m_projection,sizeof(C44Matrix));
    CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;
    TSGrowableArray<float> depthBefore;gx.DeviceReadDepths(rect,depthBefore);
    Check(depthBefore.Count()==expected.Count(),"actual RT capture depth extent mismatch");
    Check(gx.SceneCaptureCompletedWorld(frame),"actual RT completed-image capture failed");
    // The ordinary readback seam completes the capture's real GPU submission;
    // the completed-image selector is then tested without guessing completion.
    TSGrowableArray<CImVector> before;gx.DeviceReadPixels(rect,before);
    gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
    GxWorld::WorldFrame displayed;
    Check(gx.SceneComposeCompletedWorld(frame.scene,&displayed),"actual RT completed-image replay failed");
    Check(displayed.description.scene==frame.scene && displayed.description.view==frame.view &&
        displayed.description.projection==frame.projection,"actual RT image/camera publication mismatch");
    TSGrowableArray<CImVector> after;TSGrowableArray<float> depthAfter;
    gx.DeviceReadPixels(rect,after);gx.DeviceReadDepths(rect,depthAfter);
    Check(after.Count()==expected.Count() && depthAfter.Count()==depthBefore.Count(),"actual RT replay extent mismatch");
    for(unsigned i=0;i<after.Count();++i) {
        const auto a=after[i],b=expected[i];
        Check(a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a,"actual RT completed replay changed final colour/alpha");
        Check(depthAfter[i]==depthBefore[i],"actual RT completed replay changed published depth");
    }
    std::fprintf(stderr,"Actual terrain RT completed publication: exact colour/alpha/depth and paired camera, %u pixels\n",after.Count());
}
void ShadowVisibilityTick(){
    auto& gx=*test.device;const auto status=gx.SceneShadowStatus();
    if(test.ticks%30==0)std::fprintf(stderr,"World RT visibility tick=%u result=%u rendered=%u terrain=%llu models=%llu wmo=%llu triangles=%llu unsupported=%llu\n",test.ticks,unsigned(status.result),status.diagnosticRendered,
        (unsigned long long)status.terrain,(unsigned long long)status.models,(unsigned long long)status.buildings,(unsigned long long)status.triangles,(unsigned long long)status.unsupported);
    if(++test.featureFrame<(test.phase?24u:90u))return;
    CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;TSGrowableArray<CImVector> pixels;gx.DeviceReadPixels(rect,pixels);
    Check(pixels.Count()==Width*Height,"world RT visibility readback missing");
    if(!test.phase){
        Check(status.result==GxShadow::FrameResult::Disabled&&!status.diagnosticRendered,"RT-off baseline unexpectedly queried shadows");
        test.rasterBaseline.assign(&pixels[0],&pixels[0]+pixels.Count());
        if(test.terrainShadows){RasterCase capture{"terrain-ray-baseline","12","0","1","0","0","0"};SaveRasterCapture(capture,pixels);}
        test.phase=1;test.featureFrame=0;
        if(test.terrainPressure)Check(gx.SceneFarTestNearCharge(false,64ull*1024*1024),"physical near-pressure fixture reservation failed");
        Set(test.terrainShadows?CWorldParam::cvar_nativeTerrainRayShadows:CWorldParam::cvar_nativeRayTracing,"1");return;
    }
    if(test.phase==(test.terrainShadows?4u:2u)){
        Check(status.result==GxShadow::FrameResult::Disabled&&!status.diagnosticRendered,"RT-off did not restore disabled status");
        double difference=0;for(unsigned i=0;i<pixels.Count();++i){const auto a=pixels[i],b=test.rasterBaseline[i];difference+=std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b));}
        difference/=3*pixels.Count();std::fprintf(stderr,"World RT-off roundtrip mean absolute RGB difference=%g\n",difference);
        Check(difference<5,"RT-off did not restore raster appearance");
        Check(!test.networkAttempts,"world RT diagnostic attempted network");test.result=0;EventPostClose();return;
    }
    if(test.terrainShadows){
        if(test.terrainFallback||(test.terrainPressure&&test.phase==1)){
            const bool expectedFailure=test.terrainPressure?(status.result==GxShadow::FrameResult::AllocationFailed&&status.terrainReceivers==0):
                (status.result==GxShadow::FrameResult::UnsupportedCoverage&&status.unsupported>0&&status.terrainReceivers>0);
            Check(!status.terrainRendered&&expectedFailure,
                "late missing-source fixture did not reject a partially shaded frame");
            double difference=0;for(unsigned i=0;i<pixels.Count();++i){auto a=pixels[i],b=test.rasterBaseline[i];difference+=std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b));}
            difference/=3*pixels.Count();std::fprintf(stderr,"Late terrain source fallback: priorReceivers=%llu meanRGB=%g\n",(unsigned long long)status.terrainReceivers,difference);
            Check(difference<1,"late terrain source failure retained partial directional shading");
            RasterCase capture{"terrain-ray-late-fallback","12","0","1","0","0","0"};SaveRasterCapture(capture,pixels);
            test.featureFrame=0;
            if(test.terrainPressure){gx.SceneFarTestReleaseNear();test.phase=2;Check(CWorldParam::cvar_nativeTerrainRayShadows->m_intValue==1,"pressure changed saved RT intent");}
            else{test.phase=4;Set(CWorldParam::cvar_nativeTerrainRayShadows,"0");}
            return;
        }
        Check(status.terrainRendered&&status.terrainReceivers>0,"terrain directional receivers did not complete (see fallback diagnostics)");
        Check(status.result==GxShadow::FrameResult::Ready&&!status.unsupported,"terrain slice silently omitted required support");
        if(test.terrainPressure){
            std::fprintf(stderr,"Terrain RT recovered after physical pressure: receivers=%llu\n",(unsigned long long)status.terrainReceivers);
            test.phase=4;test.featureFrame=0;Set(CWorldParam::cvar_nativeTerrainRayShadows,"0");return;
        }
        if(test.phase>1){
            unsigned brighter=0;double delta=0;
            for(unsigned i=0;i<pixels.Count();++i){const auto a=pixels[i],b=test.shadowPrevious[i];const int d=int(a.r)+a.g+a.b-int(b.r)-b.g-b.b;brighter+=d>9;delta+=d;}
            std::fprintf(stderr,"Actual terrain caster isolation phase=%u brighter=%u meanDelta=%g\n",test.phase,brighter,delta/(3*pixels.Count()));
            Check(brighter>100,"removing real model/building casters did not expose their terrain shadows");
            RasterCase capture{test.phase==2?"terrain-ray-no-models":"terrain-ray-terrain-only","12","0","1","0","0","0"};SaveRasterCapture(capture,pixels);
            test.shadowPrevious.assign(&pixels[0],&pixels[0]+pixels.Count());test.featureFrame=0;
            if(test.phase++==2)gx.SceneShadowTestCasters(false,false);
            else{gx.SceneShadowTestCasters(true,true);Set(CWorldParam::cvar_nativeTerrainRayShadows,"0");}
            return;
        }
        Check(status.terrain&&status.models&&status.buildings,"terrain slice lacked actual world caster domains");
        unsigned coloured=0,changed=0;double difference=0;
        for(unsigned i=0;i<pixels.Count();++i){auto a=pixels[i],b=test.rasterBaseline[i];
            coloured+=std::max({a.r,a.g,a.b})-std::min({a.r,a.g,a.b})>8;
            const auto d=std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b));changed+=d>9;difference+=d;}
        difference/=3*pixels.Count();
        std::fprintf(stderr,"Actual terrain RT receivers=%llu coloured=%u changed=%u meanDifference=%g\n",(unsigned long long)status.terrainReceivers,coloured,changed,difference);
        Check(coloured>pixels.Count()/5&&changed>1000&&difference>1,"terrain shadow slice was a mask or had no meaningful colour effect");
        RasterCase capture{"terrain-ray-shadows","12","0","1","0","0","0"};SaveRasterCapture(capture,pixels);
        CheckCompletedWorldReplay(pixels);
        test.shadowPrevious.assign(&pixels[0],&pixels[0]+pixels.Count());
        test.phase=2;test.featureFrame=0;gx.SceneShadowTestCasters(false,true);return;
    }
    if(!status.diagnosticRendered)return;
    Check(status.result==GxShadow::FrameResult::Ready&&!status.unsupported,"strict world visibility accepted an incomplete caster frame");
    Check(status.terrain && status.models && status.buildings && status.triangles,"world RT did not collect all three actual geometry domains");
    unsigned lit=0,shadow=0;for(unsigned i=0;i<pixels.Count();++i){const auto p=pixels[i];lit+=p.r>240&&p.g>240&&p.b>240;shadow+=p.r<50&&p.g<50&&p.b<50;}
    const auto centre=pixels[Height/2*Width+Width/2];
    std::fprintf(stderr,"Actual game RT visibility lit=%u shadow=%u centreRGB=(%u,%u,%u); diagnostic, NOT full material/lighting coverage\n",lit,shadow,centre.r,centre.g,centre.b);
    RasterCase capture{"rt-visibility","12","0","1","0","0","0"};SaveRasterCapture(capture,pixels);
    Check(lit>100&&shadow>100,"actual world RT query output was vacuous");
    test.phase=2;test.featureFrame=0;Set(CWorldParam::cvar_nativeRayTracing,"0");
}
void DropDetailInstanceSources();
#include "IndependentWorldUiTest.inl"
#include "DetailDoodadInstancingTest.inl"
int32_t Tick(const void*,void*) {
    try {
        Check(++test.ticks<=(test.independentWorld?100000u:1200u),"production world did not reach required state");
        Check(std::chrono::steady_clock::now()-test.started<std::chrono::seconds(120),"production render deadline exceeded");
        if(test.independentWorld && test.ticks>(test.detailInstancing?140u:90u)){IndependentUiTick();return 1;}
        auto& gx=*test.device;auto* camera=test.frame->m_camera;auto position=camera->m_position;
        test.frame->m_elapsedSec=1.f/60;
        camera->SetGxProjectionAndView(CRect{0,0,1,1});
        test.frame->UpdateDayNight(test.frame->m_elapsedSec);
        C3Vector target=position+camera->Forward();CWorld::Update(&position,&target,&position);
        gx.SceneClear(3,CImVector(0,0,0,255));test.frame->OnWorldRender();
        if(test.detailInstancing)DetailInstancingTick();
        if(!test.independentWorld){if(test.shadowVisibility||test.terrainShadows)ShadowVisibilityTick();else if(test.rasterFeatures)RasterTick();else TerrainTick();}
        gx.ScenePresent();
    }catch(const std::exception& error) {std::fprintf(stderr,"FAIL synchronous terrain: %s\n",error.what());test.result=1;EventPostClose();}
    return 1;
}
int32_t Initialize(const void*,void*) {
    try {
        Stage("context");BaseInitializeContext();HeapUsageInitialize();MountAssets();
        if(test.terrainFallback){
            if(const auto* previous=std::getenv("WOWDECOMP_SHADER_ROOT")){test.hadShaderRoot=true;test.previousShaderRoot=previous;}
            auto root=std::filesystem::current_path()/("late-terrain-receiver-"+std::to_string(getpid()));
            Check(!std::filesystem::exists(root),"refuse existing receiver fixture directory");
            Check(std::filesystem::create_directory(root),"receiver fixture root creation failed");test.receiverFixtureRoot=root;
            const std::filesystem::path authored=WOWDECOMP_TEST_RECEIVER_SOURCE_ROOT;
            std::filesystem::copy(authored/"shared",root/"shared",std::filesystem::copy_options::recursive);
            for(const auto* relative:{"directional/vertex/vs_3_0/Terrain/32.slang","directional/pixel/ps_3_0/Terrain1/4.slang"}){
                auto target=root/relative;std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(authored/relative,target);}
            Check(setenv("WOWDECOMP_SHADER_ROOT",root.c_str(),1)==0,"receiver fixture source root setup failed");
        }
        test.device=std::make_unique<CGxDeviceVulkan>();g_theGxDevicePtr=test.device.get();
        if(test.terrainPressure)test.device->SceneFarTestAdmissionLimits(64ull*1024*1024,192ull*1024*1024,32ull*1024*1024);
        CGxFormat format;format.m_window=1;format.m_maximize=0;format.m_size={int32_t(Width),int32_t(Height)};if(test.independentWorld)format.m_vsync=test.independentVsync;
        Check(test.device->DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"native device creation failed");
        test.device->m_cursorVisible=0;
        Stage("services");AsyncFileReadRegisterCVars();TextureInitialize();test.textures=true;
        M2Initialize(GetM2CvarFlags(),0);test.models=true;
        SysMsgInitialize();Check(FrameScript_Initialize(0)!=0,"script context failed");ScrnInitialize(0);ILayerDestroy();
        Stage("database");ClientDBInitialize();CWorldParam::Initialize();CameraRegisterCVars();
        if(test.detailInstancing){
            Check(CWorldParam::cvar_nativeDetailInstancing && !CWorldParam::cvar_nativeDetailInstancing->m_intValue,"Detail instancing must default Off");
            CWorldParam::cvar_nativeDetailInstancing->Set("2",true,false,false,false);
            Check(!CWorldParam::cvar_nativeDetailInstancing->m_intValue,"Invalid instancing toggle changed saved intent");
        }
        Set(CWorldParam::cvar_landscapeDistance,(test.rasterFeatures||test.shadowVisibility||test.terrainShadows||test.independentWorld)?"0":"4000");Set(CWorldParam::cvar_nativeAA,"0");
        Set(CWorldParam::cvar_nativeRayTracing,"0");Set(CWorldParam::cvar_nativeTerrainRayShadows,"0");Set(CWorldParam::cvar_nativeRayStrict,"1");
        Set(CWorldParam::cvar_nativeHDR,"0");Set(CWorldParam::cvar_localTimePreview,"12");
        Stage("world");CWorld::Initialize();test.world=true;ShadowInit();GxuLightInitialize();GxuLightSetGridSize(16.665001f);
        SpellVisualsInitialize();test.spells=true;
        Check(!ClntObjMgrGetCurrent(),"offline manager must start empty");
        test.objects=std::make_unique<ObjectMgr>(0);NetClient::PushObjMgr(reinterpret_cast<ClntObjMgr*>(test.objects.get()));
        Stage("frame");CGGameUI::RegisterGameCVars();CoordinateSetAspectRatio(float(Width)/Height);
        test.top=std::make_unique<CSimpleTop>();test.frame=static_cast<CGWorldFrame*>(CGWorldFrame::Create(nullptr));
        Check(test.frame,"world frame creation failed");test.frame->SetAllPoints(test.top.get(),1);CLayoutFrame::ResizePending();
        auto* camera=test.frame->m_camera;Check(camera==CGWorldFrame::GetActiveCamera(),"production camera missing");
        camera->m_position=test.northrend?C3Vector(3060,5300,6000):C3Vector(-9464,-95,6000);camera->SetFacing(C3Vector(1,0,-1));
        camera->SetGxProjectionAndView(CRect{0,0,1,1});const auto* map=g_mapDB.GetRecord(test.northrend?571:0);
        Check(map && std::strcmp(map->m_directory,test.northrend?"Northrend":"Azeroth")==0,"map directory mismatch");
        if(test.rasterFeatures||test.shadowVisibility||test.terrainShadows||test.independentWorld){camera->m_position=C3Vector(-9505,70,62);camera->SetFacing(C3Vector(1,-.4f,-.15f));}
        if(test.rasterFeatures || test.independentWorld) {
            if(!g_FTLibrary){GxuFontInitialize();test.ownsFonts=true;}
            Check(GxuFontCreateFont("Fonts\\FRIZQT__.TTF",18.f/Height,test.font,8),"actual game font load failed");
            test.labelBatch=GxuFontCreateBatch(false,false);
            Check(GxuFontCreateString(test.font,"Goldshire: Native raster - Agj 0123456789",18.f/Height,C3Vector(.05f,.8f,0),
                .9f,.15f,0,test.label,GxVJ_Top,GxHJ_Left,0,CImVector(255,255,255,255),0,1),"actual font layout failed");
            GxuFontAddToBatch(test.labelBatch,test.label);if(test.rasterFeatures)SelectRasterCase();
            if(test.independentWorld){
                GxSetMaxFPS(0);GxSetMaxFPSBk(0);
                // Offline fixture has no server session; defer its periodic timer request beyond the test deadline.
                CGWorldStateInfo::m_nextUITimeUpdate=OsGetAsyncTimeMs()+120000;
                InputControlInitialize();test.inputs=true;
                auto* L=FrameScript_GetContext();lua_pushstring(L,"Fonts\\FRIZQT__.TTF");lua_setfield(L,LUA_GLOBALSINDEX,"DAMAGE_TEXT_FONT");
                WorldTextInitialize();test.worldText=true;
            }
        }
        auto position=camera->m_position;Stage("map load");CWorld::LoadMap(map->m_directory,&position,map->m_id,1);
        test.started=std::chrono::steady_clock::now();EventRegister(EVENT_ID_IDLE,Tick);Stage("render");
    }catch(const std::exception& error) {std::fprintf(stderr,"FAIL terrain bootstrap: %s\n",error.what());test.result=1;EventPostClose();}
    return 1;
}
int32_t Destroy(const void*,void*) {
    EventUnregister(EVENT_ID_IDLE,Tick);
    if(test.spells)SpellVisualsShutdown();
    if(test.label)GxuFontDestroyString(test.label);
    if(test.labelBatch)GxuFontDestroyBatch(test.labelBatch);
    if(test.font)GxuFontDestroyFont(test.font);
    test.top.reset();test.frame=nullptr;
    if(test.worldText)WorldTextShutdown();
    if(test.inputs)InputControlDestroy();
    if(test.ownsFonts)GxuFontShutdown();
    if(test.world)CWorld::Destroy();
    if(test.models)M2Destroy();
    if(test.objects){NetClient::PushObjMgr(nullptr);test.objects.reset();}
    if(test.textures)TextureGxCacheFlush();
    if(test.device) {
        test.device->DeviceDestroy();
        if(!test.device->SceneHdrTestValidationClean()) {
            std::fputs("FAIL production world emitted Vulkan diagnostics\n",stderr);test.result=1;
        }
    }
    test.device.reset();g_theGxDevicePtr=nullptr;
    for(auto i=test.archives.rbegin();i!=test.archives.rend();++i)SFile::CloseArchive(*i);
    test.archives.clear();
    if(!test.receiverFixtureRoot.empty()){
        if(test.hadShaderRoot)setenv("WOWDECOMP_SHADER_ROOT",test.previousShaderRoot.c_str(),1);else unsetenv("WOWDECOMP_SHADER_ROOT");
        std::filesystem::remove_all(test.receiverFixtureRoot);
    }
    EventInitiateShutdown();return 1;
}
}
extern "C" bool RealFarDraw(CMapAreaLow*) asm("__real__Z16FarAdtRenderTileP11CMapAreaLow");
extern "C" bool ObserveFarDraw(CMapAreaLow*) asm("__wrap__Z16FarAdtRenderTileP11CMapAreaLow");
extern "C" bool ObserveFarDraw(CMapAreaLow* area) {
    if(test.suppressPrepared)return false;
    float saved[2][1024];for(unsigned i=0;i<2;++i)std::memcpy(saved[i],CGxDevice::s_shadowConstants[i].constants,sizeof(saved[i]));
    const bool result=RealFarDraw(area);
    for(unsigned i=0;i<2;++i)Check(!std::memcmp(saved[i],CGxDevice::s_shadowConstants[i].constants,sizeof(saved[i])),"prepared terrain did not restore shader constants");
    return result;
}
extern "C" int __real_connect(int,const sockaddr*,socklen_t);
extern "C" int __wrap_connect(int socket,const sockaddr* address,socklen_t size) {
    if(address && (address->sa_family==AF_INET || address->sa_family==AF_INET6)){++test.networkAttempts;errno=EPERM;return -1;}
    return __real_connect(socket,address,size);
}
int main(int argc,char** argv) {
    if(argc!=2 || (std::strcmp(argv[1],"--elwynn-distance") && std::strcmp(argv[1],"--northrend-distance") && std::strcmp(argv[1],"--raster-features") && std::strcmp(argv[1],"--rt-visibility") && std::strcmp(argv[1],"--terrain-ray-shadows") && std::strcmp(argv[1],"--terrain-ray-fallback") && std::strcmp(argv[1],"--terrain-ray-pressure") && std::strcmp(argv[1],"--independent-ui") && std::strcmp(argv[1],"--independent-ui-rt") && std::strcmp(argv[1],"--independent-ui-vsync") && std::strcmp(argv[1],"--detail-instancing") && std::strcmp(argv[1],"--detail-instancing-rt"))) {
        std::fputs("Pass --elwynn-distance, --northrend-distance, --raster-features, --rt-visibility, --terrain-ray-shadows, --terrain-ray-fallback or --terrain-ray-pressure; environment cannot select a weaker test.\n",stderr);return 1;
    }
    const char* assets=std::getenv("WOWDECOMP_TEST_WORLD_ASSETS");
    if(!assets || !*assets){std::fputs("WOWDECOMP_TEST_WORLD_ASSETS must identify read-only Data\n",stderr);return 1;}
    test.independentVsync=!std::strcmp(argv[1],"--independent-ui-vsync");
    test.detailInstancing=!std::strcmp(argv[1],"--detail-instancing")||!std::strcmp(argv[1],"--detail-instancing-rt");
    test.independentRT=test.independentVsync||!std::strcmp(argv[1],"--independent-ui-rt")||!std::strcmp(argv[1],"--detail-instancing-rt");test.independentWorld=test.detailInstancing||test.independentRT||!std::strcmp(argv[1],"--independent-ui");
    if(test.independentWorld){Width=test.independentVsync?1428:1280;Height=test.independentVsync?2516:720;}
    test.assets=std::filesystem::absolute(assets);test.northrend=!std::strcmp(argv[1],"--northrend-distance");
    test.rasterFeatures=!std::strcmp(argv[1],"--raster-features");test.shadowVisibility=!std::strcmp(argv[1],"--rt-visibility");test.terrainFallback=!std::strcmp(argv[1],"--terrain-ray-fallback");test.terrainPressure=!std::strcmp(argv[1],"--terrain-ray-pressure");test.terrainShadows=test.terrainFallback||test.terrainPressure||!std::strcmp(argv[1],"--terrain-ray-shadows");
    Check(std::filesystem::is_directory(test.assets),"external world assets missing");
    StormRtlInitialize();BaseInitializeGlobal();EventInitialize(1,0);OsTimeStartup(static_cast<OsTimeManager::TimingMethod>(0));
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0)return 1;
    g_clientEventContext=EventCreateContextEx(1,Initialize,Destroy,0,0);EventDoMessageLoop();
    if(!test.result && test.independentWorld)std::puts("PASS original WorldFrame/strata/UI/input pipeline during repeated 500ms producer holds, paired interaction/lifecycle cancellation and synchronous rollback");
    else if(!test.result && test.terrainFallback)std::puts("PASS late missing receiver source restores whole raster frame; NOT positive shadow acceptance");
    else if(!test.result && test.terrainPressure)std::puts("PASS physical admission failure retains raster and recovers without changing RT intent; NOT full coverage acceptance");
    else if(!test.result && test.terrainShadows)std::puts("PASS actual Goldshire terrain directional receiver slice and off/on/off parity; NOT full world receiver/corpus acceptance");
    else if(!test.result && test.shadowVisibility)std::puts("PASS actual Goldshire AS visibility diagnostic only; NOT complete RT material/receiver acceptance");
    else if(!test.result)std::puts(test.rasterFeatures?
        "PASS synchronous Goldshire raster feature matrix, actual archive font, native UI, night/exposure changes and feature-off parity; no login":
        "PASS synchronous original world: 4000/10000 ADT colour/depth versus coarse path, map unload/reload and Classic resource retirement; no login");
    return test.result;
}
