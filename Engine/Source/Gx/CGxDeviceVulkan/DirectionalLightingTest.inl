// Test-only use of the original device/queue and the maintained shared coverage.
// This is a GPU material/query fixture, not production-world acceptance.
bool CGxDeviceVulkan::SceneRayQueryTestDirectionalLighting(){
    auto& s=*m_native;
    if(!s.device||!s.rayQueryCapabilities.Available()||s.properties.limits.maxBoundDescriptorSets<5){std::fprintf(stderr,"Directional fixture capabilities: device=%u ray=%u maxSets=%u\n",s.device!=VK_NULL_HANDLE,s.rayQueryCapabilities.Available(),s.properties.limits.maxBoundDescriptorSets);return false;}
    const auto before=s.sceneAdmission->Used();
    for(unsigned phase=0;phase<2;++phase){
        if(!s.BeginFrame())return false;s.EndPass();
        auto gpu=std::make_shared<ShadowGpuFrame>();gpu->device=s.device;gpu->releaseImage=[&](NativeImage& image){s.DestroyImage(image);};
        GxVulkan::Buffer result;std::array<VkDescriptorSetLayout,4> extra{};
        const auto cleanup=[&]{gpu.reset();GxVulkan::DestroyBuffer(s.device,result);for(auto layout:extra)if(layout)vkDestroyDescriptorSetLayout(s.device,layout,nullptr);};
        try{
            constexpr unsigned count=16;const float cutoff=128.f/255;
            std::array<GxShadow::GpuTriangle,count> coverage{};
            std::vector<GxVulkan::ShadowPosition> positions;std::vector<uint32_t> indices;
            for(unsigned i=0;i<count;++i){
                const float x=i*2.f+(i==4&&phase?.75f:0.f);
                for(const auto& p:std::array<std::array<float,3>,3>{{{{x,0,0}},{{x+1,0,0}},{{x,1,0}}}}){positions.push_back({{p[0],p[1],p[2]}});indices.push_back(indices.size());}
                auto& t=coverage[i];t.flags[1]=(i>=1&&i<=7)||i>=13?1:0;t.flags[2]=i==8||i==10?0:1;
                t.uv2OpacityThreshold[2]=1;t.uv2OpacityThreshold[3]=cutoff;
                float u=i==0||i==1||i==15&&!phase?.25f:.75f;
                if(i==13||i==14)u=1.25f;
                t.uv01[0]=t.uv01[2]=t.uv2OpacityThreshold[0]=u;t.uv01[1]=t.uv01[3]=t.uv2OpacityThreshold[1]=.5f;
                if(i==3)t.uv2OpacityThreshold[3]=std::nextafter(cutoff,1.f);
                if(i==5||i==6)t.flags[1]=2;
                if(i==6&&!phase)t.uv2OpacityThreshold[2]=.55f;
                if(i==7)t.flags[1]=3;
                if(i==14)t.flags[0]=1;
            }
            GxShadow::GpuDirectionalFrame frame{};for(unsigned i=0;i<4;++i)frame.values[i][i]=1;
            frame.values[4][3]=64;frame.values[5][2]=1;frame.values[5][3]=128;
            frame.values[6][0]=.015f;frame.values[6][1]=1;frame.values[6][2]=phase+1;
            const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            gpu->uniforms=GxVulkan::CreateBuffer(s.device,s.memoryProperties,sizeof(frame),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,host,s.sceneAdmission.get());
            gpu->triangles=GxVulkan::CreateBuffer(s.device,s.memoryProperties,sizeof(coverage),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,s.sceneAdmission.get());
            result=GxVulkan::CreateBuffer(s.device,s.memoryProperties,count*7*4*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,s.sceneAdmission.get());
            std::memcpy(gpu->uniforms.mapped,&frame,sizeof(frame));std::memcpy(gpu->triangles.mapped,coverage.data(),sizeof(coverage));
            std::memset(result.mapped,0,result.size);
            gpu->output=s.MakeImage(2,1,1,1,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,UINT64_MAX,&s.imageAdmission);
            const uint8_t alpha[]={255,255,255,0,255,255,255,128};
            const auto offset=s.Upload(alpha,sizeof(alpha),4);VkBufferImageCopy copy{};copy.bufferOffset=offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={2,1,1};
            s.Barrier(gpu->output.image,gpu->output.aspect,1,1,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(s.command,s.upload.buffer,gpu->output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            s.Barrier(gpu->output.image,gpu->output.aspect,1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);gpu->output.layout=VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo images[2]{};
            for(unsigned i=0;i<2;++i){VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
                sampler.addressModeU=i?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                VkSampler handle=VK_NULL_HANDLE;GX_VK(vkCreateSampler(s.device,&sampler,nullptr,&handle));gpu->samplers.push_back(handle);images[i]={handle,gpu->output.view,VK_IMAGE_LAYOUT_GENERAL};}
            gpu->scene=std::make_unique<GxVulkan::AccelerationScene>(s.device,s.physical,s.memoryProperties,s.rayQueryCapabilities,s.sceneAdmission);
            gpu->scene->Build(s.command,positions.data(),positions.size(),indices.data(),indices.size());
            const VkDescriptorSetLayoutBinding outputBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            for(unsigned i=0;i<4;++i){VkDescriptorSetLayoutCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};if(!i){create.bindingCount=1;create.pBindings=&outputBinding;}GX_VK(vkCreateDescriptorSetLayout(s.device,&create,nullptr,&extra[i]));}
            const VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
            VkDescriptorSetLayoutCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};create.bindingCount=4;create.pBindings=bindings;GX_VK(vkCreateDescriptorSetLayout(s.device,&create,nullptr,&gpu->descriptors));
            const VkDescriptorSetLayout layouts[]={extra[0],extra[1],extra[2],extra[3],gpu->descriptors};
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=5;pipelineLayout.pSetLayouts=layouts;GX_VK(vkCreatePipelineLayout(s.device,&pipelineLayout,nullptr,&gpu->layout));
            const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=4;pool.pPoolSizes=sizes;GX_VK(vkCreateDescriptorPool(s.device,&pool,nullptr,&gpu->pool));
            const VkDescriptorSetLayout allocatedLayouts[]={extra[0],gpu->descriptors};VkDescriptorSet sets[2]{};
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=gpu->pool;allocate.descriptorSetCount=2;allocate.pSetLayouts=allocatedLayouts;GX_VK(vkAllocateDescriptorSets(s.device,&allocate,sets));
            const auto top=gpu->scene->TopLevel();VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&top;
            VkDescriptorBufferInfo buffers[]={{gpu->triangles.buffer,0,gpu->triangles.size},{gpu->uniforms.buffer,0,gpu->uniforms.size},{result.buffer,0,result.size}};
            VkWriteDescriptorSet writes[5]{};for(unsigned i=0;i<5;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=sets[i==4?0:1];w.dstBinding=i==4?0:i;w.descriptorCount=i==3?2:1;w.descriptorType=i==4?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:bindings[i].descriptorType;
                if(!i)w.pNext=&as;else if(i==3)w.pImageInfo=images;else w.pBufferInfo=&buffers[i==4?2:i-1];}
            vkUpdateDescriptorSets(s.device,5,writes,0,nullptr);
            const auto code=ReadModule("directional-lighting-probe.comp.spv");VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=code.size()*4;module.pCode=code.data();GX_VK(vkCreateShaderModule(s.device,&module,nullptr,&gpu->module));
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=gpu->layout;pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=gpu->module;pipeline.stage.pName="main";GX_VK(vkCreateComputePipelines(s.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&gpu->pipeline));
            VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(s.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
            vkCmdBindPipeline(s.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu->pipeline);vkCmdBindDescriptorSets(s.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu->layout,0,1,&sets[0],0,nullptr);vkCmdBindDescriptorSets(s.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu->layout,4,1,&sets[1],0,nullptr);vkCmdDispatch(s.command,1,1,1);
            VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};read.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(s.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);s.Submit(false,true);
            const float expectedVisibility[]={0,1,0,1,float(phase),0,float(!phase),1,1,0,0,1,1,1,0,float(!phase)};
            const float components[5][3]={{.1f,.2f,.3f},{.4f,.5f,.6f},{.7f,.8f,.9f},{.25f,.125f,.375f},{.0625f,.03125f,.015625f}};
            std::array<float,count*7*4> values{};std::memcpy(values.data(),result.mapped,result.size);
            for(unsigned i=0;i<count;++i)for(unsigned group=0;group<6;++group)for(unsigned c=0;c<4;++c){const float v=expectedVisibility[i];float expected=v;
                if(c<3){if(group<3)expected=components[group][c];else if(group<5)expected=components[group][c]*(phase+1)*v;
                    else expected=components[0][c]+components[1][c]+components[2][c]+(components[3][c]+components[4][c])*(phase+1)*v;}
                const float actual=values[(i*7+group)*4+c];if(!std::isfinite(actual)||std::abs(actual-expected)>2e-6f)throw std::runtime_error("Shared directional probe alpha="+std::to_string(values[i*7*4+24])+" texture="+std::to_string(values[i*7*4+25])+" mode="+std::to_string(values[i*7*4+26])+" threshold="+std::to_string(values[i*7*4+27])+" mismatch phase="+std::to_string(phase)+" case="+std::to_string(i)+" group="+std::to_string(group)+" channel="+std::to_string(c)+" actual="+std::to_string(actual)+" expected="+std::to_string(expected));}
            cleanup();if(s.sceneAdmission->Used()!=before)throw std::runtime_error("Shared directional probe retained admitted bytes");
        }catch(const std::exception& error){std::fprintf(stderr,"Shared directional probe: %s\n",error.what());GxLog("Shared directional probe: %s",error.what());vkDeviceWaitIdle(s.device);vkResetCommandPool(s.device,s.commandPool,0);s.recording=false;s.inPass=false;cleanup();return false;}
    }
    return true;
}
