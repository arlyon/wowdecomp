#include "CGxDeviceVulkan.h"
#include "DeviceEnumeration.h"
#include "PresentMode.h"
#include "Shader.h"
#include "Slang.h"
#include "ShaderOverride.h"
#include "ResidentBuffer.h"
#include "RecordingResources.h"
#include "WorldSubmission.h"
#include <mutex>
#include <set>
#include "WorldTarget.h"
#include "TemporalHistory.h"
#include "TemporalBackend.h"
#include "HdrLutJob.h"
#include "GpuAdmission.h"
#include "RayQueryCapabilities.h"
#include "AccelerationScene.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/Buffer.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/SDL/Window.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <functional>
#include <chrono>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WOWDECOMP_VULKAN_OVERRIDE_DIR
#define WOWDECOMP_VULKAN_OVERRIDE_DIR "Shaders/Overrides"
#endif

namespace {
void VkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
#define GX_VK(call) VkCheck((call), #call)

std::vector<uint32_t> ReadModule(const char* filename) {
    std::ifstream stream(std::string(WOWDECOMP_VULKAN_SHADER_DIR) + "/" + filename, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (!stream || size < 20 || size % 4 || size > 4 * 1024 * 1024)
        throw std::runtime_error(std::string("Invalid Slang shader module: ") + filename);
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), size);
    if (!stream || code[0] != 0x07230203) throw std::runtime_error("Invalid SPIR-V shader header");
    return code;
}

VkFormat TextureFormat(EGxTexFormat format) {
    static constexpr VkFormat formats[] = {
        VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_UNDEFINED, VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_R8G8_SNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_D32_SFLOAT
    };
    return format > GxTex_Unknown && format < GxTexFormats_Last ? formats[format] : VK_FORMAT_UNDEFINED;
}

using NativeBuffer = GxVulkan::Buffer;
struct CachedGeometry {
    CGxPool* pool = nullptr;
    uint32_t index = 0, bytes = 0;
    GxVulkan::ResidentBuffer resident;
    VkBuffer binding=VK_NULL_HANDLE;
};
class FarBudgetExceeded final : public std::exception {};
struct NativeImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceSize allocationBytes = 0;
    GxVulkan::GpuAdmission* admission = nullptr;
    bool farAllocation = false;
};
EGxTexFormat FarTextureFormat(GxFarTextureFormat format) {
    switch(format) {
        case GxFarTextureFormat::Rgba8:return GxTex_Abgr8888;
        case GxFarTextureFormat::Bc1:return GxTex_Dxt1;
        case GxFarTextureFormat::Bc2:return GxTex_Dxt3;
        case GxFarTextureFormat::Bc3:return GxTex_Dxt5;
    }
    throw std::invalid_argument("Invalid FAR texture encoding");
}
struct NativeFarLease final : GxFarResourceLease {
    explicit NativeFarLease(const GxFarResourceRequest& r)
        : vertexPool(GxPoolTarget_Vertex, GxPoolUsage_Static, 0, GxPoolHintBit_Unk3, "FAR vertices"),
          indexPool(GxPoolTarget_Index, GxPoolUsage_Static, 0, GxPoolHintBit_Unk3, "FAR indices"),
          vertex(&vertexPool, r.vertexStride, r.vertexCount, 0), index(&indexPool, 2, r.indexCount, 0),
          texture(GxTex_2d, r.width, r.height, 1, FarTextureFormat(r.format), FarTextureFormat(r.format),
              CGxTexFlags(r.linearFilter ? GxTex_LinearMipLinear : GxTex_Nearest, r.wrapU, r.wrapV), nullptr, nullptr, "FAR authored material"),
          hasTexture(!r.mips.empty()) {}
    std::shared_ptr<const void> metadata;
    GxFarResourceStatus status = GxFarResourceStatus::Resident;
    CGxPool vertexPool, indexPool;
    CGxBuf vertex, index;
    CGxTex texture;
    bool hasTexture;
    GxFarResourceStatus Status() const override { return status; }
    CGxBuf* VertexBuffer() const override { return status == GxFarResourceStatus::Resident && vertex.m_size ? const_cast<CGxBuf*>(&vertex) : nullptr; }
    CGxBuf* IndexBuffer() const override { return status == GxFarResourceStatus::Resident && index.m_size ? const_cast<CGxBuf*>(&index) : nullptr; }
    CGxTex* Texture() const override { return status == GxFarResourceStatus::Resident && hasTexture ? const_cast<CGxTex*>(&texture) : nullptr; }
    void Withdraw() override { if (status == GxFarResourceStatus::Resident) status = GxFarResourceStatus::Withdrawn; }
};
struct FarBuffer { NativeBuffer buffer; VkDeviceSize allocationBytes = 0; bool farAllocation = true; };
struct FarEntry {
    std::shared_ptr<NativeFarLease> lease;
    FarBuffer vertex, index, staging;
    NativeImage image;
    uint64_t lastUse = 0;
};
struct NativeShader {
    uint64_t id = 0;
    GxVulkan::ShaderBindings bindings;
    std::vector<uint32_t> code, temporalCode;
    std::vector<uint32_t> foglessCode[2];
    bool inputFog = false, outputFog = false;
    std::string source, temporalSource;
    std::shared_ptr<GxVulkan::ShaderOverride> maintained;
    std::vector<uint32_t> hdrCode[2][2];
    bool directionalAttempted=false;
    std::string directionalError;
    std::shared_ptr<GxVulkan::ShaderOverride> directionalSource;
    GxVulkan::CompiledShader directional;
};
struct Pipeline {
    VkPipeline handle = VK_NULL_HANDLE;
};
struct ShadowGpuFrame {
    VkDevice device=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkDescriptorSet querySet=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule module=VK_NULL_HANDLE;
    NativeImage output;
    std::function<void(NativeImage&)> releaseImage;
    std::vector<VkSampler> samplers;
    GxVulkan::Buffer uniforms,triangles;
    std::unique_ptr<GxVulkan::AccelerationScene> scene;
    std::shared_ptr<const GxShadow::Frame> source;
    ~ShadowGpuFrame(){
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(module)vkDestroyShaderModule(device,module,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(pool)vkDestroyDescriptorPool(device,pool,nullptr);
        if(descriptors)vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
        for(auto sampler:samplers)vkDestroySampler(device,sampler,nullptr);
        if(releaseImage && output.image)releaseImage(output);
        GxVulkan::DestroyBuffer(device,uniforms);GxVulkan::DestroyBuffer(device,triangles);
        scene.reset();
    }
};

void WindowEvent(void* context, const SDL_WindowEvent& event) {
    auto& device = *static_cast<CGxDeviceVulkan*>(context);
    switch (event.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        case SDL_WINDOWEVENT_RESTORED:
            device.DeviceWM(GxWM_Size, 0, 0);
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: device.DeviceWM(GxWM_SetFocus, 0, 0); break;
        case SDL_WINDOWEVENT_FOCUS_LOST: device.DeviceWM(GxWM_KillFocus, 0, 0); break;
        case SDL_WINDOWEVENT_MINIMIZED: device.DeviceWM(GxWM_Size, 0, 1); break;
        default: break;
    }
}
}

// The synchronous lane retains its existing field access while its transient
// lifetime is isolated from device-wide assets. Additional lanes borrow this
// same VkDevice; they must not share this RecordingResources base subobject.
struct CGxDeviceVulkan::State : GxVulkan::RecordingResources {
    explicit State(CGxDeviceVulkan& device) : gx(device) {}
    CGxDeviceVulkan& gx;
    SDL_Window* window = nullptr;
    bool ownWindow = false, resize = false, recording = false, inPass = false, acquirePending = false;
    VkInstance instance = VK_NULL_HANDLE;
#ifdef WOWDECOMP_VULKAN_TESTING
    VkDebugUtilsMessengerEXT testMessenger = VK_NULL_HANDLE;
    std::atomic<uint32_t> testDiagnostics{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL TestMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
            static_cast<State*>(user)->testDiagnostics.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "Vulkan validation: %s\n", data && data->pMessage ? data->pMessage : "missing diagnostic");
        }
        return VK_FALSE;
    }
#endif
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    GxVulkan::RayQueryCapabilities rayQueryCapabilities;
    bool shadowRequested=false,shadowStrict=true;
    bool terrainShadowRequested=false,terrainShadowActive=false,terrainReceiver=false,terrainShadowFailed=false;
    std::string terrainLoggedState;
    std::map<std::string,uint64_t> loggedCasterIssues;
    std::map<std::string,uint64_t> loggedReceiverIssues;
    VkQueryPool shadowTimingPool=VK_NULL_HANDLE;
    uint32_t shadowTimestampBits=0,shadowTimingMask=0;
    double shadowCollectMs=0,shadowPackMs=0,shadowBuildRecordMs=0;
    std::chrono::steady_clock::time_point shadowTimingLog{};
#ifdef WOWDECOMP_VULKAN_TESTING
    bool shadowTestModels=true,shadowTestBuildings=true;
#endif
    std::shared_ptr<const GxShadow::Frame> shadowSource;
    GxShadow::Status shadowStatus;
    std::vector<std::shared_ptr<ShadowGpuFrame>> shadowRetired;
    std::shared_ptr<ShadowGpuFrame> shadowCurrent;
    void RecordWorldShadowVisibility(bool inlineOnly=false);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    uint32_t imageIndex = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    // Presentation completion is not covered by the submit fence. Reuse each
    // render-finished semaphore only after acquiring its swapchain image.
    std::vector<VkSemaphore> complete;
    std::array<VkDescriptorSetLayout, 5> descriptorLayouts{};
    uint32_t descriptorSetCount=4,shadowTextureCapacity=0;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    NativeImage depth, white, worldColour, worldDepth;
    struct CompletedImages { NativeImage colour, depth; GxShadow::Status lighting; };
    bool publishedHdr=false,publishedTemporal=false;
    std::optional<unsigned> displayedWorldSlot;
    GxWorld::CompletedWorld completedWorld;
    std::string completedWorldAllocationError;
    std::string schedulingStatus="Off: synchronous rendering";
    std::array<CompletedImages,GxWorld::CompletedWorld::SlotCount> completedImages;
    bool independentWorld=false,productionCapture=false,productionPrepared=false,savedAcquirePending=false;
    uint32_t presentationImageIndex=0;
    uint64_t worldSubmitted=0,worldCompleted=0,presentationVersion=1;
    std::optional<GxWorld::WorldFrameLease> productionLease;
    GxVulkan::RecordingResources producerRecording;
    std::unique_ptr<GxVulkan::WorldSubmission> worldSubmitter;
    std::mutex queueMutex;
    std::vector<VkImage> presentationImages;
    std::vector<VkImageView> presentationViews;
    std::set<VkImage> producerImages;
    std::set<VkBuffer> producerBuffers;
    std::set<VkSampler> producerSamplers;
    void CollectRetired();
    VkQueryPool producerTimingPool=VK_NULL_HANDLE,uiTimingPool=VK_NULL_HANDLE;
    bool uiTimingRecording=false,uiTimingPending=false;
    double worldGpuMs=-1,uiGpuMs=-1,displayedAgeMs=0;
    uint64_t uiPresents=0,lastUiPresents=0,lastWorldCompleted=0,displayedWorldGeneration=0;
    std::chrono::steady_clock::time_point cadenceLog{};
    std::array<std::chrono::steady_clock::time_point,GxWorld::CompletedWorld::SlotCount> worldBorn;
    void BeginUiTiming();
    void ReadUiTiming();
    void PollWorld();
    void RestorePresentation();
    void CommitHistory();
    void DetachTexture(CGxTex* texture);
    bool worldTargetEnabled = false, worldActive = false;
    bool hdrRequested = false, hdrCapable = false, hdrActive = false, linearOutput = false;
    float hdrEV = 0.f;
    NativeImage hdrBackdrop, hdrClassic;
    bool hdrAborted = false;
#ifdef WOWDECOMP_VULKAN_TESTING
    std::array<uint32_t,4> completedWorldTest{};
#endif
    uint32_t hdrAttachmentLimit = UINT32_MAX, hdrBoundaryFailDraw = 0, hdrDraws = 0;
    VkDeviceSize hdrImageBudget = 512ull * 1024 * 1024;
    void AbortHdr(const char* reason) {
        EndPass(); hdrAborted = true; temporalActive = false; ResetTemporal();
        GxLog("HDR frame rolled back to Classic companion: %s", reason);
    }
    void PublishClassic() {
        if (sceneScale == 1.f) GxVulkan::CopyWorldImage(command, hdrClassic.image, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, worldRect);
        else GxVulkan::BlitWorldImage(command, hdrClassic.image, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, renderRect, worldRect, VK_FILTER_LINEAR);
    }
#ifdef WOWDECOMP_VULKAN_TESTING
    NativeBuffer uiProbe;
    uint64_t uiProbeSerial=0;
#endif
    NativeBuffer hdrLutBuffer;
    std::vector<NativeBuffer> retiredHdrLuts;
    std::vector<std::array<float, 3>> hdrLutData;
    uint32_t hdrLutSize = 0;
    std::unique_ptr<GxVulkan::HdrLutJob> hdrLutJob;
    std::string hdrLutRequested, hdrLutJobPath, hdrLutStatus = "None";
    bool hdrLutNeedsLoad = false;
    uint64_t hdrLutGeneration = 0, hdrLutJobGeneration = 0;
    uint64_t HdrLutBytes() const;
    bool SetHdrLut(const float* rgb, uint32_t size);
    bool InstallHdrLut(GxVulkan::HdrLut&& lut);
    void UploadHdrLut(const std::vector<std::array<float, 3>>& data);
#ifdef WOWDECOMP_VULKAN_TESTING
    uint64_t hdrLutPeakBytes = 0, hdrLutUploadBytes = 0;
    bool hdrLutOwnerMoved = false;
#endif
    VkDescriptorSetLayout hdrDescriptors{};
    VkPipelineLayout hdrLayout{};
    VkSampler hdrSampler{};
    std::map<VkFormat, std::pair<VkRenderPass, VkPipeline>> hdrTransfers;
    std::map<std::vector<uint32_t>, std::vector<uint32_t>> hdrPixels, hdrLinearPixels;
    VkRenderPass hdrResolvePass{};
    VkPipeline hdrResolvePipeline{}, hdrUpscalePipeline{};
    VkFormat WorldFormat() const { return hdrActive ? VK_FORMAT_R32G32B32A32_SFLOAT : surfaceFormat; }
    bool HdrDraw() const {
        return hdrActive && worldActive && !hdrAborted &&
            !gx.m_textureTarget[GxBuffers_Color].m_texture && !gx.m_textureTarget[GxBuffers_Depth].m_texture;
    }
    std::vector<uint32_t> Compile(const std::string& source, bool vertex,
        const std::shared_ptr<GxVulkan::ShaderOverride>& maintained = {}, GxVulkan::ShaderBindings* bindings = nullptr) {
        if (!maintained) return shaderCompiler.Compile(source, vertex);
        auto selected = *maintained;
        selected.source = source;
        auto compiled = shaderCompiler.CompileMaintained(selected);
        if (bindings) *bindings = std::move(compiled.bindings);
        return std::move(compiled.code);
    }
    std::vector<uint32_t> CompileTemporal(const std::string& source, bool vertex, const NativeShader& shader) {
        GxVulkan::ShaderBindings layout;
        auto code = Compile(source, vertex, shader.maintained, &layout);
        if (shader.maintained && (!layout.motionParameters ||
            (vertex && !shader.bindings.registers.empty() && !layout.previousRegisters)))
            throw std::runtime_error("Maintained temporal source ABI did not expose previous registers/motion parameters");
        return code;
    }
    const std::vector<uint32_t>& BoundaryPixel(const std::vector<uint32_t>& code, bool decode, NativeShader* shader, bool temporal);
    bool PrepareHdrImages(bool temporal);
    void HdrTransfer(NativeImage& source, VkImageView target, VkFormat format, VkExtent2D output, uint32_t operation, bool sceneRect);
    void ToneWorld(NativeImage& source) { HdrTransfer(source, swapViews[imageIndex], surfaceFormat, extent, 1, true); }
    void DestroyHdr();
    VkRect2D worldRect{}, renderRect{};
    VkExtent2D renderExtent{};
    float sceneScale = 1.f;
    bool scaleCapable = false;
    bool temporalFormatsSupported = false, temporalCapable = false, temporalActive = false, motionPaused = false, historyPending = false;
    const void* motionOwner = nullptr;
    bool stableVertices = false;
    GxVulkan::TemporalHistory temporalHistory;
    GxVulkan::TemporalFrameState temporalFrame;
    GxVulkan::TemporalDraws temporalDraws;
    std::map<CGxBuf*, std::array<uint64_t,3>> temporalBuffers;
    NativeImage motion, history[2], previousDepth, depthScratch;
    unsigned historyIndex = 0;
    uint32_t upscaleSample = 0;
    std::array<float,2> previousUpscaleJitter{};
    uint64_t temporalWorldFrame = 0;
    VkDescriptorSetLayout resolveDescriptors = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
    VkRenderPass resolvePass = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE, upscalePipeline = VK_NULL_HANDLE;
    bool MotionPass() const {
        return temporalActive && worldActive && !hdrAborted && !motionPaused &&
            !gx.m_textureTarget[GxBuffers_Color].m_texture && !gx.m_textureTarget[GxBuffers_Depth].m_texture;
    }
    void ResetTemporal() {
        temporalHistory.Cancel(); temporalDraws.Reset(); historyPending = false; upscaleSample = 0; previousUpscaleJitter = {};
        temporalFrame.historyValid = false;
    }
    void ResolveWorld();
    void PublishWorldDepth(VkImage destination) {
        GxVulkan::BlitWorldImage(command, worldDepth.image, depthScratch.image,
            VK_IMAGE_ASPECT_DEPTH_BIT, {{0,0},renderExtent}, {{0,0},extent});
        GxVulkan::CopyWorldImage(command, depthScratch.image, destination, VK_IMAGE_ASPECT_DEPTH_BIT, worldRect);
    }
    void SnapshotWorldColour() {
        if (hdrAborted) { PublishClassic(); return; }
        if (hdrActive) { ToneWorld(worldColour); return; }
        GxVulkan::BlitWorldImage(command, worldColour.image, history[historyIndex ^ 1].image,
            VK_IMAGE_ASPECT_COLOR_BIT, {{0,0},renderExtent}, {{0,0},extent}, VK_FILTER_LINEAR);
        GxVulkan::CopyWorldImage(command, history[historyIndex ^ 1].image, swapImages[imageIndex],
            VK_IMAGE_ASPECT_COLOR_BIT, worldRect);
    }
    std::map<CGxPool*, std::vector<uint8_t>> pools;
    std::map<CGxBuf*, CachedGeometry> geometry;
    std::vector<NativeBuffer> retiredBuffers;
    uint64_t bufferFrame = 0, residentUploadBytes = 0;
    std::map<CGxTex*, NativeImage> textures;
    std::vector<NativeImage> retiredImages;
    std::map<CGxShader*, NativeShader> shaders;
    uint64_t nextShader = 1;
    GxVulkan::SlangCompiler shaderCompiler;
    std::vector<uint32_t> fixedVertex, fixedPixel, temporalFixedVertex, temporalFixedPixel;
    std::vector<uint32_t> fixedFoglessVertex[2];
    bool msdfFontActive = false;
    uint32_t msdfOutlinePixels = 0;
    std::vector<uint32_t> msdfFontVertex, msdfFontPixel, temporalMsdfFontPixel;
    std::map<std::vector<uint64_t>, Pipeline> pipelines;
    std::map<std::array<uint32_t, 3>, VkRenderPass> renderPasses;
    VkRenderPass currentPass = VK_NULL_HANDLE;
    VkExtent2D targetExtent{};
    uint64_t drawCount = 0;
    uint64_t submittedSerial = 0, completedSerial = 0;
    std::shared_ptr<GxVulkan::GpuAdmission> sceneAdmission = std::make_shared<GxVulkan::GpuAdmission>(UINT64_MAX, UINT64_MAX);
    GxVulkan::GpuAdmission imageAdmission{UINT64_MAX, UINT64_MAX};
    std::list<std::unique_ptr<FarEntry>> farEntries;
    std::map<CGxBuf*, FarEntry*> farBuffers;
    std::map<CGxTex*, FarEntry*> farTextures;
    FarBuffer MakeFarBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool farAllocation = true);
    void DestroyFarBuffer(FarBuffer& buffer);
    void CollectFar(bool destroy = false);
    void WithdrawFar();
    void PollFarCompletion();
    bool FarDrawAllowed(const std::array<CGxBuf*, GxVAs_Last>& buffers, const std::vector<int>& required, bool indexed) const;
    GxFarResourceAdmission CreateFar(const GxFarResourceRequest& request);
#ifdef WOWDECOMP_VULKAN_TESTING
    uint32_t farFailAllocation = 0;
    std::vector<FarBuffer> farTestNearBuffers;
    std::vector<NativeImage> farTestNearImages;
    VkSemaphore farTestGate = VK_NULL_HANDLE;
    bool farTestTimeline = false;
#endif

    uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const;
    NativeBuffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(NativeBuffer& buffer);
    NativeImage MakeImage(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
        VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage,
        VkDeviceSize maximumBytes = std::numeric_limits<VkDeviceSize>::max(),
        GxVulkan::GpuAdmission* admission = nullptr, bool farAllocation = false);
    void DestroyImage(NativeImage& image);
    void Barrier(VkImage image, VkImageAspectFlags aspect, uint32_t levels, uint32_t layers,
        VkImageLayout oldLayout, VkImageLayout newLayout);
    VkDeviceSize Upload(const void* bytes, size_t size, size_t alignment = 16);
    std::pair<VkBuffer, VkDeviceSize> Geometry(CGxBuf* buffer);
    void RetireGeometry(CGxBuf* buffer);
    void Initialize();
    void Destroy();
    void RecreateSwapchain();
    bool BeginFrame();
    void Submit(bool present, bool resume);
    void EndPass();
    void BeginPass();
    NativeImage& Texture(CGxTex* texture);
    void UploadTexture(CGxTex* texture, NativeImage& image);
    void Draw(CGxBatch& batch, bool indexed);
};

#include "IndependentWorld.inl"

uint32_t CGxDeviceVulkan::State::MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("Vulkan device has no suitable memory type");
}
NativeBuffer CGxDeviceVulkan::State::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    return GxVulkan::CreateBuffer(device, memoryProperties, size, usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}
void CGxDeviceVulkan::State::DestroyBuffer(NativeBuffer& buffer) {
    GxVulkan::DestroyBuffer(device, buffer);
}
NativeImage CGxDeviceVulkan::State::MakeImage(uint32_t width, uint32_t height, uint32_t levels,
    uint32_t layers, VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage, VkDeviceSize maximumBytes,
    GxVulkan::GpuAdmission* admission, bool farAllocation) {
    NativeImage image;
    image.width = width; image.height = height; image.levels = levels; image.layers = layers;
    image.format = format; image.aspect = aspect; image.usage = usage;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = levels; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    GX_VK(vkCreateImage(device, &info, nullptr, &image.image));
    try {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        if (requirements.size > maximumBytes) {
            if (admission) throw FarBudgetExceeded{};
            throw std::runtime_error("Scene image allocation budget exceeded");
        }
        image.allocationBytes = requirements.size;
        if (admission) {
            if (!admission->Charge(requirements.size, farAllocation)) throw FarBudgetExceeded{};
            image.admission = admission; image.farAllocation = farAllocation;
        }
#ifdef WOWDECOMP_VULKAN_TESTING
        if (admission && farAllocation && farFailAllocation && !--farFailAllocation)
            throw std::runtime_error("Injected FAR image allocation failure");
#endif
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        GX_VK(vkAllocateMemory(device, &allocation, nullptr, &image.memory));
        GX_VK(vkBindImageMemory(device, image.image, image.memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.format = format; view.subresourceRange = {aspect, 0, levels, 0, layers};
        GX_VK(vkCreateImageView(device, &view, nullptr, &image.view));
    } catch (...) { DestroyImage(image); throw; }
    return image;
}
void CGxDeviceVulkan::State::DestroyImage(NativeImage& image) {
    if (image.sampler) vkDestroySampler(device, image.sampler, nullptr);
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    if (image.admission) image.admission->Release(image.allocationBytes, image.farAllocation);
    image = {};
}
void CGxDeviceVulkan::State::Barrier(VkImage image, VkImageAspectFlags aspect,
    uint32_t levels, uint32_t layers, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {aspect, 0, levels, 0, layers};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}
VkDeviceSize CGxDeviceVulkan::State::Upload(const void* bytes, size_t size, size_t alignment) {
    const VkDeviceSize offset = (uploadOffset + alignment - 1) / alignment * alignment;
    if (size > upload.size || offset > upload.size - size) {
        GxLog("Native Vulkan upload budget exhausted: requested=%llu used=%llu capacity=%llu resident_upload=%llu",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(uploadOffset),
            static_cast<unsigned long long>(upload.size), static_cast<unsigned long long>(residentUploadBytes));
        throw std::runtime_error("Vulkan per-frame upload budget exhausted");
    }
    if (size) std::memcpy(upload.mapped + offset, bytes, size);
    uploadOffset = offset + size;
    return offset;
}

void CGxDeviceVulkan::State::Initialize() {
    sceneAdmission->withdrawFar = [this] { WithdrawFar(); };
    imageAdmission.withdrawFar = [this] { WithdrawFar(); };
    // Engineering switch for exercising the world target without temporal AA.
    worldTargetEnabled = false;
    if (const char* target = std::getenv("WOWDECOMP_VULKAN_WORLD_TARGET")) {
        if (std::strcmp(target, "0") && std::strcmp(target, "1"))
            throw std::invalid_argument("WOWDECOMP_VULKAN_WORLD_TARGET must be 0 or 1");
        worldTargetEnabled = std::strcmp(target, "1") == 0;
    }
    unsigned extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) throw std::runtime_error(SDL_GetError());
    std::vector<const char*> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) throw std::runtime_error(SDL_GetError());
    uint32_t count = 0;
    GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> available(count);
    GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()));
    const bool portability = std::any_of(available.begin(), available.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
    });
    if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#ifdef WOWDECOMP_VULKAN_TESTING
    if (std::none_of(available.begin(), available.end(), [](const auto& extension) {
        return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    })) throw std::runtime_error("Vulkan test diagnostic callback unavailable");
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    // Count API validation/performance diagnostics, not loader configuration notices.
    // The external runner separately audits the complete loader/stderr log.
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = TestMessage; debug.pUserData = this;
#endif
    std::vector<const char*> layers;
    if (const char* validation = std::getenv("WOWDECOMP_VULKAN_VALIDATION"); validation && std::strcmp(validation, "1") == 0) {
        GX_VK(vkEnumerateInstanceLayerProperties(&count, nullptr));
        std::vector<VkLayerProperties> availableLayers(count);
        GX_VK(vkEnumerateInstanceLayerProperties(&count, availableLayers.data()));
        if (std::none_of(availableLayers.begin(), availableLayers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        })) throw std::runtime_error("Requested Vulkan validation layer is unavailable");
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "wowdecomp";
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerateVersion) GX_VK(enumerateVersion(&loaderVersion));
    app.apiVersion = loaderVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app; info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data(); info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (portability) info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#ifdef WOWDECOMP_VULKAN_TESTING
    info.pNext = &debug;
#endif
    GX_VK(vkCreateInstance(&info, nullptr, &instance));
#ifdef WOWDECOMP_VULKAN_TESTING
    const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!createMessenger) throw std::runtime_error("Vulkan test diagnostic callback entry unavailable");
    GX_VK(createMessenger(instance, &debug, nullptr, &testMessenger));
#endif
    if (!SDL_Vulkan_CreateSurface(window, instance, &surface)) throw std::runtime_error(SDL_GetError());
    if (GxGetDeviceSelection().backends.empty()) GxConfigureDeviceSelection("auto", "auto");
    auto candidates = GxVulkan::EnumerateAdapters(instance, surface);
    std::vector<GxAdapterDescriptor> adapterDescriptors;
    for (const auto& candidate : candidates) adapterDescriptors.push_back(candidate.descriptor);
    const size_t selected = GxChooseAdapter(adapterDescriptors, GxGetDeviceSelection().effectiveAdapter);
    if (selected == candidates.size()) throw std::runtime_error("No compatible Vulkan graphics/present device; try removing loader filters");
    physical = candidates[selected].physical;
    queueFamily = candidates[selected].queueFamily;
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    GX_VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    available.resize(count);
    GX_VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()));
    extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (const auto& extension : available)
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
            extensions.push_back("VK_KHR_portability_subset");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = features.textureCompressionBC;
    enabled.largePoints = features.largePoints;
    enabled.independentBlend = features.independentBlend;
    temporalCapable = features.independentBlend;
    for (auto format : {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_D32_SFLOAT}) {
        VkFormatProperties support{};
        vkGetPhysicalDeviceFormatProperties(physical, format, &support);
        const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (format == VK_FORMAT_D32_SFLOAT ?
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        temporalCapable = temporalCapable && (support.optimalTilingFeatures & required) == required;
    }
    temporalFormatsSupported = temporalCapable;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queueInfo; create.pEnabledFeatures = &enabled;
    using RayRequirement = GxVulkan::RayQueryRequirement;
    rayQueryCapabilities = {};
    const bool core12 = app.apiVersion >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
    rayQueryCapabilities.Set(RayRequirement::Core12, core12);
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
    shadowTimestampBits=queueFamily<queueCount?queues[queueFamily].timestampValidBits:0;
    rayQueryCapabilities.Set(RayRequirement::ComputeQueue,
        queueFamily < queueCount && (queues[queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT));
    const auto hasExtension = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    };
    const bool accelerationExtension = hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool queryExtension = hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool deferredExtension = hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    rayQueryCapabilities.Set(RayRequirement::AccelerationExtension, accelerationExtension);
    rayQueryCapabilities.Set(RayRequirement::QueryExtension, queryExtension);
    rayQueryCapabilities.Set(RayRequirement::DeferredHostExtension, deferredExtension);
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAcceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR supportedQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceShaderDrawParametersFeatures supportedDraw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    if (core12) {
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        supported.pNext = &supported12;
        if (accelerationExtension && queryExtension && deferredExtension) {
            supported12.pNext = &supportedAcceleration; supportedAcceleration.pNext = &supportedQuery;
            supportedQuery.pNext = &supportedDraw;
        }
        vkGetPhysicalDeviceFeatures2(physical, &supported);
    }
    rayQueryCapabilities.Set(RayRequirement::BufferAddress, supported12.bufferDeviceAddress);
    rayQueryCapabilities.Set(RayRequirement::AccelerationFeature, supportedAcceleration.accelerationStructure);
    rayQueryCapabilities.Set(RayRequirement::QueryFeature, supportedQuery.rayQuery);
    rayQueryCapabilities.Set(RayRequirement::RuntimeTextureArray, supported12.runtimeDescriptorArray);
    rayQueryCapabilities.Set(RayRequirement::NonUniformTextureIndex, supported12.shaderSampledImageArrayNonUniformIndexing);
    rayQueryCapabilities.Set(RayRequirement::VariableTextureCount, supported12.descriptorBindingVariableDescriptorCount);
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAcceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR enabledQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceShaderDrawParametersFeatures enabledDraw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    if (rayQueryCapabilities.Available()) {
        extensions.insert(extensions.end(), {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});
        enabled12.bufferDeviceAddress = VK_TRUE; enabled12.runtimeDescriptorArray = VK_TRUE;
        enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabled12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        enabledAcceleration.accelerationStructure = VK_TRUE; enabledQuery.rayQuery = VK_TRUE;
        enabled12.pNext = &enabledAcceleration; enabledAcceleration.pNext = &enabledQuery;
        enabledDraw.shaderDrawParameters = supportedDraw.shaderDrawParameters;
        if (enabledDraw.shaderDrawParameters) enabledQuery.pNext = &enabledDraw;
        create.pNext = &enabled12;
    }
#ifdef WOWDECOMP_VULKAN_TESTING
    farTestTimeline = core12 && supported12.timelineSemaphore;
    if (farTestTimeline) { enabled12.timelineSemaphore = VK_TRUE; create.pNext = &enabled12; }
#endif
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size()); create.ppEnabledExtensionNames = extensions.data();
    GX_VK(vkCreateDevice(physical, &create, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    RecordingResources::Initialize(device, queueFamily, memoryProperties);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    GX_VK(vkCreateSemaphore(device, &semaphore, nullptr, &acquired));
    descriptorSetCount=4;shadowTextureCapacity=0;
    if(rayQueryCapabilities.Available() && properties.limits.maxBoundDescriptorSets>=5 &&
        properties.limits.maxPerStageDescriptorSamplers>16 && properties.limits.maxPerStageResources>22 &&
        properties.limits.maxDescriptorSetSamplers>20 && properties.limits.maxDescriptorSetSampledImages>20){
        shadowTextureCapacity=std::min({128u,properties.limits.maxPerStageDescriptorSamplers-16,
            properties.limits.maxPerStageResources-22,properties.limits.maxDescriptorSetSamplers-20,
            properties.limits.maxDescriptorSetSampledImages-20});
        descriptorSetCount=5;
    }
    for (unsigned set = 0; set < descriptorSetCount; ++set) {
        if(set==4){
            const VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                {2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,shadowTextureCapacity,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
            const VkDescriptorBindingFlags flags[]={0,0,0,VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
            VkDescriptorSetLayoutBindingFlagsCreateInfo variable{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};variable.bindingCount=4;variable.pBindingFlags=flags;
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.pNext=&variable;layout.bindingCount=4;layout.pBindings=bindings;
            GX_VK(vkCreateDescriptorSetLayout(device,&layout,nullptr,&descriptorLayouts[set]));continue;
        }
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        const bool uniform = set % 2;
        for (unsigned binding = 0; binding < (uniform ? 3u : set == 0 ? 4u : 16u); ++binding)
            bindings.push_back({binding, uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, set < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<uint32_t>(bindings.size()); layout.pBindings = bindings.data();
        GX_VK(vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptorLayouts[set]));
    }
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = descriptorSetCount; layout.pSetLayouts = descriptorLayouts.data();
    GX_VK(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout));
    fixedVertex = ReadModule("fixed.vert.spv"); fixedPixel = ReadModule("fixed.frag.spv");
    msdfFontVertex = ReadModule("font-msdf.vert.spv");
    msdfFontPixel = ReadModule("font-msdf.frag.spv");
    RecreateSwapchain();
    GxReportDeviceSelection("vulkan", std::move(adapterDescriptors), selected);
    CGxDevice::Log("Native Vulkan: %s, API %u.%u; direct Vulkan submission (no DXVK)", properties.deviceName,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
}

void CGxDeviceVulkan::State::RecreateSwapchain() {
    // SDL caches window geometry in its event pump. Synchronise it before the
    // initial viewport/font measurements as well as subsequent resizes.
    SDL_PumpEvents();
    if(worldSubmitter){worldSubmitter->FlushSubmission();worldSubmitter->Stop();worldSubmitter.reset();}
    GX_VK(vkDeviceWaitIdle(device));
    producerRecording.Destroy();productionLease.reset();productionPrepared=false;producerImages.clear();
    if(producerTimingPool)vkDestroyQueryPool(device,producerTimingPool,nullptr);
    if(uiTimingPool)vkDestroyQueryPool(device,uiTimingPool,nullptr);
    producerTimingPool=uiTimingPool=VK_NULL_HANDLE;uiTimingRecording=uiTimingPending=false;
    ++presentationVersion;
    completedWorld.ResetAfterIdle();displayedWorldSlot.reset();displayedWorldGeneration=0;
    for (auto& images : completedImages) { DestroyImage(images.colour); DestroyImage(images.depth); }
    for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (auto view : swapViews) vkDestroyImageView(device, view, nullptr);
    swapViews.clear(); swapImages.clear();
    for (auto semaphore : complete) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
    complete.clear();
    DestroyHdr();
    ResetTemporal();
    if (upscalePipeline) vkDestroyPipeline(device, upscalePipeline, nullptr);
    upscalePipeline = VK_NULL_HANDLE;
    if (resolvePipeline) vkDestroyPipeline(device, resolvePipeline, nullptr);
    if (resolvePass) vkDestroyRenderPass(device, resolvePass, nullptr);
    resolvePipeline = VK_NULL_HANDLE; resolvePass = VK_NULL_HANDLE;
    DestroyImage(depth);
    VkSurfaceCapabilitiesKHR capabilities;
    GX_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities));
    uint32_t count = 0;
    GX_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    GX_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
    if (formats.empty()) throw std::runtime_error("Vulkan surface has no colour formats");
    auto chosen = formats.front();
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = format;
    if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
    surfaceFormat = chosen.format;
    int width, height;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 1)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 1)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Vulkan swapchain does not support screenshot readback");
    VkFormatProperties colourSupport{};
    vkGetPhysicalDeviceFormatProperties(physical, surfaceFormat, &colourSupport);
    const auto colourRequired = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    temporalCapable = temporalFormatsSupported && (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (surfaceFormat == VK_FORMAT_B8G8R8A8_UNORM || surfaceFormat == VK_FORMAT_R8G8B8A8_UNORM) &&
        (colourSupport.optimalTilingFeatures & colourRequired) == colourRequired;
    VkFormatProperties depthSupport{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &depthSupport);
    const auto depthRequired = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatProperties motionSupport{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32A32_SFLOAT, &motionSupport);
    const auto motionRequired = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    scaleCapable = (motionSupport.optimalTilingFeatures & motionRequired) == motionRequired &&
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (surfaceFormat == VK_FORMAT_B8G8R8A8_UNORM || surfaceFormat == VK_FORMAT_R8G8B8A8_UNORM) &&
        (colourSupport.optimalTilingFeatures & (colourRequired | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
            (colourRequired | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
        (depthSupport.optimalTilingFeatures & depthRequired) == depthRequired;
    if (!scaleCapable) sceneScale = 1.f;
    const auto hdrRequired = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    hdrCapable = scaleCapable && temporalFormatsSupported && properties.limits.maxFragmentOutputAttachments >= 3 &&
        (motionSupport.optimalTilingFeatures & hdrRequired) == hdrRequired;
    if (worldTargetEnabled && !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        GxLog("Vulkan world target disabled: surface does not support transfer destinations");
        worldTargetEnabled = false;
    }
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface; create.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount) create.minImageCount = std::min(create.minImageCount, capabilities.maxImageCount);
    create.imageFormat = chosen.format; create.imageColorSpace = chosen.colorSpace; create.imageExtent = extent;
    create.imageArrayLayers = 1; create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (worldTargetEnabled || temporalCapable || scaleCapable) create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & create.compositeAlpha))
        for (auto flag : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & flag) { create.compositeAlpha = flag; break; }
    uint32_t presentCount = 0;
    GX_VK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &presentCount, nullptr));
    std::vector<VkPresentModeKHR> presentModes(presentCount);
    GX_VK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &presentCount, presentModes.data()));
    create.presentMode = GxVulkan::SelectPresentMode(gx.m_format.m_vsync != 0, presentModes.data(), presentCount);
    create.clipped = true; create.oldSwapchain = swapchain;
    VkSwapchainKHR next;
    GX_VK(vkCreateSwapchainKHR(device, &create, nullptr, &next));
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = next;
    GX_VK(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
    swapImages.resize(count); swapViews.resize(count);
    complete.resize(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& finished : complete) GX_VK(vkCreateSemaphore(device, &semaphore, nullptr, &finished));
    GX_VK(vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data()));
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapImages[i]; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = surfaceFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        GX_VK(vkCreateImageView(device, &view, nullptr, &swapViews[i]));
    }
    depth = MakeImage(extent.width, extent.height, 1, 1, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CRect rect;
    rect.minX = rect.minY = 0; rect.maxX = static_cast<float>(extent.width); rect.maxY = static_cast<float>(extent.height);
    gx.DeviceSetDefWindow(rect); gx.DeviceSetCurWindow(rect);
    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    gx.m_format.m_size = {windowWidth, windowHeight};
    std::fprintf(stderr, "Native display: driver=%s window=%dx%d drawable=%dx%d swapchain=%ux%u\n",
        SDL_GetCurrentVideoDriver(), windowWidth, windowHeight, width, height, extent.width, extent.height);
    gx.m_viewportDirty = 1; resize = false;
}

bool CGxDeviceVulkan::State::BeginFrame() {
    if (recording) return true;
    if(productionCapture){
        if(!TryReset())throw std::logic_error("World producer recording is still in use");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GX_VK(vkBeginCommandBuffer(command,&begin));recording=true;acquirePending=false;
        if(shadowTimingPool)vkCmdResetQueryPool(command,shadowTimingPool,0,4);
        if(producerTimingPool){vkCmdResetQueryPool(command,producerTimingPool,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,producerTimingPool,0);}
        ++bufferFrame;return true;
    }
    int width, height;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        ResetTemporal(); return false;
    }
    GX_VK(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
    completedSerial = submittedSerial;
    ReadUiTiming();PollWorld();
    if(!independentWorld&&shadowTimingPool&&shadowTimingMask==15){
        uint64_t times[4]{};
        if(vkGetQueryPoolResults(device,shadowTimingPool,0,4,sizeof(times),times,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
            const auto now=std::chrono::steady_clock::now();
            if(now-shadowTimingLog>=std::chrono::seconds(2)){
                const uint64_t mask=shadowTimestampBits>=64?UINT64_MAX:(uint64_t(1)<<shadowTimestampBits)-1;
                const double unit=properties.limits.timestampPeriod/1e6;
                std::fprintf(stderr,"RT timing: collectCPU=%.2fms packUploadCPU=%.2fms buildRecordCPU=%.2fms ASsetupGPU=%.2fms terrainDrawGPU=%.2fms resolution=%ux%u triangles=%llu\n",shadowCollectMs,shadowPackMs,shadowBuildRecordMs,((times[1]-times[0])&mask)*unit,((times[3]-times[2])&mask)*unit,renderExtent.width,renderExtent.height,(unsigned long long)shadowStatus.triangles);
                shadowTimingLog=now;
            }
        }
    }
    if(!independentWorld)shadowTimingMask=0;
    CollectFar();
    if(!productionLease){shadowCurrent.reset();shadowRetired.clear();}
    ClearFramebuffers();CollectRetired();
    if (resize) RecreateSwapchain();
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    }
    if (result == VK_SUBOPTIMAL_KHR) resize = true;
    else VkCheck(result, "vkAcquireNextImageKHR");
    if (!TryReset()) throw std::logic_error("Completed synchronous recording lane became busy");
    residentUploadBytes = 0;
    ++bufferFrame;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GX_VK(vkBeginCommandBuffer(command, &begin));
    recording = true; acquirePending = true;
    BeginUiTiming();
    if(!independentWorld&&shadowTimingPool)vkCmdResetQueryPool(command,shadowTimingPool,0,4);
    Barrier(swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    if (depth.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        Barrier(depth.image, depth.aspect, 1, 1, depth.layout, VK_IMAGE_LAYOUT_GENERAL);
        depth.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    return true;
}
void CGxDeviceVulkan::State::EndPass() {
    if (!inPass) return;
    vkCmdEndRenderPass(command); inPass = false;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void CGxDeviceVulkan::State::BeginPass() {
    if (inPass) return;
    auto& colourTarget = gx.m_textureTarget[GxBuffers_Color];
    auto& depthTarget = gx.m_textureTarget[GxBuffers_Depth];
    NativeImage* colourImage = colourTarget.m_texture ? &Texture(colourTarget.m_texture) :
        (worldActive ? (hdrAborted ? &hdrClassic : &worldColour) : nullptr);
    NativeImage* depthImage = depthTarget.m_texture ? &Texture(depthTarget.m_texture) :
        (worldActive && !colourTarget.m_texture ? &worldDepth : &depth);
    if (colourTarget.m_plane || depthTarget.m_plane)
        throw std::runtime_error("Vulkan cubemap render-target plane is not implemented");
    targetExtent = colourImage ? VkExtent2D{colourImage->width, colourImage->height} : extent;
    if (depthImage->width < targetExtent.width || depthImage->height < targetExtent.height)
        throw std::runtime_error("Vulkan depth target is smaller than the colour target");
    const VkFormat colourFormat = colourImage ? colourImage->format : surfaceFormat;
    const bool motionPass = MotionPass(), hdrPass = HdrDraw();
    const unsigned attachmentCount = 2 + unsigned(motionPass) + unsigned(hdrPass);
    const std::array<uint32_t,3> key{uint32_t(colourFormat), uint32_t(depthImage->format),
        (motionPass ? 1u : 0u) | (hdrPass ? 2u : 0u)};
    auto found = renderPasses.find(key);
    if (found == renderPasses.end()) {
        VkAttachmentDescription attachments[4]{};
        for (unsigned i = 0; i < attachmentCount; ++i) {
            attachments[i].format = i == 0 ? colourFormat : i == 1 ? depthImage->format :
                (hdrPass && i == attachmentCount - 1 ? surfaceFormat : motion.format);
            attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[i].initialLayout = attachments[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkAttachmentReference colours[] = {{0, VK_IMAGE_LAYOUT_GENERAL},
            {motionPass ? 2u : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL},
            {hdrPass ? attachmentCount - 1 : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = hdrPass ? 3 : motionPass ? 2 : 1;
        subpass.pColorAttachments = colours; subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
        dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        create.attachmentCount = attachmentCount; create.pAttachments = attachments;
        create.subpassCount = 1; create.pSubpasses = &subpass; create.dependencyCount = 1; create.pDependencies = &dependency;
        VkRenderPass pass;
        GX_VK(vkCreateRenderPass(device, &create, nullptr, &pass));
        found = renderPasses.emplace(key, pass).first;
    }
    currentPass = found->second;
    std::vector<VkImageView> views{colourImage ? colourImage->view : swapViews[imageIndex], depthImage->view};
    if (motionPass) views.push_back(motion.view);
    if (hdrPass) views.push_back(hdrClassic.view);
    VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create.renderPass = currentPass; create.attachmentCount = attachmentCount; create.pAttachments = views.data();
    create.width = targetExtent.width; create.height = targetExtent.height; create.layers = 1;
    VkFramebuffer framebuffer;
    GX_VK(vkCreateFramebuffer(device, &create, nullptr, &framebuffer));
    framebuffers.push_back(framebuffer);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = currentPass; begin.framebuffer = framebuffer; begin.renderArea.extent = targetExtent;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    inPass = true;
}

void CGxDeviceVulkan::State::Destroy() {
    if(productionCapture)RestorePresentation();
    if(worldSubmitter){worldSubmitter->Stop();worldSubmitter.reset();}
    hdrLutJob.reset();
    if (device) {
#ifdef WOWDECOMP_VULKAN_TESTING
        if (farTestGate) {
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore = farTestGate; signal.value = 1;
            vkSignalSemaphore(device, &signal);
        }
#endif
        vkDeviceWaitIdle(device);
        producerRecording.Destroy();productionLease.reset();productionPrepared=false;producerImages.clear();
        if(producerTimingPool)vkDestroyQueryPool(device,producerTimingPool,nullptr);
        if(uiTimingPool)vkDestroyQueryPool(device,uiTimingPool,nullptr);
        producerTimingPool=uiTimingPool=VK_NULL_HANDLE;uiTimingRecording=uiTimingPending=false;
        ++presentationVersion;
        if (commandPool) vkResetCommandPool(device, commandPool, 0);
        recording = inPass = false;
        for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
#ifdef WOWDECOMP_VULKAN_TESTING
        DestroyBuffer(uiProbe);uiProbeSerial=0;
        gx.SceneFarTestReleaseNear();
        if (farTestGate) { vkDestroySemaphore(device, farTestGate, nullptr); farTestGate = VK_NULL_HANDLE; }
#endif
        completedSerial = submittedSerial;
        CollectFar(true);
        shadowSource.reset();shadowCurrent.reset();shadowRetired.clear();
        for (auto& pipeline : pipelines) vkDestroyPipeline(device, pipeline.second.handle, nullptr);
        pipelines.clear();
        for (auto& pass : renderPasses) vkDestroyRenderPass(device, pass.second, nullptr);
        renderPasses.clear();
        for (auto& entry : textures) {
            entry.first->m_apiSpecificData = nullptr;
            entry.first->m_needsCreation = 1;
            DestroyImage(entry.second);
        }
        textures.clear();
        for (auto& image : retiredImages) DestroyImage(image);
        retiredImages.clear();
        DestroyHdr();
        for (auto& entry : geometry) entry.second.resident.Destroy(device);
        geometry.clear(); temporalBuffers.clear();
        for (auto& buffer : retiredBuffers) DestroyBuffer(buffer);
        retiredBuffers.clear();
        for (auto& images : completedImages) { DestroyImage(images.colour); DestroyImage(images.depth); }
        completedWorld.ResetAfterIdle();displayedWorldSlot.reset();displayedWorldGeneration=0;
        DestroyImage(white); DestroyImage(depth); DestroyImage(worldColour); DestroyImage(worldDepth); DestroyImage(depthScratch);
        DestroyImage(motion); DestroyImage(history[0]); DestroyImage(history[1]); DestroyImage(previousDepth);
        if (upscalePipeline) vkDestroyPipeline(device, upscalePipeline, nullptr);
        upscalePipeline = VK_NULL_HANDLE;
        if (resolvePipeline) vkDestroyPipeline(device, resolvePipeline, nullptr);
        if (resolvePass) vkDestroyRenderPass(device, resolvePass, nullptr);
        if (resolveLayout) vkDestroyPipelineLayout(device, resolveLayout, nullptr);
        if (resolveDescriptors) vkDestroyDescriptorSetLayout(device, resolveDescriptors, nullptr);
        resolvePipeline = VK_NULL_HANDLE; resolvePass = VK_NULL_HANDLE;
        resolveLayout = VK_NULL_HANDLE; resolveDescriptors = VK_NULL_HANDLE;
        ResetTemporal(); temporalActive = worldActive = false;
        RecordingResources::Destroy();
        for (auto view : swapViews) if (view) vkDestroyImageView(device, view, nullptr);
        swapViews.clear(); swapImages.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        for (auto layout : descriptorLayouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
        if (acquired) vkDestroySemaphore(device, acquired, nullptr);
        for (auto semaphore : complete) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        complete.clear();
        if(shadowTimingPool){vkDestroyQueryPool(device,shadowTimingPool,nullptr);shadowTimingPool=VK_NULL_HANDLE;}
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        // Reuse of this State (including failed reinitialization) must not pass
        // destroyed handles to a new device or an idempotent teardown.
        swapchain = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        descriptorLayouts.fill(VK_NULL_HANDLE);
        acquired = VK_NULL_HANDLE; queue = VK_NULL_HANDLE;
        acquirePending = false; resize = false;
    }
    if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
#ifdef WOWDECOMP_VULKAN_TESTING
    if (testMessenger) {
        const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) destroyMessenger(instance, testMessenger, nullptr);
        testMessenger = VK_NULL_HANDLE;
    }
#endif
    if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
    if (OsGuiGetWindow(0) == window) OsGuiSetGxWindow(nullptr);
    if (window && ownWindow) SDL_DestroyWindow(window);
    window = nullptr;
}

NativeImage& CGxDeviceVulkan::State::Texture(CGxTex* texture) {
    if(productionLease && texture && texture->m_needsUpdate)DetachTexture(texture);
    const bool inputUpload=productionCapture && ((!texture&&!white.image) || (texture &&
        (textures.find(texture)==textures.end() || texture->m_needsCreation || texture->m_needsUpdate || texture->m_needsFlagUpdate)));
    if(inputUpload)EndPass();
    struct RestoreCommand {VkCommandBuffer& target;VkCommandBuffer original;~RestoreCommand(){target=original;}} restore{command,command};
    if(inputUpload)command=producerRecording.command;
    const auto far = farTextures.find(texture);
    if (far != farTextures.end()) {
        if (far->second->lease->Status() != GxFarResourceStatus::Resident)
            throw std::runtime_error("Withdrawn FAR texture");
        far->second->lastUse = submittedSerial + 1;
        return far->second->image;
    }
    if (!texture) {
        if (!white.image) {
            EndPass();
            white = MakeImage(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Barrier(white.image, white.aspect, 1, 1, white.layout, VK_IMAGE_LAYOUT_GENERAL);
            white.layout = VK_IMAGE_LAYOUT_GENERAL;
            const uint32_t pixel = 0xFFFFFFFF;
            VkBufferImageCopy copy{};
            copy.bufferOffset = Upload(&pixel, sizeof(pixel));
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(command, upload.buffer, white.image, white.layout, 1, &copy);
            Barrier(white.image, white.aspect, 1, 1, white.layout, white.layout);
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            GX_VK(vkCreateSampler(device, &sampler, nullptr, &white.sampler));
        }
        return white;
    }
    auto found = textures.find(texture);
    if (found == textures.end() || texture->m_needsCreation) {
        EndPass();
        if (found != textures.end()) {
            retiredImages.push_back(found->second); textures.erase(found);
        }
        uint32_t width, height, baseMip, mipCount;
        gx.ITexWHDStartEnd(texture, width, height, baseMip, mipCount);
        VkFormat format = TextureFormat(texture->m_format);
        VkImageAspectFlags aspect = texture->m_format == GxTex_D24X8 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (texture->m_flags.m_renderTarget) usage |= aspect == VK_IMAGE_ASPECT_DEPTH_BIT ?
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const uint32_t levels = texture->m_flags.m_renderTarget ? 1 : mipCount - baseMip;
        NativeImage image = MakeImage(width, height, std::max(levels, 1u), texture->m_target == GxTex_CubeMap ? 6 : 1,
            format, aspect, usage);
        Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, VK_IMAGE_LAYOUT_GENERAL);
        image.layout = VK_IMAGE_LAYOUT_GENERAL;
        if(inputUpload && texture->m_flags.m_renderTarget){
            const VkImageSubresourceRange range{image.aspect,0,image.levels,0,image.layers};
            if(image.aspect==VK_IMAGE_ASPECT_DEPTH_BIT){const VkClearDepthStencilValue value{1,0};vkCmdClearDepthStencilImage(command,image.image,image.layout,&value,1,&range);}
            else {const VkClearColorValue value{{0,0,0,0}};vkCmdClearColorImage(command,image.image,image.layout,&value,1,&range);}
            Barrier(image.image,image.aspect,image.levels,image.layers,image.layout,image.layout);
        }
        found = textures.emplace(texture, image).first;
        texture->m_apiSpecificData = &found->second;
        texture->m_needsCreation = 0; texture->m_needsFlagUpdate = 1;
    }
    NativeImage& image = found->second;
    if (!image.sampler || texture->m_needsFlagUpdate) {
        if (image.sampler) {
            NativeImage old; old.sampler = image.sampler; retiredImages.push_back(old);
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = texture->m_flags.m_filter == GxTex_Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.minFilter = texture->m_flags.m_filter == GxTex_Nearest || texture->m_flags.m_filter == GxTex_NearestMipNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.mipmapMode = texture->m_flags.m_filter >= GxTex_LinearMipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = texture->m_flags.m_wrapU ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = texture->m_flags.m_wrapV ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = static_cast<float>(image.levels - 1);
        GX_VK(vkCreateSampler(device, &sampler, nullptr, &image.sampler));
        texture->m_needsFlagUpdate = 0;
    }
    if (texture->m_needsUpdate) {
        if (texture->m_userFunc && !texture->m_flags.m_renderTarget) UploadTexture(texture, image);
        gx.CGxDevice::ITexMarkAsUpdated(texture);
    }
    if(productionCapture){producerImages.insert(image.image);if(image.sampler)producerSamplers.insert(image.sampler);}
    return image;
}
void CGxDeviceVulkan::State::UploadTexture(CGxTex* texture, NativeImage& image) {
    EndPass();
    uint32_t stride = 0;
    const void* texels = nullptr;
    texture->m_userFunc(GxTex_Lock, texture->m_width, texture->m_height, 0, 0, texture->m_userArg, stride, texels);
    struct Unlock {
        CGxTex* texture;
        ~Unlock() {
            uint32_t stride = 0; const void* texels = nullptr;
            texture->m_userFunc(GxTex_Unlock, texture->m_width, texture->m_height, 0, 0,
                texture->m_userArg, stride, texels);
        }
    } unlock{texture};
    uint32_t width, height, baseMip, mipCount;
    gx.ITexWHDStartEnd(texture, width, height, baseMip, mipCount);
    Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
    for (uint32_t face = 0; face < image.layers; ++face) {
        for (uint32_t level = 0; level < image.levels; ++level) {
            const uint32_t mip = baseMip + level;
            const uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
            texels = nullptr;
            texture->m_userFunc(GxTex_Latch, w, h, face, mip, texture->m_userArg, stride, texels);
            if (!texels) throw std::runtime_error("Texture upload callback returned no texels");
            uint32_t left = std::clamp(texture->m_updateRect.minX >> mip, 0, static_cast<int>(w));
            uint32_t top = std::clamp(texture->m_updateRect.minY >> mip, 0, static_cast<int>(h));
            uint32_t right = std::clamp(std::max(texture->m_updateRect.maxX >> mip, static_cast<int>(left + 1)), 0, static_cast<int>(w));
            uint32_t bottom = std::clamp(std::max(texture->m_updateRect.maxY >> mip, static_cast<int>(top + 1)), 0, static_cast<int>(h));
            const bool compressed = texture->m_format >= GxTex_Dxt1 && texture->m_format <= GxTex_Dxt5;
            if (compressed) { left &= ~3u; top &= ~3u; right = std::min((right + 3) & ~3u, w); bottom = std::min((bottom + 3) & ~3u, h); }
            if (left >= right || top >= bottom) continue;
            const bool sourceCompressed = texture->m_dataFormat >= GxTex_Dxt1 && texture->m_dataFormat <= GxTex_Dxt5;
            auto source = static_cast<const uint8_t*>(texels);
            if (!texture->m_flags.m_bit15) {
                source += sourceCompressed ? s_texFormatBytesPerBlock[texture->m_dataFormat] * (left / 4) + stride * (top / 4) :
                    left * s_texFormatBitDepth[texture->m_dataFormat] / 8 + stride * top;
            }
            const uint32_t row = GxCalcTexelStrideInBytes(texture->m_format, right - left);
            const uint32_t rows = compressed ? (bottom - top + 3) / 4 : bottom - top;
            std::vector<uint8_t> pixels(static_cast<size_t>(row) * rows);
            C2iVector size{static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)};
            if (!Blit(size, BlitAlpha_0, source, stride, GxGetBlitFormat(texture->m_dataFormat),
                pixels.data(), row, GxGetBlitFormat(texture->m_format)))
                throw std::runtime_error("Unsupported Vulkan texture conversion: " +
                    std::to_string(texture->m_dataFormat) + " -> " + std::to_string(texture->m_format));
            VkBufferImageCopy copy{};
            copy.bufferOffset = Upload(pixels.data(), pixels.size(), 16);
            copy.imageSubresource = {image.aspect, level, face, 1};
            copy.imageOffset = {static_cast<int32_t>(left), static_cast<int32_t>(top), 0};
            copy.imageExtent = {right - left, bottom - top, 1};
            vkCmdCopyBufferToImage(command, upload.buffer, image.image, image.layout, 1, &copy);
        }
    }
    Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
}

namespace {
}

std::pair<VkBuffer, VkDeviceSize> CGxDeviceVulkan::State::Geometry(CGxBuf* buffer) {
    const auto far = farBuffers.find(buffer);
    if (far != farBuffers.end()) {
        auto& entry = *far->second;
        if (entry.lease->Status() != GxFarResourceStatus::Resident) throw std::runtime_error("Withdrawn FAR geometry");
        entry.lastUse = submittedSerial + 1;
        return {buffer == &entry.lease->vertex ? entry.vertex.buffer.buffer : entry.index.buffer.buffer, 0};
    }
    auto& data = pools[buffer->m_pool];
    if (!buffer->m_size || buffer->m_index > data.size() || buffer->m_size > data.size() - buffer->m_index)
        throw std::runtime_error("Geometry buffer lies outside its pool");
    if (buffer->m_pool->m_usage == GxPoolUsage_Stream)
        return {upload.buffer, Upload(data.data() + buffer->m_index, buffer->m_size)};
    auto& cached = geometry[buffer];
    if (cached.pool != buffer->m_pool || cached.index != buffer->m_index || cached.bytes != buffer->m_size) {
        cached.resident.MarkDirty();
        cached.pool = buffer->m_pool; cached.index = buffer->m_index; cached.bytes = buffer->m_size;
    }
    if (cached.resident.NeedsUpload(buffer->m_size)) {
        EndPass();
        if(productionLease && !productionCapture && producerBuffers.count(cached.binding))cached.resident.Retire(retiredBuffers);
    }
    cached.binding=cached.resident.Resolve(device,memoryProperties,productionCapture?producerRecording.command:command,bufferFrame,
        data.data()+buffer->m_index,buffer->m_size,retiredBuffers,residentUploadBytes);
    if(productionCapture)producerBuffers.insert(cached.binding);
    return {cached.binding,0};
}
void CGxDeviceVulkan::State::RetireGeometry(CGxBuf* buffer) {
    auto found = geometry.find(buffer);
    if (found != geometry.end()) {
        found->second.resident.Retire(retiredBuffers);
        geometry.erase(found);
    }
}

void CGxDeviceVulkan::State::Draw(CGxBatch& batch, bool indexed) {
    if (!batch.m_count || !BeginFrame()) return;
    gx.IRsSync(0);
    auto* vertexShader = static_cast<CGxShader*>(gx.m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);
    auto* pixelShader = static_cast<CGxShader*>(gx.m_appRenderStates[GxRs_PixelShader].m_value.m_data.p);
    const bool msdfFont = msdfFontActive;
    if (msdfFont) { vertexShader = nullptr; pixelShader = nullptr; }
    const bool fixedVertexStage = !vertexShader;
    const bool fixedPixelStage = !pixelShader;
    NativeShader *v = nullptr, *p = nullptr;
    {
        auto load = [&](CGxShader* shader) -> NativeShader* {
            if (!shader->m_loaded) gx.IShaderCreate(shader);
            auto found = shaders.find(shader);
            if (!shader->m_valid || found == shaders.end()) throw std::runtime_error("Vulkan shader compilation failed; see Gx log");
            return &found->second;
        };
        if (vertexShader) v = load(vertexShader);
        if (pixelShader) p = load(pixelShader);
    }
    const bool terrainUse=terrainShadowActive&&terrainReceiver&&!hdrAborted;
    bool terrainDraw=terrainUse&&!terrainShadowFailed&&shadowCurrent&&shadowCurrent->querySet;
    if(terrainUse){
        const auto prepare=[&](NativeShader* shader,CGxShader* original){
            if(!shader||!original)throw std::runtime_error("Terrain receiver requires authored vertex and pixel programs");
            if(!shader->directionalAttempted){
                shader->directionalAttempted=true;
                try{
                    auto id=GxVulkan::IdentifyShader(original->m_target==GxSh_Vertex?GxVulkan::ShaderStage::Vertex:GxVulkan::ShaderStage::Pixel,original->m_key.m_str,original->m_code.Ptr(),original->m_code.Count());
                    if(!id)throw std::runtime_error("Terrain receiver lacks shader identity");
                    const char* configured=std::getenv("WOWDECOMP_SHADER_ROOT");
                    auto selected=GxVulkan::ResolveShaderOverride(configured?configured:WOWDECOMP_VULKAN_OVERRIDE_DIR,*id,GxVulkan::MaintainedShaderVariant::Directional);
                    if(!selected)throw std::runtime_error("Missing terrain directional source: "+id->profile+"/"+id->name+"/"+std::to_string(id->permutation));
                    shader->directionalSource=std::make_shared<GxVulkan::ShaderOverride>(std::move(*selected));
                    shader->directional=shaderCompiler.CompileMaintained(*shader->directionalSource,GxVulkan::MaintainedShaderVariant::Directional);
                }catch(const std::exception& e){shader->directionalError=e.what();}
            }
            if(!shader->directionalError.empty())throw std::runtime_error(shader->directionalError);
        };
        const auto check=[&](NativeShader* shader,CGxShader* original){
            try{prepare(shader,original);}
            catch(const std::exception& e){
                terrainShadowFailed=true;shadowStatus.result=GxShadow::FrameResult::UnsupportedCoverage;shadowStatus.detail=e.what();++shadowStatus.unsupported;terrainDraw=false;
                auto issue=loggedReceiverIssues.emplace(e.what(),0);++issue.first->second;
                if(issue.second)std::fprintf(stderr,"RT receiver rejected: %s (continuing to enumerate remaining receiver programs)\n",e.what());
            }
        };
        check(v,vertexShader);check(p,pixelShader);
        if(terrainDraw)++shadowStatus.terrainReceivers;
    }
    const auto* vertexBindings=v?(terrainDraw?&v->directional.bindings:&v->bindings):nullptr;
    const auto* pixelBindings=p?(terrainDraw?&p->directional.bindings:&p->bindings):nullptr;
    const bool temporal = MotionPass(), hdrDraw = HdrDraw();
    const auto temporalCode = [&](NativeShader* shader, CGxShader* source) -> const std::vector<uint32_t>& {
        if (shader->temporalCode.empty()) {
            shader->temporalSource = GxVulkan::TemporalShaderSource(shader->source, source->m_target == GxSh_Vertex);
            if (source->m_target == GxSh_Vertex)
                shader->temporalSource = GxVulkan::TemporalViewportSource(std::move(shader->temporalSource));
            shader->temporalCode = CompileTemporal(shader->temporalSource, source->m_target == GxSh_Vertex, *shader);
        }
        return shader->temporalCode;
    };
    if (temporal && temporalFixedVertex.empty()) {
        temporalFixedVertex = ReadModule("fixed-temporal.vert.spv");
        temporalFixedPixel = ReadModule("fixed-temporal.frag.spv");
    }
    if (temporal && msdfFont && temporalMsdfFontPixel.empty())
        temporalMsdfFontPixel = ReadModule("font-msdf-temporal.frag.spv");
    const std::vector<uint32_t>* foglessVertex = nullptr;
    if (!msdfFont && fixedVertexStage && p && !p->inputFog) {
        auto& code = fixedFoglessVertex[temporal ? 1 : 0];
        if (code.empty()) code = ReadModule(temporal ? "fixed-temporal-fogless.vert.spv" : "fixed-fogless.vert.spv");
        foglessVertex = &code;
    }
    if (!msdfFont && !fixedVertexStage && v && p && !p->inputFog) {
        if (v->outputFog) {
            auto& code = v->foglessCode[temporal ? 1 : 0];
            if (code.empty()) {
                auto source = GxVulkan::PruneVertexFogOutput(v->source);
                if (temporal) source = GxVulkan::TemporalViewportSource(GxVulkan::TemporalShaderSource(std::move(source), true));
                code = temporal ? CompileTemporal(source, true, *v) : Compile(source, true, v->maintained);
            }
            foglessVertex = &code;
        }
    }
    const auto& vertexCode = terrainDraw?v->directional.code:foglessVertex ? *foglessVertex : msdfFont ? msdfFontVertex : fixedVertexStage ?
        (temporal ? temporalFixedVertex : fixedVertex) : (temporal ? temporalCode(v, vertexShader) : v->code);
    const auto& originalPixelCode = terrainDraw?p->directional.code:msdfFont ? (temporal ? temporalMsdfFontPixel : msdfFontPixel) : fixedPixelStage ?
        (temporal ? temporalFixedPixel : fixedPixel) : (temporal ? temporalCode(p, pixelShader) : p->code);
    const std::vector<uint32_t>* selectedPixelCode = &originalPixelCode;
    if (hdrDraw&&!terrainDraw) {
        try {
            if (hdrBoundaryFailDraw && ++hdrDraws == hdrBoundaryFailDraw)
                throw std::runtime_error("injected late output-boundary failure");
            selectedPixelCode = &BoundaryPixel(originalPixelCode, !linearOutput&&!terrainShadowActive, p, temporal);
        } catch (const std::exception& error) {
            AbortHdr(error.what()); Draw(batch, indexed); return;
        }
    }
    const auto& pixelCode = *selectedPixelCode;
    std::vector<int> required;
    if (msdfFont) required = {GxVA_Position, GxVA_Color0, GxVA_TexCoord0};
    else if (fixedVertexStage) {
        required = {GxVA_Position, GxVA_Normal, GxVA_Color0};
        for (int i = 0; i < 8; ++i) required.push_back(GxVA_TexCoord0 + i);
    } else required = vertexBindings->attributes;
    std::array<const CGxVertexAttrib*, GxVAs_Last> declarations{};
    std::array<CGxBuf*, GxVAs_Last> buffers{};
    if (gx.m_primVertexFormat < GxVertexBufferFormats_Last) {
        const auto& description = Buffer::s_vertexBufDesc[gx.m_primVertexFormat];
        for (uint32_t i = 0; i < description.attribCount; ++i) {
            const auto& attribute = description.attribs[i];
            declarations[attribute.m_attrib] = &attribute; buffers[attribute.m_attrib] = gx.m_primVertexBuf;
        }
    } else {
        for (int i = 0; i < GxVAs_Last; ++i)
            if (gx.m_primVertexMask & (1u << i)) {
                declarations[i] = &gx.m_primVertexFormatAttrib[i]; buffers[i] = gx.m_primVertexFormatBuf[i];
            }
    }
    static constexpr VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_USCALED,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
        VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R32_SFLOAT};
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkDeviceSize> offsets;
    std::vector<VkBuffer> vertexBuffers;
    std::map<CGxBuf*, uint32_t> streams;
    if (!FarDrawAllowed(buffers, required, indexed)) return;
    for (int location : required) {
        const auto* declaration = declarations[location];
        CGxBuf* buffer = buffers[location];
        if (!declaration || !buffer) {
            if (location == GxVA_Position) throw std::runtime_error("Draw has no position buffer");
            std::array<float, 4> value{0, 0, 0, 1};
            if (location == GxVA_Color0 || location == GxVA_Color1) value = {1, 1, 1, 1};
            if (location == GxVA_Normal) value = {0, 0, 1, 0};
            const auto binding = static_cast<uint32_t>(bindings.size());
            bindings.push_back({binding, 0, VK_VERTEX_INPUT_RATE_VERTEX});
            offsets.push_back(Upload(value.data(), sizeof(value)));
            vertexBuffers.push_back(upload.buffer);
            attributes.push_back({static_cast<uint32_t>(location), binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            continue;
        }
        if (declaration->m_type >= std::size(formats)) throw std::runtime_error("Unknown Gx vertex format");
        auto found = streams.find(buffer);
        if (found == streams.end()) {
            const auto binding = static_cast<uint32_t>(bindings.size());
            found = streams.emplace(buffer, binding).first;
            const auto source = Geometry(buffer);
            bindings.push_back({binding, buffer->m_itemSize ? buffer->m_itemSize : declaration->m_bufSize, VK_VERTEX_INPUT_RATE_VERTEX});
            offsets.push_back(source.second);
            vertexBuffers.push_back(source.first);
        }
        attributes.push_back({static_cast<uint32_t>(location), found->second, formats[declaration->m_type], declaration->m_offset});
    }
    const auto enabled = [&](EGxRenderState state, EGxMasterEnables master) {
        return gx.m_appRenderStates[state].m_value.m_data.i[0] && (gx.m_appMasterEnables & (1u << master));
    };
    const auto blend = gx.m_appRenderStates[GxRs_BlendingMode].m_value.m_data.u[0];
    if (blend >= GxBlends_Last) throw std::runtime_error("Unknown Gx blend mode");
    std::array<std::array<VkDescriptorImageInfo, 16>, 2> images{};
    // Resolve uploads before beginning a render pass, since transfers cannot
    // occur inside it. Descriptor snapshots remain valid until the frame fence.
    bool withdrawnFarTexture = false;
    auto sample = [&](unsigned set, uint32_t slot, bool useBoundTexture = true) {
        if (slot >= (set == 0 ? 4u : 16u)) throw std::runtime_error("Shader texture register is out of range");
        auto& descriptor = images[set == 0 ? 0 : 1][slot];
        if (!descriptor.imageView) {
            auto* texture = useBoundTexture ? static_cast<CGxTex*>(gx.m_appRenderStates[GxRs_Texture0 + slot].m_value.m_data.p) : nullptr;
            const auto far = farTextures.find(texture);
            if (far != farTextures.end() && far->second->lease->Status() != GxFarResourceStatus::Resident) {
                withdrawnFarTexture = true; return;
            }
            auto& image = Texture(texture);
            descriptor = {image.sampler, image.view, image.layout};
        }
    };
    std::array<bool, 8> fixedStageEnabled{};
    if (msdfFont) {
        sample(2, 0);
    } else if (fixedPixelStage) {
        bool enabledChain = true;
        for (unsigned stage = 0; stage < fixedStageEnabled.size(); ++stage) {
            auto* texture = static_cast<CGxTex*>(gx.m_appRenderStates[GxRs_Texture0 + stage].m_value.m_data.p);
            enabledChain = enabledChain && texture;
            fixedStageEnabled[stage] = enabledChain;
            if (enabledChain && texture->m_target == GxTex_CubeMap)
                throw std::runtime_error("Fixed-function cube-map sampling is not implemented");
            sample(2, stage, enabledChain);
        }
    } else {
        for (int slot : pixelBindings->samplers) sample(2, slot);
    }
    if (v) for (int slot : vertexBindings->samplers) sample(0, slot);
    C44Matrix transform = gx.m_xforms[GxXform_World].m_mtx[gx.m_xforms[GxXform_World].m_level] *
        gx.m_xforms[GxXform_View].m_mtx[gx.m_xforms[GxXform_View].m_level] * gx.m_projNative;
    if (withdrawnFarTexture) return;
    std::vector<float> vertexUniforms, pixelUniforms;
    if (msdfFont) {
        const auto* matrix = reinterpret_cast<const float*>(&transform);
        vertexUniforms.assign(matrix, matrix + 16);
        pixelUniforms = {gx.m_appRenderStates[GxRs_AlphaRef].m_value.m_data.u[0] / 255.0f,
            float(msdfOutlinePixels), 8.0f, 256.0f};
    } else if (fixedVertexStage || fixedPixelStage) {
        const auto& world = gx.m_xforms[GxXform_World].m_mtx[gx.m_xforms[GxXform_World].m_level];
        const auto& view = gx.m_xforms[GxXform_View].m_mtx[gx.m_xforms[GxXform_View].m_level];
        const C44Matrix worldView = world * view;
        C44Matrix normalTransform;
        for (unsigned stage = 0; stage < 8; ++stage) {
            const auto generation = gx.m_appRenderStates[GxRs_TexGen0 + stage].m_value.m_data.u[0];
            if (fixedVertexStage && (fixedStageEnabled[stage] || !fixedPixelStage) && generation >= GxTexGen_ReflectionMap)
                normalTransform = worldView.Inverse().Transpose();
        }
        const auto appendMatrix = [&](const C44Matrix& matrix) {
            const auto* data = reinterpret_cast<const float*>(&matrix);
            vertexUniforms.insert(vertexUniforms.end(), data, data + 16);
        };
        appendMatrix(transform); appendMatrix(worldView); appendMatrix(normalTransform);
        std::array<float, 32> coordinates{};
        pixelUniforms = {gx.m_appRenderStates[GxRs_AlphaRef].m_value.m_data.u[0] / 255.0f, 0, 0, 0};
        for (unsigned stage = 0; stage < 8; ++stage) {
            C44Matrix texGen;
            const bool enabledStage = fixedStageEnabled[stage] || !fixedPixelStage;
            const auto coordinate = gx.m_appRenderStates[GxRs_TextureCoord0 + stage].m_value.m_data.u[0];
            const auto generation = gx.m_appRenderStates[GxRs_TexGen0 + stage].m_value.m_data.u[0];
            const auto transformMode = gx.m_appRenderStates[GxRs_TextureShader0 + stage].m_value.m_data.u[0];
            const auto colourOp = gx.m_appRenderStates[GxRs_ColorOp0 + stage].m_value.m_data.u[0];
            const auto alphaOp = gx.m_appRenderStates[GxRs_AlphaOp0 + stage].m_value.m_data.u[0];
            if ((fixedVertexStage && enabledStage && (coordinate >= 8 || generation > GxTexGen_SphereMap || transformMode > 2)) ||
                (fixedPixelStage && fixedStageEnabled[stage] && (colourOp >= 6 || alphaOp >= 6)))
                throw std::runtime_error("Unsupported fixed-function texture-stage state");
            if (enabledStage && (generation == GxTexGen_ObjectLinear || generation == GxTexGen_EyeLinear)) {
                texGen = view.AffineInverse();
                if (generation == GxTexGen_ObjectLinear) texGen = texGen * world.Inverse();
            } else if (enabledStage && generation == GxTexGen_SphereMap) {
                texGen.a0 = texGen.b1 = 0.5f; texGen.d0 = texGen.d1 = 0.5f;
            }
            if (enabledStage && transformMode)
                texGen = texGen * gx.m_xforms[stage].m_mtx[gx.m_xforms[stage].m_level];
            appendMatrix(texGen);
            coordinates[stage * 4] = static_cast<float>(coordinate);
            coordinates[stage * 4 + 1] = static_cast<float>(generation);
            coordinates[stage * 4 + 2] = static_cast<float>(transformMode);
            coordinates[stage * 4 + 3] = enabledStage ? 1.0f : 0.0f;
            pixelUniforms.insert(pixelUniforms.end(), {fixedStageEnabled[stage] ? 1.0f : 0.0f,
                static_cast<float>(colourOp), static_cast<float>(alphaOp), 0});
        }
        vertexUniforms.insert(vertexUniforms.end(), coordinates.begin(), coordinates.end());
        const bool fog = enabled(GxRs_Fog, GxMasterEnable_Fog);
        vertexUniforms.insert(vertexUniforms.end(), {fog ? 1.f : 0.f,
            gx.m_appRenderStates[GxRs_FogStart].m_value.m_data.f[0],
            gx.m_appRenderStates[GxRs_FogEnd].m_value.m_data.f[0], 0.f});
        const uint32_t fogColour = gx.m_appRenderStates[GxRs_FogColor].m_value.m_data.u[0];
        pixelUniforms.insert(pixelUniforms.end(), {((fogColour >> 16) & 255) / 255.f,
            ((fogColour >> 8) & 255) / 255.f, (fogColour & 255) / 255.f, fog ? 1.f : 0.f});
    }
    if (v) vertexUniforms = GxVulkan::ShaderUniforms(*vertexBindings, CGxDevice::s_shadowConstants[1].constants, 256);
    if (p) pixelUniforms = GxVulkan::ShaderUniforms(*pixelBindings, CGxDevice::s_shadowConstants[0].constants, 256);
    if (vertexUniforms.empty()) vertexUniforms.resize(4);
    if (pixelUniforms.empty()) pixelUniforms.resize(4);
    std::vector<float> previousUniforms;
    std::array<float,4> temporalOptions{0, 1, 0, 1};
    bool jitterGeometry = false;
    if (temporal) {
        previousUniforms = vertexUniforms;
        GxVulkan::TemporalDraws::Key identity{reinterpret_cast<uint64_t>(motionOwner),
            v ? v->id : 0, p ? p->id : 0, uint64_t(indexed), uint64_t(batch.m_primType),
            batch.m_start, batch.m_count, batch.m_minIndex, batch.m_maxIndex};
        GxVulkan::TemporalDraws::Pose pose;
        pose.constants = vertexUniforms;
        pose.minDepth = gx.m_viewport.m_z.m_l; pose.maxDepth = gx.m_viewport.m_z.m_h;
        pose.valid = !msdfFont && motionOwner && stableVertices && blend <= GxBlend_AlphaKey &&
            enabled(GxRs_DepthTest, GxMasterEnable_DepthTest) &&
            enabled(GxRs_DepthWrite, GxMasterEnable_DepthWrite) &&
            (!v || v->bindings.samplers.empty());
        const auto identifyBuffer = [&](CGxBuf* buffer) {
            identity.push_back(reinterpret_cast<uint64_t>(buffer));
            if (!buffer) return;
            identity.insert(identity.end(), {reinterpret_cast<uint64_t>(buffer->m_pool), buffer->m_index,
                buffer->m_size, buffer->m_itemSize});
            pose.buffers.push_back(buffer);
            temporalBuffers[buffer] = {reinterpret_cast<uint64_t>(buffer->m_pool), buffer->m_index, buffer->m_size};
        };
        for (int location : required) {
            identity.push_back(location);
            const auto* declaration = declarations[location];
            identity.push_back(declaration ? declaration->m_offset : UINT64_MAX);
            identity.push_back(declaration ? declaration->m_type : UINT64_MAX);
            identity.push_back(declaration ? declaration->m_bufSize : UINT64_MAX);
            identifyBuffer(buffers[location]);
        }
        if (indexed) identifyBuffer(gx.m_primIndexBuf);
        // A different viewport/depth mapping is a different draw. History itself
        // resets on world viewport changes; secondary per-draw viewports reject.
        const float viewportValues[] = {gx.m_viewport.m_x.m_l, gx.m_viewport.m_x.m_h,
            gx.m_viewport.m_y.m_l, gx.m_viewport.m_y.m_h, pose.minDepth, pose.maxDepth};
        for (float value : viewportValues) {
            uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); identity.push_back(bits);
        }
        pose.valid = pose.valid && !temporalDraws.Seen(identity);
        // Jitter only once a usable prior pose exists. Untracked or repeatedly
        // invalidated geometry must resolve as stable, unjittered fresh colour.
        const auto* previous = temporalDraws.Previous(identity);
        if (pose.valid && temporalFrame.historyValid && previous && previous->constants.size() == vertexUniforms.size()) {
            previousUniforms = previous->constants;
            temporalOptions = {1, 0, previous->minDepth, previous->maxDepth};
            jitterGeometry = true;
        }
        if (motionOwner) temporalDraws.Record(identity, std::move(pose));
    }
    const size_t alignment = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 16);
    VkDescriptorBufferInfo uniforms[] = {
        {upload.buffer, Upload(vertexUniforms.data(), vertexUniforms.size() * 4, alignment), vertexUniforms.size() * 4},
        {upload.buffer, Upload(pixelUniforms.data(), pixelUniforms.size() * 4, alignment), pixelUniforms.size() * 4},
        {upload.buffer, 0, 16}
    };
    const float alphaTest[] = {gx.m_appRenderStates[GxRs_AlphaRef].m_value.m_data.u[0] / 255.0f, 0, 0, 0};
    uniforms[2].offset = Upload(alphaTest, sizeof(alphaTest), alignment);
    // BeginPass runs below, after texture preparation. targetExtent still belongs
    // to the preceding pass here (often the native clear/UI pass).
    const auto jitterExtent = worldActive ? renderExtent : extent;
    const float viewportWidth = (gx.m_viewport.m_x.m_h-gx.m_viewport.m_x.m_l)*jitterExtent.width;
    const float viewportHeight = (gx.m_viewport.m_y.m_h-gx.m_viewport.m_y.m_l)*jitterExtent.height;
    const float jitter[] = {jitterGeometry && viewportWidth > 0 ? 2*temporalFrame.jitterPixels[0]/viewportWidth : 0,
        jitterGeometry && viewportHeight > 0 ? -2*temporalFrame.jitterPixels[1]/viewportHeight : 0,
        viewportWidth/jitterExtent.width, viewportHeight/jitterExtent.height};
    VkDescriptorBufferInfo temporalUniforms[] = {
        {upload.buffer, 0, previousUniforms.size()*sizeof(float)},
        {upload.buffer, 0, sizeof(jitter)}, {upload.buffer, 0, sizeof(temporalOptions)}
    };
    if (temporal) {
        temporalUniforms[0].offset = Upload(previousUniforms.data(), temporalUniforms[0].range, alignment);
        temporalUniforms[1].offset = Upload(jitter, sizeof(jitter), alignment);
        temporalUniforms[2].offset = Upload(temporalOptions.data(), sizeof(temporalOptions), alignment);
    }
    std::array<VkDescriptorSet, 4> sets;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool; allocate.descriptorSetCount = 4; allocate.pSetLayouts = descriptorLayouts.data();
    GX_VK(vkAllocateDescriptorSets(device, &allocate, sets.data()));
    std::vector<VkWriteDescriptorSet> writes;
    for (unsigned stage = 0; stage < 2; ++stage) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[stage * 2 + 1]; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1; write.pBufferInfo = &uniforms[stage]; writes.push_back(write);
    }
    VkWriteDescriptorSet alpha{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    alpha.dstSet = sets[3]; alpha.dstBinding = 1; alpha.descriptorCount = 1;
    alpha.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; alpha.pBufferInfo = &uniforms[2]; writes.push_back(alpha);
    if (temporal) for (unsigned i = 0; i < 3; ++i) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[i == 2 ? 3 : 1]; write.dstBinding = i == 2 ? 2 : i+1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.descriptorCount = 1;
        write.pBufferInfo = &temporalUniforms[i]; writes.push_back(write);
    }
    const auto writeSampler = [&](unsigned set, uint32_t binding) {
        if (binding >= (set == 0 ? 4u : 16u)) throw std::runtime_error("Shader sampler exceeds its stage limit");
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[set]; write.dstBinding = binding; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &images[set == 0 ? 0 : 1][binding]; writes.push_back(write);
    };
    if (v) for (int slot : vertexBindings->samplers) writeSampler(0, slot);
    if (msdfFont) writeSampler(2, 0);
    else if (fixedPixelStage) for (unsigned stage = 0; stage < 8; ++stage) writeSampler(2, stage);
    else for (int slot : pixelBindings->samplers) writeSampler(2, slot);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    std::pair<VkBuffer, VkDeviceSize> residentIndex{};
    if (indexed && batch.m_primType != GxPrim_TriangleFan) {
        auto* buffer = gx.m_primIndexBuf;
        if (!buffer || !buffer->m_pool) throw std::runtime_error("Indexed draw has no index buffer");
        if (buffer->m_pool->m_usage != GxPoolUsage_Stream) residentIndex = Geometry(buffer);
    }
    BeginPass();
    const uint32_t depthTest = enabled(GxRs_DepthTest, GxMasterEnable_DepthTest) ? 1u : 0u;
    const uint32_t depthWrite = enabled(GxRs_DepthWrite, GxMasterEnable_DepthWrite) ? 1u : 0u;
    const uint32_t cull = enabled(GxRs_Culling, GxMasterEnable_Culling) ? gx.m_appRenderStates[GxRs_Culling].m_value.m_data.u[0] : 0;
    const uint32_t colourMask = enabled(GxRs_ColorWrite, GxMasterEnable_ColorWrite) ? gx.m_appRenderStates[GxRs_ColorWrite].m_value.m_data.u[0] : 0;
    const auto depthFunction = gx.m_appRenderStates[GxRs_DepthFunc].m_value.m_data.u[0];
    const auto primitive = batch.m_primType == GxPrim_TriangleFan ? GxPrim_Triangles : batch.m_primType;
    std::vector<uint64_t> key{fixedVertexStage ? 0 : v->id, fixedPixelStage ? 0 : p->id, reinterpret_cast<uint64_t>(currentPass),
        blend, depthTest, depthWrite, cull, colourMask, depthFunction, static_cast<uint64_t>(primitive)};
    key.push_back(terrainDraw?1:0);
    key.push_back(msdfFont ? 1 : 0); // Authored fonts cannot alias fixed/legacy pipelines.
    key.push_back(hdrDraw && !linearOutput && !terrainShadowActive ? 1 : 0); // Encoded and explicit-linear providers have distinct output boundaries.
    for (const auto& binding : bindings) { key.push_back(binding.binding); key.push_back(binding.stride); }
    key.push_back(UINT64_MAX);
    for (const auto& attribute : attributes) {
        key.push_back(attribute.location); key.push_back(attribute.binding); key.push_back(attribute.format); key.push_back(attribute.offset);
    }
    auto found = pipelines.find(key);
    if (found == pipelines.end()) {
        VkShaderModule modules[2]{};
        try {
            const std::vector<uint32_t>* codes[] = {&vertexCode, &pixelCode};
            for (unsigned i = 0; i < 2; ++i) {
                VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                module.codeSize = codes[i]->size() * 4; module.pCode = codes[i]->data();
                GX_VK(vkCreateShaderModule(device, &module, nullptr, &modules[i]));
            }
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (unsigned i = 0; i < 2; ++i) {
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                stages[i].module = modules[i]; stages[i].pName = "main";
            }
            VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            inputs.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()); inputs.pVertexBindingDescriptions = bindings.data();
            inputs.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()); inputs.pVertexAttributeDescriptions = attributes.data();
            static constexpr VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
            if (primitive >= GxPrims_Last) throw std::runtime_error("Unknown Gx primitive type");
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = topologies[primitive];
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1.0f;
            raster.cullMode = cull == 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            raster.frontFace = cull == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            static constexpr VkCompareOp comparisons[] = {VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS};
            if (depthFunction >= std::size(comparisons)) throw std::runtime_error("Unknown Gx depth comparison");
            VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthState.depthTestEnable = depthTest; depthState.depthWriteEnable = depthWrite;
            depthState.depthCompareOp = comparisons[depthFunction];
            static constexpr VkBlendFactor src[] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA,
                VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_ALPHA};
            static constexpr VkBlendFactor dst[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.blendEnable = blend > GxBlend_AlphaKey;
            attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = src[blend];
            attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = dst[blend];
            attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            attachment.colorWriteMask = colourMask & 15;
            VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState colourAttachments[] = {attachment, {}, attachment};
            if (temporal) colourAttachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blending.attachmentCount = hdrDraw ? 3 : temporal ? 2 : 1; blending.pAttachments = colourAttachments;
            const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 3; dynamic.pDynamicStates = dynamicStates;
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            create.stageCount = 2; create.pStages = stages; create.pVertexInputState = &inputs; create.pInputAssemblyState = &assembly;
            create.pViewportState = &viewport; create.pRasterizationState = &raster; create.pMultisampleState = &samples;
            create.pDepthStencilState = &depthState; create.pColorBlendState = &blending; create.pDynamicState = &dynamic;
            create.layout = pipelineLayout; create.renderPass = currentPass;
            Pipeline pipeline;
            GX_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline.handle));
            found = pipelines.emplace(std::move(key), pipeline).first;
        } catch (...) {
            for (auto module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
            if (hdrDraw) {
                AbortHdr("HDR-only graphics pipeline creation failed"); Draw(batch, indexed); return;
            }
            throw;
        }
        for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, found->second.handle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 4, sets.data(), 0, nullptr);
    if(terrainDraw)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,4,1,&shadowCurrent->querySet,0,nullptr);
    vkCmdBindVertexBuffers(command, 0, static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data(), offsets.data());
    VkViewport viewport{};
    viewport.x = gx.m_viewport.m_x.m_l * targetExtent.width;
    // Like the D3D backend, render textures use a top-origin valid region.
    // The backbuffer retains the engine's bottom-origin viewport convention.
    const bool renderToTexture = gx.m_textureTarget[GxBuffers_Color].m_texture || gx.m_textureTarget[GxBuffers_Depth].m_texture;
    viewport.y = (renderToTexture ? gx.m_viewport.m_y.m_h : 1.0f - gx.m_viewport.m_y.m_l) * targetExtent.height;
    viewport.width = (gx.m_viewport.m_x.m_h - gx.m_viewport.m_x.m_l) * targetExtent.width;
    viewport.height = -(gx.m_viewport.m_y.m_h - gx.m_viewport.m_y.m_l) * targetExtent.height;
    viewport.minDepth = gx.m_viewport.m_z.m_l; viewport.maxDepth = gx.m_viewport.m_z.m_h;
    vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, targetExtent};
    if (gx.m_appRenderStates[GxRs_ScissorTest].m_value.m_data.i[0]) {
        const int left = std::clamp(static_cast<int>(gx.m_scissorRect.minX * targetExtent.width + 0.5f), 0, static_cast<int>(targetExtent.width));
        const int top = std::clamp(static_cast<int>((1.0f - gx.m_scissorRect.maxY) * targetExtent.height + 0.5f), 0, static_cast<int>(targetExtent.height));
        const int right = std::clamp(static_cast<int>(gx.m_scissorRect.maxX * targetExtent.width + 0.5f), left, static_cast<int>(targetExtent.width));
        const int bottom = std::clamp(static_cast<int>((1.0f - gx.m_scissorRect.minY) * targetExtent.height + 0.5f), top, static_cast<int>(targetExtent.height));
        scissor = {{left, top}, {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}};
    }
    vkCmdSetScissor(command, 0, 1, &scissor);
    const float blendConstants[] = {1, 1, 1, 1};
    vkCmdSetBlendConstants(command, blendConstants);
    VkDeviceSize indexOffset = 0;
    if (batch.m_primType == GxPrim_TriangleFan) {
        if (batch.m_count < 3) return;
        std::vector<uint32_t> indices;
        const uint16_t* original = nullptr;
        if (indexed) {
            auto* buffer = gx.m_primIndexBuf;
            if (!buffer) throw std::runtime_error("Indexed draw has no index buffer");
            const auto far = farBuffers.find(buffer);
            const auto* data = far == farBuffers.end() ? &pools[buffer->m_pool] : nullptr;
            const size_t available = data ? data->size() : buffer->m_size;
            if (buffer->m_index > available || (static_cast<uint64_t>(batch.m_start) + batch.m_count) * 2 > available - buffer->m_index)
                throw std::runtime_error("Triangle fan index buffer overrun");
            const auto* source = data ? data->data() : far->second->index.buffer.mapped;
            original = reinterpret_cast<const uint16_t*>(source + buffer->m_index) + batch.m_start;
            if (!data) far->second->lastUse = submittedSerial + 1;
        }
        for (uint32_t i = 1; i + 1 < batch.m_count; ++i) {
            indices.push_back(original ? original[0] : batch.m_start);
            indices.push_back(original ? original[i] : batch.m_start + i);
            indices.push_back(original ? original[i + 1] : batch.m_start + i + 1);
        }
        indexOffset = Upload(indices.data(), indices.size() * 4, 4);
        vkCmdBindIndexBuffer(command, upload.buffer, indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    } else if (indexed) {
        auto* buffer = gx.m_primIndexBuf;
        if (!buffer) throw std::runtime_error("Indexed draw has no index buffer");
        const auto* data = farBuffers.count(buffer) ? nullptr : &pools[buffer->m_pool];
        const size_t available = data ? data->size() : buffer->m_size;
        if (buffer->m_index > available || (static_cast<uint64_t>(batch.m_start) + batch.m_count) * 2 > available - buffer->m_index)
            throw std::runtime_error("Index buffer overrun");
        VkBuffer indexBuffer = upload.buffer;
        if (residentIndex.first) {
            indexBuffer = residentIndex.first;
            indexOffset = residentIndex.second + static_cast<uint64_t>(batch.m_start) * 2;
        } else indexOffset = Upload(data->data() + buffer->m_index + batch.m_start * 2, batch.m_count * 2, 4);
        vkCmdBindIndexBuffer(command, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(command, batch.m_count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(command, batch.m_count, 1, batch.m_start, 0);
    }
    ++drawCount;
}

void CGxDeviceVulkan::State::Submit(bool present, bool resume) {
    if(productionCapture)throw std::runtime_error("Synchronous readback/submission inside independent world capture is unsupported");
    EndPass();
    if (present) Barrier(swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if(present && uiTimingRecording){vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,uiTimingPool,1);uiTimingRecording=false;uiTimingPending=true;}
    auto result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) ResetTemporal();
    VkCheck(result, "vkEndCommandBuffer");
    result = vkResetFences(device, 1, &fence);
    if (result != VK_SUCCESS) ResetTemporal();
    VkCheck(result, "vkResetFences");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (acquirePending) {
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
    }
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    if (present) { submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &complete[imageIndex]; }
#ifdef WOWDECOMP_VULKAN_TESTING
    VkSemaphore testWaits[2]{};
    VkPipelineStageFlags testWaitStages[2]{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    uint64_t testWaitValues[2]{};
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (farTestGate) {
        unsigned waits = 0;
        if (acquirePending) testWaits[waits++] = acquired;
        testWaits[waits] = farTestGate; testWaitValues[waits++] = 1;
        timeline.waitSemaphoreValueCount = waits; timeline.pWaitSemaphoreValues = testWaitValues;
        submit.pNext = &timeline; submit.waitSemaphoreCount = waits;
        submit.pWaitSemaphores = testWaits; submit.pWaitDstStageMask = testWaitStages;
    }
#endif
    VkResult submitted;
    {std::lock_guard<std::mutex> lock(queueMutex);submitted=vkQueueSubmit(queue,1,&submit,fence);}
    if (submitted != VK_SUCCESS) ResetTemporal();
    VkCheck(submitted, "vkQueueSubmit");
    ++submittedSerial;
    if (!independentWorld) CommitHistory();
    recording = false; acquirePending = false;
    if (present) {
        VkPresentInfoKHR presentation{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentation.waitSemaphoreCount = 1; presentation.pWaitSemaphores = &complete[imageIndex];
        presentation.swapchainCount = 1; presentation.pSwapchains = &swapchain; presentation.pImageIndices = &imageIndex;
        VkResult result;
        {std::lock_guard<std::mutex> lock(queueMutex);result=vkQueuePresentKHR(queue,&presentation);}
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) resize = true;
        else VkCheck(result, "vkQueuePresentKHR");
    }
    if (resume) {
        GX_VK(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
        completedSerial = submittedSerial;
        CollectFar();
        GX_VK(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GX_VK(vkBeginCommandBuffer(command, &begin));
        recording = true;
    }
}

CGxDeviceVulkan::CGxDeviceVulkan() : m_native(std::make_unique<State>(*this)) {
    m_api = GxApi_Vulkan;
    m_caps.m_colorFormat = GxCF_argb;
    DeviceCreatePools();
    DeviceCreateStreamBufs();
}
CGxDeviceVulkan::~CGxDeviceVulkan() { DeviceDestroy(); }
CGxDevice* CreateCGxDeviceVulkan() { return new CGxDeviceVulkan; }
int32_t CGxDeviceVulkan::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    m_windowProc = windowProc;
    m_native->ownWindow = true;
    return DeviceSetFormat(format);
}
int32_t CGxDeviceVulkan::DeviceCreate(void* window, const CGxFormat& format) {
    if (!window) return 0;
    m_native->window = static_cast<SDL_Window*>(window);
    m_native->ownWindow = false;
    return DeviceSetFormat(format);
}
bool CGxDeviceVulkan::DeviceGetWindowLogicalSize(C2iVector& size) const {
    if (!m_native || !m_native->window) return false;
    int width = 0, height = 0;
    SDL_GetWindowSize(m_native->window, &width, &height);
    if (width <= 0 || height <= 0) return false;
    size = {width, height};
    return true;
}
int32_t CGxDeviceVulkan::DeviceSetFormat(const CGxFormat& format) {
    auto& state = *m_native;
    if (format.m_size.x < 0 || format.m_size.y < 0) return 0;
    if (!format.m_window && !OsGuiSupportsExclusiveFullscreen()) {
        CGxDevice::Log("Exclusive fullscreen unsupported by the current SDL video driver");
        return 0;
    }
    try {
        CGxDevice::DeviceSetFormat(format);
        if (state.device) {
            OsWindowPlacement placement;
            OsGuiGetWindowPlacement(state.window, placement);
            const auto flags = SDL_GetWindowFlags(state.window);
            const uint32_t fullscreen = m_format.m_window ? (m_format.m_maximize == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) : SDL_WINDOW_FULLSCREEN;
            const auto previous = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
            if (!m_format.m_window && !OsGuiSetWindowMode(state.window, false, 0,
                m_format.m_size, m_format.m_refreshRate)) throw std::runtime_error(SDL_GetError());
            if (previous != fullscreen) {
                if (!OsGuiSetWindowMode(state.window, m_format.m_window, m_format.m_maximize,
                    m_format.m_size, m_format.m_refreshRate)) throw std::runtime_error(SDL_GetError());
                if (!fullscreen) OsGuiRestoreWindowPlacement(state.window, placement, true);
            } else if (!fullscreen && !(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
                int width, height;
                SDL_GetWindowSize(state.window, &width, &height);
                if (width != m_format.m_size.x || height != m_format.m_size.y)
                    SDL_SetWindowSize(state.window, m_format.m_size.x, m_format.m_size.y);
            }
            state.resize = true;
            return 1;
        }
        if (!state.window) {
            if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) throw std::runtime_error("SDL video is not initialized");
            uint32_t flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
            state.window = SDL_CreateWindow("World of Warcraft (Vulkan)", m_format.m_pos.x, m_format.m_pos.y,
                m_format.m_size.x ? m_format.m_size.x : 1024, m_format.m_size.y ? m_format.m_size.y : 768, flags);
            if (!state.window) throw std::runtime_error(SDL_GetError());
            auto placement = OsGuiGetSavedWindowPlacement();
            if (m_format.m_window && !m_format.m_maximize) {
                placement.x = m_format.m_pos.x; placement.y = m_format.m_pos.y;
                placement.width = m_format.m_size.x ? m_format.m_size.x : 1024;
                placement.height = m_format.m_size.y ? m_format.m_size.y : 768;
            }
            OsGuiRestoreWindowPlacement(state.window, placement, m_format.m_window && !m_format.m_maximize);
            const auto fullscreen = !m_format.m_window ? SDL_WINDOW_FULLSCREEN :
                m_format.m_maximize == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
            if (fullscreen && !OsGuiSetWindowMode(state.window, m_format.m_window, m_format.m_maximize,
                m_format.m_size, m_format.m_refreshRate)) throw std::runtime_error(SDL_GetError());
        }
        OsGuiSetGxWindow(state.window);
        state.Initialize();
        m_caps.m_numTmus = std::min(state.properties.limits.maxPerStageDescriptorSamplers, 16u);
        m_caps.m_numStreams = std::min(state.properties.limits.maxVertexInputBindings, 14u);
        m_caps.m_streamOffset = 1; m_caps.m_maxIndex = 65535;
        m_caps.m_pixelCenterOnEdge = m_caps.m_texelCenterOnEdge = 1;
        m_caps.m_texNonPow2 = 1;
        for (unsigned i = 0; i < GxTexTargets_Last; ++i) {
            m_caps.m_texTarget[i] = i != GxTex_Rectangle; // Rectangle coordinates are unnormalised.
            m_caps.m_texMaxSize[i] = i == GxTex_CubeMap ? state.properties.limits.maxImageDimensionCube : state.properties.limits.maxImageDimension2D;
        }
        for (unsigned i = 1; i < GxTexFormats_Last; ++i) {
            VkFormatProperties properties;
            vkGetPhysicalDeviceFormatProperties(state.physical, TextureFormat(static_cast<EGxTexFormat>(i)), &properties);
            m_caps.m_texFmt[i] = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
            m_caps.m_rttFormat[i] = (properties.optimalTilingFeatures & (i == GxTex_D24X8 ?
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) != 0;
        }
        m_caps.m_shaderTargets[GxSh_Vertex] = 3; m_caps.m_shaderTargets[GxSh_Pixel] = 4;
        m_caps.m_shaderConstants[GxSh_Vertex] = 256; m_caps.m_shaderConstants[GxSh_Pixel] = 224;
        m_caps.m_texFilterTrilinear = 1; m_caps.m_colorWrite = 1;
        m_caps.m_blendFactor = 1; m_caps.m_shaderAlphaRef = 0;
        m_caps.m_hwCursor = 0; m_caps.m_occlusionQuery = 0;
        m_context = m_windowVisible = 1;
        m_windowFocus = (SDL_GetWindowFlags(state.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        ICursorCreate(m_format);
        SDL_ShowCursor(SDL_DISABLE);
        OsGuiSetNativeWindowEventProc(WindowEvent, this);
        SDL_PumpEvents();
        DeviceWM(GxWM_Size, 0, 0);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native Vulkan initialization: %s\n", error.what());
        CGxDevice::Log("Native Vulkan initialization: %s", error.what());
        DeviceDestroy();
        return 0;
    }
}
void CGxDeviceVulkan::DeviceDestroy() {
    if (!m_native) return;
    if (GxGetDeviceSelection().activeBackend == "vulkan") GxReportDeviceSelection("", {}, 0);
    if (OsGuiGetWindow(0) == m_native->window) OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    ICursorDestroy();
    m_context = m_windowVisible = m_windowFocus = 0;
    m_native->Destroy();
    CGxDevice::DeviceDestroy();
    SDL_ShowCursor(SDL_ENABLE);
}
void* CGxDeviceVulkan::DeviceWindow() { return m_native->window; }
int32_t CGxDeviceVulkan::DeviceWM(EGxWM wm, uintptr_t, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size: {
            m_native->resize = true;
            m_windowVisible = param2 != 1 && param2 != 2;
            if (m_windowVisible) {
                int width, height;
                SDL_Vulkan_GetDrawableSize(m_native->window, &width, &height);
                if (width > 0 && height > 0) {
                    CRect rect;
                    rect.minX = rect.minY = 0;
                    rect.maxX = static_cast<float>(width); rect.maxY = static_cast<float>(height);
                    // Size callbacks measure fonts before the deferred swapchain
                    // recreation. Publish the new dimensions synchronously.
                    DeviceSetDefWindow(rect);
                    m_viewportDirty = 1;
                }
            }
            return 1;
        }
        case GxWM_SetFocus: m_windowFocus = 1; return 1;
        case GxWM_KillFocus: m_windowFocus = 0; return 1;
        case GxWM_DisplayChange: m_native->resize = true; return 1;
        case GxWM_Destroy: DeviceDestroy(); return 1;
    }
    return 0;
}
void CGxDeviceVulkan::CapsWindowSize(CRect& rect) { rect = DeviceCurWindow(); }
void CGxDeviceVulkan::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = DeviceCurWindow();
    int x = 0, y = 0;
    SDL_GetWindowPosition(m_native->window, &x, &y);
    rect.minX += x; rect.maxX += x; rect.minY += y; rect.maxY += y;
}
void CGxDeviceVulkan::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);
    // Convert GL's [-w,w] clip depth to Vulkan's [0,w], keeping the engine's
    // matrix convention. A negative viewport height handles the Y convention.
    m_projNative = matrix;
    m_projNative.a2 = (matrix.a2 + matrix.a3) * 0.5f;
    m_projNative.b2 = (matrix.b2 + matrix.b3) * 0.5f;
    m_projNative.c2 = (matrix.c2 + matrix.c3) * 0.5f;
    m_projNative.d2 = (matrix.d2 + matrix.d3) * 0.5f;
}
#include "TemporalResolve.inl"
#include "HdrScene.inl"
#include "FarResources.inl"
#include "WorldShadows.inl"

void CGxDeviceVulkan::State::PollFarCompletion() {
    if(productionLease)return;
    if (!device || !fence) return;
    const auto result = vkGetFenceStatus(device, fence);
    if (result == VK_SUCCESS) completedSerial = submittedSerial;
    else if (result != VK_NOT_READY) VkCheck(result, "vkGetFenceStatus");
}

#ifdef WOWDECOMP_VULKAN_TESTING
std::array<uint64_t,4> CGxDeviceVulkan::SceneHdrTestLutState() const {
    return {m_native->HdrLutBytes(),m_native->hdrLutSize,m_native->hdrLutJob?1u:0u,m_native->retiredHdrLuts.size()};
}
std::array<uint64_t,8> CGxDeviceVulkan::SceneHdrTestLutMemory() const {
    const auto& s=*m_native;
    const auto allocation=[&](const NativeBuffer& b) { VkMemoryRequirements r{};if(b.buffer) vkGetBufferMemoryRequirements(s.device,b.buffer,&r);return r.size; };
    uint64_t retired=0;for(const auto& buffer:s.retiredHdrLuts) retired+=allocation(buffer);
    return {s.hdrLutData.size()*sizeof(s.hdrLutData[0]),s.hdrLutData.capacity()*sizeof(s.hdrLutData[0]),
        allocation(s.hdrLutBuffer),retired,s.hdrLutJob?GxVulkan::HdrLutJob::Reservation:0,s.hdrLutPeakBytes,s.hdrLutOwnerMoved?1u:0u,s.hdrLutUploadBytes};
}
bool CGxDeviceVulkan::SceneHdrTestValidationClean() const { return m_native->testDiagnostics.load()==0; }
void CGxDeviceVulkan::SceneHdrTestLimits(uint32_t slots, uint64_t budget, uint32_t draw) {
    SceneEndWorld();m_native->ResetTemporal();m_native->hdrAttachmentLimit=slots;m_native->hdrImageBudget=budget;m_native->hdrBoundaryFailDraw=draw;
}
#endif

void CGxDeviceVulkan::SceneRequestHdrLut(const char* path, bool reload) {
    auto& s=*m_native;
    if(!GxHdrLutPathValid(path)) { s.hdrLutStatus="Invalid .cube path; current LUT retained";return; }
    const std::string requested=path;
    if(reload || requested!=s.hdrLutRequested) {
        ++s.hdrLutGeneration;
        s.hdrLutRequested=requested;
        s.hdrLutNeedsLoad=true;
        s.hdrLutStatus="Pending (current LUT retained)";
    }
    if(s.hdrLutJob && s.hdrLutJob->Ready()) {
        const bool current=s.hdrLutJobGeneration==s.hdrLutGeneration && s.hdrLutNeedsLoad;
        GxVulkan::HdrLut prepared;
        std::string error;
        if(current) {
            error=std::move(s.hdrLutJob->error);
            prepared=std::move(s.hdrLutJob->lut);
        }
        // Ready's acquire publication follows stream/parser destruction. Move
        // only the RGB owner, then drain the finished thread and its scratch
        // before admitting GPU replacement. Install charges prepared once.
#ifdef WOWDECOMP_VULKAN_TESTING
        const auto* preparedOwner=prepared.rgb.data();
#endif
        s.hdrLutJob.reset();
        if(current) {
            if(!error.empty()) s.hdrLutStatus=error+"; current LUT retained";
            else s.hdrLutStatus=s.InstallHdrLut(std::move(prepared))?"Loaded (scene only)":"LUT budget/upload refused; current LUT retained";
#ifdef WOWDECOMP_VULKAN_TESTING
            s.hdrLutOwnerMoved=error.empty() && s.hdrLutData.data()==preparedOwner;
#endif
            s.hdrLutNeedsLoad=false;
        }
    }
    if(!s.hdrLutNeedsLoad || s.hdrLutJob) return;
    if(s.hdrLutRequested.empty()) {
        s.hdrLutStatus=SceneSetHdrLut(nullptr,0)?"None":"Cannot retire LUT; current LUT retained";s.hdrLutNeedsLoad=false;return;
    }
    if(!SceneHdrSupported()) {
        s.hdrLutStatus="LUT unsupported; waiting for a supported device";return;
    }
    if(!GxVulkan::HdrLutJob::ValidPath(s.hdrLutRequested)) {
        s.hdrLutStatus="Invalid .cube path; current LUT retained";s.hdrLutNeedsLoad=false;return;
    }
    constexpr uint64_t budget=GxVulkan::HdrLutBudget;
    const auto used=s.HdrLutBytes();
    if(used>budget || GxVulkan::HdrLutJob::Reservation>budget-used) {
        s.hdrLutStatus="LUT job budget refused; current LUT retained";s.hdrLutNeedsLoad=false;return;
    }
    try {
        s.hdrLutJobPath=s.hdrLutRequested;
        s.hdrLutJobGeneration=s.hdrLutGeneration;
        s.hdrLutJob=std::make_unique<GxVulkan::HdrLutJob>(s.hdrLutJobPath);
        s.hdrLutStatus="Pending (current LUT retained)";
    } catch(const std::exception&) {
        s.hdrLutStatus="LUT worker unavailable; current LUT retained";s.hdrLutNeedsLoad=false;
    }
}
const char* CGxDeviceVulkan::SceneHdrLutStatus() const { return m_native->hdrLutStatus.c_str(); }
bool CGxDeviceVulkan::SceneSetHdrLut(const float* rgb, uint32_t size) { return m_native->SetHdrLut(rgb,size); }
bool CGxDeviceVulkan::State::SetHdrLut(const float* rgb, uint32_t size) {
    auto& s=*this;
    if(!size) {
        // Descriptors from in-flight frames may still refer to this buffer.
        try {
            if(s.hdrLutBuffer.buffer) {
                if(s.retiredHdrLuts.size()==s.retiredHdrLuts.capacity()) {
                    const auto used=s.HdrLutBytes();
                    const auto metadata=(s.retiredHdrLuts.size()+1)*sizeof(NativeBuffer);
                    if(used>GxVulkan::HdrLutBudget || metadata>GxVulkan::HdrLutBudget-used) return false;
                    s.retiredHdrLuts.reserve(s.retiredHdrLuts.size()+1);
                }
                s.retiredHdrLuts.push_back(s.hdrLutBuffer);
            }
        }
        catch(const std::exception&) { return false; }
        s.hdrLutBuffer={};
        std::vector<std::array<float,3>>().swap(s.hdrLutData);
        s.hdrLutSize=0;s.ResetTemporal();return true;
    }
    if(!rgb || size<2 || size>33) return false;
    const size_t count=size_t(size)*size*size;
    for(size_t i=0;i<count*3;++i) if(!std::isfinite(rgb[i]) || rgb[i]<0 || rgb[i]>1) return false;
    constexpr uint64_t budget=GxVulkan::HdrLutBudget;
    const uint64_t cpu=count*sizeof(std::array<float,3>), gpu=count*16u, used=s.HdrLutBytes();
    if(used>budget || cpu>budget-used || gpu>budget-used-cpu) return false;
    try {
        GxVulkan::HdrLut lut;lut.size=size;lut.rgb.resize(count);
        for(size_t i=0;i<count;++i) for(unsigned c=0;c<3;++c) lut.rgb[i][c]=rgb[i*3+c];
        return s.InstallHdrLut(std::move(lut));
    } catch(const std::exception& e) { GxLog("HDR LUT replacement refused: %s",e.what());return false; }
}
bool CGxDeviceVulkan::State::InstallHdrLut(GxVulkan::HdrLut&& lut) {
    if(lut.size<2 || lut.size>33 || lut.rgb.size()!=size_t(lut.size)*lut.size*lut.size) return false;
    for(const auto& row:lut.rgb) for(float value:row)
        if(!std::isfinite(value) || value<0 || value>1) return false;
    const uint64_t used=HdrLutBytes(), cpu=lut.rgb.capacity()*sizeof(lut.rgb[0]);
    if(used>GxVulkan::HdrLutBudget || cpu>GxVulkan::HdrLutBudget-used) return false;
    try {
        if(device) UploadHdrLut(lut.rgb);
        hdrLutData=std::move(lut.rgb);hdrLutSize=lut.size;
    } catch(const std::exception& e) { GxLog("HDR LUT replacement refused: %s",e.what());return false; }
    ResetTemporal();return true;
}
bool CGxDeviceVulkan::SceneReadHdrPixel(uint32_t x, uint32_t y, std::array<float,4>& value) {
    auto& s=*m_native;
    if (!s.HdrDraw() || x>=s.renderExtent.width || y>=s.renderExtent.height) return false;
    s.EndPass(); auto buffer=s.MakeBuffer(16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        s.Barrier(s.worldColour.image,VK_IMAGE_ASPECT_COLOR_BIT,1,1,s.worldColour.layout,VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageOffset={int32_t(x),int32_t(y),0};copy.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(s.command,s.worldColour.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer.buffer;host.size=16;
        vkCmdPipelineBarrier(s.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        s.Submit(false,true); std::memcpy(value.data(),buffer.mapped,16);
    } catch(...) {s.DestroyBuffer(buffer);throw;}
    s.DestroyBuffer(buffer); return true;
}
bool CGxDeviceVulkan::SceneHdrSupported() const { return m_native && m_native->device && m_native->hdrCapable && m_native->properties.limits.maxColorAttachments>=3 && m_native->hdrAttachmentLimit>=3; }
bool CGxDeviceVulkan::SceneHdrActive() const { return m_native && m_native->hdrRequested && m_native->hdrActive && !m_native->hdrAborted && m_native->worldActive; }
bool CGxDeviceVulkan::SceneSetHdr(bool enabled, float ev) {
    if (!std::isfinite(ev) || ev < -8 || ev > 8 || (enabled && !SceneHdrSupported())) return false;
    auto& s=*m_native;
    if (s.hdrRequested == enabled && s.hdrEV == ev) return true;
    SceneEndWorld(); s.ResetTemporal(); s.hdrRequested=enabled; s.hdrEV=ev;
    return true;
}
bool CGxDeviceVulkan::SceneSetLinearOutput(bool linear) {
    const bool previous=m_native->linearOutput; m_native->linearOutput=linear; return previous;
}


bool CGxDeviceVulkan::SceneTemporalSupported() const { return m_native->device && m_native->temporalCapable; }
bool CGxDeviceVulkan::SceneRenderScaleSupported() const { return m_native->device && m_native->scaleCapable; }
bool CGxDeviceVulkan::SceneSetRenderScale(float scale) {
    if (!std::isfinite(scale) || scale < .5f || scale > 1.f ||
        (scale != 1.f && !SceneRenderScaleSupported())) return false;
    auto& state = *m_native;
    if (scale == state.sceneScale) return true;
    SceneEndWorld();
    state.ResetTemporal();
    state.sceneScale = scale;
    return true;
}
void CGxDeviceVulkan::ScenePauseWorldMotion() {
    // FFX copies in native screen coordinates and then writes the default
    // output target. Reconstruct before it, not after its explicit-target phase.
    if (m_native->worldActive && (m_native->sceneScale != 1.f || m_native->hdrActive)) SceneEndWorld();
    m_native->EndPass(); m_native->motionPaused = true;
    m_native->motionOwner = nullptr; m_native->stableVertices = false;
}
void CGxDeviceVulkan::SceneInvalidateHistory() {
    m_native->EndPass(); m_native->ResetTemporal(); m_native->temporalActive = false;
}
void CGxDeviceVulkan::SceneSetMotionOwner(const void* owner, bool stableVertices) {
    m_native->motionOwner = owner; m_native->stableVertices = stableVertices;
}
void CGxDeviceVulkan::SceneForgetMotionOwner(const void* owner) {
    m_native->temporalDraws.ForgetOwner(owner);
    if (m_native->motionOwner == owner) SceneSetMotionOwner(nullptr, false);
}
void CGxDeviceVulkan::SceneConfigureShadows(bool requested,bool strict) {
    if(m_native->shadowRequested!=requested||m_native->shadowStrict!=strict)
        std::fprintf(stderr,"RT diagnostic settings: visibilityMask=%s incompleteInputSafety=%s (separate from terrain shadows)\n",requested?"On":"Off",strict?"On":"Off");
    if(requested&&!m_native->shadowRequested){m_native->loggedCasterIssues.clear();m_native->loggedReceiverIssues.clear();}
    m_native->shadowRequested=requested;m_native->shadowStrict=strict;
    if(!requested){m_native->shadowSource.reset();m_native->shadowStatus={};}
    else if(!m_native->rayQueryCapabilities.Available())m_native->shadowStatus.result=GxShadow::FrameResult::Unavailable;
    else if(m_native->shadowStatus.result==GxShadow::FrameResult::Disabled)m_native->shadowStatus.result=GxShadow::FrameResult::PendingGeometry;
}
bool CGxDeviceVulkan::SceneShadowsSupported() const {return m_native->rayQueryCapabilities.Available();}
bool CGxDeviceVulkan::SceneShadowsRequested() const {
    return (m_native->shadowRequested || m_native->terrainShadowActive) && m_native->rayQueryCapabilities.Available() && m_native->worldActive;
}
void CGxDeviceVulkan::SceneSetShadowFrame(std::shared_ptr<const GxShadow::Frame> frame){
    if(SceneShadowsRequested()){
        m_native->shadowSource=std::move(frame);m_native->EndPass();m_native->RecordWorldShadowVisibility(m_native->terrainShadowActive);
        if(m_native->terrainShadowActive && !m_native->shadowCurrent)m_native->terrainShadowFailed=true;
    }
}
GxShadow::Status CGxDeviceVulkan::SceneShadowStatus() const {
    const auto& s=*m_native;
    if(s.independentWorld && !s.productionCapture){
        if(s.displayedWorldSlot)return s.completedImages[*s.displayedWorldSlot].lighting;
        GxShadow::Status status;status.result=(s.shadowRequested||s.terrainShadowRequested)?GxShadow::FrameResult::PendingGeometry:GxShadow::FrameResult::Disabled;
        return status;
    }
    return s.shadowStatus;
}
void CGxDeviceVulkan::SceneConfigureTerrainShadows(bool requested){
    if(m_native->terrainShadowRequested!=requested){
        std::fprintf(stderr,"Terrain RT request: %s (basic near-terrain receivers; M2/WMO cast only)\n",requested?"On":"Off");
        m_native->terrainLoggedState.clear();
        if(requested){m_native->loggedCasterIssues.clear();m_native->loggedReceiverIssues.clear();}
    }
    m_native->terrainShadowRequested=requested;
    if(requested&&m_native->device&&m_native->shadowTimestampBits&&!m_native->shadowTimingPool){
        auto& s=*m_native;s.EndPass();
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=4;
        if(vkCreateQueryPool(s.device,&info,nullptr,&s.shadowTimingPool)==VK_SUCCESS&&s.recording)vkCmdResetQueryPool(s.command,s.shadowTimingPool,0,4);
    }
}
bool CGxDeviceVulkan::SceneTerrainShadowsRequested() const {return m_native->terrainShadowActive&&m_native->worldActive;}
void CGxDeviceVulkan::SceneSetTerrainReceiver(bool enabled){
    auto& s=*m_native;s.terrainReceiver=enabled;
    if(s.terrainShadowActive&&s.shadowTimingPool){s.EndPass();vkCmdWriteTimestamp(s.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.shadowTimingPool,enabled?2:3);s.shadowTimingMask|=enabled?4u:8u;}
}
void CGxDeviceVulkan::SceneFinishTerrainShadows(){
    auto& s=*m_native;if(!s.terrainShadowActive)return;
    s.EndPass();
    if(s.terrainShadowFailed || !s.shadowStatus.terrainReceivers){
        s.Barrier(s.hdrClassic.image,s.hdrClassic.aspect,1,1,s.hdrClassic.layout,VK_IMAGE_LAYOUT_GENERAL);s.hdrClassic.layout=VK_IMAGE_LAYOUT_GENERAL;
        s.Barrier(s.worldColour.image,s.worldColour.aspect,1,1,s.worldColour.layout,VK_IMAGE_LAYOUT_GENERAL);s.worldColour.layout=VK_IMAGE_LAYOUT_GENERAL;
        GxVulkan::BlitWorldImage(s.command,s.hdrClassic.image,s.worldColour.image,VK_IMAGE_ASPECT_COLOR_BIT,s.renderRect,s.renderRect,VK_FILTER_NEAREST);
    }else s.shadowStatus.terrainRendered=true;
}
bool CGxDeviceVulkan::SceneBeginWorld(bool temporal, uint64_t scene) {
    auto& state = *m_native;
    if (temporal && !SceneTemporalSupported()) return false;
    if ((!temporal && !state.productionCapture && !state.worldTargetEnabled && !state.hdrRequested && !((state.shadowRequested||state.terrainShadowRequested) && state.rayQueryCapabilities.Available()) && state.sceneScale == 1.f) || !m_context || !state.BeginFrame()) return false;
    state.shadowSource.reset();state.shadowCurrent.reset();
    if(state.shadowRequested){state.shadowStatus={};state.shadowStatus.result=GxShadow::FrameResult::PendingGeometry;}
    // One world history producer per acquired frame. Readback splits do not
    // advance bufferFrame and must not create a second temporal generation.
    if (state.historyPending || (temporal && state.temporalWorldFrame == state.bufferFrame)) return false;
    if (state.worldActive) throw std::logic_error("Nested Vulkan world target");
    // Other Gx render targets (e.g. previews) retain their own ownership/coordinates.
    if (m_textureTarget[GxBuffers_Color].m_texture || m_textureTarget[GxBuffers_Depth].m_texture) return false;
    state.EndPass();
    state.worldRect = GxVulkan::WorldViewportRect(state.extent,
        m_viewport.m_x.m_l, m_viewport.m_x.m_h, m_viewport.m_y.m_l, m_viewport.m_y.m_h);
    if (!state.worldRect.extent.width || !state.worldRect.extent.height) return false;
    state.renderExtent = GxVulkan::WorldRenderExtent(state.extent, state.sceneScale,
        std::min(state.properties.limits.maxImageDimension2D,
            std::min(state.properties.limits.maxFramebufferWidth, state.properties.limits.maxFramebufferHeight)));
    state.renderRect = GxVulkan::WorldViewportRect(state.renderExtent,
        m_viewport.m_x.m_l, m_viewport.m_x.m_h, m_viewport.m_y.m_l, m_viewport.m_y.m_h);
    state.hdrAborted = false; state.hdrDraws = 0;
    state.terrainShadowActive=state.terrainShadowRequested && state.shadowTextureCapacity && SceneHdrSupported() && !state.hdrRequested && !temporal && state.sceneScale==1.f;
    state.terrainShadowFailed=false;state.terrainReceiver=false;
    if(state.terrainShadowRequested&&!state.terrainShadowActive){
        state.shadowStatus.result=GxShadow::FrameResult::Unavailable;
        if(!state.shadowTextureCapacity||!SceneHdrSupported())state.shadowStatus.detail="Required terrain query/output features unavailable on this renderer/device";
        else if(state.hdrRequested)state.shadowStatus.detail="HDR is On: turn off float scene transport in Scene Colour and Apply";
        else if(temporal)state.shadowStatus.detail="AA is On: turn off experimental AA in Landscape & AA and Apply";
        else if(state.sceneScale!=1.f)state.shadowStatus.detail="Scene scale is not 100%: set it to 100% in Landscape & AA and Apply";
    }
    state.hdrActive = (state.hdrRequested||state.terrainShadowActive) && SceneHdrSupported();
    if (state.hdrActive && !state.PrepareHdrImages(temporal)) {
        GxLog("HDR attachment set refused; using Classic rendering for this frame");
        if(state.terrainShadowActive)state.shadowStatus.result=GxShadow::FrameResult::AllocationFailed;
        state.hdrActive = false;state.terrainShadowActive=false; state.ResetTemporal();
    }
    if (!state.hdrActive && (state.worldColour.width != state.renderExtent.width || state.worldColour.height != state.renderExtent.height ||
        state.worldColour.format != state.surfaceFormat)) {
        state.ResetTemporal();
        // Retire after framebuffers, at the next completed fence, just like Gx textures.
        if (state.worldColour.image) state.retiredImages.push_back(state.worldColour);
        if (state.worldDepth.image) state.retiredImages.push_back(state.worldDepth);
        state.worldColour = {}; state.worldDepth = {};
        const auto transfers = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        state.worldColour = state.MakeImage(state.renderExtent.width, state.renderExtent.height, 1, 1,
            state.surfaceFormat, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
        state.worldDepth = state.MakeImage(state.renderExtent.width, state.renderExtent.height, 1, 1,
            VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
    }
    state.temporalActive = temporal;
    state.motionPaused = false;
    state.motionOwner = nullptr; state.stableVertices = false;
    if (temporal || state.sceneScale != 1.f) {
        state.temporalWorldFrame = state.bufferFrame;
        if (state.motion.width != state.renderExtent.width || state.motion.height != state.renderExtent.height ||
            state.history[0].width != state.extent.width || state.history[0].height != state.extent.height ||
            state.history[0].format != state.WorldFormat() || (state.sceneScale != 1.f && !state.depthScratch.image)) {
            state.ResetTemporal();
            for (auto* image : {&state.motion, &state.history[0], &state.history[1], &state.previousDepth, &state.depthScratch}) {
                if (image->image) state.retiredImages.push_back(*image);
                *image = {};
            }
            const auto transfers = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            state.motion = state.MakeImage(state.renderExtent.width, state.renderExtent.height, 1, 1,
                VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
            for (auto& history : state.history)
                history = state.MakeImage(state.extent.width, state.extent.height, 1, 1,
                    state.WorldFormat(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
            state.previousDepth = state.MakeImage(state.extent.width, state.extent.height, 1, 1,
                VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
            if (state.sceneScale != 1.f)
                state.depthScratch = state.MakeImage(state.extent.width, state.extent.height, 1, 1,
                    VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | transfers);
        }
        for (auto* image : {&state.motion, &state.history[0], &state.history[1], &state.previousDepth, &state.depthScratch}) {
            if (!image->image) continue;
            const bool fresh = image->layout == VK_IMAGE_LAYOUT_UNDEFINED;
            state.Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_GENERAL);
            image->layout = VK_IMAGE_LAYOUT_GENERAL;
            const VkImageSubresourceRange range{image->aspect,0,1,0,1};
            if (image == &state.motion || fresh) {
                if (image->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
                    const VkClearDepthStencilValue depth{1,0};
                    vkCmdClearDepthStencilImage(state.command, image->image, image->layout, &depth, 1, &range);
                } else {
                    const VkClearColorValue colour{{0,0,image == &state.motion ? -1.f : 0.f,1}};
                    vkCmdClearColorImage(state.command, image->image, image->layout, &colour, 1, &range);
                }
                state.Barrier(image->image, image->aspect, 1, 1, image->layout, image->layout);
            }
        }
        const auto& rect = state.worldRect;
        state.temporalFrame = state.temporalHistory.Begin({state.productionCapture?state.productionLease->generation:state.bufferFrame, scene,
            state.extent.width, state.extent.height,
            {uint32_t(rect.offset.x),uint32_t(rect.offset.y),rect.extent.width,rect.extent.height},temporal,false});
        if (!state.temporalFrame.historyValid) { state.temporalDraws.Reset(); state.upscaleSample = 0; }
        if (state.sceneScale != 1.f)
            state.temporalFrame.jitterPixels = GxVulkan::TemporalUpscaleJitter(state.upscaleSample);
    } else state.ResetTemporal();
    for (auto* image : {&state.worldColour, &state.worldDepth}) {
        state.Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_GENERAL);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const VkRect2D full{{0, 0}, state.extent};
    if (state.hdrActive) {
        for (auto* image : {&state.hdrBackdrop, &state.hdrClassic}) {
            state.Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_GENERAL);
            image->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        GxVulkan::CopyWorldImage(state.command, state.swapImages[state.imageIndex], state.hdrBackdrop.image, VK_IMAGE_ASPECT_COLOR_BIT, full);
        if (state.sceneScale == 1.f) {
            GxVulkan::CopyWorldImage(state.command, state.swapImages[state.imageIndex], state.hdrClassic.image, VK_IMAGE_ASPECT_COLOR_BIT, full);
            GxVulkan::CopyWorldImage(state.command, state.depth.image, state.worldDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, full);
        } else {
            const VkRect2D internal{{0,0}, state.renderExtent};
            GxVulkan::BlitWorldImage(state.command, state.swapImages[state.imageIndex], state.hdrClassic.image, VK_IMAGE_ASPECT_COLOR_BIT, full, internal);
            GxVulkan::BlitWorldImage(state.command, state.depth.image, state.worldDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, full, internal);
        }
        try {
            if(state.terrainShadowActive)GxVulkan::BlitWorldImage(state.command,state.hdrBackdrop.image,state.worldColour.image,VK_IMAGE_ASPECT_COLOR_BIT,full,full,VK_FILTER_NEAREST);
            else state.HdrTransfer(state.hdrBackdrop, state.worldColour.view, state.WorldFormat(), state.renderExtent, 0, false);
        }
        catch (const std::exception& error) { state.AbortHdr(error.what()); }
    } else if (state.sceneScale == 1.f) {
        GxVulkan::CopyWorldImage(state.command, state.swapImages[state.imageIndex], state.worldColour.image,
            VK_IMAGE_ASPECT_COLOR_BIT, full);
        GxVulkan::CopyWorldImage(state.command, state.depth.image, state.worldDepth.image,
            VK_IMAGE_ASPECT_DEPTH_BIT, full);
    } else {
        const VkRect2D internal{{0,0},state.renderExtent};
        GxVulkan::BlitWorldImage(state.command, state.swapImages[state.imageIndex], state.worldColour.image,
            VK_IMAGE_ASPECT_COLOR_BIT, full, internal);
        GxVulkan::BlitWorldImage(state.command, state.depth.image, state.worldDepth.image,
            VK_IMAGE_ASPECT_DEPTH_BIT, full, internal);
    }
    state.worldActive = true;
    return true;
}
void CGxDeviceVulkan::SceneEndWorld() {
    auto& state = *m_native;
    if (!state.worldActive) return;
    state.EndPass();
    // FFX owns its colour snapshot and may overwrite raster depth. Query before
    // FFX, publish the diagnostic afterwards rather than postprocess the mask.
    if(state.shadowCurrent&&state.shadowCurrent->output.image){
        state.Barrier(state.worldColour.image,state.worldColour.aspect,1,1,state.worldColour.layout,VK_IMAGE_LAYOUT_GENERAL);
        state.worldColour.layout=VK_IMAGE_LAYOUT_GENERAL;
        GxVulkan::BlitWorldImage(state.command,state.shadowCurrent->output.image,state.worldColour.image,VK_IMAGE_ASPECT_COLOR_BIT,state.renderRect,state.renderRect,VK_FILTER_NEAREST);
    }
    try {
        if (state.hdrAborted) state.PublishClassic();
        else if (state.temporalActive || state.sceneScale != 1.f) state.ResolveWorld();
        else if (state.terrainShadowActive) GxVulkan::BlitWorldImage(state.command,state.worldColour.image,state.swapImages[state.imageIndex],VK_IMAGE_ASPECT_COLOR_BIT,state.renderRect,state.worldRect,VK_FILTER_NEAREST);
        else if (state.hdrActive) state.ToneWorld(state.worldColour);
        else GxVulkan::CopyWorldImage(state.command, state.worldColour.image, state.swapImages[state.imageIndex],
            VK_IMAGE_ASPECT_COLOR_BIT, state.worldRect);
    } catch (const std::exception& error) {
        if (!state.hdrActive) throw;
        state.AbortHdr(error.what()); state.PublishClassic();
    }
    // Keep output-sized depth consumers (picking/readback) on the existing contract.
    if (state.sceneScale == 1.f)
        GxVulkan::CopyWorldImage(state.command, state.worldDepth.image, state.depth.image,
            VK_IMAGE_ASPECT_DEPTH_BIT, state.worldRect);
    else state.PublishWorldDepth(state.depth.image);
    if(state.terrainShadowActive&&state.hdrAborted){state.shadowStatus.terrainRendered=false;state.shadowStatus.result=GxShadow::FrameResult::AllocationFailed;state.shadowStatus.detail="World output transport failed; original raster retained";}
    if(state.terrainShadowRequested){
        const std::string current=std::to_string(unsigned(state.shadowStatus.result))+":"+(state.shadowStatus.terrainRendered?"running":"raster")+":"+state.shadowStatus.detail;
        if(current!=state.terrainLoggedState){
            state.terrainLoggedState=current;
            std::fprintf(stderr,"Terrain RT state: %s reason=%u detail=%s receivers=%llu triangles=%llu terrain=%llu models=%llu buildings=%llu region=64 rayDistance=128\n",
                state.shadowStatus.terrainRendered?"Running":"Raster retained",unsigned(state.shadowStatus.result),state.shadowStatus.detail.empty()?"none":state.shadowStatus.detail.c_str(),
                (unsigned long long)state.shadowStatus.terrainReceivers,(unsigned long long)state.shadowStatus.triangles,(unsigned long long)state.shadowStatus.terrain,(unsigned long long)state.shadowStatus.models,(unsigned long long)state.shadowStatus.buildings);
        }
    }
#ifdef WOWDECOMP_VULKAN_TESTING
    state.completedWorldTest={uint32_t(state.hdrRequested && state.hdrActive && !state.hdrAborted),uint32_t(state.temporalActive),
        state.renderRect.extent.width,state.renderRect.extent.height};
#endif
    state.publishedHdr=state.hdrRequested && state.hdrActive && !state.hdrAborted;
    state.publishedTemporal=state.temporalActive;
    state.worldActive = state.temporalActive = state.hdrActive = false;
    state.motionOwner = nullptr; state.stableVertices = false;
}
bool CGxDeviceVulkan::SceneCaptureCompletedWorld(const GxWorld::WorldFrameDescription& frame) {
    auto& state = *m_native;
    if (state.independentWorld || !m_context || !(state.worldTargetEnabled || state.temporalCapable || state.scaleCapable) || !state.BeginFrame()) return false;
    if (state.worldActive || m_textureTarget[GxBuffers_Color].m_texture || m_textureTarget[GxBuffers_Depth].m_texture)
        throw std::logic_error("World publication requires finished default-target output");
    state.completedWorld.Configure(frame.scene, state.extent.width, state.extent.height);
    state.completedWorld.Poll(state.completedSerial, state.completedSerial);
    state.completedWorld.Latest();
    const auto lease = state.completedWorld.Begin(frame);
    if (!lease) return false;
    auto& images = state.completedImages[lease->slot];
    if (images.colour.width != frame.width || images.colour.height != frame.height || images.colour.format != state.surfaceFormat) {
        NativeImage colour, depth;
        try {
            const auto transfers = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            colour = state.MakeImage(frame.width, frame.height, 1, 1, state.surfaceFormat,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | transfers);
            depth = state.MakeImage(frame.width, frame.height, 1, 1, VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | transfers);
        } catch (const std::exception& error) {
            state.DestroyImage(colour); state.DestroyImage(depth); state.completedWorld.Abort(*lease);
            if (state.completedWorldAllocationError != error.what()) {
                state.completedWorldAllocationError = error.what();
                GxLog("Completed world allocation refused; synchronous output retained: %s", error.what());
            }
            return false;
        }
        state.DestroyImage(images.colour); state.DestroyImage(images.depth);
        images.colour = colour; images.depth = depth;
    }
    state.EndPass();
    for (auto* image : {&images.colour, &images.depth}) {
        state.Barrier(image->image, image->aspect, 1, 1, image->layout, VK_IMAGE_LAYOUT_GENERAL);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const auto& rect = frame.viewport;
    const VkRect2D pixels{{int32_t(rect[0]),int32_t(rect[1])},{rect[2],rect[3]}};
    GxVulkan::CopyWorldImage(state.command, state.swapImages[state.imageIndex], images.colour.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels);
    GxVulkan::CopyWorldImage(state.command, state.depth.image, images.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, pixels);
    // Publication stays Pending until the recording's real fence completes.
    state.completedWorld.Submit(*lease, state.submittedSerial + 1);
    state.completedWorldAllocationError.clear();
    return true;
}
bool CGxDeviceVulkan::SceneComposeCompletedWorld(uint64_t scene, GxWorld::WorldFrame* displayed) {
    auto& state = *m_native;
    if (!m_context || !(state.worldTargetEnabled || state.temporalCapable || state.scaleCapable) || !state.BeginFrame()) return false;
    if (state.worldActive || m_textureTarget[GxBuffers_Color].m_texture || m_textureTarget[GxBuffers_Depth].m_texture)
        throw std::logic_error("World composition requires the default presentation target");
    state.completedWorld.Configure(scene, state.extent.width, state.extent.height);
    state.PollWorld();
    state.completedWorld.Poll(state.independentWorld?state.worldCompleted:state.completedSerial,state.completedSerial);
    const auto lease = state.completedWorld.Latest();
    if (!lease) return false;
    const auto& frame = state.completedWorld.Get(*lease).frame;
    const auto& rect = frame.viewport;
    const VkRect2D pixels{{int32_t(rect[0]),int32_t(rect[1])},{rect[2],rect[3]}};
    const auto& images = state.completedImages[lease->slot];
    state.EndPass();
    GxVulkan::CopyWorldImage(state.command, images.colour.image, state.swapImages[state.imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, pixels);
    GxVulkan::CopyWorldImage(state.command, images.depth.image, state.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, pixels);
    state.completedWorld.Consume(*lease, state.submittedSerial + 1);
    if(state.independentWorld){state.displayedWorldSlot=lease->slot;state.displayedWorldGeneration=lease->generation;state.displayedAgeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-state.worldBorn[lease->slot]).count();}
    if (displayed) *displayed = {*lease,frame};
    return true;
}
void CGxDeviceVulkan::SceneInvalidateCompletedWorld() {
    m_native->completedWorld.Invalidate();m_native->displayedWorldSlot.reset();m_native->displayedWorldGeneration=0;m_native->ResetTemporal();
}
#ifdef WOWDECOMP_VULKAN_TESTING
bool CGxDeviceVulkan::SceneWorldTestHistoryValid() const{return m_native->temporalFrame.historyValid;}
std::array<uint32_t,4> CGxDeviceVulkan::SceneWorldTestCompletedFrame() const { return m_native->completedWorldTest; }
void CGxDeviceVulkan::SceneShadowTestCasters(bool models,bool buildings){m_native->shadowTestModels=models;m_native->shadowTestBuildings=buildings;}
#include "DirectionalLightingTest.inl"
bool CGxDeviceVulkan::SceneRayQueryTestReadback() {
    auto& state = *m_native;
    if (!state.device || !state.rayQueryCapabilities.Available() || !state.BeginFrame()) return false;
    state.EndPass();
    const auto before = state.sceneAdmission->Used();
    struct Ray { float originMin[4], directionMax[4]; };
    struct Result { uint32_t hit, primitive; float distance; uint32_t padding; };
    static_assert(sizeof(Ray) == 32 && sizeof(Result) == 16, "Acceleration probe ABI");
    const Ray rays[] = {
        {{.2f,.2f,1,.001f},{0,0,-1,10}}, {{2,2,1,.001f},{0,0,-1,10}},
        {{.2f,.2f,-1,.001f},{0,0,-1,10}}, {{.2f,.2f,.005f,.01f},{0,0,-1,10}},
        {{.2f,.2f,1,.001f},{0,0,1,10}}, {{.2f,.2f,1,.001f},{0,0,-1,.5f}},
        {{.2f,.2f,-1,.001f},{0,0,1,10}}
    };
    constexpr size_t count = sizeof(rays)/sizeof(rays[0]);
    GxVulkan::Buffer input, output;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::unique_ptr<GxVulkan::AccelerationScene> scene;
    const auto cleanup = [&] {
        if (pipeline) vkDestroyPipeline(state.device,pipeline,nullptr);
        if (module) vkDestroyShaderModule(state.device,module,nullptr);
        if (layout) vkDestroyPipelineLayout(state.device,layout,nullptr);
        if (pool) vkDestroyDescriptorPool(state.device,pool,nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(state.device,setLayout,nullptr);
        GxVulkan::DestroyBuffer(state.device,input); GxVulkan::DestroyBuffer(state.device,output);
        scene.reset();
    };
    bool passed = false;
    try {
        const auto memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        input = GxVulkan::CreateBuffer(state.device,state.memoryProperties,sizeof(rays),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,memory,state.sceneAdmission.get());
        output = GxVulkan::CreateBuffer(state.device,state.memoryProperties,count*sizeof(Result),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,memory,state.sceneAdmission.get());
        std::memcpy(input.mapped,rays,sizeof(rays)); std::memset(output.mapped,0xcd,output.size);
        VkDescriptorSetLayoutBinding bindings[] = {{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; setInfo.bindingCount=3; setInfo.pBindings=bindings;
        GX_VK(vkCreateDescriptorSetLayout(state.device,&setInfo,nullptr,&setLayout));
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layoutInfo.setLayoutCount=1; layoutInfo.pSetLayouts=&setLayout;
        GX_VK(vkCreatePipelineLayout(state.device,&layoutInfo,nullptr,&layout));
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets=1; poolInfo.poolSizeCount=2; poolInfo.pPoolSizes=sizes;
        GX_VK(vkCreateDescriptorPool(state.device,&poolInfo,nullptr,&pool));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE; GX_VK(vkAllocateDescriptorSets(state.device,&allocate,&set));
        const auto code=ReadModule("acceleration-probe.comp.spv");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; moduleInfo.codeSize=code.size()*4; moduleInfo.pCode=code.data();
        GX_VK(vkCreateShaderModule(state.device,&moduleInfo,nullptr,&module));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; compute.layout=layout;
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module=module; compute.stage.pName="main";
        GX_VK(vkCreateComputePipelines(state.device,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline));
        scene=std::make_unique<GxVulkan::AccelerationScene>(state.device,state.physical,state.memoryProperties,state.rayQueryCapabilities,state.sceneAdmission);
        const GxVulkan::ShadowPosition vertices[]={{{0,0,0}},{{1,0,0}},{{0,1,0}}}; const uint32_t indices[]={0,1,2};
        scene->Build(state.command,vertices,3,indices,3);
        const auto top=scene->TopLevel();
        VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; acceleration.accelerationStructureCount=1; acceleration.pAccelerationStructures=&top;
        VkDescriptorBufferInfo buffers[]={{input.buffer,0,input.size},{output.buffer,0,output.size}};
        VkWriteDescriptorSet writes[3]{};
        for(unsigned i=0;i<3;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1;
            writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            if(i) writes[i].pBufferInfo=&buffers[i-1]; else writes[i].pNext=&acceleration; }
        vkUpdateDescriptorSets(state.device,3,writes,0,nullptr);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(state.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
        vkCmdBindPipeline(state.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(state.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdDispatch(state.command,1,1,1);
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(state.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        state.Submit(false,true);
        passed=true;
        for(size_t i=0;i<count;++i) {
            Result result{}; std::memcpy(&result,output.mapped+i*sizeof(Result),sizeof(Result));
            const bool expected=i==0 || i==6;
            passed=passed && result.hit==uint32_t(expected) && (!expected || (result.primitive==0 && std::fabs(result.distance-1.f)<.00001f));
        }
        cleanup();
    } catch (...) {
        vkDeviceWaitIdle(state.device);
        if (state.recording) { vkResetCommandPool(state.device,state.commandPool,0); state.recording=false; state.inPass=false; }
        cleanup(); throw;
    }
    return passed && state.sceneAdmission->Used()==before;
}
bool CGxDeviceVulkan::SceneRayQueryTestAccelerationBuild() {
    auto& state = *m_native;
    if (!state.device || !state.rayQueryCapabilities.Available() || !state.BeginFrame()) return false;
    state.EndPass();
    const auto before = state.sceneAdmission->Used();
    const GxVulkan::ShadowPosition vertices[] = {{{0,0,0}},{{1,0,0}},{{0,1,0}}};
    const uint32_t indices[] = {0,1,2};
    auto pressure = std::make_shared<GxVulkan::GpuAdmission>(1);
    bool rejected = false;
    try {
        GxVulkan::AccelerationScene failed(state.device,state.physical,state.memoryProperties,state.rayQueryCapabilities,pressure);
        failed.Build(state.command,vertices,3,indices,3);
    } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected || pressure->Used()) throw std::runtime_error("Failed AS admission did not unwind");
    auto scene = std::make_unique<GxVulkan::AccelerationScene>(state.device,state.physical,state.memoryProperties,
        state.rayQueryCapabilities,state.sceneAdmission);
    scene->Build(state.command,vertices,3,indices,3);
    const auto usage = scene->Usage();
    const bool recorded = scene->TopLevel() && usage.vertices == 3 && usage.triangles == 1 &&
        usage.bottomBytes && usage.topBytes && usage.scratchBytes && state.sceneAdmission->Used() == before + usage.physicalBytes;
    bool immutable = false;
    try { scene->Build(state.command,vertices,3,indices,3); } catch (const std::logic_error&) { immutable = true; }
    // Original queue/fence completion, not a host event or another device.
    state.Submit(false,true);
    scene.reset();
    return recorded && immutable && state.sceneAdmission->Used() == before;
}
bool CGxDeviceVulkan::SceneRayQueryTestDeviceAddress() {
    auto& state = *m_native;
    if (!state.device || !state.rayQueryCapabilities.Available()) return false;
    const auto before = state.sceneAdmission->Used();
    auto buffer = GxVulkan::CreateBuffer(state.device, state.memoryProperties, 64,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, state.sceneAdmission.get());
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer = buffer.buffer;
    const auto address = vkGetBufferDeviceAddress(state.device, &info);
    const bool charged = state.sceneAdmission->Used() == before + buffer.allocationBytes;
    GxVulkan::DestroyBuffer(state.device, buffer);
    return address != 0 && charged && state.sceneAdmission->Used() == before;
}
#endif
void CGxDeviceVulkan::SceneClear(uint32_t mask, CImVector colour) {
#ifdef WOWDECOMP_VULKAN_TESTING
    m_native->completedWorldTest={};
#endif
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) return;
    state.BeginPass();
    std::vector<VkClearAttachment> attachments;
    if (mask & 1) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color = {{colour.r / 255.0f, colour.g / 255.0f, colour.b / 255.0f, colour.a / 255.0f}};
        if (state.HdrDraw()) {
            auto classic = clear;
            classic.colorAttachment = 2;
            attachments.push_back(classic);
            for (unsigned i = 0; !state.terrainShadowActive && i < 3; ++i) {
                const float encoded = clear.clearValue.color.float32[i];
                clear.clearValue.color.float32[i] = encoded <= .04045f ? encoded / 12.92f : std::pow((encoded + .055f) / 1.055f, 2.4f);
            }
        }
        attachments.push_back(clear);
    }
    if ((mask & 3) && state.MotionPass()) {
        VkClearAttachment motion{}; motion.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        motion.colorAttachment = 1; motion.clearValue.color = {{0,0,-1,1}}; attachments.push_back(motion);
    }
    if (mask & 2) {
        VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil.depth = 1.0f;
        attachments.push_back(clear);
    }
    VkClearRect rect{}; rect.rect.extent = state.targetExtent; rect.layerCount = 1;
    if (!attachments.empty()) vkCmdClearAttachments(state.command, static_cast<uint32_t>(attachments.size()), attachments.data(), 1, &rect);
}
void CGxDeviceVulkan::ScenePresent() {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) return;
    ICursorDraw();
    if (m_scrShotClick) DeviceScreenShot();
    WaitForFPSCap();
    state.Submit(true, false);
    if(state.productionPrepared){
        state.worldSubmitter->Dispatch(state.producerRecording.command,state.producerRecording.fence);
        state.productionPrepared=false;
    }
    ++state.uiPresents;
    if(state.independentWorld){
        const auto now=std::chrono::steady_clock::now();const double seconds=std::chrono::duration<double>(now-state.cadenceLog).count();
        if(seconds>=2){
            uint64_t bytes=0;for(const auto& slot:state.completedImages)bytes+=slot.colour.allocationBytes+slot.depth.allocationBytes;
            std::fprintf(stderr,"Independent cadence: UI=%.1f/s world=%.1f/s displayed=%llu age=%.1fms worldGPU=%.2fms UI-GPU-span=%.2fms slots=3 imageBytes=%llu queueFamily=%u (-1ms means unavailable)\n",
                (state.uiPresents-state.lastUiPresents)/seconds,(state.worldCompleted-state.lastWorldCompleted)/seconds,
                (unsigned long long)state.displayedWorldGeneration,state.displayedAgeMs,state.worldGpuMs,state.uiGpuMs,(unsigned long long)bytes,state.queueFamily);
            state.lastUiPresents=state.uiPresents;state.lastWorldCompleted=state.worldCompleted;state.cadenceLog=now;
        }
    }
    CGxDevice::ScenePresent();
}
void CGxDeviceVulkan::Draw(CGxBatch* batch, int32_t indexed) {
    if (m_context && batch) m_native->Draw(*batch, indexed != 0);
}
void CGxDeviceVulkan::IRsSendToHw(EGxRenderState) {
    // Gx still tracks dirty state, but Vulkan snapshots it into each draw's
    // pipeline key/descriptors rather than emitting mutable driver state here.
}
void CGxDeviceVulkan::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (!pool || size > INT32_MAX) throw std::invalid_argument("Invalid Gx pool size");
    for (const auto& entry : m_native->temporalBuffers)
        if (entry.second[0] == reinterpret_cast<uint64_t>(pool)) m_native->temporalDraws.ForgetBuffer(entry.first);
    m_native->pools[pool].resize(size); pool->m_size = static_cast<int32_t>(size);
    for (auto& entry : m_native->geometry)
        if (entry.second.pool == pool) entry.second.resident.MarkDirty();
}
void CGxDeviceVulkan::PoolDestroy(CGxPool* pool) {
    if (pool) {
        for (auto entry = m_native->temporalBuffers.begin(); entry != m_native->temporalBuffers.end();) {
            if (entry->second[0] == reinterpret_cast<uint64_t>(pool)) {
                m_native->temporalDraws.ForgetBuffer(entry->first);
                entry = m_native->temporalBuffers.erase(entry);
            } else ++entry;
        }
        for (auto entry = m_native->geometry.begin(); entry != m_native->geometry.end();) {
            if (entry->second.pool == pool) {
                entry->second.resident.Retire(m_native->retiredBuffers);
                entry = m_native->geometry.erase(entry);
            } else ++entry;
        }
        m_native->pools.erase(pool);
    }
    CGxDevice::PoolDestroy(pool);
}
void CGxDeviceVulkan::BufDestroy(CGxBuf*& buffer) {
    if (buffer) {
        m_native->temporalDraws.ForgetBuffer(buffer); m_native->temporalBuffers.erase(buffer);
        m_native->RetireGeometry(buffer);
    }
    CGxDevice::BufDestroy(buffer);
}
char* CGxDeviceVulkan::BufLock(CGxBuf* buffer) {
    if (!buffer || !buffer->m_pool) throw std::invalid_argument("Invalid Gx buffer lock");
    CGxDevice::BufLock(buffer);
    auto* pool = buffer->m_pool;
    if (pool->m_usage == GxPoolUsage_Stream) {
        if (!buffer->m_itemSize) throw std::runtime_error("Stream buffer has no item stride");
        uint32_t offset = (static_cast<uint32_t>(pool->m_offset) + buffer->m_itemSize - 1) / buffer->m_itemSize * buffer->m_itemSize;
        if (offset > static_cast<uint32_t>(std::max(pool->m_size, 0)) || buffer->m_size > static_cast<uint32_t>(std::max(pool->m_size, 0)) - offset) {
            pool->Discard(); offset = 0;
        }
        buffer->m_index = offset;
        pool->m_offset = static_cast<int32_t>(offset + buffer->m_size);
    }
    auto& data = m_native->pools[pool];
    const uint64_t required = static_cast<uint64_t>(buffer->m_index) + buffer->m_size;
    if (required > INT32_MAX) throw std::runtime_error("Gx buffer size overflow");
    if (data.size() < required) data.resize(std::max<uint64_t>(required, std::max(pool->m_size, 0)));
    for (auto& entry : m_native->geometry) {
        const auto& cached = entry.second;
        if (cached.pool == pool && cached.index < required &&
            buffer->m_index < static_cast<uint64_t>(cached.index) + cached.bytes)
            entry.second.resident.MarkDirty();
    }
    for (const auto& entry : m_native->temporalBuffers) {
        const auto& range = entry.second;
        if (range[0] == reinterpret_cast<uint64_t>(pool) && range[1] < required &&
            buffer->m_index < range[1]+range[2]) m_native->temporalDraws.ForgetBuffer(entry.first);
    }
    return reinterpret_cast<char*>(data.data() + buffer->m_index);
}
int32_t CGxDeviceVulkan::BufUnlock(CGxBuf* buffer, uint32_t size) {
    if (!buffer || size > buffer->m_size) throw std::invalid_argument("Invalid Gx buffer unlock size");
    buffer->m_valid = buffer->m_hasData = 1;
    return CGxDevice::BufUnlock(buffer, size);
}
void CGxDeviceVulkan::BufData(CGxBuf* buffer, const void* data, uint32_t size, uint32_t offset) {
    if (!buffer || (size && !data) || offset > buffer->m_size || size > buffer->m_size - offset)
        throw std::invalid_argument("Invalid Gx buffer update");
    char* destination = BufLock(buffer);
    if (size) std::memcpy(destination + offset, data, size);
    BufUnlock(buffer, buffer->m_size);
}
int32_t CGxDeviceVulkan::TexCreate(EGxTexTarget target, uint32_t width, uint32_t height, uint32_t depth,
    EGxTexFormat format, EGxTexFormat dataFormat, CGxTexFlags flags, void* userArg,
    GxTexUserFunc userFunc, const char* name, CGxTex*& texture) {
    if (target < GxTex_2d || target >= GxTexTargets_Last || !m_caps.m_texTarget[target]) return 0;
    if (format <= GxTex_Unknown || format >= GxTexFormats_Last) return 0;
    // Packed ARGB4444 has no mandatory Vulkan equivalent. Keep its source
    // format and use the existing Blit conversion into an advertised format.
    if (format == GxTex_Argb4444) format = GxTex_Abgr8888;
    else if (!m_caps.m_texFmt[format] && format != GxTex_D24X8) format = GxTex_Argb8888;
    return CGxDevice::TexCreate(target, width, height, depth, format, dataFormat, flags, userArg, userFunc, name, texture);
}
void CGxDeviceVulkan::ITexMarkAsUpdated(CGxTex* texture) {
    if (m_context && texture && m_native->BeginFrame()) m_native->Texture(texture);
}
void CGxDeviceVulkan::TexDestroy(CGxTex* texture) {
    auto found = m_native->textures.find(texture);
    if (found != m_native->textures.end()) {
        m_native->EndPass();
        m_native->retiredImages.push_back(found->second); m_native->textures.erase(found);
    }
    texture->m_apiSpecificData = nullptr;
    CGxDevice::TexDestroy(texture);
}
bool CGxDeviceVulkan::NativeMsdfFontsSupported() const {
    return m_native && m_native->device && !m_native->msdfFontVertex.empty() && !m_native->msdfFontPixel.empty();
}
void CGxDeviceVulkan::NativeMsdfFontBegin(uint32_t outlinePixels) {
    m_native->msdfOutlinePixels = std::min(outlinePixels, 2u);
    m_native->msdfFontActive = true;
}
void CGxDeviceVulkan::NativeMsdfFontEnd() { m_native->msdfFontActive = false; }

void CGxDeviceVulkan::IShaderCreate(CGxShader* shader) {
    shader->m_loaded = 1; shader->m_valid = 0; shader->m_apiSpecific = nullptr;
    std::string origin = "generated compatibility: ";
    origin += shader->m_key.m_str ? shader->m_key.m_str : "anonymous program";
    try {
        if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel)
            throw std::runtime_error("Native shader loader supports only vertex/pixel stages");
        const bool vertex = shader->m_target == GxSh_Vertex;
        const auto identity = GxVulkan::IdentifyShader(vertex ? GxVulkan::ShaderStage::Vertex : GxVulkan::ShaderStage::Pixel,
            shader->m_key.m_str, shader->m_code.Ptr(), shader->m_code.Count());
        std::optional<GxVulkan::ShaderOverride> selected;
        if (identity) {
            const char* configured = std::getenv("WOWDECOMP_SHADER_ROOT");
            const std::string label = identity->profile + "/" + identity->name + "/" + std::to_string(identity->permutation);
            origin = "override resolution: " + label;
            selected = GxVulkan::ResolveShaderOverride(configured ? configured : WOWDECOMP_VULKAN_OVERRIDE_DIR, *identity);
            if (!selected) origin = "generated compatibility: " + label;
        }
        NativeShader created;
        created.id = m_native->nextShader++;
        if (selected) {
            origin = "maintained override: " + selected->filename.generic_string();
            created.maintained = std::make_shared<GxVulkan::ShaderOverride>(std::move(*selected));
            created.source = created.maintained->source;
            created.code = m_native->Compile(created.source, vertex, created.maintained, &created.bindings);
        } else {
            auto recovered = GxVulkan::RecoverSlangShader(shader->m_code.Ptr(), shader->m_code.Count());
            created.source = std::move(recovered.source);
            created.bindings = GxVulkan::LegacyShaderBindings(*recovered.reflection);
            created.code = m_native->Compile(created.source, vertex);
        }
        m_native->shaders.insert_or_assign(shader, std::move(created));
        auto& native = m_native->shaders.at(shader);
        native.inputFog = shader->m_target == GxSh_Pixel && GxVulkan::ShaderUsesInputLocation(native.code, 10);
        native.outputFog = shader->m_target == GxSh_Vertex && GxVulkan::ShaderUsesOutputLocation(native.code, 10);
        shader->m_apiSpecific = &native; shader->m_valid = 1;
    } catch (const std::exception& error) {
        CGxDevice::Log("Vulkan/Slang shader [%s]: %s", origin.c_str(), error.what());
        std::fprintf(stderr, "Vulkan/Slang shader [%s]: %s\n", origin.c_str(), error.what());
    }
}
void CGxDeviceVulkan::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    m_native->shaders.erase(shader); shader->m_apiSpecific = nullptr;
    IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    IShaderCreate(shader);
}
void CGxDeviceVulkan::ShaderDestroy(CGxShader*& shader) {
    if (shader && shader->m_refCount == 1) {
        m_native->shaders.erase(shader); shader->m_apiSpecific = nullptr;
    }
    CGxDevice::ShaderDestroy(shader);
}

void CGxDeviceVulkan::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (buffer >= GxBuffers_Last) throw std::invalid_argument("Invalid Gx render target");
    m_native->EndPass();
    m_native->DetachTexture(texture);
    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
}
int32_t CGxDeviceVulkan::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos,
    const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!dest || size.x <= 0 || size.y <= 0) return 0;
    CiRect destination, source;
    destination.minX = destPos.x; destination.minY = destPos.y;
    destination.maxX = destPos.x + size.x; destination.maxY = destPos.y + size.y;
    source.minX = sourcePos.x; source.minY = sourcePos.y;
    source.maxX = sourcePos.x + size.x; source.maxY = sourcePos.y + size.y;
    return TexStretch(dest, &destination, &source, level, plane);
}
int32_t CGxDeviceVulkan::TexStretch(CGxTex* dest, const CiRect* destRect,
    const CiRect* sourceRect, uint32_t level, uint32_t face) {
    auto& state = *m_native;
    if (!dest || !m_context || !state.BeginFrame()) return 0;
    state.EndPass();
    state.DetachTexture(dest);
    auto& destination = state.Texture(dest);
    auto* sourceTarget = m_textureTarget[GxBuffers_Color].m_texture;
    // Legacy UNORM screen/refraction copies cannot preserve float scene transport.
    // Publish the complete original-colour companion before copying or resuming draws.
    if (state.HdrDraw() && !sourceTarget) state.AbortHdr("legacy screen copy/refraction transport");
    const bool reduced = state.worldActive && (state.sceneScale != 1.f || state.hdrActive) && !sourceTarget;
    if (reduced) state.SnapshotWorldColour();
    auto* sourceImage = sourceTarget ? &state.Texture(sourceTarget) : (state.worldActive && !reduced ? &state.worldColour : nullptr);
    const VkImage source = sourceImage ? sourceImage->image : state.swapImages[state.imageIndex];
    const auto width = sourceImage ? sourceImage->width : state.extent.width;
    const auto height = sourceImage ? sourceImage->height : state.extent.height;
    if (destination.aspect != VK_IMAGE_ASPECT_COLOR_BIT || level >= destination.levels || face >= destination.layers)
        return 0;
    if (source == destination.image) throw std::runtime_error("Overlapping render-target blit");
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
    blit.srcOffsets[0] = {sourceRect ? sourceRect->minX : 0, sourceRect ? sourceRect->minY : 0, 0};
    blit.srcOffsets[1] = {sourceRect ? sourceRect->maxX : static_cast<int32_t>(width), sourceRect ? sourceRect->maxY : static_cast<int32_t>(height), 1};
    blit.dstOffsets[0] = {destRect ? destRect->minX : 0, destRect ? destRect->minY : 0, 0};
    blit.dstOffsets[1] = {destRect ? destRect->maxX : static_cast<int32_t>(std::max(destination.width >> level, 1u)),
        destRect ? destRect->maxY : static_cast<int32_t>(std::max(destination.height >> level, 1u)), 1};
    state.Barrier(source, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    state.Barrier(destination.image, destination.aspect, destination.levels, destination.layers, destination.layout, destination.layout);
    vkCmdBlitImage(state.command, source, VK_IMAGE_LAYOUT_GENERAL, destination.image, destination.layout, 1, &blit, VK_FILTER_NEAREST);
    state.Barrier(destination.image, destination.aspect, destination.levels, destination.layers, destination.layout, destination.layout);
    return 1;
}
namespace {
void ReadbackBarrier(VkCommandBuffer command, VkBuffer buffer) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
}
}
void CGxDeviceVulkan::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) { pixels.SetCount(0); return; }
    ClampRectToWindow(rect);
    const uint32_t width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    if (!width || !height) { pixels.SetCount(0); return; }
    state.EndPass();
    auto* target = m_textureTarget[GxBuffers_Color].m_texture;
    const bool reduced = state.worldActive && state.sceneScale != 1.f && !target;
    if (reduced) state.SnapshotWorldColour();
    auto* image = target ? &state.Texture(target) : (state.worldActive && !reduced ? &state.worldColour : nullptr);
    const auto source = image ? image->image : state.swapImages[state.imageIndex];
    const auto format = image ? image->format : state.surfaceFormat;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Unsupported colour readback format");
    NativeBuffer readback = state.MakeBuffer(static_cast<uint64_t>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        state.Barrier(source, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {rect.minX, rect.minY, 0}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(state.command, source, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        ReadbackBarrier(state.command, readback.buffer);
        state.Submit(false, true);
        pixels.SetCount(width * height);
        std::memcpy(pixels.Ptr(), readback.mapped, static_cast<size_t>(width) * height * 4);
        if (format == VK_FORMAT_R8G8B8A8_UNORM)
            for (uint32_t i = 0; i < width * height; ++i) std::swap(pixels[i].r, pixels[i].b);
    } catch (...) { state.DestroyBuffer(readback); throw; }
    state.DestroyBuffer(readback);
}
#ifdef WOWDECOMP_VULKAN_TESTING
void CGxDeviceVulkan::SceneWorldTestProbeUi(uint32_t x,uint32_t y,uint32_t worldX,uint32_t worldY){
    auto& s=*m_native;
    if(!s.recording || s.productionCapture || s.worldActive || x>=s.extent.width || y>=s.extent.height)throw std::logic_error("UI probe requires current presentation");
    if(!s.uiProbe.buffer)s.uiProbe=s.MakeBuffer(8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.EndPass();const auto image=s.swapImages[s.imageIndex];s.Barrier(image,VK_IMAGE_ASPECT_COLOR_BIT,1,1,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageOffset={int32_t(x),int32_t(y),0};copy.imageExtent={1,1,1};
    vkCmdCopyImageToBuffer(s.command,image,VK_IMAGE_LAYOUT_GENERAL,s.uiProbe.buffer,1,&copy);
    copy.bufferOffset=4;copy.imageOffset={int32_t(worldX),int32_t(worldY),0};
    vkCmdCopyImageToBuffer(s.command,image,VK_IMAGE_LAYOUT_GENERAL,s.uiProbe.buffer,1,&copy);ReadbackBarrier(s.command,s.uiProbe.buffer);
    s.uiProbeSerial=s.submittedSerial+1;
}
bool CGxDeviceVulkan::SceneWorldTestReadUiProbe(CImVector& pixel,uint32_t index) const{
    const auto& s=*m_native;if(index>1 || !s.uiProbeSerial || s.completedSerial<s.uiProbeSerial)return false;
    std::memcpy(&pixel,static_cast<const uint8_t*>(s.uiProbe.mapped)+index*4,4);if(s.surfaceFormat==VK_FORMAT_R8G8B8A8_UNORM)std::swap(pixel.r,pixel.b);return true;
}
#endif
void CGxDeviceVulkan::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) { depths.SetCount(0); return; }
    ClampRectToWindow(rect);
    const uint32_t width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    if (!width || !height) { depths.SetCount(0); return; }
    state.EndPass();
    auto* target = m_textureTarget[GxBuffers_Depth].m_texture;
    const bool reduced = state.worldActive && state.sceneScale != 1.f && !target;
    if (reduced) state.PublishWorldDepth(state.depth.image);
    auto& image = target ? state.Texture(target) :
        (state.worldActive && !reduced && !m_textureTarget[GxBuffers_Color].m_texture ? state.worldDepth : state.depth);
    if (image.format != VK_FORMAT_D32_SFLOAT) throw std::runtime_error("Unsupported depth readback format");
    NativeBuffer readback = state.MakeBuffer(static_cast<uint64_t>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        state.Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.imageOffset = {rect.minX, rect.minY, 0}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(state.command, image.image, image.layout, readback.buffer, 1, &copy);
        ReadbackBarrier(state.command, readback.buffer);
        state.Submit(false, true);
        depths.SetCount(width * height);
        std::memcpy(depths.Ptr(), readback.mapped, static_cast<size_t>(width) * height * 4);
    } catch (...) { state.DestroyBuffer(readback); throw; }
    state.DestroyBuffer(readback);
}
// These capabilities are deliberately not advertised. Return unsupported rather
// than synthesising occlusion results or pretending vendor stereo is available.
bool CGxDeviceVulkan::QueryBegin(CGxQuery*) { return false; }
bool CGxDeviceVulkan::QueryEnd(CGxQuery*) { return false; }
bool CGxDeviceVulkan::QueryGetParam(CGxQuery*, EGxQueryParam, uint32_t&) { return false; }
bool CGxDeviceVulkan::QueryGetData(CGxQuery*, uint32_t&) { return false; }
void CGxDeviceVulkan::StereoSetConvergence(float) {}
float CGxDeviceVulkan::StereoGetConvergence() { return 0.0f; }
void CGxDeviceVulkan::StereoSetSeparation(float) {}
float CGxDeviceVulkan::StereoGetSeparation() { return 0.0f; }
bool CGxDeviceVulkan::StereoEnabled() { return false; }
// These three hooks are also empty in the reconstructed D3D9 backend.
void CGxDeviceVulkan::DeviceUnk81(uint32_t, uint32_t) {}
void CGxDeviceVulkan::DeviceUnk82() {}
void CGxDeviceVulkan::DeviceUnk83(uint32_t, uint32_t) {}

int32_t VulkanAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    const int count = SDL_GetNumDisplayModes(0);
    for (int i = 0; i < count; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(0, i, &mode) != 0 || mode.w < 640 || mode.h < 480) continue;
        CGxFormat format;
        format.m_size = {mode.w, mode.h}; format.m_refreshRate = mode.refresh_rate;
        format.m_colorFormat = CGxFormat::Fmt_Argb8888; format.m_depthFormat = CGxFormat::Fmt_Ds320;
        format.m_multisampleCount = 1;
        if (!CGxDevice::FormatExists(formats, format)) *formats.New() = format;
    }
    return formats.Count() != 0;
}
int32_t VulkanAdapterID(uint16_t& vendor, uint16_t& deviceID, uint32_t& driverHi, uint32_t& driverLo) {
    VkInstance instance = VK_NULL_HANDLE;
    try {
        uint32_t count = 0;
        GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()));
        const bool portability = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        });
        const char* portabilityName = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        if (portability) {
            create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &portabilityName;
        }
        GX_VK(vkCreateInstance(&create, nullptr, &instance));
        GX_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        GX_VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[0], &properties);
        vendor = static_cast<uint16_t>(properties.vendorID); deviceID = static_cast<uint16_t>(properties.deviceID);
        driverHi = 0; driverLo = properties.driverVersion;
        vkDestroyInstance(instance, nullptr);
        return 1;
    } catch (const std::exception& error) {
        if (instance) vkDestroyInstance(instance, nullptr);
        std::fprintf(stderr, "Vulkan adapter query: %s\n", error.what());
        return 0;
    }
}
