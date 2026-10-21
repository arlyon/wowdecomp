// Native-only immutable FAR storage. No asset I/O, conversion, queue submit or
// wait is performed here. Geometry is host-visible immutable vertex/index data;
// image upload staging shares its physical allocation envelope.
namespace {
constexpr uint64_t FarShare = 32ull * 1024 * 1024;
}
FarBuffer CGxDeviceVulkan::State::MakeFarBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool farAllocation) {
    FarBuffer result;
    result.buffer.size = bytes;
    result.farAllocation = farAllocation;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = bytes; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    GX_VK(vkCreateBuffer(device, &create, nullptr, &result.buffer.buffer));
    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer.buffer, &requirements);
        if (!sceneAdmission->Charge(requirements.size, farAllocation)) throw FarBudgetExceeded{};
        result.allocationBytes = requirements.size;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
#ifdef WOWDECOMP_VULKAN_TESTING
        if (farAllocation && farFailAllocation && !--farFailAllocation) throw std::runtime_error("Injected FAR allocation failure");
#endif
        GX_VK(vkAllocateMemory(device, &allocation, nullptr, &result.buffer.memory));
        GX_VK(vkBindBufferMemory(device, result.buffer.buffer, result.buffer.memory, 0));
        GX_VK(vkMapMemory(device, result.buffer.memory, 0, bytes, 0, reinterpret_cast<void**>(&result.buffer.mapped)));
    } catch (...) { DestroyFarBuffer(result); throw; }
    return result;
}
void CGxDeviceVulkan::State::DestroyFarBuffer(FarBuffer& buffer) {
    DestroyBuffer(buffer.buffer);
    if (buffer.allocationBytes) sceneAdmission->Release(buffer.allocationBytes, buffer.farAllocation);
    buffer = {};
}
void CGxDeviceVulkan::State::CollectFar(bool destroy) {
    if(!destroy && productionLease)return;
    for (auto it = farEntries.begin(); it != farEntries.end();) {
        auto& entry = **it;
        if (destroy) entry.lease->status = GxFarResourceStatus::DeviceExpired;
        else if (entry.lease.use_count() == 1) entry.lease->Withdraw();
        if (destroy || entry.lastUse <= completedSerial) {
            DestroyFarBuffer(entry.staging);
            if (entry.lease->Status() != GxFarResourceStatus::Resident) {
                DestroyFarBuffer(entry.vertex); DestroyFarBuffer(entry.index); DestroyImage(entry.image);
            }
        }
        if (destroy || (entry.lease.use_count() == 1 && entry.lastUse <= completedSerial)) {
            temporalDraws.ForgetBuffer(&entry.lease->vertex); temporalDraws.ForgetBuffer(&entry.lease->index);
            temporalBuffers.erase(&entry.lease->vertex); temporalBuffers.erase(&entry.lease->index);
            farBuffers.erase(&entry.lease->vertex); farBuffers.erase(&entry.lease->index);
            farTextures.erase(&entry.lease->texture);
            // Drop stale binding references before destroying their CPU wrappers.
            for (unsigned slot = 0; slot < 16; ++slot) {
                auto& state = gx.m_appRenderStates[GxRs_Texture0+slot];
                if (state.m_value.m_data.p == &entry.lease->texture) state.m_value.m_data.p = nullptr;
            }
            for (uint32_t i = 0; i < gx.m_pushedStates.Count(); ++i) {
                auto& pushed = gx.m_pushedStates[i];
                if (pushed.m_which >= GxRs_Texture0 && pushed.m_which <= GxRs_Texture15 &&
                    pushed.m_value.m_data.p == &entry.lease->texture) pushed.m_value.m_data.p = nullptr;
            }
            if (gx.m_primVertexBuf == &entry.lease->vertex) gx.m_primVertexBuf = nullptr;
            if (gx.m_primIndexBuf == &entry.lease->index) gx.m_primIndexBuf = nullptr;
            for (auto& buffer : gx.m_primVertexFormatBuf)
                if (buffer == &entry.lease->vertex) buffer = nullptr;
            it = farEntries.erase(it);
        } else ++it;
    }
}
void CGxDeviceVulkan::State::WithdrawFar() {
    for (auto& entry : farEntries) entry->lease->Withdraw();
    // Poll is nonblocking. Recorded/unsubmitted readers use submittedSerial+1
    // and can never be reclaimed by an earlier signalled fence.
    PollFarCompletion();
    CollectFar();
}
bool CGxDeviceVulkan::State::FarDrawAllowed(const std::array<CGxBuf*, GxVAs_Last>& buffers,
    const std::vector<int>& required, bool indexed) const {
    const auto valid = [&](CGxBuf* buffer) {
        auto found = farBuffers.find(buffer);
        return found == farBuffers.end() || found->second->lease->Status() == GxFarResourceStatus::Resident;
    };
    if (indexed && !valid(gx.m_primIndexBuf)) return false;
    for (int location : required) if (!valid(buffers[location])) return false;
    return true;
}
bool CGxDeviceVulkan::SceneFarResourcesSupported() const { return m_native && m_native->device; }
GxFarResourceAdmission CGxDeviceVulkan::State::CreateFar(const GxFarResourceRequest& request) {
    const uint64_t vertexBytes = uint64_t(request.vertexStride) * request.vertexCount;
    const uint64_t indexBytes = uint64_t(request.indexCount) * sizeof(uint16_t);
    uint64_t uploadBytes = 0;
    if (request.mips.size() > 32 || vertexBytes > FarShare || indexBytes > FarShare) return {GxFarResourceResult::Invalid, {}};
    bool invalid = !GxFarMipBytes(request.format,1,1) || vertexBytes + indexBytes > FarShare ||
        bool(request.vertices) != bool(vertexBytes) || bool(request.indices) != bool(indexBytes) ||
        (!vertexBytes && (request.vertexStride || request.vertexCount)) ||
        (request.mips.empty() && (request.width || request.height));
    if (!request.mips.empty()) {
        invalid |= !request.width || !request.height;
        uint32_t w = request.width, h = request.height;
        for (size_t level = 0; level < request.mips.size(); ++level) {
            const auto& mip = request.mips[level];
            const uint64_t bytes = GxFarMipBytes(request.format,w,h);
            if (!mip.data || !bytes || bytes > FarShare || mip.bytes != bytes || uploadBytes > FarShare - std::min(bytes, FarShare)) invalid = true;
            else uploadBytes += bytes;
            if (w == 1 && h == 1 && level + 1 != request.mips.size()) invalid = true;
            w = std::max(w >> 1, 1u); h = std::max(h >> 1, 1u);
        }
    }
    if (!vertexBytes && !indexBytes && !uploadBytes) invalid = true;
    if (uploadBytes > FarShare - std::min(vertexBytes + indexBytes, FarShare)) invalid = true;
    if (invalid) return {GxFarResourceResult::Invalid, {}};
    if (!device) return {GxFarResourceResult::Unavailable, {}};
    if (request.width > properties.limits.maxImageDimension2D || request.height > properties.limits.maxImageDimension2D)
        return {GxFarResourceResult::Invalid, {}};
    if (vertexBytes && indexBytes)
        for (uint32_t i = 0; i < request.indexCount; ++i)
            if (request.indices[i] >= request.vertexCount) return {GxFarResourceResult::Invalid, {}};
    if(uploadBytes) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical,TextureFormat(FarTextureFormat(request.format)),&properties);
        const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            (request.linearFilter?VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT:0);
        if((properties.optimalTilingFeatures&needed)!=needed) return {GxFarResourceResult::Unsupported,{}};
    }
    // Do not start/acquire a frame for invalid requests. The normal frame owner
    // must already have begun recording (e.g. SceneClear/SceneBeginWorld/Draw).
    if (!recording) return {GxFarResourceResult::Unavailable, {}};
    CollectFar();
    // Per-entry reservation covers the entry, wrapper, list node, up to three
    // red-black registry nodes and shared control blocks. Retained by the lease
    // even after withdrawal/device expiry, until its final external reader dies.
    constexpr size_t metadataBytes = sizeof(FarEntry) + sizeof(NativeFarLease) +
        3*(sizeof(std::pair<void* const, void*>) + 4*sizeof(void*)) + 8*sizeof(void*);
    std::shared_ptr<const void> metadata;
    try { metadata = GxFar::ReserveFarCpuBytes(metadataBytes, GxFar::FarCpuClass::ResourceMetadata); }
    catch (...) { return {GxFarResourceResult::AllocationFailure, {}}; }
    if (!metadata) return {GxFarResourceResult::Budget, {}};
    std::unique_ptr<FarEntry> entry;
    try {
        entry = std::make_unique<FarEntry>();
        entry->lease = std::make_shared<NativeFarLease>(request);
        entry->lease->metadata = std::move(metadata);
        if (vertexBytes) {
            entry->vertex = MakeFarBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            std::memcpy(entry->vertex.buffer.mapped, request.vertices, vertexBytes);
        }
        if (indexBytes) {
            entry->index = MakeFarBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            std::memcpy(entry->index.buffer.mapped, request.indices, indexBytes);
        }
        if (uploadBytes) {
            entry->staging = MakeFarBuffer(uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            entry->image = MakeImage(request.width, request.height, request.mips.size(), 1,
                TextureFormat(FarTextureFormat(request.format)), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                FarShare, &imageAdmission, true);
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.magFilter = sampler.minFilter = request.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sampler.mipmapMode = request.linearFilter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = request.wrapU ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.addressModeV = request.wrapV ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.maxLod = float(request.mips.size()-1);
            GX_VK(vkCreateSampler(device, &sampler, nullptr, &entry->image.sampler));
            size_t offset = 0;
            for (const auto& mip : request.mips) {
                std::memcpy(entry->staging.buffer.mapped+offset, mip.data, mip.bytes);
                offset += mip.bytes;
            }
        }
        if (vertexBytes) farBuffers.emplace(&entry->lease->vertex, entry.get());
        if (indexBytes) farBuffers.emplace(&entry->lease->index, entry.get());
        if (uploadBytes) farTextures.emplace(&entry->lease->texture, entry.get());
        // Allocate the list node before recording transfer commands.
        farEntries.push_back(nullptr);
    } catch (...) {
        if (entry && entry->lease) {
            farBuffers.erase(&entry->lease->vertex); farBuffers.erase(&entry->lease->index);
            farTextures.erase(&entry->lease->texture);
        }
        if (entry) { DestroyFarBuffer(entry->vertex); DestroyFarBuffer(entry->index); DestroyFarBuffer(entry->staging); DestroyImage(entry->image); }
        try { throw; }
        catch (const FarBudgetExceeded&) { return {GxFarResourceResult::Budget, {}}; }
        catch (...) { return {GxFarResourceResult::AllocationFailure, {}}; }
    }
    auto& admitted = *entry;
    farEntries.back() = std::move(entry); // list node reserved before any commands
    if (uploadBytes) {
        EndPass();
        admitted.lastUse = submittedSerial+1;
        Barrier(admitted.image.image, admitted.image.aspect, admitted.image.levels, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        admitted.image.layout = VK_IMAGE_LAYOUT_GENERAL;
        VkDeviceSize offset = 0;
        uint32_t w = request.width, h = request.height;
        for (uint32_t level = 0; level < request.mips.size(); ++level) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            copy.imageExtent = {w,h,1};
            vkCmdCopyBufferToImage(command, admitted.staging.buffer.buffer, admitted.image.image, admitted.image.layout, 1, &copy);
            offset += request.mips[level].bytes;
            w = std::max(w >> 1, 1u); h = std::max(h >> 1, 1u);
        }
        Barrier(admitted.image.image, admitted.image.aspect, admitted.image.levels, 1, admitted.image.layout, admitted.image.layout);
    }
    return {GxFarResourceResult::Ready, admitted.lease};
}
GxFarResourceAdmission CGxDeviceVulkan::SceneCreateFarResources(const GxFarResourceRequest& request) { return m_native->CreateFar(request); }
GxFarResourceUsage CGxDeviceVulkan::SceneFarResourceUsage() const {
    return {m_native->sceneAdmission->Used(), m_native->sceneAdmission->Far(),
        m_native->imageAdmission.Used(), m_native->imageAdmission.Far()};
}
void CGxDeviceVulkan::SceneWithdrawFarResources() { m_native->WithdrawFar(); }
#ifdef WOWDECOMP_VULKAN_TESTING
void CGxDeviceVulkan::SceneFarTestAdmissionLimits(uint64_t scene,uint64_t images,uint64_t far) {
    if(m_native->device) throw std::logic_error("FAR test limits must precede device creation");
    m_native->sceneAdmission->SetEmptyLimitsForTesting(scene,far);
    m_native->imageAdmission.SetEmptyLimitsForTesting(images,far);
}
void CGxDeviceVulkan::SceneFarTestFailAllocation(uint32_t ordinal) { m_native->farFailAllocation = ordinal; }
bool CGxDeviceVulkan::SceneFarTestNearCharge(bool images, uint64_t bytes) {
    auto& s = *m_native;
    if (!s.device || !bytes || bytes > (images ? 192ull : 256ull)*1024*1024) return false;
    try {
        if (images) {
            s.farTestNearImages.reserve(s.farTestNearImages.size()+1);
            s.farTestNearImages.push_back(s.MakeImage(4096, (bytes+16383)/16384, 1, 1,
                VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
                192ull*1024*1024, &s.imageAdmission));
        } else {
            s.farTestNearBuffers.reserve(s.farTestNearBuffers.size()+1);
            s.farTestNearBuffers.push_back(s.MakeFarBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false));
        }
        return true;
    } catch (const FarBudgetExceeded&) { return false; }
}
void CGxDeviceVulkan::SceneFarTestReleaseNear() {
    auto& s = *m_native;
    for (auto& buffer : s.farTestNearBuffers) s.DestroyFarBuffer(buffer);
    s.farTestNearBuffers.clear();
    for (auto& image : s.farTestNearImages) s.DestroyImage(image);
    s.farTestNearImages.clear();
}
void CGxDeviceVulkan::SceneFarTestHoldGpu() {
    auto& s = *m_native;
    if (!s.recording || s.farTestGate) throw std::logic_error("FAR GPU hold requires one recording frame");
    if (!s.farTestTimeline) throw std::runtime_error("FAR lifetime fixture requires Vulkan 1.2 timeline semaphore support");
    s.EndPass();
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue = 0;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; semaphore.pNext = &type;
    GX_VK(vkCreateSemaphore(s.device, &semaphore, nullptr, &s.farTestGate));
    // Timeline wait-before-host-signal is defined; host-signalled pending event
    // waits are not. This optional fixture feature never enters production.
    s.Submit(false, false);
}
void CGxDeviceVulkan::SceneFarTestReleaseGpu() {
    auto& s = *m_native;
    if (!s.farTestGate || s.recording) throw std::logic_error("FAR GPU hold is not pending");
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = s.farTestGate; signal.value = 1;
    GX_VK(vkSignalSemaphore(s.device, &signal));
    GX_VK(vkWaitForFences(s.device, 1, &s.fence, true, UINT64_MAX));
    s.completedSerial = s.submittedSerial;
    vkDestroySemaphore(s.device, s.farTestGate, nullptr); s.farTestGate = VK_NULL_HANDLE;
    GX_VK(vkResetCommandPool(s.device, s.commandPool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GX_VK(vkBeginCommandBuffer(s.command, &begin));
    s.recording = true;
    s.CollectFar();
}
#endif
