-- Engine-owned extension using stock templates without changing archives.
if not VideoOptionsFramePanelContainer or not VideoOptionsPanel_OnLoad then
    error("Native video settings require the stock Video Settings panel")
end
if NativeRendererVideoPanel then return end

-- Display dimensions belong to the live window, not a staged video mode.
do
    local display = VideoOptionsResolutionPanel
    if display then
        local resolution = VideoOptionsResolutionPanelResolutionDropDown
        local refreshRate = VideoOptionsResolutionPanelRefreshDropDown
        for index = #display.controls, 1, -1 do
            local control = display.controls[index]
            if control == resolution or control == refreshRate then
                control.newValue = nil
                table.remove(display.controls, index)
            end
        end
        if resolution then resolution:Hide() end
        if refreshRate then refreshRate:Hide() end
        display.default = VideoOptionsPanel_Default
        if VideoOptionsResolutionPanelSubText then
            VideoOptionsResolutionPanelSubText:SetText("Display size follows the live window. Resolution and refresh selectors do not resize it.")
        end
        local samples = VideoOptionsResolutionPanelMultiSampleDropDown
        if samples then
            samples:ClearAllPoints()
            samples:SetPoint("TOPLEFT", display, "TOPLEFT", 0, -92)
        end
        if samples and VideoOptionsResolutionPanelVSync then
            VideoOptionsResolutionPanelVSync:ClearAllPoints()
            VideoOptionsResolutionPanelVSync:SetPoint("TOPLEFT", samples, "BOTTOMLEFT", 16, -8)
        end
        if VideoOptionsResolutionPanelUseUIScale then
            VideoOptionsResolutionPanelUseUIScale:ClearAllPoints()
            VideoOptionsResolutionPanelUseUIScale:SetPoint("TOPLEFT", display, "TOPLEFT", 176, -124)
        end
    end
end

-- Restart-only preferences never mutate the playing device.
do
    local devices = CreateFrame("Frame", "NativeRendererVideoPanel", VideoOptionsFramePanelContainer)
    devices.name = "Renderer"
    devices.controls = {}
    devices:Hide()
    devices:SetAllPoints(VideoOptionsFramePanelContainer)
    local function label(y, font)
        local text = devices:CreateFontString(nil, "ARTWORK", font or "GameFontHighlightSmall")
        text:SetPoint("TOPLEFT", 16, y)
        text:SetWidth(330)
        text:SetJustifyH("LEFT")
        return text
    end
    label(-16, "GameFontNormalLarge"):SetText("Renderer and GPU")
    label(-48):SetText("Apply saves your choice. A full CLIENT RESTART is required; your current session will not switch devices.")
    label(-92, "GameFontNormal"):SetText("Graphics renderer")
    label(-164, "GameFontNormal"):SetText("GPU adapter (active renderer)")
    local backend = CreateFrame("Frame", "NativeRendererBackendDropDown", devices, "UIDropDownMenuTemplate")
    backend:SetPoint("TOPLEFT", 0, -108)
    local adapter = CreateFrame("Frame", "NativeRendererAdapterDropDown", devices, "UIDropDownMenuTemplate")
    adapter:SetPoint("TOPLEFT", 0, -180)
    UIDropDownMenu_SetWidth(backend, 290)
    UIDropDownMenu_SetWidth(adapter, 290)
    local status = label(-232)
    local originalBackend, originalAdapter, pendingBackend, pendingAdapter
    local snapshot
    local function escaped(value)
        local text = tostring(value or "")
        if #text > 120 then text = string.sub(text, 1, 117) .. "..." end
        return string.gsub(string.gsub(text, "[%c]", " "), "|", "||")
    end
    local function choices(kind)
        local list = {{id="auto", name="Auto"}}
        for _, item in ipairs(snapshot[kind]) do table.insert(list, item) end
        return list
    end
    local function selectedName(list, id)
        for _, item in ipairs(list) do
            if item.id == id then return escaped(item.name) end
        end
        return "Unavailable saved choice: " .. escaped(id)
    end
    local function show()
        snapshot = GetNativeGraphicsDevices()
        UIDropDownMenu_SetText(backend, selectedName(choices("backends"), pendingBackend))
        UIDropDownMenu_SetText(adapter, selectedName(choices("adapters"), pendingAdapter))
        local copy = "Saved/requested: " .. escaped(GetCVar("gxBackend")) .. " / " .. escaped(GetCVar("gxAdapter"))
            .. "\nActive: " .. escaped(snapshot.activeBackend) .. " / " .. escaped(snapshot.activeName)
            .. "\nActive ID: " .. escaped(snapshot.activeAdapter)
        if pendingBackend ~= originalBackend or pendingAdapter ~= originalAdapter
            or pendingBackend ~= snapshot.requestedBackend or pendingAdapter ~= snapshot.requestedAdapter then
            copy = copy .. "\nCLIENT RESTART required for selected choices."
        end
        if snapshot.backendOverride ~= "" then copy = copy .. "\nBackend forced by WOWDECOMP_GX_BACKEND: " .. escaped(snapshot.backendOverride) end
        if snapshot.adapterOverride ~= "" then copy = copy .. "\nAdapter forced by WOWDECOMP_GX_ADAPTER: " .. escaped(snapshot.adapterOverride) end
        if snapshot.loaderFilter ~= "" then copy = copy .. "\nLoader-filtered devices only: " .. escaped(snapshot.loaderFilter)
        else copy = copy .. "\nAdapters visible to the current Vulkan loader only." end
        if snapshot.warning ~= "" then copy = copy .. "\n" .. escaped(snapshot.warning) end
        status:SetText(copy)
    end
    local function initialize(dropdown, kind)
        UIDropDownMenu_Initialize(dropdown, function()
            for _, item in ipairs(choices(kind)) do
                local choice = item
                local info = UIDropDownMenu_CreateInfo()
                info.text = escaped(choice.name)
                if kind == "adapters" and choice.id ~= "auto" then
                    info.text = info.text .. " / " .. escaped(choice.driver) .. " [" .. string.sub(choice.id, 1, 8) .. "]"
                    info.disabled = not choice.compatible or choice.id == ""
                    info.tooltipTitle = escaped(choice.name)
                    info.tooltipText = escaped(choice.reason) .. "\n" .. escaped(choice.id)
                    info.tooltipOnButton = true
                end
                info.checked = choice.id == (kind == "backends" and pendingBackend or pendingAdapter)
                info.func = function()
                    if kind == "backends" then pendingBackend = choice.id else pendingAdapter = choice.id end
                    show()
                    VideoOptionsFrameApply:Enable()
                end
                UIDropDownMenu_AddButton(info)
            end
        end)
    end
    local function refresh()
        originalBackend, originalAdapter = GetCVar("gxBackend"), GetCVar("gxAdapter")
        pendingBackend, pendingAdapter = originalBackend, originalAdapter
        show()
    end
    local function okay()
        SetCVar("gxBackend", pendingBackend)
        SetCVar("gxAdapter", pendingAdapter)
        refresh()
    end
    local function cancel()
        pendingBackend, pendingAdapter = originalBackend, originalAdapter
        show()
    end
    local function defaults()
        pendingBackend, pendingAdapter = "auto", "auto"
        show()
        VideoOptionsFrameApply:Enable()
    end
    devices:SetScript("OnShow", show)
    -- Initializers can run immediately, so populate snapshot first.
    refresh()
    initialize(backend, "backends")
    initialize(adapter, "adapters")
    VideoOptionsPanel_OnLoad(devices, okay, cancel, defaults, refresh)
end

-- The UI remains at native resolution, independently of the 3D scene.
do
    local panel = CreateFrame("Frame", "NativeLandscapeVideoPanel", VideoOptionsFramePanelContainer)
    panel.name = "Landscape & AA"
    panel.controls = {}
    panel:Hide()
    panel:SetAllPoints(VideoOptionsFramePanelContainer)
    local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 16, -16)
    title:SetText("Landscape, Scene Scale & AA")
    local description = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    description:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -12)
    description:SetWidth(330)
    description:SetJustifyH("LEFT")
    description:SetText("Extend distant landscape and outdoor fog together. Nearby detail and actor visibility are unchanged. Scene scaling affects only the 3D world; UI stays native.")
    local distance = CreateFrame("Slider", "NativeLandscapeDistanceSlider", panel, "OptionsSliderTemplate")
    distance:SetPoint("TOPLEFT", 24, -112)
    distance:SetWidth(290)
    distance:SetMinMaxValues(0, 13)
    distance:SetValueStep(1)
    distance.tooltipText = "Classic preserves the original horizon. Extended distance moves outdoor fog back while retaining zone colours. Indoor/underwater fog is unchanged. Greater distances may increase rendering cost."
    _G.NativeLandscapeDistanceSliderText:SetText("Landscape Distance")
    _G.NativeLandscapeDistanceSliderLow:SetText("Classic")
    _G.NativeLandscapeDistanceSliderHigh:SetText("10,000")
    local distanceText = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    distanceText:SetPoint("TOP", distance, "BOTTOM", 0, -16)
    local originalDistance, pendingDistance = 0, 0
    local scale = CreateFrame("Slider", "NativeSceneScaleSlider", panel, "OptionsSliderTemplate")
    scale:SetPoint("TOPLEFT", 24, -210)
    scale:SetWidth(290)
    scale:SetMinMaxValues(50, 100)
    scale:SetValueStep(5)
    _G.NativeSceneScaleSliderText:SetText("3D Scene Resolution")
    _G.NativeSceneScaleSliderLow:SetText("50%")
    _G.NativeSceneScaleSliderHigh:SetText("Native")
    scale.tooltipText = "50% to 100% world resolution, without changing the UI. Independent of temporal AA."
    local scaleText = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    scaleText:SetPoint("TOP", scale, "BOTTOM", 0, -16)
    local temporal = CreateFrame("CheckButton", "NativeTemporalAACheck", panel, "UICheckButtonTemplate")
    temporal:SetPoint("TOPLEFT", 16, -282)
    temporal.tooltipText = "Experimental temporal anti-aliasing. Defaults off. Unsupported devices retain the existing raster path."
    local label = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
    label:SetPoint("LEFT", temporal, "RIGHT", 2, 0)
    label:SetText("Temporal Anti-Aliasing (Experimental)")
    local note = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    note:SetPoint("TOPLEFT", 24, -324)
    note:SetWidth(320)
    note:SetJustifyH("LEFT")
    local originalScale, pendingScale, originalAA, pendingAA = 1, 1, 0, 0
    local updating = false
    local function show()
        updating = true
        local scaling, aa = IsNativeSceneScalingSupported(), IsNativeTemporalAASupported()
        distance:SetValue(pendingDistance == 0 and 0 or math.max(1, math.min(13, 1 + math.floor((pendingDistance - 4000) / 500 + 0.5))))
        distanceText:SetText(pendingDistance == 0 and "Classic" or string.format("%.0f world units", pendingDistance))
        scale:SetValue(pendingScale * 100)
        scaleText:SetText(scaling and string.format("%.0f%% scene resolution; native UI", pendingScale * 100) or "Scene scaling unavailable on this renderer")
        if scaling then scale:Enable() else scale:Disable() end
        temporal:SetChecked(aa and pendingAA == 1)
        if aa then temporal:Enable() else temporal:Disable() end
        note:SetText(aa and "Experimental quality/performance. Apply or Okay saves; Cancel restores; no reload needed." or "Temporal AA unavailable on this renderer. Existing raster rendering is retained.")
        updating = false
    end
    local function refresh()
        originalDistance = tonumber(GetCVar("landscapeDistance")) or 0
        pendingDistance = originalDistance
        originalScale = tonumber(GetCVar("nativeSceneScale")) or 1
        originalAA = tonumber(GetCVar("nativeAA")) or 0
        pendingScale, pendingAA = originalScale, originalAA
        show()
    end
    local function okay()
        SetCVar("landscapeDistance", tostring(pendingDistance))
        SetCVar("nativeSceneScale", tostring(IsNativeSceneScalingSupported() and pendingScale or 1))
        SetCVar("nativeAA", tostring(IsNativeTemporalAASupported() and pendingAA or 0))
        refresh()
    end
    local function cancel()
        pendingDistance = originalDistance
        pendingScale, pendingAA = originalScale, originalAA
        show()
    end
    local function defaults()
        pendingDistance = 0
        pendingScale, pendingAA = 1, 0
        show()
        VideoOptionsFrameApply:Enable()
    end
    distance:SetScript("OnValueChanged", function(self, value)
        if updating then return end
        local index = math.max(0, math.min(13, math.floor(value + 0.5)))
        pendingDistance = index == 0 and 0 or 4000 + (index - 1) * 500
        show()
        VideoOptionsFrameApply:Enable()
    end)
    scale:SetScript("OnValueChanged", function(self, value)
        if updating or not IsNativeSceneScalingSupported() then return end
        pendingScale = math.max(50, math.min(100, math.floor(value / 5 + 0.5) * 5)) / 100
        show()
        VideoOptionsFrameApply:Enable()
    end)
    temporal:SetScript("OnClick", function(self)
        if not IsNativeTemporalAASupported() then show(); return end
        pendingAA = self:GetChecked() and 1 or 0
        VideoOptionsFrameApply:Enable()
    end)
    panel:SetScript("OnShow", show)
    VideoOptionsPanel_OnLoad(panel, okay, cancel, defaults, refresh)
    refresh()
end

-- Visual lighting controls do not change server/gameplay time.
do
    local lighting = CreateFrame("Frame", "NativeNightLightingPanel", VideoOptionsFramePanelContainer)
    lighting.name = "Night Lighting"
    lighting.controls = {}
    lighting:Hide()
    lighting:SetAllPoints(VideoOptionsFramePanelContainer)
    local heading = lighting:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
    heading:SetPoint("TOPLEFT", 16, -16)
    heading:SetText("Night Lighting")
    local description = lighting:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    description:SetPoint("TOPLEFT", heading, "BOTTOMLEFT", 0, -12)
    description:SetWidth(330)
    description:SetJustifyH("LEFT")
    description:SetText("Reduce outdoor ambient light at night. Direct moonlight, lamps, spells, sky and UI are unchanged. Local time preview changes lighting only.")
    local function slider(name, y, low, high, step, title, lowLabel, highLabel)
        local control = CreateFrame("Slider", name, lighting, "OptionsSliderTemplate")
        control:SetPoint("TOPLEFT", 24, y)
        control:SetWidth(290)
        control:SetMinMaxValues(low, high)
        control:SetValueStep(step)
        _G[name.."Text"]:SetText(title)
        _G[name.."Low"]:SetText(lowLabel)
        _G[name.."High"]:SetText(highLabel)
        return control
    end
    local darkness = slider("NativeNightDarknessSlider", -128, 0, 100, 10, "Night Darkness", "Classic", "Darkest")
    darkness.tooltipText = "Reduces outdoor night ambient light only. Indoor, underwater and authored lighting overrides are preserved."
    local text = lighting:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    text:SetPoint("TOP", darkness, "BOTTOM", 0, -16)
    local clock = slider("NativeLocalTimePreviewSlider", -214, -1, 24, 1, "Local Time Preview (24-hour)", "Server Time", "24:00")
    clock.tooltipText = "Server Time restores live or authored map lighting. 0 to 24 freezes visual time; 24 is midnight. Gameplay time is unchanged."
    local clockText = lighting:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    clockText:SetPoint("TOP", clock, "BOTTOM", 0, -16)
    local note = lighting:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    note:SetPoint("TOPLEFT", 24, -290)
    note:SetWidth(320)
    note:SetJustifyH("LEFT")
    note:SetText("Apply or Okay saves changes. Cancel restores the previous selection. No UI reload is needed.")
    local originalDarkness, originalTime, pendingDarkness, pendingTime = 0, -1, 0, -1
    local updating = false
    local function show()
        updating = true
        darkness:SetValue(pendingDarkness)
        text:SetText(pendingDarkness == 0 and "Classic" or string.format("%.0f%% ambient reduction at night", pendingDarkness))
        clock:SetValue(pendingTime)
        clockText:SetText(pendingTime == -1 and "Server Time (live)" or string.format("%02d:%02d local preview", math.floor(pendingTime) % 24, math.floor((pendingTime % 1) * 60)))
        updating = false
    end
    local function refresh()
        originalDarkness = tonumber(GetCVar("nightDarkness")) or 0
        originalTime = tonumber(GetCVar("localTimePreview")) or -1
        pendingDarkness, pendingTime = originalDarkness, originalTime
        show()
    end
    local function okay()
        SetCVar("nightDarkness", tostring(pendingDarkness))
        SetCVar("localTimePreview", tostring(pendingTime))
        refresh()
    end
    local function cancel()
        pendingDarkness, pendingTime = originalDarkness, originalTime
        show()
    end
    local function defaults()
        pendingDarkness, pendingTime = 0, -1
        show()
        VideoOptionsFrameApply:Enable()
    end
    darkness:SetScript("OnValueChanged", function(self, value)
        if updating then return end
        pendingDarkness = math.max(0, math.min(100, math.floor(value / 10 + 0.5) * 10))
        show()
        VideoOptionsFrameApply:Enable()
    end)
    clock:SetScript("OnValueChanged", function(self, value)
        if updating then return end
        pendingTime = math.max(-1, math.min(24, math.floor(value + 0.5)))
        show()
        VideoOptionsFrameApply:Enable()
    end)
    lighting:SetScript("OnShow", show)
    VideoOptionsPanel_OnLoad(lighting, okay, cancel, defaults, refresh)
    refresh()
end

-- A real world-query diagnostic, deliberately not advertised as finished RT lighting.
do
    local panel = CreateFrame("Frame", "NativeRayVideoPanel", VideoOptionsFramePanelContainer)
    panel.name = "Ray Visibility (Development)"
    panel.controls = {}
    panel:Hide()
    panel:SetAllPoints(VideoOptionsFramePanelContainer)
    local function label(y, text, font)
        local item = panel:CreateFontString(nil, "ARTWORK", font or "GameFontHighlightSmall")
        item:SetPoint("TOPLEFT", 16, y)
        item:SetWidth(330)
        item:SetJustifyH("LEFT")
        item:SetText(text)
        return item
    end
    label(-16, "World Ray-query Visibility", "GameFontNormalLarge")
    label(-48, "Development view of actual world ray queries. Directional material lighting, GI and volumetric fog are NOT integrated yet.")
    local enabled = CreateFrame("CheckButton", "NativeRayVisibilityCheck", panel, "UICheckButtonTemplate")
    enabled:SetPoint("TOPLEFT", 16, -116)
    label(-124, "       Show world visibility mask", "GameFontHighlight")
    local strict = CreateFrame("CheckButton", "NativeRayStrictCheck", panel, "UICheckButtonTemplate")
    strict:SetPoint("TOPLEFT", 16, -158)
    label(-166, "       Refuse incomplete caster frames", "GameFontHighlight")
    local status = label(-214, "")
    label(-302, "White = visible to the directional light; dark = occluded; grey = outside the diagnostic region. Strict checks the current caster frame, not full corpus acceptance.")
    label(-368, "Apply saves changes. Cancel restores the selection. Default: visibility Off, strict On.")
    local originalOn, originalStrict, pendingOn, pendingStrict = 0, 1, 0, 1
    local function showStatus()
        local current = GetNativeShadowStatus()
        status:SetText(current.supported and (tostring(current.status).."\nCaster triangles: "..tostring(current.triangles))
            or "Ray queries unavailable on this renderer/device. Saved preferences are retained.")
        if current.supported then enabled:Enable(); strict:Enable()
        else enabled:Disable(); strict:Disable() end
    end
    local function show()
        enabled:SetChecked(pendingOn == 1)
        strict:SetChecked(pendingStrict == 1)
        showStatus()
    end
    local function refresh()
        originalOn = tonumber(GetCVar("nativeRayTracing")) or 0
        originalStrict = tonumber(GetCVar("nativeRayStrict")) or 1
        pendingOn, pendingStrict = originalOn, originalStrict
        show()
    end
    local function okay()
        if not GetNativeShadowStatus().supported then refresh(); return end
        SetCVar("nativeRayTracing", tostring(pendingOn))
        SetCVar("nativeRayStrict", tostring(pendingStrict))
        refresh()
    end
    local function cancel() pendingOn, pendingStrict = originalOn, originalStrict; show() end
    local function defaults() pendingOn, pendingStrict = 0, 1; show(); VideoOptionsFrameApply:Enable() end
    enabled:SetScript("OnClick", function(self) pendingOn = self:GetChecked() and 1 or 0; VideoOptionsFrameApply:Enable() end)
    strict:SetScript("OnClick", function(self) pendingStrict = self:GetChecked() and 1 or 0; VideoOptionsFrameApply:Enable() end)
    local elapsed = 0
    panel:SetScript("OnUpdate", function(self, delta) elapsed = elapsed + delta; if elapsed >= 1 then elapsed = 0; showStatus() end end)
    panel:SetScript("OnShow", show)
    VideoOptionsPanel_OnLoad(panel, okay, cancel, defaults, refresh)
    refresh()
end

-- One HDR transaction in the stock Video Settings category list. Only the 3D
-- scene is tone mapped/graded; native UI and the HDR-Off glow path are untouched.
do
    local hdr = CreateFrame("Frame", "NativeHdrVideoPanel", VideoOptionsFramePanelContainer)
    hdr.name = "Scene Colour"
    hdr.controls = {}
    hdr:Hide()
    hdr:SetAllPoints(VideoOptionsFramePanelContainer)
    local function label(y, text, font)
        local item = hdr:CreateFontString(nil, "ARTWORK", font or "GameFontHighlightSmall")
        item:SetPoint("TOPLEFT", 16, y)
        item:SetWidth(330)
        item:SetJustifyH("LEFT")
        item:SetText(text)
        return item
    end
    label(-16, "Floating-Point Scene Colour", "GameFontNormalLarge")
    label(-48, "Float scene transport, manual EV and SDR tone mapping. Not HDR monitor output. UI/text stay native and ungraded; legacy glow is disabled.")
    local enabled = CreateFrame("CheckButton", "NativeHdrCheck", hdr, "UICheckButtonTemplate")
    enabled:SetPoint("TOPLEFT", 16, -96)
    label(-104, "       Enable float scene transport", "GameFontHighlight")
    local ev = CreateFrame("Slider", "NativeHdrExposureSlider", hdr, "OptionsSliderTemplate")
    ev:SetPoint("TOPLEFT", 24, -158)
    ev:SetWidth(290)
    ev:SetMinMaxValues(-8, 8)
    ev:SetValueStep(0.25)
    _G[ev:GetName().."Text"]:SetText("Manual Exposure (EV)")
    _G[ev:GetName().."Low"]:SetText("-8")
    _G[ev:GetName().."High"]:SetText("+8")
    local evText = label(-192, "")
    label(-224, "Optional normalized .cube LUT path (empty = None)")
    local path = CreateFrame("EditBox", "NativeHdrLutPath", hdr, "InputBoxTemplate")
    path:SetPoint("TOPLEFT", 24, -248)
    path:SetWidth(300)
    path:SetHeight(24)
    path:SetAutoFocus(false)
    path:SetMaxLetters(1024)
    path:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
    path:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)
    local status = label(-286, "")
    label(-356, "Apply or Okay saves changes. LUTs load in the background; failures retain the current LUT. Defaults are Off, EV 0 and None.")
    local originalOn, originalEV, originalPath = 0, 0, ""
    local pendingOn, pendingEV, pendingPath = 0, 0, ""
    local updating = false
    local function finiteEV(value)
        value = tonumber(value) or 0
        if value ~= value or value == math.huge or value == -math.huge then return 0 end
        return math.max(-8, math.min(8, value))
    end
    local function validPath(value)
        return value == "" or (#value <= 1024 and string.sub(value, -5) == ".cube" and not string.find(value, "[%c]"))
    end
    local function showStatus()
        if not validPath(pendingPath) then
            status:SetText("Invalid local .cube path (maximum 1024 bytes; no control characters). Current settings are unchanged.")
            return
        end
        local current = GetNativeHdrStatus()
        local text
        if not current.supported then text = "HDR unavailable on this renderer. Saved preferences are retained."
        elseif GetCVar("nativeHDR") ~= "1" then text = "HDR Off (Classic glow). LUT bypassed."
        else text = "HDR requested; per-frame support may retain Classic rendering." end
        local detail = tostring(current.lut or "Unknown LUT state")
        detail = string.gsub(string.gsub(string.sub(detail, 1, 180), "[%c]", " "), "|", "||")
        status:SetText(text.."\nLUT: "..detail)
    end
    local function show()
        updating = true
        local available = GetNativeHdrStatus().supported
        enabled:SetChecked(pendingOn == 1)
        ev:SetValue(pendingEV)
        evText:SetText(string.format("%+.2f EV (manual)", pendingEV))
        path:SetText(pendingPath)
        if available then enabled:Enable(); ev:Enable(); path:EnableMouse(true)
        else enabled:Disable(); ev:Disable(); path:EnableMouse(false) end
        updating = false
        showStatus()
    end
    local function refresh()
        originalOn = tonumber(GetCVar("nativeHDR")) or 0
        originalEV = finiteEV(GetCVar("nativeHDRExposure"))
        originalPath = GetCVar("nativeHDRLut") or ""
        pendingOn, pendingEV, pendingPath = originalOn, originalEV, originalPath
        show()
    end
    local function okay()
        -- Do not turn unsupported preferences into an apparent successful apply.
        if not GetNativeHdrStatus().supported then show(); return end
        if not validPath(pendingPath) then showStatus(); return end
        SetCVar("nativeHDR", tostring(pendingOn))
        SetCVar("nativeHDRExposure", tostring(finiteEV(pendingEV)))
        SetCVar("nativeHDRLut", pendingPath)
        ReloadNativeHdrLut()
        refresh()
    end
    local function cancel()
        pendingOn, pendingEV, pendingPath = originalOn, originalEV, originalPath
        show()
    end
    local function defaults()
        pendingOn, pendingEV, pendingPath = 0, 0, ""
        show()
        VideoOptionsFrameApply:Enable()
    end
    enabled:SetScript("OnClick", function(self)
        if not GetNativeHdrStatus().supported then show(); return end
        pendingOn = self:GetChecked() and 1 or 0
        show()
        VideoOptionsFrameApply:Enable()
    end)
    ev:SetScript("OnValueChanged", function(self, value)
        if updating then return end
        if not GetNativeHdrStatus().supported then show(); return end
        pendingEV = finiteEV(value)
        show()
        VideoOptionsFrameApply:Enable()
    end)
    path:SetScript("OnTextChanged", function(self)
        if updating then return end
        if not GetNativeHdrStatus().supported then return end
        pendingPath = self:GetText()
        showStatus()
        VideoOptionsFrameApply:Enable()
    end)
    hdr:SetScript("OnShow", show)
    local elapsedStatus = 0
    hdr:SetScript("OnUpdate", function(self, elapsed)
        elapsedStatus = elapsedStatus + elapsed
        if elapsedStatus < 0.5 then return end
        elapsedStatus = 0
        showStatus()
    end)
    VideoOptionsPanel_OnLoad(hdr, okay, cancel, defaults, refresh)
    refresh()
end
