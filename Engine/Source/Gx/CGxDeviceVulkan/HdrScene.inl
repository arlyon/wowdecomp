// Engineering scene-output adapter. Legacy shading remains display-encoded art;
// only its final RGB crosses into the linear floating-point scene transport.
const std::vector<uint32_t>& CGxDeviceVulkan::State::BoundaryPixel(const std::vector<uint32_t>& code, bool decode, NativeShader* shader, bool temporal) {
    if (shader) {
        // Generation-owned variants cannot alias a different override merely
        // because its base SPIR-V happens to contain identical instructions.
        auto& cached=shader->hdrCode[temporal?1:0][decode?1:0];
        if(cached.empty())cached=Compile(GxVulkan::ShaderOutputBoundary(temporal?shader->temporalSource:shader->source,decode),false,shader->maintained);
        return cached;
    }
    auto& hdrPixels=decode ? this->hdrPixels : hdrLinearPixels;
    auto found=hdrPixels.find(code);
    if (found!=hdrPixels.end()) return found->second;
    // Authored fixed/font modules have explicit maintained output-boundary variants.
    if(!decode) throw std::runtime_error("Linear output provider requires recovered float4 C0 ABI");
    const char* name=nullptr;
    if (code==fixedPixel) name="fixed-hdr.frag.spv";
    else if (code==msdfFontPixel) name="font-msdf-hdr.frag.spv";
    else if (code==temporalMsdfFontPixel) name="font-msdf-temporal-hdr.frag.spv";
    else if (code==temporalFixedPixel) name="fixed-temporal-hdr.frag.spv";
    if (!name) throw std::runtime_error("HDR pixel output boundary unavailable (no shader substitution)");
    return hdrPixels.emplace(code,ReadModule(name)).first->second;
}

bool CGxDeviceVulkan::State::PrepareHdrImages(bool temporal) {
    // Atomic image-set allocation: count current AND all fence-retired images.
    // Reuse existing set only when every required attachment agrees.
    const bool historyNeeded=temporal || sceneScale!=1.f;
    const uint32_t rw=renderExtent.width,rh=renderExtent.height;
    struct Spec { NativeImage* image; uint32_t w,h; VkFormat format; VkImageAspectFlags aspect; };
    std::vector<Spec> specs{{&worldColour,rw,rh,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT},
        {&worldDepth,rw,rh,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT},
        {&hdrBackdrop,extent.width,extent.height,surfaceFormat,VK_IMAGE_ASPECT_COLOR_BIT},
        {&hdrClassic,rw,rh,surfaceFormat,VK_IMAGE_ASPECT_COLOR_BIT}};
    if(historyNeeded) {
        specs.push_back({&motion,rw,rh,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT});
        for(auto& h:history) specs.push_back({&h,extent.width,extent.height,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT});
        specs.push_back({&previousDepth,extent.width,extent.height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT});
        if(sceneScale!=1.f) specs.push_back({&depthScratch,extent.width,extent.height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT});
    }
    const VkDeviceSize budget=hdrImageBudget;
    VkDeviceSize used=0;
    for(auto* i:{&worldColour,&worldDepth,&hdrBackdrop,&hdrClassic,&motion,&history[0],&history[1],&previousDepth,&depthScratch}) used+=i->allocationBytes;
    for(auto& i:retiredImages) used+=i.allocationBytes;
    if(used>budget) return false;
    std::vector<NativeImage> replacement(specs.size());
    try {
        retiredImages.reserve(retiredImages.size()+specs.size());
        for(size_t n=0;n<specs.size();++n) {
            auto& s=specs[n];
            if(s.image->image && s.image->width==s.w && s.image->height==s.h && s.image->format==s.format) continue;
            if(used>budget) throw std::runtime_error("HDR image budget exceeded");
            const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
                (s.aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            replacement[n]=MakeImage(s.w,s.h,1,1,s.format,s.aspect,usage,budget-used);
            used+=replacement[n].allocationBytes;
        }
    } catch(const std::exception& e) {
        for(auto& image:replacement) DestroyImage(image);
        GxLog("HDR set preflight failed: %s",e.what());
        return false;
    }
    for(size_t n=0;n<specs.size();++n) if(replacement[n].image) {
        if(specs[n].image->image) retiredImages.push_back(*specs[n].image);
        *specs[n].image=replacement[n]; ResetTemporal();
    }
    return true;
}

uint64_t CGxDeviceVulkan::State::HdrLutBytes() const {
    const auto allocation=[&](const NativeBuffer& b) { VkMemoryRequirements r{};if(b.buffer) vkGetBufferMemoryRequirements(device,b.buffer,&r);return r.size; };
    // Bounded path/status strings and owner request copies are reserved even
    // when no worker exists. Container storage is charged separately below.
    uint64_t used=16u*1024u+allocation(hdrLutBuffer)+hdrLutData.capacity()*sizeof(hdrLutData[0])
        +retiredHdrLuts.capacity()*sizeof(NativeBuffer);
    for(const auto& b:retiredHdrLuts) used+=allocation(b);
    if(hdrLutJob) used+=GxVulkan::HdrLutJob::Reservation;
    return used;
}
void CGxDeviceVulkan::State::UploadHdrLut(const std::vector<std::array<float,3>>& data) {
    constexpr VkDeviceSize budget=GxVulkan::HdrLutBudget;
    const bool replacement=&data!=&hdrLutData;
    const VkDeviceSize temporary=replacement?data.capacity()*sizeof(data[0]):0;
    VkDeviceSize used=HdrLutBytes()+temporary;
    const VkDeviceSize bytes=std::max<size_t>(16,data.size()*16);
    if(used>budget || bytes>budget-used) throw std::runtime_error("HDR LUT CPU/job/live+retired buffer budget exceeded");
    if(retiredHdrLuts.size()==retiredHdrLuts.capacity()) {
        const auto metadata=(retiredHdrLuts.size()+1)*sizeof(NativeBuffer);
        if(metadata>budget-used) throw std::runtime_error("HDR LUT retirement metadata budget exceeded");
        // Old and replacement vector storage overlap during reserve.
        retiredHdrLuts.reserve(retiredHdrLuts.size()+1);
        used=HdrLutBytes()+temporary;
        if(used>budget || bytes>budget-used) throw std::runtime_error("HDR LUT retirement budget exceeded");
    }
    NativeBuffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size=bytes;info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    GX_VK(vkCreateBuffer(device,&info,nullptr,&buffer.buffer));
    try {
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer.buffer,&requirements);
        // Refuse physical allocation before it can exceed the combined cap.
        if(requirements.size>budget-used) throw std::runtime_error("HDR LUT allocation budget exceeded");
#ifdef WOWDECOMP_VULKAN_TESTING
        hdrLutUploadBytes=used+requirements.size;
        hdrLutPeakBytes=std::max(hdrLutPeakBytes,hdrLutUploadBytes);
#endif
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        GX_VK(vkAllocateMemory(device,&allocation,nullptr,&buffer.memory));
        GX_VK(vkBindBufferMemory(device,buffer.buffer,buffer.memory,0));
        void* mapped=nullptr;GX_VK(vkMapMemory(device,buffer.memory,0,bytes,0,&mapped));
        buffer.mapped=static_cast<uint8_t*>(mapped);buffer.size=bytes;
    } catch(...) { DestroyBuffer(buffer);throw; }
    if(data.empty()) std::memset(buffer.mapped,0,size_t(bytes));
    else {
        // The cube owns RGB only. Expand directly into the admitted GPU mapping;
        // there is no CPU RGBA staging/flatten buffer or change to RGB floats.
        for(size_t i=0;i<data.size();++i) {
            const std::array<float,4> row{data[i][0],data[i][1],data[i][2],1.f};
            std::memcpy(buffer.mapped+i*sizeof(row),row.data(),sizeof(row));
        }
    }
    if(hdrLutBuffer.buffer) retiredHdrLuts.push_back(hdrLutBuffer);
    hdrLutBuffer=buffer;
}

void CGxDeviceVulkan::State::HdrTransfer(NativeImage& source,VkImageView target,VkFormat format,VkExtent2D output,uint32_t operation,bool sceneRect) {
    EndPass();
    if(!hdrLutBuffer.buffer) UploadHdrLut(hdrLutData);
    if(!hdrDescriptors) {
        VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo d{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};d.bindingCount=3;d.pBindings=bindings;
        GX_VK(vkCreateDescriptorSetLayout(device,&d,nullptr,&hdrDescriptors));
    }
    if(!hdrLayout) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
        VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};l.setLayoutCount=1;l.pSetLayouts=&hdrDescriptors;l.pushConstantRangeCount=1;l.pPushConstantRanges=&push;
        GX_VK(vkCreatePipelineLayout(device,&l,nullptr,&hdrLayout));
    }
    if(!hdrSampler) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        GX_VK(vkCreateSampler(device,&sampler,nullptr,&hdrSampler));
    }
    auto& pair=hdrTransfers[format];
    if(!pair.first) {
        VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0};
        VkRenderPassCreateInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};p.dependencyCount=1;p.pDependencies=&dep;p.attachmentCount=1;p.pAttachments=&attachment;p.subpassCount=1;p.pSubpasses=&sub;
        GX_VK(vkCreateRenderPass(device,&p,nullptr,&pair.first));
    }
    if(!pair.second) {
        VkShaderModule modules[2]{};
        try {
            VkPipelineShaderStageCreateInfo stages[2]{};
            for(unsigned i=0;i<2;++i) {
                auto code=ReadModule(i?"hdr-world.frag.spv":"hdr-world.vert.spv");
                VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};m.codeSize=code.size()*4;m.pCode=code.data();GX_VK(vkCreateShaderModule(device,&m,nullptr,&modules[i]));
                stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=modules[i];stages[i].pName="main";
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};r.polygonMode=VK_POLYGON_MODE_FILL;r.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ca;
            VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dynamic;
            VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};p.stageCount=2;p.pStages=stages;p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pViewportState=&vp;p.pRasterizationState=&r;p.pMultisampleState=&ms;p.pColorBlendState=&cb;p.pDynamicState=&ds;p.layout=hdrLayout;p.renderPass=pair.first;
            GX_VK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&p,nullptr,&pair.second));
        } catch(...) {for(auto m:modules) if(m) vkDestroyShaderModule(device,m,nullptr);throw;}
        for(auto m:modules) vkDestroyShaderModule(device,m,nullptr);
    }
    VkDescriptorSet set;VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=descriptorPool;a.descriptorSetCount=1;a.pSetLayouts=&hdrDescriptors;GX_VK(vkAllocateDescriptorSets(device,&a,&set));
    Barrier(source.image,source.aspect,1,1,source.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);source.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,source.view,source.layout},{hdrSampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet writes[2]{};for(unsigned i=0;i<2;++i) {auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&images[i];}
    VkDescriptorBufferInfo lutBuffer{hdrLutBuffer.buffer,0,hdrLutBuffer.size};
    VkWriteDescriptorSet lutWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};lutWrite.dstSet=set;lutWrite.dstBinding=2;lutWrite.descriptorCount=1;lutWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;lutWrite.pBufferInfo=&lutBuffer;
    vkUpdateDescriptorSets(device,2,writes,0,nullptr);vkUpdateDescriptorSets(device,1,&lutWrite,0,nullptr);
    VkFramebuffer fb;VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=pair.first;f.attachmentCount=1;f.pAttachments=&target;f.width=output.width;f.height=output.height;f.layers=1;
    framebuffers.reserve(framebuffers.size()+1);GX_VK(vkCreateFramebuffer(device,&f,nullptr,&fb));framebuffers.push_back(fb);
    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=pair.first;b.framebuffer=fb;b.renderArea.extent=output;
    vkCmdBeginRenderPass(command,&b,VK_SUBPASS_CONTENTS_INLINE);inPass=true;
    const VkViewport viewport{0,0,float(output.width),float(output.height),0,1};vkCmdSetViewport(command,0,1,&viewport);
    const VkRect2D rect=sceneRect?worldRect:VkRect2D{{0,0},output};vkCmdSetScissor(command,0,1,&rect);
    struct Options {uint32_t operation;float ev,width,height;uint32_t lut[4];} options{operation,hdrEV,float(output.width),float(output.height),{operation?hdrLutSize:0u,0,0,0}};
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pair.second);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,hdrLayout,0,1,&set,0,nullptr);vkCmdPushConstants(command,hdrLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(options),&options);vkCmdDraw(command,3,1,0,0);EndPass();
    Barrier(source.image,source.aspect,1,1,source.layout,VK_IMAGE_LAYOUT_GENERAL);source.layout=VK_IMAGE_LAYOUT_GENERAL;
}

void CGxDeviceVulkan::State::DestroyHdr() {
    for(auto& p:hdrTransfers) { if(p.second.second) vkDestroyPipeline(device,p.second.second,nullptr);if(p.second.first) vkDestroyRenderPass(device,p.second.first,nullptr); }
    hdrTransfers.clear();
    for(auto p:{hdrResolvePipeline,hdrUpscalePipeline}) if(p) vkDestroyPipeline(device,p,nullptr);
    if(hdrResolvePass) vkDestroyRenderPass(device,hdrResolvePass,nullptr);
    hdrResolvePipeline=hdrUpscalePipeline=VK_NULL_HANDLE;hdrResolvePass=VK_NULL_HANDLE;
    if(hdrLayout) vkDestroyPipelineLayout(device,hdrLayout,nullptr);
    if(hdrDescriptors) vkDestroyDescriptorSetLayout(device,hdrDescriptors,nullptr);
    if(hdrSampler) vkDestroySampler(device,hdrSampler,nullptr);
    hdrLayout=VK_NULL_HANDLE;hdrDescriptors=VK_NULL_HANDLE;hdrSampler=VK_NULL_HANDLE;
    DestroyBuffer(hdrLutBuffer);for(auto& b:retiredHdrLuts) DestroyBuffer(b);retiredHdrLuts.clear();
    DestroyImage(hdrBackdrop);DestroyImage(hdrClassic);hdrActive=false;hdrAborted=false;
}
