#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/DeviceSelection.h"
#include "WoW/Source/Ui/NativeVideoOptions.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
std::map<std::string,lua_CFunction> functions;
void Check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
class Device : public CGxDevice {
public:
    void IRsSendToHw(EGxRenderState) override {}
    void* DeviceWindow() override {return nullptr;}
    void DeviceReadPixels(CiRect&,TSGrowableArray<CImVector>&) override {}
    void DeviceReadDepths(CiRect&,TSGrowableArray<float>&) override {}
    int32_t DeviceWM(EGxWM,uintptr_t,uintptr_t) override {return 0;}
    void CapsWindowSize(CRect&) override {}
    void CapsWindowSizeInScreenCoords(CRect&) override {}
    void PoolSizeSet(CGxPool*,uint32_t) override {}
    int32_t TexCopy(CGxTex*,const C2iVector&,const C2iVector&,const C2iVector&,uint32_t,uint32_t) override {return 0;}
    int32_t TexStretch(CGxTex*,const CiRect*,const CiRect*,uint32_t,uint32_t) override {return 0;}
    bool QueryBegin(CGxQuery*) override {return false;}
    bool QueryEnd(CGxQuery*) override {return false;}
    bool QueryGetParam(CGxQuery*,EGxQueryParam,uint32_t&) override {return false;}
    bool QueryGetData(CGxQuery*,uint32_t&) override {return false;}
    void IShaderReload(CGxShader*,const char*,const char*) override {}
    void IShaderCreate(CGxShader*) override {}
    void StereoSetConvergence(float) override {}
    float StereoGetConvergence() override {return 0;}
    void StereoSetSeparation(float) override {}
    float StereoGetSeparation() override {return 0;}
    bool StereoEnabled() override {return false;}
    void DeviceUnk81(uint32_t,uint32_t) override {}
    void DeviceUnk82() override {}
    void DeviceUnk83(uint32_t,uint32_t) override {}
    unsigned reloads=0;
    std::string path;
    bool SceneHdrSupported() const override {return true;}
    void SceneRequestHdrLut(const char* value,bool reload) override {path=value;reloads+=reload;}
    const char* SceneHdrLutStatus() const override {return "Pending (current LUT retained)";}
};
}
// Host registration and logging only: real CVar storage/callbacks and native
// FrameScript bridges are exercised, without config, archives or game startup.
void ConsoleWrite(const char*,COLOR_T) {}
void ConsolePrintf(const char*,...) {}
void ConsoleWriteA(const char*,COLOR_T,...) {}
void ConsolePostClose() {}
void ConsoleCommandExecute(const char*,int32_t) {}
void FrameScript_RegisterFunction(const char* name,lua_CFunction function) {functions[name]=function;}
void FrameScript_Execute(const char*,const char*,const char*) {}
int main() {
    try {
        CWorldParam::InitializeNativeHDRCVars();
        auto* on=CWorldParam::cvar_nativeHDR;
        auto* ev=CWorldParam::cvar_nativeHDRExposure;
        auto* lut=CWorldParam::cvar_nativeHDRLut;
        Check(on && ev && lut && on->m_intValue==0 && ev->m_floatValue==0 && std::string(lut->m_stringValue.m_str).empty(),"production defaults differ from Off/EV0/None");
        Check(CVar::Lookup("nativeHDRLut")==lut,"production LUT CVar not registered");
        ev->Set("2",true,false,false,true);
        for(const char* bad:{"nan","inf","9","-9","2x"}) {
            ev->Set(bad,true,false,false,true);
            Check(ev->m_floatValue==2,"production EV callback accepted invalid value");
        }
        ev->Set("-8",true,false,false,true);Check(ev->m_floatValue==-8,"minimum EV refused");
        ev->Set("8",true,false,false,true);Check(ev->m_floatValue==8,"maximum EV refused");
        on->Set("1",true,false,false,true);Check(on->m_intValue==1,"production HDR CVar failed");
        for(const char* bad:{"2","-1","nan","inf","1x"}) {
            on->Set(bad,true,false,false,true);
            Check(on->m_intValue==1 && std::string(on->m_stringValue.m_str)=="1","invalid HDR toggle stored");
        }
        lut->Set("controlled.cube",true,false,false,true);
        lut->Set("invalid.txt",true,false,false,true);
        Check(std::string(lut->m_stringValue.m_str)=="controlled.cube" && CVar::m_needsSave,"invalid path stored or persistence not marked");
        InstallNativeVideoOptions();
        auto* state=luaL_newstate();Check(state,"Lua state allocation failed");
        functions.at("GetNativeHdrStatus")(state);
        lua_getfield(state,-1,"supported");Check(!lua_toboolean(state,-1),"absent device reported supported");lua_settop(state,0);
        Device device;g_theGxDevicePtr=&device;
        functions.at("GetNativeHdrStatus")(state);
        lua_getfield(state,-1,"supported");Check(lua_toboolean(state,-1),"native support bridge lost capability");lua_pop(state,1);
        lua_getfield(state,-1,"lut");Check(std::string(lua_tostring(state,-1)).find("Pending")!=std::string::npos,"native status bridge lost pending state");lua_settop(state,0);
        functions.at("ReloadNativeHdrLut")(state);
        functions.at("ReloadNativeHdrLut")(state);
        Check(device.reloads==2 && device.path=="controlled.cube","Apply did not reload the actual persisted production CVar");
        g_theGxDevicePtr=nullptr;lua_close(state);
        std::cout<<"PASS: production CVar registration/defaults, finite EV rejection, path retention, persistence flag and actual registered native Lua bridges\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
