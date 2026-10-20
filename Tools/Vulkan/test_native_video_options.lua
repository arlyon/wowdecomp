-- Exercise the production settings script in the client's bundled Lua runtime.
local frames, saved, writes, categories = {}, {gxBackend="auto",gxAdapter="auto"}, 0, 0
local methods = {}
for _, key in ipairs({"SetAllPoints","SetWidth","SetJustifyH"}) do methods[key] = function() end end
function methods:Hide() self.hidden=true end
function methods:ClearAllPoints() self.point=nil end
function methods:SetPoint(...) self.point={...} end
function methods:SetText(text) self.text=text end
function methods:SetScript(name, fn) self.scripts[name]=fn end
function methods:CreateFontString()
    local text=setmetatable({scripts={}}, {__index=methods})
    self.fontStrings=self.fontStrings or {};table.insert(self.fontStrings,text)
    return text
end
function CreateFrame(kind,name,parent)
    local frame=setmetatable({name=name,scripts={},parent=parent}, {__index=methods})
    frames[name]=frame;_G[name]=frame;return frame
end
local identity=string.rep("a",32)..":3"
local snapshot={backends={{id="vulkan",name="Native Vulkan"}},
    adapters={{id=identity,name="GPU |cBAD",driver="RADV",compatible=true,reason="present verified"}},
    activeBackend="vulkan",activeAdapter=identity,activeName="GPU",requestedBackend="auto",requestedAdapter="auto",
    backendOverride="",adapterOverride="",loaderFilter="",warning=""}
function GetNativeGraphicsDevices() return snapshot end
function UIDropDownMenu_SetWidth() end
function UIDropDownMenu_SetText(frame,text) frame.text=text end
function UIDropDownMenu_Initialize(frame,fn) frame.initialize=fn;fn() end
function UIDropDownMenu_CreateInfo() return {} end
local menu={}
function UIDropDownMenu_AddButton(info) table.insert(menu,info) end
VideoOptionsFramePanelContainer={}
VideoOptionsFrameApply={Enable=function() end}
function GetCVar(name) assert(saved[name],"Unexpected feature CVar: "..name);return saved[name] end
function SetCVar(name,value) assert(saved[name]);saved[name]=value;writes=writes+1 end
function VideoOptionsPanel_OnLoad(panel,okay,cancel,defaults,refresh)
    categories=categories+1;panel.okay=okay;panel.cancel=cancel;panel.default=defaults;panel.refresh=refresh
end
local display=CreateFrame("Frame","VideoOptionsResolutionPanel")
local modeWrites, remainingWrites=0,0
local function control(suffix,mode)
    local value=CreateFrame("Frame","VideoOptionsResolutionPanel"..suffix,display)
    value.newValue=2;value.defaultValue=0;value.value=1
    value.SetValue=function(self,n) if mode then modeWrites=modeWrites+1 else remainingWrites=remainingWrites+1 end;self.value=n end
    return value
end
local resolution=control("ResolutionDropDown",true)
local refresh=control("RefreshDropDown",true)
local samples=control("MultiSampleDropDown",false)
local ui=control("UseUIScale",false)
local vsync=control("VSync",false)
local window=control("Windowed",false)
local gamma=control("GammaSlider",false)
display.controls={resolution,samples,refresh,ui,vsync,window,gamma}
VideoOptionsResolutionPanelSubText=display:CreateFontString()
local function apply(self) for _,v in ipairs(self.controls) do v:SetValue(v.value) end end
display.okay=apply;display.cancel=apply
display.default=function() error("Legacy resolution defaults invoked") end
function VideoOptionsPanel_Default(self) for _,v in ipairs(self.controls) do v:SetValue(v.defaultValue) end end
loadProduction()()
assert(categories==1 and writes==0 and modeWrites==0,"Installation changed settings or installed unavailable features")
assert(resolution.hidden and refresh.hidden and resolution.newValue==nil and refresh.newValue==nil)
assert(#display.controls==5 and display.controls[1]==samples and display.controls[2]==ui and display.controls[3]==vsync and display.controls[4]==window and display.controls[5]==gamma)
assert(samples.point[2]==display and vsync.point[2]==samples and ui.point[2]==display)
display:okay();display:cancel();display:default()
assert(modeWrites==0 and remainingWrites==15,"Display Apply/Cancel/Defaults changed size or omitted remaining controls")
local renderer=frames.NativeRendererVideoPanel
local backend=frames.NativeRendererBackendDropDown
local adapter=frames.NativeRendererAdapterDropDown
menu={};backend.initialize();assert(#menu==2 and menu[2].text=="Native Vulkan")
menu[2].func();assert(saved.gxBackend=="auto","Backend changed before Apply")
renderer.cancel();renderer.okay();assert(saved.gxBackend=="auto")
menu={};adapter.initialize();assert(menu[2].text:find("||cBAD",1,true),"GPU markup not escaped")
menu[2].func();assert(saved.gxAdapter=="auto");renderer.scripts.OnShow();renderer.okay();assert(saved.gxAdapter==identity)
local before=writes
renderer.default();assert(writes==before and saved.gxAdapter==identity,"Defaults saved before Apply")
renderer.cancel();assert(saved.gxAdapter==identity)
renderer.default();renderer.okay();assert(saved.gxBackend=="auto" and saved.gxAdapter=="auto")
saved.gxBackend="dxvk";renderer.refresh();assert(backend.text:find("Unavailable",1,true))
renderer.okay();assert(saved.gxBackend=="dxvk" and snapshot.activeBackend=="vulkan")
snapshot.adapters[1].compatible=false;menu={};adapter.initialize();assert(menu[2].disabled)
snapshot.adapters[1].compatible=true;snapshot.adapters[1].id=""
menu={};adapter.initialize();assert(menu[2].disabled,"Missing identity selectable")
snapshot.adapters[1].id=identity
snapshot.backendOverride="vulkan";snapshot.adapterOverride=identity;snapshot.loaderFilter="RADV";snapshot.warning="Recovery"
renderer.refresh()
local status=renderer.fontStrings[#renderer.fontStrings].text
assert(status:find("WOWDECOMP_GX_BACKEND",1,true) and status:find("Loader-filtered",1,true) and status:find("Recovery",1,true))
loadProduction()();assert(categories==1,"Duplicate installation")
assert(not NativeLandscapeVideoPanel and not NativeNightLightingPanel and not NativeHdrVideoPanel,"Unimplemented graphics features exposed")
