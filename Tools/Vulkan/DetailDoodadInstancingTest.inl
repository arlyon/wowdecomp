void DropDetailInstanceSources(){
    for(uint32_t i=0;i<CDetailDoodad::s_doodadList.Count();++i)if(auto* data=CDetailDoodad::s_doodadList[i]){
        CDetailDoodad::FreeInstancedBuffer(data->m_instanceVertexBuf);CDetailDoodad::FreeInstancedBuffer(data->m_instanceIndexBuf);data->m_instanceSkin=nullptr;
    }
    for(auto* area:CMap::m_areaTable){if(!area || area->m_asyncObject)continue;
        for(auto* chunk:area->m_mapChunks)if(chunk && chunk->m_detailDoodadInst)
            for(auto& batch:chunk->m_detailDoodadInst->m_batches)CDetailDoodad::FreeInstancedBuffer(batch.m_instanceBuf);
    }
}
void CaptureDetailOnly(TSGrowableArray<CImVector>& pixels,TSGrowableArray<float>& depth){
    auto& gx=*test.device;gx.SceneClear(3,CImVector(0,0,0,255));gx.RsPush();gx.XformPush(GxXform_World);gx.XformPush(GxXform_Tex1);
    Check(CDetailDoodad::InitializeRenderState(),"Detail-only oracle needs authored shaders");
    for(auto* inst=CMap::s_detailDoodadUpdateList.Head();inst;inst=CMap::s_detailDoodadUpdateList.Next(inst)){
        auto* chunk=inst->m_chunk;if(!chunk)continue;CM2Lighting lighting(chunk->m_sphere);
        World::s_m2Scene->SelectLights(&lighting);chunk->SelectLights(&lighting);
        C44Matrix world;world.Translate(chunk->m_topLeftCoords-CWorldScene::s_activeWorldView);CDetailDoodad::SetupVertexShader(world,0);
        for(auto& batch:inst->m_batches){
            if(batch.m_adds.Count()<2)continue;bool same=true;for(uint32_t i=1;i<batch.m_adds.Count();++i)same&=batch.m_adds[i].m_doodadId==batch.m_adds[0].m_doodadId;
            if(same)batch.Draw();
        }
    }
    GxXformPop(GxXform_Tex1);GxXformPop(GxXform_World);gx.RsPop();
    CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;gx.DeviceReadPixels(rect,pixels);gx.DeviceReadDepths(rect,depth);
}
void CheckDetailOnlyParity(){
    TSGrowableArray<CImVector> pixels;TSGrowableArray<float> depth;CaptureDetailOnly(pixels,depth);
    uint32_t covered=0,different=0,depthEdges=0;double error=0;
    for(uint32_t i=0;i<pixels.Count();++i){
        const auto a=test.detailOnlyColour[i],b=pixels[i];covered+=unsigned(a.r)+a.g+a.b>10;
        const unsigned e=std::abs(int(a.r)-b.r)+std::abs(int(a.g)-b.g)+std::abs(int(a.b)-b.b)+std::abs(int(a.a)-b.a);error+=e;different+=e>8;
        depthEdges+=std::fabs(depth[i]-test.detailOnlyDepth[i])>2e-5f;
    }
    std::fprintf(stderr,"Detail-only parity: tick=%u covered=%u significantColourPixels=%u depthEdgePixels=%u errorPerCoveredChannel=%.6f\n",test.ticks,covered,different,depthEdges,error/(4*std::max(1u,covered)));
    Check(covered>100 && different<=std::max(4u,covered*3/100) && depthEdges<=std::max(4u,covered*3/100) && error/(4*covered)<2,"Isolated clutter disappeared or changed visual semantics");
}
void DetailInstancingTick(){
    auto& gx=*test.device;
    if(test.ticks==119){
        CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;
        TSGrowableArray<CImVector> pixels;TSGrowableArray<float> depth;gx.DeviceReadPixels(rect,pixels);gx.DeviceReadDepths(rect,depth);
        test.detailedColour.assign(pixels.Ptr(),pixels.Ptr()+pixels.Count());test.detailedDepth.assign(depth.Ptr(),depth.Ptr()+depth.Count());
        CaptureDetailOnly(pixels,depth);test.detailOnlyColour.assign(pixels.Ptr(),pixels.Ptr()+pixels.Count());test.detailOnlyDepth.assign(depth.Ptr(),depth.Ptr()+depth.Count());
        CDetailDoodad::s_instancingStats={};Set(CWorldParam::cvar_nativeDetailInstancing,"1");
    }
    if(test.ticks==120){
        const auto s=CDetailDoodad::s_instancingStats;
        std::fprintf(stderr,"Detail instancing: draws=%llu instances=%llu meshBuilt=%llu instanceBuilt=%llu legacyEquivalent=%llu fallback=%llu\n",(unsigned long long)s.draws,(unsigned long long)s.instances,(unsigned long long)s.meshBuiltBytes,(unsigned long long)s.instanceBuiltBytes,(unsigned long long)s.legacyEquivalentBytes,(unsigned long long)s.fallbacks);
        std::fprintf(stderr,"Detail CPU build: mesh=%.6fms instances=%.6fms (excludes shader compilation and GPU execution)\n",s.meshBuildMs,s.instanceBuildMs);
        Check(s.draws && s.instances>s.draws,"Actual world had no eligible instanced ground clutter");
        Check(s.meshBuiltBytes+s.instanceBuiltBytes<s.legacyEquivalentBytes,"Instancing did not reduce actual world geometry build bytes");
        CiRect rect;rect.minX=rect.minY=0;rect.maxX=Width;rect.maxY=Height;
        TSGrowableArray<CImVector> pixels;TSGrowableArray<float> depth;gx.DeviceReadPixels(rect,pixels);gx.DeviceReadDepths(rect,depth);
        Check(pixels.Count()==test.detailedColour.size() && depth.Count()==test.detailedDepth.size(),"Detail parity readback extent changed");
        uint32_t colourDifferent=0,depthDifferent=0;double colourError=0;float maxDepth=0;
        for(uint32_t i=0;i<pixels.Count();++i){
            const auto a=pixels[i],b=test.detailedColour[i];const unsigned error=std::abs(int(a.r)-b.r)+std::abs(int(a.g)-b.g)+std::abs(int(a.b)-b.b)+std::abs(int(a.a)-b.a);
            colourError+=error;if(error>8)++colourDifferent;
            const auto difference=std::fabs(depth[i]-test.detailedDepth[i]);maxDepth=std::max(maxDepth,difference);if(difference>2e-5f)++depthDifferent;
        }
        std::fprintf(stderr,"Detail parity: significantColourPixels=%u depthPixels=%u meanRGBAError=%.6f maxDepthError=%.8f\n",colourDifferent,depthDifferent,colourError/(pixels.Count()*4),maxDepth);
        Check(colourDifferent<pixels.Count()/1000 && depthDifferent<pixels.Count()/1000 && colourError/(pixels.Count()*4)<.25,"Instanced clutter changed scene colour/depth");
        CheckDetailOnlyParity();
        test.detailDraws=CDetailDoodad::s_instancingStats.draws;Set(CWorldParam::cvar_nativeAA,"1");
    }
    if(test.ticks==121){
        Check(CWorldParam::cvar_nativeAA->m_intValue==1 && gx.SceneWorldTestCompletedFrame()[1],"Instancing changed requested AA");
        Check(CDetailDoodad::s_instancingStats.draws==test.detailDraws,"AA did not use the supported legacy detail path");
        Set(CWorldParam::cvar_nativeAA,"0");Set(CWorldParam::cvar_nativeHDR,"1");
    }
    if(test.ticks==122){
        Check(gx.SceneWorldTestCompletedFrame()[0] && CDetailDoodad::s_instancingStats.draws>test.detailDraws,"HDR did not retain instanced detail rendering");
        Set(CWorldParam::cvar_nativeHDR,"0");
    }
    if(test.ticks==126){test.frame->m_camera->m_position.x+=7;Set(CWorldParam::cvar_nativeDetailInstancing,"0");}
    if(test.ticks==127){
        TSGrowableArray<CImVector> pixels;TSGrowableArray<float> depth;CaptureDetailOnly(pixels,depth);
        test.detailOnlyColour.assign(pixels.Ptr(),pixels.Ptr()+pixels.Count());test.detailOnlyDepth.assign(depth.Ptr(),depth.Ptr()+depth.Count());
        Set(CWorldParam::cvar_nativeDetailInstancing,"1");
    }
    if(test.ticks==128)CheckDetailOnlyParity();
}
