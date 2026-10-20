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
