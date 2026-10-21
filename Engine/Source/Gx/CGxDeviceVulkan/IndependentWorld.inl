void CGxDeviceVulkan::State::CollectRetired(){
    auto images=retiredImages.begin();
    for(auto it=retiredImages.begin();it!=retiredImages.end();++it){
        if(productionLease && (producerImages.count(it->image)||producerSamplers.count(it->sampler))){if(images!=it)*images=*it;++images;}
        else DestroyImage(*it);
    }
    retiredImages.erase(images,retiredImages.end());
    const auto buffers=[&](std::vector<NativeBuffer>& retired){
        auto keep=retired.begin();
        for(auto it=retired.begin();it!=retired.end();++it){
            if(productionLease && producerBuffers.count(it->buffer)){if(keep!=it)*keep=*it;++keep;}
            else DestroyBuffer(*it);
        }
        retired.erase(keep,retired.end());
    };
    buffers(retiredBuffers);buffers(retiredHdrLuts);
}
void CGxDeviceVulkan::State::BeginUiTiming(){
    if(!independentWorld || !shadowTimestampBits || uiTimingRecording)return;
    if(!uiTimingPool){VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;GX_VK(vkCreateQueryPool(device,&info,nullptr,&uiTimingPool));}
    vkCmdResetQueryPool(command,uiTimingPool,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,uiTimingPool,0);uiTimingRecording=true;
}
void CGxDeviceVulkan::State::ReadUiTiming(){
    if(!uiTimingPool || !uiTimingPending)return;
    uint64_t times[2]{};
    const auto result=vkGetQueryPoolResults(device,uiTimingPool,0,2,sizeof(times),times,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
    if(result==VK_SUCCESS){const uint64_t mask=shadowTimestampBits>=64?UINT64_MAX:(uint64_t(1)<<shadowTimestampBits)-1;uiGpuMs=((times[1]-times[0])&mask)*properties.limits.timestampPeriod/1e6;}
    else if(result!=VK_NOT_READY)VkCheck(result,"UI GPU timestamps");
    uiTimingPending=false;
}
void CGxDeviceVulkan::State::CommitHistory() {
    if(!historyPending)return;
    temporalHistory.Commit();temporalDraws.Commit();historyPending=false;historyIndex^=1;
    if(sceneScale!=1.f){previousUpscaleJitter=temporalFrame.historyValid?temporalFrame.jitterPixels:std::array<float,2>{};upscaleSample=(upscaleSample+1)%32;}
}
void CGxDeviceVulkan::State::PollWorld() {
    if(!independentWorld || productionCapture)return;
    if(productionLease && !productionPrepared && worldSubmitter) {
        const auto submitted=worldSubmitter->Result();
        if(submitted!=VK_NOT_READY) {
            VkCheck(submitted,"independent world submission");CommitHistory();
            if(producerRecording.Complete()){
                if(producerTimingPool){uint64_t times[2]{};const auto result=vkGetQueryPoolResults(device,producerTimingPool,0,2,sizeof(times),times,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
                    if(result==VK_SUCCESS){const uint64_t mask=shadowTimestampBits>=64?UINT64_MAX:(uint64_t(1)<<shadowTimestampBits)-1;worldGpuMs=((times[1]-times[0])&mask)*properties.limits.timestampPeriod/1e6;}
                    else if(result!=VK_NOT_READY)VkCheck(result,"World GPU timestamps");}
                producerRecording.ClearFramebuffers();
                worldCompleted=worldSubmitted;productionLease.reset();producerImages.clear();producerBuffers.clear();producerSamplers.clear();
            }
        }
    }
    completedWorld.Poll(worldCompleted,completedSerial);
}
void CGxDeviceVulkan::State::RestorePresentation() {
    EndPass();
    if(!productionCapture)return;
    auto& images=completedImages[productionLease->slot];
    std::swap(depth,images.depth);swapImages.swap(presentationImages);swapViews.swap(presentationViews);
    imageIndex=presentationImageIndex;RecordingResources::Swap(producerRecording);
    productionCapture=false;recording=true;inPass=false;acquirePending=savedAcquirePending;
    worldActive=temporalActive=hdrActive=false;motionOwner=nullptr;stableVertices=false;
}
bool CGxDeviceVulkan::SceneIndependentWorldSupported() const {
    return m_context && (m_native->worldTargetEnabled || m_native->temporalCapable || m_native->scaleCapable);
}
bool CGxDeviceVulkan::SceneConfigureIndependentWorld(bool enabled) {
    auto& s=*m_native;
    if(enabled && !SceneIndependentWorldSupported())return false;
    if(s.independentWorld==enabled)return true;
    if(s.productionCapture || s.productionPrepared)throw std::logic_error("Cannot change scheduling inside a world recording");
    if(s.worldSubmitter){s.worldSubmitter->FlushSubmission();s.worldSubmitter->Stop();s.worldSubmitter.reset();}
    GX_VK(vkDeviceWaitIdle(s.device));
    // The current UI buffer may have begun in the other scheduling mode.
    if(s.recording && s.shadowTimingPool){s.EndPass();vkCmdResetQueryPool(s.command,s.shadowTimingPool,0,4);}
    s.shadowTimingMask=0;
    s.producerRecording.Destroy();s.productionLease.reset();s.producerImages.clear();s.producerBuffers.clear();s.producerSamplers.clear();
    s.completedWorld.ResetAfterIdle();s.worldSubmitted=s.worldCompleted=0;s.displayedWorldSlot.reset();s.displayedWorldGeneration=0;s.ResetTemporal();
    if(enabled){
        try{s.worldSubmitter=std::make_unique<GxVulkan::WorldSubmission>(s.queue,s.queueMutex);}
        catch(const std::exception& error){s.schedulingStatus=std::string("Unable to start world submitter: ")+error.what();return false;}
    }
    s.independentWorld=enabled;++s.presentationVersion;
    s.cadenceLog=std::chrono::steady_clock::now();s.lastUiPresents=s.uiPresents;s.lastWorldCompleted=0;s.worldGpuMs=s.uiGpuMs=-1;
    s.schedulingStatus=enabled?"On: completed-frame scheduling, one device (shared GPU contention remains)":"Off: synchronous rendering";
    GxLog("Independent world scheduling: %s (one device; shared-queue GPU contention remains)",enabled?"On":"Off");
    return true;
}
bool CGxDeviceVulkan::SceneWorldProductionReady() {
    auto& s=*m_native;
    if(!s.independentWorld || !m_context || !s.BeginFrame())return false;
    s.PollWorld();return !s.productionLease;
}
bool CGxDeviceVulkan::SceneBeginWorldProduction(uint64_t scene) {
    auto& s=*m_native;
    if(!SceneWorldProductionReady())return false;
    if(m_textureTarget[GxBuffers_Color].m_texture || m_textureTarget[GxBuffers_Depth].m_texture)
        throw std::logic_error("Independent world requires the presentation target");
    s.completedWorld.Configure(scene,s.extent.width,s.extent.height);s.completedWorld.Latest();
    GxWorld::WorldFrameDescription frame;frame.scene=scene;frame.width=s.extent.width;frame.height=s.extent.height;
    frame.viewport={0,0,frame.width,frame.height};
    const auto lease=s.completedWorld.Begin(frame);if(!lease)return false;
    auto& images=s.completedImages[lease->slot];
    try {
        if(images.colour.width!=frame.width || images.colour.height!=frame.height || images.colour.format!=s.surfaceFormat){
            NativeImage colour,depth;
            try {
                const auto transfers=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                colour=s.MakeImage(frame.width,frame.height,1,1,s.surfaceFormat,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|transfers);
                depth=s.MakeImage(frame.width,frame.height,1,1,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|transfers);
            }catch(...){s.DestroyImage(colour);s.DestroyImage(depth);throw;}
            s.DestroyImage(images.colour);s.DestroyImage(images.depth);images.colour=colour;images.depth=depth;
        }
        if(!s.producerRecording.commandPool)s.producerRecording.Initialize(s.device,s.queueFamily,s.memoryProperties);
        if(!s.worldSubmitter)s.worldSubmitter=std::make_unique<GxVulkan::WorldSubmission>(s.queue,s.queueMutex);
        if(!s.producerTimingPool && s.shadowTimestampBits){VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;GX_VK(vkCreateQueryPool(s.device,&info,nullptr,&s.producerTimingPool));}
    }catch(...){s.completedWorld.Abort(*lease);throw;}
    s.EndPass();s.BeginUiTiming();s.worldBorn[lease->slot]=std::chrono::steady_clock::now();
    for(auto* image:{&images.colour,&images.depth}){
        s.Barrier(image->image,image->aspect,1,1,image->layout,VK_IMAGE_LAYOUT_GENERAL);image->layout=VK_IMAGE_LAYOUT_GENERAL;
    }
    // Preserve the original prefix, including transparent/partial world views.
    // These copies and all first-use input uploads execute in the UI submission
    // BEFORE the worker is dispatched. The world never borrows a WSI image.
    const VkRect2D full{{0,0},s.extent};
    GxVulkan::CopyWorldImage(s.command,s.swapImages[s.imageIndex],images.colour.image,VK_IMAGE_ASPECT_COLOR_BIT,full);
    GxVulkan::CopyWorldImage(s.command,s.depth.image,images.depth.image,VK_IMAGE_ASPECT_DEPTH_BIT,full);
    s.presentationImages={images.colour.image};s.presentationViews={images.colour.view};
    s.swapImages.swap(s.presentationImages);s.swapViews.swap(s.presentationViews);
    s.presentationImageIndex=s.imageIndex;s.imageIndex=0;std::swap(s.depth,images.depth);
    s.savedAcquirePending=s.acquirePending;s.RecordingResources::Swap(s.producerRecording);
    s.productionLease=lease;s.productionCapture=true;s.recording=false;s.inPass=false;s.producerImages.clear();s.producerBuffers.clear();s.producerSamplers.clear();
    s.worldRect={};s.BeginFrame();return true;
}
GxWorld::WorldFrameLease CGxDeviceVulkan::SceneEndWorldProduction(const GxWorld::WorldFrameDescription& frame) {
    auto& s=*m_native;
    if(!s.productionCapture || !s.productionLease)throw std::logic_error("No world production recording");
    if(s.worldActive)throw std::logic_error("World output/effects scope is unfinished");
    const auto lease=*s.productionLease;
    auto final=frame;final.width=s.extent.width;final.height=s.extent.height;
    final.viewport={uint32_t(s.worldRect.offset.x),uint32_t(s.worldRect.offset.y),s.worldRect.extent.width,s.worldRect.extent.height};
    final.renderScale=s.sceneScale;final.hdr=s.publishedHdr;final.temporal=s.publishedTemporal;
    s.completedImages[lease.slot].lighting=s.shadowStatus;
    s.completedWorld.Describe(lease,final);s.EndPass();
    if(s.producerTimingPool)vkCmdWriteTimestamp(s.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.producerTimingPool,1);
    GX_VK(vkEndCommandBuffer(s.command));GX_VK(vkResetFences(s.device,1,&s.fence));
    s.completedWorld.Submit(lease,++s.worldSubmitted);
    s.RestorePresentation();s.productionPrepared=true;
    return lease;
}
void CGxDeviceVulkan::SceneAbortWorldProduction() {
    auto& s=*m_native;
    if(s.productionCapture) {
        // Execute already-recorded transitions only after the UI input uploads,
        // but withdraw this generation. Discarding its predicted image layouts
        // would corrupt shared render-target state on a subsequent fallback.
        auto frame=s.completedWorld.Get(*s.productionLease).frame;
        s.EndPass();CGxDevice::DeviceSetRenderTarget(GxBuffers_Color,nullptr,0);CGxDevice::DeviceSetRenderTarget(GxBuffers_Depth,nullptr,0);
        if(!s.worldRect.extent.width || !s.worldRect.extent.height)s.worldRect={{0,0},s.extent};
        s.worldActive=false;SceneEndWorldProduction(frame);s.ResetTemporal();
    }
    SceneInvalidateCompletedWorld();
}
const char* CGxDeviceVulkan::SceneIndependentWorldStatus() const{
    if(!SceneIndependentWorldSupported())return "Unavailable: completed colour/depth transport unsupported; synchronous rendering retained";
    return m_native->schedulingStatus.c_str();
}
void CGxDeviceVulkan::SceneReportIndependentWorldFailure(const char* error){
    auto& s=*m_native;
    const std::string status=error&&*error?std::string("Synchronous fallback: ")+error:
        s.independentWorld?"On: completed-frame scheduling, one device (shared GPU contention remains)":"Off: synchronous rendering";
    if(status!=s.schedulingStatus){s.schedulingStatus=status;if(error&&*error){GxLog("Independent world: %s",status.c_str());std::fprintf(stderr,"Independent world: %s\n",status.c_str());}}
}
uint64_t CGxDeviceVulkan::ScenePresentationVersion() const{return m_context?m_native->presentationVersion:0;}
#ifdef WOWDECOMP_VULKAN_TESTING
bool CGxDeviceVulkan::SceneWorldTestProducerHeld() const{return m_native->worldSubmitter && m_native->worldSubmitter->Held();}
void CGxDeviceVulkan::SceneWorldTestHoldProducer(bool hold){
    if(!m_native->worldSubmitter)throw std::logic_error("Independent world is not enabled");
    m_native->worldSubmitter->Hold(hold);
}
#endif
void CGxDeviceVulkan::State::DetachTexture(CGxTex* texture) {
    if(!productionLease || !texture)return;
    if(productionCapture && (texture->m_flags.m_renderTarget || !texture->m_userFunc))return;
    auto found=textures.find(texture);
    if(found==textures.end() || !producerImages.count(found->second.image))return;
    const auto old=found->second;
    if(!texture->m_flags.m_renderTarget && texture->m_userFunc){
        texture->m_needsCreation=1;texture->m_needsUpdate=1;
        texture->m_updateRect={0,0,int32_t(texture->m_height),int32_t(texture->m_width)};
        return;
    }
    EndPass();
    auto replacement=MakeImage(old.width,old.height,old.levels,old.layers,old.format,old.aspect,old.usage);
    try{retiredImages.push_back(old);}catch(...){DestroyImage(replacement);throw;}
    Barrier(replacement.image,replacement.aspect,replacement.levels,replacement.layers,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Barrier(old.image,old.aspect,old.levels,old.layers,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for(uint32_t level=0;level<old.levels;++level){
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={old.aspect,level,0,old.layers};
        copy.extent={std::max(1u,old.width>>level),std::max(1u,old.height>>level),1};
        vkCmdCopyImage(command,old.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,replacement.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    }
    Barrier(old.image,old.aspect,old.levels,old.layers,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    Barrier(replacement.image,replacement.aspect,replacement.levels,replacement.layers,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    replacement.layout=VK_IMAGE_LAYOUT_GENERAL;found->second=replacement;texture->m_apiSpecificData=&found->second;texture->m_needsFlagUpdate=1;
}
