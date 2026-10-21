-- Exercise the production settings script in the client's bundled Lua runtime.
local frames, saved, writes, categories = {}, {gxBackend="auto",gxAdapter="auto",nightDarkness="0",localTimePreview="-1",nativeAA="0",nativeSceneScale="1",nativeHDR="0",nativeHDRExposure="0",nativeHDRLut="",landscapeDistance="0",nativeRayTracing="0",nativeRayStrict="1",nativeTerrainRayShadows="0",nativeIndependentWorld="0"}, 0, 0
local methods = {}
for _, key in ipairs({"SetAllPoints","SetWidth","SetJustifyH","SetMinMaxValues","SetValueStep","SetHeight","SetAutoFocus","SetMaxLetters","ClearFocus"}) do methods[key] = function() end end
function methods:Enable() self.enabled=true end
function methods:Disable() self.enabled=false end
function methods:SetChecked(value) self.checked=value end
function methods:GetChecked() return self.checked end
function methods:GetValue() return self.value end
function methods:SetValue(value)
    local changed = self.value ~= value
    self.value = value
    if changed and self.scripts.OnValueChanged then self.scripts.OnValueChanged(self, value) end
end
function methods:Hide() self.hidden=true end
function methods:ClearAllPoints() self.point=nil end
function methods:SetPoint(...) self.point={...} end
function methods:GetName() return self.name end
function methods:GetText() return self.text or "" end
function methods:EnableMouse(enabled) self.mouseEnabled=enabled end
function methods:SetText(text)
    local changed=self.text~=text;self.text=text
    if changed and self.scripts.OnTextChanged then self.scripts.OnTextChanged(self) end
end
function methods:SetScript(name, fn) self.scripts[name]=fn end
function methods:CreateFontString()
    local text=setmetatable({scripts={}}, {__index=methods})
    self.fontStrings=self.fontStrings or {};table.insert(self.fontStrings,text)
    return text
end
function CreateFrame(kind,name,parent)
    local frame=setmetatable({name=name,scripts={},parent=parent}, {__index=methods})
    frames[name]=frame;_G[name]=frame
    if kind == "Slider" then
        for _,suffix in ipairs({"Text","Low","High"}) do _G[name..suffix]=frame:CreateFontString() end
    end
    return frame
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
local scaleSupported, aaSupported = true, true
function IsNativeSceneScalingSupported() return scaleSupported end
function IsNativeTemporalAASupported() return aaSupported end
local independentSupported = true
function IsNativeIndependentWorldSupported() return independentSupported end
function GetNativeIndependentWorldStatus() return independentSupported and "Off: synchronous rendering" or "Unavailable; saved choice preserved" end
local hdrSupported, lutState, reloads=true,"None",0
function GetNativeHdrStatus() return {supported=hdrSupported,active=false,lut=lutState} end
local raySupported=true
local rayMessage="Off (original raster)"
function GetNativeShadowStatus() return {supported=raySupported,rendered=false,terrainRendered=false,status=rayMessage,triangles=0,receivers=0} end
function ReloadNativeHdrLut() reloads=reloads+1 end
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
assert(categories==6 and writes==0 and modeWrites==0,"Installation changed settings or installed unavailable features")
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
loadProduction()();assert(categories==6,"Duplicate installation")
assert(not NativeRayTracedShadowsCheck,"Unimplemented graphics features exposed")
local worldPanel=frames.NativeWorldPresentationPanel
local independent=frames.NativeIndependentWorldCheck
local beforeIndependent=writes
independent:SetChecked(true);independent.scripts.OnClick(independent)
worldPanel.scripts.OnShow();assert(writes==beforeIndependent and independent:GetChecked(),"World scheduling applied before Apply")
worldPanel.cancel();worldPanel.okay();assert(saved.nativeIndependentWorld=="0","World scheduling Cancel changed settings")
independent:SetChecked(true);independent.scripts.OnClick(independent);worldPanel.okay()
assert(saved.nativeIndependentWorld=="1","World scheduling Apply did not save")
worldPanel.default();assert(saved.nativeIndependentWorld=="1" and not independent:GetChecked(),"World scheduling Defaults saved early")
worldPanel.cancel();assert(independent:GetChecked(),"World scheduling Cancel lost saved intent")
independentSupported=false;worldPanel.refresh();worldPanel.okay()
assert(saved.nativeIndependentWorld=="1" and not independent.enabled and independent:GetChecked(),"Unsupported world scheduling erased saved intent")
independentSupported=true;worldPanel.default();worldPanel.okay();assert(saved.nativeIndependentWorld=="0","World scheduling Defaults did not apply")
local lighting=frames.NativeNightLightingPanel
local darkness=frames.NativeNightDarknessSlider
local clock=frames.NativeLocalTimePreviewSlider
assert(darkness:GetValue()==0 and clock:GetValue()==-1)
local oldWrites=writes
darkness:SetValue(80);clock:SetValue(18);lighting.scripts.OnShow()
assert(writes==oldWrites and darkness:GetValue()==80 and clock:GetValue()==18,"Lighting applied early or lost staged state")
lighting.cancel();assert(darkness:GetValue()==0 and clock:GetValue()==-1 and writes==oldWrites)
darkness:SetValue(80);clock:SetValue(18);lighting.okay()
assert(saved.nightDarkness=="80" and saved.localTimePreview=="18")
oldWrites=writes;lighting.default()
assert(writes==oldWrites and saved.nightDarkness=="80" and saved.localTimePreview=="18","Lighting defaults persisted early")
lighting.cancel();assert(darkness:GetValue()==80 and clock:GetValue()==18)
lighting.default();lighting.okay();assert(saved.nightDarkness=="0" and saved.localTimePreview=="-1")
darkness:SetValue(500);clock:SetValue(100);lighting.okay();assert(saved.nightDarkness=="100" and saved.localTimePreview=="24")
darkness:SetValue(-20);clock:SetValue(-20);lighting.okay();assert(saved.nightDarkness=="0" and saved.localTimePreview=="-1")
saved.localTimePreview="12.5";lighting.refresh();assert(clock:GetValue()==12.5,"External fractional preview lost")
local scene=frames.NativeLandscapeVideoPanel
local scale=frames.NativeSceneScaleSlider
local aa=frames.NativeTemporalAACheck
assert(scale:GetValue()==100 and not aa:GetChecked(),"Scene scale or AA default changed")
oldWrites=writes;scale:SetValue(75);aa:SetChecked(true);aa.scripts.OnClick(aa);scene.scripts.OnShow()
assert(writes==oldWrites and scale:GetValue()==75 and aa:GetChecked(),"Scene settings applied early or lost staging")
scene.cancel();assert(scale:GetValue()==100 and not aa:GetChecked() and writes==oldWrites)
scale:SetValue(75);aa:SetChecked(true);aa.scripts.OnClick(aa);scene.okay()
assert(saved.nativeSceneScale=="0.75" and saved.nativeAA=="1")
oldWrites=writes;scene.default();assert(writes==oldWrites and saved.nativeAA=="1","Scene defaults persisted early")
scene.cancel();assert(scale:GetValue()==75 and aa:GetChecked())
scene.default();scene.okay();assert(saved.nativeSceneScale=="1" and saved.nativeAA=="0")
scale:SetValue(0);scene.okay();assert(saved.nativeSceneScale=="0.5")
scale:SetValue(1000);scene.okay();assert(saved.nativeSceneScale=="1")
saved.nativeAA="1";saved.nativeSceneScale="0.5";scaleSupported=false;aaSupported=false
scene.refresh();assert(not scale.enabled and not aa.enabled and not aa:GetChecked(),"Unavailable scene features enabled")
scale:SetValue(75);aa:SetChecked(true);aa.scripts.OnClick(aa);scene.okay()
assert(saved.nativeAA=="0" and saved.nativeSceneScale=="1","Unsupported scene features persisted")
local distance=frames.NativeLandscapeDistanceSlider
assert(distance:GetValue()==0 and saved.landscapeDistance=="0")
oldWrites=writes;distance:SetValue(9);scene.scripts.OnShow()
assert(saved.landscapeDistance=="0" and distance:GetValue()==9 and writes==oldWrites,"Landscape applied early or lost staging")
scene.cancel();assert(distance:GetValue()==0 and writes==oldWrites)
distance:SetValue(9);scene.okay();assert(saved.landscapeDistance=="8000")
oldWrites=writes;scene.default();assert(writes==oldWrites and saved.landscapeDistance=="8000","Landscape defaults persisted early")
scene.cancel();assert(distance:GetValue()==9)
distance:SetValue(99);scene.okay();assert(saved.landscapeDistance=="10000")
distance:SetValue(-9);scene.okay();assert(saved.landscapeDistance=="0")
for index=1,13 do distance:SetValue(index);scene.okay();assert(tonumber(saved.landscapeDistance)==4000+(index-1)*500,"Landscape slider emitted wrong value") end
scene.default();scene.okay();assert(saved.landscapeDistance=="0")
local hdr=frames.NativeHdrVideoPanel
local enabled=frames.NativeHdrCheck
local ev=frames.NativeHdrExposureSlider
local path=frames.NativeHdrLutPath
assert(not enabled:GetChecked() and ev:GetValue()==0 and path:GetText()=="")
oldWrites=writes;local oldReloads=reloads
enabled:SetChecked(true);enabled.scripts.OnClick(enabled);ev:SetValue(2);path:SetText("controlled.cube");hdr.scripts.OnShow()
assert(writes==oldWrites and reloads==oldReloads and ev:GetValue()==2 and path:GetText()=="controlled.cube","HDR applied before commit or lost staging")
hdr.cancel();assert(not enabled:GetChecked() and ev:GetValue()==0 and path:GetText()=="" and writes==oldWrites)
enabled:SetChecked(true);enabled.scripts.OnClick(enabled);ev:SetValue(2);path:SetText("controlled.cube");hdr.okay()
assert(saved.nativeHDR=="1" and saved.nativeHDRExposure=="2" and saved.nativeHDRLut=="controlled.cube" and reloads==oldReloads+1)
oldWrites=writes;oldReloads=reloads
hdr.default();assert(writes==oldWrites and saved.nativeHDR=="1","HDR defaults saved early")
hdr.cancel();assert(enabled:GetChecked() and ev:GetValue()==2 and path:GetText()=="controlled.cube")
path:SetText("invalid.txt");hdr.okay();assert(writes==oldWrites and reloads==oldReloads,"Invalid LUT committed")
path:SetText("invalid.cube\n");hdr.okay();assert(writes==oldWrites,"Control characters accepted in LUT path")
hdr.cancel();lutState="Invalid |cBAD\nretained";hdr.scripts.OnUpdate(hdr,0.5)
local hdrStatus=hdr.fontStrings[#hdr.fontStrings-1].text
assert(hdrStatus:find("||cBAD",1,true) and not hdrStatus:find("\nretained",1,true),"LUT status injected markup/control text")
hdr.default();hdr.okay();assert(saved.nativeHDR=="0" and saved.nativeHDRExposure=="0" and saved.nativeHDRLut=="")
ev:SetValue(100);assert(ev:GetValue()==8);ev:SetValue(-100);assert(ev:GetValue()==-8);hdr.cancel()
hdrSupported=false;saved.nativeHDR="1";saved.nativeHDRExposure="2";saved.nativeHDRLut="controlled.cube";hdr.refresh()
oldWrites=writes;oldReloads=reloads;hdr.default();hdr.okay()
assert(not enabled.enabled and not ev.enabled and not path.mouseEnabled and writes==oldWrites and reloads==oldReloads,"Unsupported HDR preferences overwritten or falsely applied")
local ray=frames.NativeRayVideoPanel
local visibility,strict=frames.NativeRayVisibilityCheck,frames.NativeRayStrictCheck
oldWrites=writes
visibility:SetChecked(true);visibility.scripts.OnClick(visibility)
strict:SetChecked(false);strict.scripts.OnClick(strict)
assert(writes==oldWrites and saved.nativeRayTracing=="0","Ray controls saved before Apply")
ray.cancel();assert(not visibility:GetChecked() and strict:GetChecked(),"Ray Cancel did not restore selection")
visibility:SetChecked(true);visibility.scripts.OnClick(visibility);ray.okay()
assert(saved.nativeRayTracing=="1" and saved.nativeRayStrict=="1","Ray Apply failed")
oldWrites=writes;ray.default();assert(writes==oldWrites,"Ray defaults saved early")
ray.cancel();assert(visibility:GetChecked() and strict:GetChecked(),"Ray defaults could not be cancelled")
ray.default();ray.okay();assert(saved.nativeRayTracing=="0" and saved.nativeRayStrict=="1","Ray defaults not Off/strict On")
raySupported=false;saved.nativeRayTracing="1";ray.refresh();oldWrites=writes;ray.default();ray.okay()
assert(not visibility.enabled and not strict.enabled and writes==oldWrites and saved.nativeRayTracing=="1","Unsupported ray preferences were overwritten")
assert(ray.fontStrings[2].text:find("do not receive",1,true),"Terrain-only scope was not explained")
raySupported=true;saved.nativeHDR="0";saved.nativeAA="0";saved.nativeSceneScale="1";saved.nativeRayTracing="0";saved.nativeTerrainRayShadows="0";ray.refresh()
local terrain=frames.NativeTerrainRayShadowsCheck
oldWrites=writes;terrain:SetChecked(true);terrain.scripts.OnClick(terrain)
assert(writes==oldWrites and saved.nativeTerrainRayShadows=="0","Terrain option saved before Apply")
ray.cancel();assert(not terrain:GetChecked(),"Terrain Cancel did not restore selection")
terrain:SetChecked(true);terrain.scripts.OnClick(terrain);ray.okay()
assert(saved.nativeTerrainRayShadows=="1" and saved.nativeRayTracing=="0","Terrain Apply did not select actual shadows")
visibility:SetChecked(true);visibility.scripts.OnClick(visibility)
assert(not terrain:GetChecked(),"Mask and terrain choices are not exclusive")
ray.okay();assert(saved.nativeTerrainRayShadows=="0" and saved.nativeRayTracing=="1","Mask Apply did not deselect terrain")
terrain:SetChecked(true);terrain.scripts.OnClick(terrain);assert(not visibility:GetChecked(),"Terrain did not deselect mask")
saved.nativeHDR="1";ray.scripts.OnUpdate(ray,1)
local rayStatus=ray.fontStrings[#ray.fontStrings-1].text
assert(rayStatus:find("Scene Colour",1,true) and rayStatus:find("Apply",1,true),"HDR requirement lacks UI instructions")
assert(saved.nativeHDR=="1","Terrain selection silently changed HDR")
rayMessage="Missing shader |cBAD\n104";ray.scripts.OnUpdate(ray,1);rayStatus=ray.fontStrings[#ray.fontStrings-1].text
assert(rayStatus:find("||cBAD",1,true) and not rayStatus:find("\n104",1,true),"Renderer status injected UI markup/control text")
ray.cancel();ray.default();ray.okay();assert(saved.nativeTerrainRayShadows=="0" and saved.nativeRayTracing=="0" and saved.nativeRayStrict=="1","Ray defaults inconsistent")
