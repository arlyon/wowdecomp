struct IndependentUiMetrics {
    unsigned phase=0,windows=0,frames=0;
    bool holding=false,waiting=false;
    CSimpleTexture* hud=nullptr;
    unsigned checkedPixels=0;
    bool probeIssued=false,probeRed=false,probeWorldVisible=false,checkedCameraOwner=false;
    GxWorld::WorldFrameLease probeLease{},checkedWorldLease{};
    CImVector checkedWorldColour;
    double seconds=0;
    GxWorld::WorldFrame frozen;
    std::chrono::steady_clock::time_point holdStart{},lastPresent{},lastUpdate{};
    std::vector<double> intervals,inputLatency;
} independentUi;
double P95(std::vector<double> values){
    Check(!values.empty(),"No UI timing samples");std::sort(values.begin(),values.end());
    return values[size_t(std::ceil(values.size()*.95))-1];
}
void IndependentUiTick(){
    auto& m=independentUi;auto& gx=*test.device;const auto started=std::chrono::steady_clock::now();
    if(!m.phase){
        Set(CWorldParam::cvar_nativeIndependentWorld,"1");
        if(test.independentRT)Set(CWorldParam::cvar_nativeTerrainRayShadows,"1");
        test.frame->Show();m.phase=1;
        auto* panel=new(CSimpleFrame::s_allocator.GetData(0,STORM_TYPE_NAME(CSimpleFrame),SLOG_OBJECT)) CSimpleFrame(nullptr);
        panel->SetFrameStrata(FRAME_STRATA_MEDIUM);panel->SetAllPoints(test.top.get(),1);panel->Show();
        uint32_t layer=0;Check(StringToDrawLayer("ARTWORK",layer),"UI draw layer missing");
        m.hud=new(CSimpleTexture::s_allocator.GetData(0,STORM_TYPE_NAME(CSimpleTexture),SLOG_OBJECT)) CSimpleTexture(panel,layer,1);
        Check(m.hud->SetTexture("Interface\\Buttons\\WHITE8X8",0,0,GxTex_LinearMipLinear,ImageMode_Normal),"Actual UI atlas texture unavailable");
        m.hud->SetPoint(FRAMEPOINT_BOTTOMLEFT,panel,FRAMEPOINT_BOTTOMLEFT,NDCToDDCWidth(.05f),NDCToDDCHeight(.4f),1);
        m.hud->SetWidth(NDCToDDCWidth(.2f));m.hud->SetHeight(NDCToDDCHeight(.2f));
        CLayoutFrame::ResizePending();
    }
    gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
    if(m.probeIssued){
        CImVector pixel;Check(gx.SceneWorldTestReadUiProbe(pixel),"Textured UI probe was not complete at its UI fence");
        if(!((m.probeRed?pixel.r:pixel.b)>240 && (m.probeRed?pixel.b:pixel.r)<8 && pixel.g<8)){
            std::fprintf(stderr,"Textured UI mismatch: tick=%u expected=%s got=%u,%u,%u,%u held=%u\n",test.ticks-1,m.probeRed?"red":"blue",pixel.r,pixel.g,pixel.b,pixel.a,gx.SceneWorldTestProducerHeld());
            throw std::runtime_error("Original textured UI was missing/black/stale");
        }
        ++m.checkedPixels;
        if(m.probeWorldVisible){
            Check(gx.SceneWorldTestReadUiProbe(pixel,1),"World pixel probe incomplete");
            Check(unsigned(pixel.r)+pixel.g+pixel.b>20,"Completed world was black");
            if(m.checkedWorldLease==m.probeLease)Check(std::memcmp(&pixel,&m.checkedWorldColour,4)==0,"Same displayed world generation changed/cleared while its replacement was pending");
            m.checkedWorldLease=m.probeLease;m.checkedWorldColour=pixel;
        }
    }
    EVENT_DATA_MOUSE mouse{};mouse.m_mode=MOUSE_MODE_NORMAL;mouse.m_x=.1f+float(test.ticks%20)*.01f;mouse.m_y=.1f;mouse.m_time=test.ticks;
    const auto injected=std::chrono::steady_clock::now();CSimpleTop::OnMouseMove(&mouse,test.top.get());
    const float elapsed=m.lastUpdate.time_since_epoch().count()?float(std::chrono::duration<double>(started-m.lastUpdate).count()):1.f/60;
    m.lastUpdate=started;FrameTime::Update(elapsed,OsGetAsyncTimeMs());
    test.top->OnLayerUpdate(elapsed);
    const bool red=test.ticks%2;m.hud->SetVertexColor(red?CImVector(0,0,255,255):CImVector(255,0,0,255));
    test.top->OnLayerRender();
    Check(test.top->m_mousePosition.m_time==mouse.m_time,"Original UI did not consume synthetic mouse input");
    GxuFontSetStringColor(test.label,CImVector(test.top->m_mousePosition.m_time%2?20:240,test.top->m_mousePosition.m_time%2?240:20,30,255));
    GxuFontRenderBatch(test.labelBatch);
    const double inputMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-injected).count();
    const char* status=gx.SceneIndependentWorldStatus();
    if(std::strstr(status,"fallback"))throw std::runtime_error(status);
    // Queue the probe in the ordinary UI submission; read it at next UI fence.
    // Synchronous DeviceReadPixels here would change the producer/UI ordering.
    GxWorld::WorldFrame probed;m.probeWorldVisible=test.frame->DisplayedWorld(probed);m.probeLease=probed.lease;
    gx.SceneWorldTestProbeUi(unsigned(.15f*Width),Height/2,unsigned(.8f*Width),Height/2);m.probeIssued=true;m.probeRed=red;
    gx.ScenePresent();const auto now=std::chrono::steady_clock::now();
    GxWorld::WorldFrame displayed;const bool visible=test.frame->DisplayedWorld(displayed);
    if(m.phase==1 && visible){gx.SceneWorldTestHoldProducer(true);m.phase=2;}
    if(m.phase==2){
        if(!m.holding && !m.waiting && gx.SceneWorldTestProducerHeld()){
            Check(visible,"Producer hold had no completed image");
            if(!m.checkedCameraOwner){
                const auto epoch=WorldDisplayEpoch();test.frame->SetCameraTarget(nullptr,0);
                Check(epoch==WorldDisplayEpoch() && test.frame->HasDisplayedWorld(),"Reapplying unchanged camera owner invalidated completed world");m.checkedCameraOwner=true;
            }
            m.frozen=displayed;m.holding=true;m.holdStart=m.lastPresent=now;
            if(test.detailInstancing && !m.windows)DropDetailInstanceSources();
            const float rayX=NDCToDDCWidth(.5f),rayY=NDCToDDCHeight(.5f);
            C3Vector a,b,c,d;Check(test.frame->GetLineSegment(rayX,rayY,&a,&b),"Displayed camera had no ray");
            const auto camera=test.frame->m_camera->m_position;test.frame->m_camera->m_position.x+=100;
            Check(test.frame->GetLineSegment(rayX,rayY,&c,&d),"Current camera change invalidated displayed picking");
            test.frame->m_camera->m_position=camera;
            Check(a.x==c.x&&a.y==c.y&&a.z==c.z&&b.x==d.x&&b.y==d.y&&b.z==d.z,"Picking used current rather than displayed camera");
        }else if(m.holding){
            Check(visible && displayed.lease==m.frozen.lease,"Unfinished world changed displayed pixels/interaction");
            Check(gx.SceneWorldTestProducerHeld(),"CPU producer hold ended without release");
            m.intervals.push_back(std::chrono::duration<double,std::milli>(now-m.lastPresent).count());m.inputLatency.push_back(inputMs);
            m.lastPresent=now;++m.frames;
            const double seconds=std::chrono::duration<double>(now-m.holdStart).count();
            if(seconds>=.5){m.seconds+=seconds;++m.windows;m.holding=false;m.waiting=true;gx.SceneWorldTestHoldProducer(false);}
        }else if(m.waiting && visible && !(displayed.lease==m.frozen.lease)){
            if(m.seconds>=10){
                const auto epoch=WorldDisplayEpoch();
                // Simulate removal of a prior camera owner without a live server actor.
                test.frame->m_camera->m_targetGUID=WOWGUID{123};test.frame->SetCameraTarget(nullptr,0);
                Check(WorldDisplayEpoch()!=epoch && !test.frame->HasDisplayedWorld(),"Changed camera owner failed to withdraw displayed interaction immediately");
                m.phase=3;
            }else {m.waiting=false;gx.SceneWorldTestHoldProducer(true);}
        }
    }else if(m.phase==3 && visible){
        Check(displayed.description.scene==WorldDisplayEpoch(),"Old scene reappeared after cancellation");
        Set(CWorldParam::cvar_nativeIndependentWorld,"0");m.phase=4;
    }else if(m.phase==4){
        Check(!test.frame->HasDisplayedWorld() && std::strstr(gx.SceneIndependentWorldStatus(),"Off"),"Synchronous rollback did not take effect");
        const double fps=m.frames/m.seconds,presentP95=P95(m.intervals),inputP95=P95(m.inputLatency);
        std::fprintf(stderr,"Original UI independence: RT=%u resolution=%ux%u windows=%u holdSeconds=%.3f UI=%.2f/s presentP95=%.3fms inputUpdateP95=%.3fms\n",test.independentRT,Width,Height,m.windows,m.seconds,fps,presentP95,inputP95);
        Check(fps>=30 && presentP95<=50 && inputP95<=50,"Original UI failed controlled CPU-hold cadence/input thresholds");
        Check(CWorldParam::cvar_nativeTerrainRayShadows->m_intValue==int(test.independentRT),"Scheduling changed saved RT intent");
        Check(m.checkedPixels>30,"Textured UI did not cover repeated frames");
        Check(!test.networkAttempts,"Independent UI fixture attempted network");test.result=0;EventPostClose();
    }
}
