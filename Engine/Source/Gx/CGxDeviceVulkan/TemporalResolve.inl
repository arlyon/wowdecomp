// Private live-backend pass. Included after State's definition.
void CGxDeviceVulkan::State::ResolveWorld() {
    const bool upscale = sceneScale != 1.f;
    auto& pipelineHandle = upscale ? upscalePipeline : resolvePipeline;
    if (!resolveDescriptors) {
        VkDescriptorSetLayoutBinding bindings[6]{};
        for (uint32_t i = 0; i < 6; ++i)
            bindings[i] = {i == 4 ? 5u : i == 5 ? 6u : i, i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptors.bindingCount = 6; descriptors.pBindings = bindings;
        GX_VK(vkCreateDescriptorSetLayout(device, &descriptors, nullptr, &resolveDescriptors));
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &resolveDescriptors;
        GX_VK(vkCreatePipelineLayout(device, &layout, nullptr, &resolveLayout));
    }
    if (!resolvePass) {
        VkAttachmentDescription attachment{};
        attachment.format = surfaceFormat; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 1; pass.pAttachments = &attachment; pass.subpassCount = 1; pass.pSubpasses = &subpass;
        GX_VK(vkCreateRenderPass(device, &pass, nullptr, &resolvePass));
    }
    if (!pipelineHandle) {
        VkShaderModule modules[2]{};
        try {
            const auto vertex = ReadModule(upscale ? "temporal-upscale.vert.spv" : "temporal-resolve.vert.spv"),
                pixel = ReadModule(upscale ? "temporal-upscale.frag.spv" : "temporal-resolve.frag.spv");
            const std::vector<uint32_t>* codes[] = {&vertex, &pixel};
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (unsigned i = 0; i < 2; ++i) {
                VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                module.codeSize = codes[i]->size()*4; module.pCode = codes[i]->data();
                GX_VK(vkCreateShaderModule(device, &module, nullptr, &modules[i]));
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                stages[i].module = modules[i]; stages[i].pName = "main";
            }
            VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState colour{}; colour.colorWriteMask = 15;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1; blend.pAttachments = &colour;
            const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &inputs;
            pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &samples;
            pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic;
            pipeline.layout = resolveLayout; pipeline.renderPass = resolvePass;
            GX_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipelineHandle));
        } catch (...) {
            for (auto module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
        for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
    }
    auto& output = history[historyIndex ^ 1];
    NativeImage* inputs[] = {&worldColour, &history[historyIndex], &motion, &previousDepth, &worldDepth};
    for (auto* image : inputs) {
        Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    Barrier(output.image, output.aspect, 1, 1, output.layout, VK_IMAGE_LAYOUT_GENERAL);
    output.layout = VK_IMAGE_LAYOUT_GENERAL;
    struct alignas(16) Constants {
        std::array<uint32_t,4> extentValidity;
        std::array<int32_t,4> viewport, previousViewport;
        std::array<float,4> accumulation, reserved;
        std::array<uint32_t,4> renderExtent;
        std::array<int32_t,4> renderViewport;
        std::array<float,4> previousJitter;
    };
    static_assert(sizeof(Constants) == 128, "Temporal upscale ABI (native prefix remains 80 bytes)");
    const std::array<int32_t,4> bounds{worldRect.offset.x, worldRect.offset.y,
        worldRect.offset.x+int32_t(worldRect.extent.width), worldRect.offset.y+int32_t(worldRect.extent.height)};
    // A later duplicate cannot retract the first draw's recorded motion.
    const Constants constants{{extent.width, extent.height,
        temporalActive && temporalFrame.historyValid && !temporalDraws.Ambiguous() ? 1u : 0u, 0},
        bounds, bounds, {.9f, .0001f, 0, 0},
        {upscale ? temporalFrame.jitterPixels[0] : 0.f, upscale ? temporalFrame.jitterPixels[1] : 0.f,
            upscale && temporalActive ? 1.f : 0.f, 0},
        {renderExtent.width,renderExtent.height,0,0},
        {renderRect.offset.x,renderRect.offset.y,renderRect.offset.x+int32_t(renderRect.extent.width),
            renderRect.offset.y+int32_t(renderRect.extent.height)},
        {previousUpscaleJitter[0],previousUpscaleJitter[1],0,0}};
    const size_t constantBytes = upscale ? sizeof(constants) : 80;
    VkDescriptorBufferInfo uniform{upload.buffer, Upload(&constants, constantBytes,
        std::max<VkDeviceSize>(16, properties.limits.minUniformBufferOffsetAlignment)), constantBytes};
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &resolveDescriptors;
    GX_VK(vkAllocateDescriptorSets(device, &allocate, &set));
    VkDescriptorImageInfo images[5]{};
    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set;
        writes[i].dstBinding = i == 4 ? 5 : i == 5 ? 6 : i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (i == 4) writes[i].pBufferInfo = &uniform;
        else {
            const unsigned imageIndex = i == 5 ? 4 : i;
            images[imageIndex].imageView = inputs[imageIndex]->view;
            images[imageIndex].imageLayout = inputs[imageIndex]->layout;
            writes[i].pImageInfo = &images[imageIndex];
        }
    }
    vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
    VkFramebuffer framebuffer;
    VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create.renderPass = resolvePass; create.attachmentCount = 1; create.pAttachments = &output.view;
    create.width = extent.width; create.height = extent.height; create.layers = 1;
    GX_VK(vkCreateFramebuffer(device, &create, nullptr, &framebuffer)); framebuffers.push_back(framebuffer);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = resolvePass; begin.framebuffer = framebuffer; begin.renderArea.extent = extent;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    inPass = true;
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    const VkRect2D scissor{{0,0}, extent}; vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineHandle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolveLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    EndPass();
    for (auto* image : inputs) {
        Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_GENERAL);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    GxVulkan::CopyWorldImage(command, output.image, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, worldRect);
    if (temporalActive) {
        if (upscale) PublishWorldDepth(previousDepth.image);
        else GxVulkan::CopyWorldImage(command, worldDepth.image, previousDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, worldRect);
        historyPending = true;
    }
}
