#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include <memory>
#include <array>

class CGxDeviceVulkan final : public CGxDevice {
public:
    CGxDeviceVulkan();
    ~CGxDeviceVulkan() override;
    int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) override;
    int32_t DeviceCreate(void* window, const CGxFormat& format) override;
    void DeviceDestroy() override;
    int32_t DeviceSetFormat(const CGxFormat& format) override;
    bool DeviceGetWindowLogicalSize(C2iVector& size) const override;
    void* DeviceWindow() override;
    int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) override;
    void CapsWindowSize(CRect& rect) override;
    void CapsWindowSizeInScreenCoords(CRect& rect) override;
    void XformSetProjection(const C44Matrix& matrix) override;
    void SceneClear(uint32_t mask, CImVector color) override;
    void ScenePresent() override;
    bool SceneBeginWorld(bool temporal = false, uint64_t scene = 0) override;
    void SceneEndWorld() override;
    void SceneRequestHdrLut(const char* path, bool reload = false) override;
    const char* SceneHdrLutStatus() const override;
    bool SceneSetHdrLut(const float* rgb, uint32_t size) override;
    bool SceneReadHdrPixel(uint32_t x, uint32_t y, std::array<float,4>& value);
#ifdef WOWDECOMP_VULKAN_TESTING
    std::array<uint64_t,4> SceneHdrTestLutState() const;
    std::array<uint64_t,8> SceneHdrTestLutMemory() const;
    bool SceneHdrTestValidationClean() const;
    // Completed world boundary: HDR published, AA resolved, render width/height.
    std::array<uint32_t,4> SceneWorldTestCompletedFrame() const;
    void SceneHdrTestLimits(uint32_t slots, uint64_t budget, uint32_t draw);
#endif
    bool SceneHdrSupported() const override;
    bool SceneHdrActive() const override;
    bool SceneSetHdr(bool enabled, float exposureEV) override;
    bool SceneSetLinearOutput(bool linear) override;
    bool SceneFarResourcesSupported() const override;
    GxFarResourceAdmission SceneCreateFarResources(const GxFarResourceRequest&) override;
    GxFarResourceUsage SceneFarResourceUsage() const override;
    void SceneWithdrawFarResources() override;
#ifdef WOWDECOMP_VULKAN_TESTING
    void SceneFarTestAdmissionLimits(uint64_t scene, uint64_t images, uint64_t far);
    void SceneFarTestFailAllocation(uint32_t ordinal);
    bool SceneFarTestNearCharge(bool images, uint64_t bytes);
    void SceneFarTestReleaseNear();
    void SceneFarTestHoldGpu();
    void SceneFarTestReleaseGpu();
#endif
    bool SceneTemporalSupported() const override;
    bool SceneRenderScaleSupported() const override;
    bool SceneSetRenderScale(float scale) override;
    void ScenePauseWorldMotion() override;
    void SceneInvalidateHistory() override;
    void SceneSetMotionOwner(const void* owner, bool stableVertices) override;
    void SceneForgetMotionOwner(const void* owner) override;
    void Draw(CGxBatch* batch, int32_t indexed) override;
    void IRsSendToHw(EGxRenderState which) override;
    void PoolSizeSet(CGxPool* pool, uint32_t size) override;
    void PoolDestroy(CGxPool* pool) override;
    void BufDestroy(CGxBuf*& buf) override;
    char* BufLock(CGxBuf* buf) override;
    int32_t BufUnlock(CGxBuf* buf, uint32_t size) override;
    void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) override;
    int32_t TexCreate(EGxTexTarget target, uint32_t width, uint32_t height, uint32_t depth,
        EGxTexFormat format, EGxTexFormat dataFormat, CGxTexFlags flags, void* userArg,
        GxTexUserFunc userFunc, const char* name, CGxTex*& texture) override;
    void ITexMarkAsUpdated(CGxTex* texture) override;
    void TexDestroy(CGxTex* texture) override;
    int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos,
        const C2iVector& size, uint32_t level, uint32_t plane) override;
    int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect,
        uint32_t level, uint32_t face) override;
    void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) override;
    void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) override;
    void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) override;
    bool NativeMsdfFontsSupported() const override;
    void NativeMsdfFontBegin(uint32_t outlinePixels) override;
    void NativeMsdfFontEnd() override;
    void IShaderCreate(CGxShader* shader) override;
    void IShaderReload(CGxShader* shader, const char* path, const char* name) override;
    void ShaderDestroy(CGxShader*& shader) override;
    bool QueryBegin(CGxQuery* query) override;
    bool QueryEnd(CGxQuery* query) override;
    bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) override;
    bool QueryGetData(CGxQuery* query, uint32_t& data) override;
    void StereoSetConvergence(float convergence) override;
    float StereoGetConvergence() override;
    void StereoSetSeparation(float separation) override;
    float StereoGetSeparation() override;
    bool StereoEnabled() override;
    void DeviceUnk81(uint32_t a, uint32_t b) override;
    void DeviceUnk82() override;
    void DeviceUnk83(uint32_t a, uint32_t b) override;

private:
    struct State;
    std::unique_ptr<State> m_native;
};

CGxDevice* CreateCGxDeviceVulkan();
int32_t VulkanAdapterFormats(TSGrowableArray<CGxFormat>& formats);
int32_t VulkanAdapterID(uint16_t& vendor, uint16_t& device, uint32_t& driverHi, uint32_t& driverLo);

#endif
