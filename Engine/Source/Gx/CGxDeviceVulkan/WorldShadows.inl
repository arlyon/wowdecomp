// Actual game-world visibility view. Uses the original raster depth and shared
// current-pose AS; it does not darken ambient/local lighting as a shortcut.
void CGxDeviceVulkan::State::RecordWorldShadowVisibility(){
    using namespace GxShadow;
    if(!shadowRequested)return;
    shadowStatus={};
    if(!rayQueryCapabilities.Available()){shadowStatus.result=FrameResult::Unavailable;return;}
    if(!shadowSource){shadowStatus.result=FrameResult::PendingGeometry;return;}
    const auto& source=*shadowSource;
    shadowStatus={source.result,source.terrain,source.models,source.buildings,source.triangles,source.unsupported,false};
    if(source.result!=FrameResult::Ready&&shadowStrict)return;
    if(source.meshes.empty())return;
    auto gpu=std::make_shared<ShadowGpuFrame>();gpu->device=device;gpu->source=shadowSource;
    gpu->releaseImage=[this](NativeImage& image){DestroyImage(image);};
    shadowRetired.push_back(gpu); // Keep failed/recorded allocations until fence too.
    struct Triangle {float uv01[4],uv2OpacityThreshold[4];uint32_t flags[4];};
    struct Uniform {float inverse[16],camera[4],light[4],depthRange[4];uint32_t rect[4],size[4];};
    static_assert(sizeof(Triangle)==48&&sizeof(Uniform)==144,"World visibility ABI");
    try {
        std::vector<GxVulkan::ShadowPosition> positions;std::vector<uint32_t> indices;std::vector<Triangle> triangles;
        std::vector<VkDescriptorImageInfo> texturesInfo;
        std::map<std::tuple<CGxTex*,bool,bool,uint32_t>,uint32_t> textureSlots;
        const auto textureSlot=[&](CGxTex* texture,bool wrapU,bool wrapV){
            const uint32_t filter=texture?texture->m_flags.m_filter:GxTex_Linear;
            const auto key=std::make_tuple(texture,wrapU,wrapV,filter);const auto found=textureSlots.find(key);if(found!=textureSlots.end())return found->second;
            auto& image=Texture(texture);
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter=filter==GxTex_Nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
            info.minFilter=filter==GxTex_Nearest||filter==GxTex_NearestMipNearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
            info.mipmapMode=filter>=GxTex_LinearMipLinear?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU=wrapU?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV=wrapV?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;info.maxLod=float(image.levels-1);
            VkSampler sampler=VK_NULL_HANDLE;GX_VK(vkCreateSampler(device,&info,nullptr,&sampler));gpu->samplers.push_back(sampler);
            const auto slot=uint32_t(texturesInfo.size());texturesInfo.push_back({sampler,image.view,VK_IMAGE_LAYOUT_GENERAL});textureSlots.emplace(key,slot);return slot;
        };
        textureSlot(nullptr,false,false);
        for(const auto& mesh:source.meshes){
            const auto& material=mesh.material;
            if(material.mode==CoverageMode::NonCaster)continue;
            if(material.mode!=CoverageMode::Opaque&&material.mode!=CoverageMode::Cutout&&material.mode!=CoverageMode::ForcedCutout)throw std::invalid_argument("Unknown world coverage");
            const bool masked=material.mode!=CoverageMode::Opaque;
            if(mesh.indices.size()%3||positions.size()+mesh.positions.size()>UINT32_MAX ||
                (masked&&(material.textureCount!=1||mesh.coverage.size()!=mesh.positions.size())))throw std::invalid_argument("Incomplete world coverage geometry");
            const uint32_t slot=masked?textureSlot(material.textures[0],material.wrapU[0],material.wrapV[0]):0;
            const uint32_t base=positions.size();
            for(const auto& p:mesh.positions)positions.push_back({{p.xyz[0],p.xyz[1],p.xyz[2]}});
            for(size_t i=0;i<mesh.indices.size();i+=3){Triangle tri{};tri.flags[0]=slot;tri.flags[1]=uint32_t(material.mode);tri.flags[2]=material.twoSided;
                tri.uv2OpacityThreshold[2]=material.opacity;tri.uv2OpacityThreshold[3]=material.threshold;
                for(unsigned k=0;k<3;++k){const auto index=mesh.indices[i+k];if(index>=mesh.positions.size())throw std::invalid_argument("World shadow index");indices.push_back(base+index);
                    if(masked){float* uv=k<2?tri.uv01+2*k:tri.uv2OpacityThreshold;uv[0]=mesh.coverage[index].uv[0][0];uv[1]=mesh.coverage[index].uv[0][1];}}
                triangles.push_back(tri);
            }
        }
        if(indices.empty())return;
        const uint32_t textureCount=texturesInfo.size();
        if(textureCount>properties.limits.maxPerStageDescriptorSamplers || textureCount+1>properties.limits.maxPerStageDescriptorSampledImages)
            throw std::runtime_error("World coverage exceeds device texture binding capacity");
        Uniform data{};std::memcpy(data.inverse,source.inverseViewProjection.data(),64);
        for(unsigned i=0;i<3;++i){data.camera[i]=source.camera[i];data.light[i]=source.lightDirection[i];}
        data.camera[3]=source.regionRadius;data.light[3]=source.rayDistance;
        data.depthRange[0]=source.viewportDepthRange[0];data.depthRange[1]=source.viewportDepthRange[1];
        data.rect[0]=renderRect.offset.x;data.rect[1]=renderRect.offset.y;data.rect[2]=renderRect.extent.width;data.rect[3]=renderRect.extent.height;
        data.size[0]=renderExtent.width;data.size[1]=renderExtent.height;
        const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        gpu->uniforms=GxVulkan::CreateBuffer(device,memoryProperties,sizeof(data),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,host,sceneAdmission.get());
        gpu->triangles=GxVulkan::CreateBuffer(device,memoryProperties,triangles.size()*sizeof(Triangle),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,sceneAdmission.get());
        std::memcpy(gpu->uniforms.mapped,&data,sizeof(data));std::memcpy(gpu->triangles.mapped,triangles.data(),gpu->triangles.size);
        gpu->output=MakeImage(renderExtent.width,renderExtent.height,1,1,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,UINT64_MAX,&imageAdmission);
        VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{4,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,textureCount,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=6;layout.pBindings=bindings;
        GX_VK(vkCreateDescriptorSetLayout(device,&layout,nullptr,&gpu->descriptors));
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayoutInfo.setLayoutCount=1;pipelineLayoutInfo.pSetLayouts=&gpu->descriptors;
        GX_VK(vkCreatePipelineLayout(device,&pipelineLayoutInfo,nullptr,&gpu->layout));
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,textureCount}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=6;pool.pPoolSizes=sizes;
        GX_VK(vkCreateDescriptorPool(device,&pool,nullptr,&gpu->pool));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=gpu->pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&gpu->descriptors;
        VkDescriptorSet set=VK_NULL_HANDLE;GX_VK(vkAllocateDescriptorSets(device,&allocate,&set));
        const auto code=ReadModule("world-shadow-visibility.comp.spv");VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=code.size()*4;module.pCode=code.data();
        GX_VK(vkCreateShaderModule(device,&module,nullptr,&gpu->module));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.layout=gpu->layout;compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=gpu->module;compute.stage.pName="main";
        GX_VK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&compute,nullptr,&gpu->pipeline));
        gpu->scene=std::make_unique<GxVulkan::AccelerationScene>(device,physical,memoryProperties,rayQueryCapabilities,sceneAdmission);
        gpu->scene->Build(command,positions.data(),positions.size(),indices.data(),indices.size());
        Barrier(worldDepth.image,worldDepth.aspect,1,1,worldDepth.layout,VK_IMAGE_LAYOUT_GENERAL);worldDepth.layout=VK_IMAGE_LAYOUT_GENERAL;
        Barrier(gpu->output.image,gpu->output.aspect,1,1,gpu->output.layout,VK_IMAGE_LAYOUT_GENERAL);gpu->output.layout=VK_IMAGE_LAYOUT_GENERAL;
        const auto top=gpu->scene->TopLevel();VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&top;
        VkDescriptorBufferInfo bufferInfo[]={{gpu->uniforms.buffer,0,gpu->uniforms.size},{gpu->triangles.buffer,0,gpu->triangles.size}};
        VkDescriptorImageInfo imageInfo[]={{VK_NULL_HANDLE,worldDepth.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,gpu->output.view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[6]{};
        for(unsigned i=0;i<6;++i){auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=i;write.descriptorCount=i==5?textureCount:1;write.descriptorType=bindings[i].descriptorType;
            if(!i)write.pNext=&as;else if(i<3)write.pBufferInfo=&bufferInfo[i-1];else if(i<5)write.pImageInfo=&imageInfo[i-3];else write.pImageInfo=texturesInfo.data();}
        vkUpdateDescriptorSets(device,6,writes,0,nullptr);
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu->pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu->layout,0,1,&set,0,nullptr);
        vkCmdDispatch(command,(renderExtent.width+7)/8,(renderExtent.height+7)/8,1);
        Barrier(gpu->output.image,gpu->output.aspect,1,1,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        shadowCurrent=gpu;shadowStatus.diagnosticRendered=true;
    }catch(const std::exception& error){shadowStatus.result=FrameResult::AllocationFailed;GxLog("RT visibility fallback: %s",error.what());}
}
