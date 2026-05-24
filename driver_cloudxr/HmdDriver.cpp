// MIT License
//
// Copyright(c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "ControllerDriver.h"
#include "HandDriver.h"
#include "HmdDriver.h"
#include "SharedMemory.h"
#include "Tracing.h"
#include "Utilities.h"

#define A_CPU
#include <ffx-cas/ffx_a.h>
#include <ffx-cas/ffx_cas.h>

#include "FullscreenQuadVS.h"
#include "SharpeningCS.h"
#include "SharpeningPS.h"
#include "SharpeningSrgbCS.h"

using namespace driver;
using namespace util;
using namespace xr::math;

namespace {
    enum Component {
        ComponentSystemClick,
        ComponentPresence,
        ComponentEyeGaze,

        ComponentCount,
    };

    struct SharpenConstants {
        alignas(16) uint32_t const0[4];
        alignas(16) uint32_t const1[4];
        alignas(8) XrOffset2Di topLeft;
        alignas(8) XrExtent2Di extent;
    };

    class HmdDriver : public IHmdDriver, public vr::IVRDisplayComponent, public vr::IVRDriverDirectModeComponent {
      public:
        HmdDriver(xr::InstanceHandle& instance, xr::ExtensionContext& extensions, sample::SystemContext& system)
            : m_instance(instance), m_extensions(extensions), m_system(system) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_Ctor");

            // Cache useful state.
            m_hasEyeTracking = m_system.EyeGazeInteractionProperties.supportsEyeGazeInteraction;
            DriverLog(m_hasEyeTracking ? "Instance supports eye tracking" : "Instance does not support eye tracking");
            m_hasHandTracking = m_system.HandTrackingProperties.supportsHandTracking;
            DriverLog(m_hasHandTracking ? "Instance supports hand tracking"
                                        : "Instance does not support hand tracking");
            DriverLog(m_extensions.SupportsVisibilityMask ? "Instance supports visibility mask"
                                                          : "Instance does not support visibility mask");
            {
                XrViewConfigurationProperties properties = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
                CHECK_XRCMD(xrGetViewConfigurationProperties(
                    m_instance.Get(), m_system.Id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &properties));
                m_isFovMutable = properties.fovMutable;
                DriverLog(m_isFovMutable ? "Instance supports mutable FOV" : "Instance does not support mutable FOV");
                vr::VRSettings()->SetBool("driver_cloudxr", "allow_fov_tangents", m_isFovMutable);
            }

            // KRVR specific: Open the opaque side-channel used for haptic packets. Fails soft if the extension isn't
            // available.
            if (krvr::OpaqueChannel::Get().Init(m_instance.Get(), m_system.Id, xrGetInstanceProcAddr)) {
                DriverLog("Using KRVR haptics channel");
            }

            TraceLoggingWriteStop(local, "HmdDriver_Ctor");
        }

        ~HmdDriver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_Dtor");

            TraceLoggingWriteStop(local, "HmdDriver_Dtor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_Activate", TLArg(unObjectId, "ObjectId"));

            m_deviceIndex = unObjectId;

            DriverLog("Headset: %s (VID: %04x)", m_system.Properties.systemName, m_system.Properties.vendorId);

            InitializeSession();

            // Create the controller drivers early, so we can register the XrActions.
            for (uint32_t side = 0; side < 2; side++) {
                m_controllerDriver[side] = CreateControllerDriver(m_instance,
                                                                  m_session,
                                                                  m_referenceSpace,
                                                                  side == 0 ? vr::TrackedControllerRole_LeftHand
                                                                            : vr::TrackedControllerRole_RightHand);
                if (m_extensions.SupportsHandJointTracking &&
                    vr::VRSettings()->GetBool("driver_cloudxr", "enable_hand_tracking")) {
                    m_handDriver[side] = CreateHandDriver(m_instance,
                                                          m_session,
                                                          m_referenceSpace,
                                                          side == 0 ? vr::TrackedControllerRole_LeftHand
                                                                    : vr::TrackedControllerRole_RightHand);
                }
            }

            InitializeInputs();

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "CloudXR");
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_ModelNumber_String, m_system.Properties.systemName);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "CloudXR");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, GetSerialNumber());

            vr::VRProperties()->SetStringProperty(container, vr::Prop_RenderModelName_String, "generic_hmd");
            vr::VRProperties()->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 1);

            vr::VRProperties()->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);

            const auto adapterLuid = (((uint64_t)m_adapterLuid.HighPart) << 32) | m_adapterLuid.LowPart;
            vr::VRProperties()->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, adapterLuid);

            // We control vsync from our PostPresent() method.
            vr::VRProperties()->SetBoolProperty(container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);

            // TODO: Investigate if this is a good idea, let CloudXR do all the prediction.
            // vr::VRProperties()->SetBoolProperty(container, vr::Prop_DoNotApplyPrediction_Bool, true);

            if (m_isFovMutable) {
                m_horizontalFovTangent = vr::VRSettings()->GetFloat("driver_cloudxr", "horizontal_fov_tangent");
                m_verticalFovTangent = vr::VRSettings()->GetFloat("driver_cloudxr", "vertical_fov_tangent");
                TraceLoggingWriteTagged(local,
                                        "HmdDriver_Activate",
                                        TLArg(m_horizontalFovTangent, "HorizontalFovTangent"),
                                        TLArg(m_verticalFovTangent, "VerticalFovTangent"));
            }

            const bool useFovTangent = m_horizontalFovTangent < 1 || m_verticalFovTangent < 1;
            if (!useFovTangent) {
                vr::CVRHiddenAreaHelpers helpers = {vr::VRPropertiesRaw()};
                for (int eye = 0; eye < xr::StereoView::Count; eye++) {
                    // Query the visibility meshes from OpenXR.
                    const auto getMaskVertices = [&](XrVisibilityMaskTypeKHR type) {
                        std::vector<vr::HmdVector2_t> vertices;

                        XrVisibilityMaskKHR mask = {XR_TYPE_VISIBILITY_MASK_KHR};
                        if (m_extensions.SupportsVisibilityMask) {
                            CHECK_XRCMD(xrGetVisibilityMaskKHR(
                                m_session.Get(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask));
                        }
                        if (mask.vertexCountOutput != 0 && mask.indexCountOutput != 0) {
                            mask.vertexCapacityInput = mask.vertexCountOutput;
                            mask.indexCapacityInput = mask.indexCountOutput;
                            std::vector<XrVector2f> visibilityMaskVertices(mask.vertexCountOutput);
                            std::vector<uint32_t> visibilityMaskIndices(mask.indexCountOutput);

                            mask.vertices = visibilityMaskVertices.data();
                            mask.indices = visibilityMaskIndices.data();

                            CHECK_XRCMD(xrGetVisibilityMaskKHR(
                                m_session.Get(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask));

                            for (const auto index : visibilityMaskIndices) {
                                // NDC to UV.
                                // TODO: The scale of the mask seems to be off.
                                vertices.emplace_back(vr::HmdVector2_t{(visibilityMaskVertices[index].x + 1.f) / 2.f,
                                                                       (visibilityMaskVertices[index].y + 1.f) / 2.f});
                            }
                        }

                        return vertices;
                    };

                    {
                        auto vertices = getMaskVertices(XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR);
                        helpers.SetHiddenArea((vr::EVREye)eye,
                                              vr::k_eHiddenAreaMesh_Standard,
                                              vertices.data(),
                                              (uint32_t)vertices.size());
                    }
                    {
                        auto vertices = getMaskVertices(XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR);
                        helpers.SetHiddenArea(
                            (vr::EVREye)eye, vr::k_eHiddenAreaMesh_Inverse, vertices.data(), (uint32_t)vertices.size());
                    }
                }
            }
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_InputProfilePath_String, "{cloudxr}/input/cloudxrhmd_profile.json");
            vr::VRDriverInput()->CreateBooleanComponent(
                container, "/input/system/click", &m_components[ComponentSystemClick]);
            vr::VRDriverInput()->CreateBooleanComponent(container, "/proximity", &m_components[ComponentPresence]);

            vr::VRProperties()->SetStringProperty(container, vr::Prop_ExpectedControllerType_String, "oculus_touch");

            if (m_hasEyeTracking) {
                DriverLog("Supports eye tracking");
                vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);
                vr::VRDriverInput()->CreateEyeTrackingComponent(
                    container, "/eyetracking", &m_components[ComponentEyeGaze]);
            }

            // clang-format off
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceOff_String, "{cloudxr}/icons/cxr_off.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearching_String, "{cloudxr}/icons/cxr_searching.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{cloudxr}/icons/cxr_searching.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String, "{cloudxr}/icons/cxr_ready.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{cloudxr}/icons/cxr_ready.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceNotReady_String, "{cloudxr}/icons/cxr_off.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceStandby_String, "{cloudxr}/icons/cxr_standby.png");
            // clang-format on

            vr::VRProperties()->SetStringProperty(container, vr::Prop_ResourceRoot_String, "cloudxr");
            vr::VRProperties()->SetStringProperty(container,
                                                  vr::Prop_AdditionalDeviceSettingsPath_String,
                                                  "{cloudxr}/settings/settingsschema.vrsettings");

            // Setup IPC with the client utility.
            *m_sharedFileHandle.put() = CreateFileMapping(INVALID_HANDLE_VALUE,
                                                          nullptr,
                                                          PAGE_READWRITE,
                                                          0,
                                                          sizeof(*m_sharedMemory),
                                                          L"CloudXR.SteamVR.SharedMemory");
            if (m_sharedFileHandle) {
                m_sharedMemory = (shared::SharedMemory*)MapViewOfFile(
                    m_sharedFileHandle.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*m_sharedMemory));
                memset(m_sharedMemory, 0, sizeof(*m_sharedMemory));
            }

            ApplySettingsChanges();

            // Go time!
            {
                DriverLog("Waiting for OpenXR session to become ready...");
                bool sessionReady = false;
                while (!sessionReady) {
                    XrEventDataBuffer buffer = {XR_TYPE_EVENT_DATA_BUFFER};
                    if (xrPollEvent(m_instance.Get(), &buffer) == XR_SUCCESS) {
                        switch (buffer.type) {
                        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                            {
                                const auto eventData = *(XrEventDataSessionStateChanged*)(&buffer);
                                TraceLoggingWriteTagged(local,
                                                        "HmdDriver_Activate_WaitForReady",
                                                        TLArg(xr::ToCString(eventData.state), "SessionState"));
                                if (eventData.state == XR_SESSION_STATE_READY) {
                                    DriverLog("Ready.");
                                    sessionReady = true;
                                }
                            }
                            break;
                        }
                    }
                    TraceLoggingWriteTagged(local, "HmdDriver_Activate_WaitForReady");
                    std::this_thread::yield();
                }

                XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                sessionBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                CHECK_XRCMD(xrBeginSession(m_session.Get(), &sessionBeginInfo));
            }

            {
                CHECK_XRCMD(xrWaitFrame(m_session.Get(), nullptr, &m_frameState));
                TraceLoggingWriteTagged(local,
                                        "HmdDriver_Activate_WaitFrame",
                                        TLArg(m_frameState.predictedDisplayTime, "PredictedDisplayTime"),
                                        TLArg(m_frameState.predictedDisplayPeriod, "PredictedDisplayPeriod"));

                // Attempt to retrieve initial eye poses and FOV.
                UpdateHeadProperties(m_frameState.predictedDisplayTime);

                // Bootstrap the first frame, since our frame management is taking place in PostPresent().
                CHECK_XRCMD(xrBeginFrame(m_session.Get(), nullptr));
            }

            m_isFirstFrame = true;

            TraceLoggingWriteStop(local, "HmdDriver_Activate");

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            if (m_sharedMemory) {
                UnmapViewOfFile(m_sharedMemory);
                m_sharedMemory = nullptr;
            }
            if (m_sharedFileHandle) {
                m_sharedFileHandle.reset();
            }

            xrRequestExitSession(m_session.Get());

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

            TraceLoggingWriteStop(local, "HmdDriver_Deactivate");
        }

        void EnterStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_EnterStandby", TLArg(m_deviceIndex, "ObjectId"));

            TraceLoggingWriteStop(local, "HmdDriver_EnterStandby");
        }

        void LeaveStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_LeaveStandby", TLArg(m_deviceIndex, "ObjectId"));

            // Dispatch to the controllers.
            for (uint32_t side = 0; side < 2; side++) {
                m_controllerDriver[side]->LeaveStandby();
                if (m_handDriver[side]) {
                    m_handDriver[side]->LeaveStandby();
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_LeaveStandby");
        }

        void* GetComponent(const char* pchComponentNameAndVersion) override {
            if (strcmp(vr::IVRDisplayComponent_Version, pchComponentNameAndVersion) == 0) {
                return (vr::IVRDisplayComponent*)this;
            } else if (strcmp(vr::IVRDriverDirectModeComponent_Version, pchComponentNameAndVersion) == 0) {
                return (vr::IVRDriverDirectModeComponent*)this;
            }
            return nullptr;
        }

        vr::DriverPose_t GetPose() override {
            // This method is not used by SteamVR.
            return {};
        }

        void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {
            if (unResponseBufferSize >= 1) {
                pchResponseBuffer[0] = 0;
            }
        }

        bool IsDisplayOnDesktop() override {
            return false;
        }

        bool IsDisplayRealDisplay() override {
            return true;
        }

        void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_GetRecommendedRenderTargetSize", TLArg(m_deviceIndex, "ObjectId"));

            *pnWidth = (uint32_t)(m_renderTargetWidth * m_horizontalFovTangent);
            *pnWidth = (*pnWidth + 3) / 4 * 4;
            *pnHeight = (uint32_t)(m_renderTargetHeight * m_verticalFovTangent);
            *pnHeight = (*pnHeight + 3) / 4 * 4;

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetRecommendedRenderTargetSize",
                                  TLArg(*pnWidth, "RecommendedWidth"),
                                  TLArg(*pnHeight, "RecommendedHeight"));
        }

        void GetEyeOutputViewport(
            vr::EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
            // Not used for direct mode component drivers.
            *pnX = *pnY = 0;
            *pnWidth = *pnHeight = 1440;
        }

        void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_GetProjectionRaw",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(eEye == vr::Eye_Left ? "Left" : "Right", "Eye"));

            *pfLeft = tan(m_cachedEyeFov[eEye].angleLeft) * m_horizontalFovTangent;
            *pfRight = tan(m_cachedEyeFov[eEye].angleRight) * m_horizontalFovTangent;
            // Top and bottom are backwards per SteamVR documentation.
            *pfTop = tan(m_cachedEyeFov[eEye].angleDown) * m_verticalFovTangent;
            *pfBottom = tan(m_cachedEyeFov[eEye].angleUp) * m_verticalFovTangent;

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetProjectionRaw",
                                  TLArg(*pfLeft, "Left"),
                                  TLArg(*pfRight, "Right"),
                                  TLArg(*pfBottom, "Bottom"),
                                  TLArg(*pfTop, "Top"));
        }

        vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eEye, float fU, float fV) override {
            // Not used for direct mode component drivers.
            return {};
        }

        void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
            // Not used for direct mode component drivers.
            *pnX = *pnY = 0;
            *pnWidth = *pnHeight = 1440;
        }

        bool ComputeInverseDistortion(
            vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override {
            // Not used for direct mode component drivers.
            return false;
        }

        void CreateSwapTextureSet(uint32_t unPid,
                                  const SwapTextureSetDesc_t* pSwapTextureSetDesc,
                                  SwapTextureSet_t* pOutSwapTextureSet) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_CreateSwapTextureSet",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(unPid, "Pid"),
                                   TLArg(pSwapTextureSetDesc->nWidth, "Width"),
                                   TLArg(pSwapTextureSetDesc->nHeight, "Height"),
                                   TLArg(pSwapTextureSetDesc->nFormat, "Format"),
                                   TLArg(pSwapTextureSetDesc->nSampleCount, "SampleCount"));

            if (pSwapTextureSetDesc->nWidth && pSwapTextureSetDesc->nHeight) {
                std::unique_lock lock(m_swapsetsMutex);
                auto& swapset = m_swapsets.emplace_back(std::make_unique<TextureSwapset>());
                swapset->pid = unPid;

                swapset->info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
                swapset->info.arraySize = 1;
                swapset->info.mipCount = 1;
                swapset->info.faceCount = 1;
                swapset->info.format = (int64_t)GetTypedFormat((DXGI_FORMAT)pSwapTextureSetDesc->nFormat);
                swapset->info.width = pSwapTextureSetDesc->nWidth;
                swapset->info.height = pSwapTextureSetDesc->nHeight;
                swapset->info.sampleCount = pSwapTextureSetDesc->nSampleCount;
                swapset->info.usageFlags = (!IsDepthFormat((DXGI_FORMAT)pSwapTextureSetDesc->nFormat)
                                                ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                                                : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                                           XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

                // Most runtimes are capable of creating UAV-compatible swapchains, even with SRGB formats. However
                // CloudXR is not. Try with a fallback, and sharpening needs to be run with a Pixel Shader.
                swapset->info.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
                if (XR_FAILED(xrCreateSwapchain(
                        m_session.Get(), &swapset->info, swapset->swapchain.Put(xrDestroySwapchain)))) {
                    swapset->info.usageFlags &= ~XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
                    CHECK_XRCMD(
                        xrCreateSwapchain(m_session.Get(), &swapset->info, swapset->swapchain.Put(xrDestroySwapchain)));
                }

                uint32_t count = 0;
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapset->swapchain.Get(), 0, &count, nullptr));

                std::vector<XrSwapchainImageD3D11KHR> images(3, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapset->swapchain.Get(),
                                                       count,
                                                       &count,
                                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

                // CloudXR does not give us shareable textures. We will manage a copy.

                D3D11_TEXTURE2D_DESC desc{};
                images[0].texture->GetDesc(&desc);
                TraceLoggingWriteTagged(local,
                                        "HmdDriver_CreateSwapTextureSet_Desc",
                                        TLArg(desc.Width, "Width"),
                                        TLArg(desc.Height, "Height"),
                                        TLArg(desc.ArraySize, "ArraySize"),
                                        TLArg(desc.MipLevels, "MipCount"),
                                        TLArg(desc.SampleDesc.Count, "SampleCount"),
                                        TLArg((UINT)desc.Format, "Format"),
                                        TLArg((UINT)desc.Usage, "Usage"),
                                        TLArg(desc.BindFlags, "BindFlags"),
                                        TLArg(desc.CPUAccessFlags, "CPUAccessFlags"),
                                        TLArg(desc.MiscFlags, "MiscFlags"));

                uint32_t index = 0;
                for (const auto& image : images) {
                    swapset->swapchainTextures[index] = image.texture;

                    if (!index) {
                        // Create a shareable copy of the texture.
                        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                        CHECK_HRCMD(m_d3d11Device->CreateTexture2D(
                            &desc, nullptr, swapset->textures[index].ReleaseAndGetAddressOf()));
                    } else {
                        // We only create one texture to save memory. Since we do a copy in Present(), there is no need
                        // for the runtime to hold onto that texture.
                        swapset->textures[index] = swapset->textures[0];
                    }

                    ComPtr<IDXGIResource1> resource;
                    CHECK_HRCMD(swapset->textures[index]->QueryInterface(resource.ReleaseAndGetAddressOf()));

                    HANDLE handle = {};
                    CHECK_HRCMD(resource->GetSharedHandle(&handle));
                    TraceLoggingWriteTagged(local, "HmdDriver_CreateSwapTextureSet_Texture", TLPArg(handle, "Handle"));
                    swapset->handles[index] = handle;
                    pOutSwapTextureSet->rSharedTextureHandles[index++] = (vr::SharedTextureHandle_t)handle;
                }

                // Acquire the first image.
                swapset->acquiredIndex = swapset->nextIndex++;
                swapset->nextIndex = swapset->nextIndex < 3 ? swapset->nextIndex : 0;
            }

            TraceLoggingWriteStop(local, "HmdDriver_CreateSwapTextureSet");
        }

        void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_DestroySwapTextureSet",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLPArg((HANDLE)sharedTextureHandle, "Handle"));

            std::unique_lock lock(m_swapsetsMutex);
            for (auto it = m_swapsets.begin(); it != m_swapsets.end(); it++) {
                if ((*it)->handles[0] == (HANDLE)sharedTextureHandle ||
                    (*it)->handles[1] == (HANDLE)sharedTextureHandle ||
                    (*it)->handles[2] == (HANDLE)sharedTextureHandle) {
                    m_swapsets.erase(it);
                    break;
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_DestroySwapTextureSet", TLArg(m_deviceIndex, "ObjectId"));
        }

        void DestroyAllSwapTextureSets(uint32_t unPid) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HmdDriver_DestroyAllSwapTextureSets", TLArg(m_deviceIndex, "ObjectId"), TLArg(unPid, "Pid"));

            std::unique_lock lock(m_swapsetsMutex);
            for (auto it = m_swapsets.begin(); it != m_swapsets.end();) {
                if ((*it)->pid == unPid) {
                    it = m_swapsets.erase(it);
                } else {
                    it++;
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_DestroyAllSwapTextureSets");
        }

        void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                        uint32_t (*pIndices)[2]) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_GetNextSwapTextureSetIndex",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLPArg((HANDLE)sharedTextureHandles[0], "Handle0"),
                                   TLPArg((HANDLE)sharedTextureHandles[1], "Handle1"));

            std::shared_lock lock(m_swapsetsMutex);

            uint32_t goodIndicesCount = 0;
            for (uint32_t eye = 0; eye < 2; eye++) {
                for (auto it = m_swapsets.begin(); it != m_swapsets.end(); it++) {
                    if ((*it)->handles[0] == (HANDLE)sharedTextureHandles[eye] ||
                        (*it)->handles[1] == (HANDLE)sharedTextureHandles[eye] ||
                        (*it)->handles[2] == (HANDLE)sharedTextureHandles[eye]) {
                        auto& swapset = *it;
                        (*pIndices)[eye] = *swapset->acquiredIndex;
                        goodIndicesCount++;
                        break;
                    }
                }
            }

            if (goodIndicesCount != 2) {
                TraceLoggingWriteTagged(local, "HmdDriver_GetNextSwapTextureSetIndex_SwapsetNotFound");
            }

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetNextSwapTextureSetIndex",
                                  TLArg((*pIndices)[0], "Index0"),
                                  TLArg((*pIndices)[1], "Index1"));
        }

        void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_SubmitLayer",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLPArg((HANDLE)perEye[0].hTexture, "Handle0"),
                                   TLPArg((HANDLE)perEye[1].hTexture, "Handle1"),
                                   TLPArg((HANDLE)perEye[0].hDepthTexture, "DepthHandle0"),
                                   TLPArg((HANDLE)perEye[1].hDepthTexture, "DepthHandle1"));

            std::unique_lock lock(m_swapsetsMutex);

            // TODO: "Now playing" layer seems to obstruct game layer.
            FrameLayer layer;
            uint32_t goodViewsCount = 0;
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                if (!perEye[eye].hTexture) {
                    continue;
                }

                StoreXrPose(&layer.views[eye].pose, LoadHmdMatrix34(perEye[eye].mHmdPose));
                DirectX::XMFLOAT4X4 projection;
                DirectX::XMStoreFloat4x4(&projection, LoadHmdMatrix44(perEye[eye].mProjection));
                layer.views[eye].fov = DecomposeProjectionMatrix(projection);
                for (auto it = m_swapsets.begin(); it != m_swapsets.end(); it++) {
                    if ((*it)->handles[0] == (HANDLE)perEye[eye].hTexture ||
                        (*it)->handles[1] == (HANDLE)perEye[eye].hTexture ||
                        (*it)->handles[2] == (HANDLE)perEye[eye].hTexture) {
                        auto& swapset = *it;
                        layer.views[eye].subImage.swapchain = swapset->swapchain.Get();
                        layer.views[eye].subImage.imageRect = swapset->layerRect = {
                            {
                                std::clamp((int32_t)std::round(perEye[eye].bounds.uMin * swapset->info.width),
                                           0,
                                           (int32_t)swapset->info.width),
                                std::clamp((int32_t)std::round(perEye[eye].bounds.vMin * swapset->info.height),
                                           0,
                                           (int32_t)swapset->info.height),
                            },
                            {
                                std::clamp((int32_t)std::round((perEye[eye].bounds.uMax - perEye[eye].bounds.uMin) *
                                                               swapset->info.width),
                                           0,
                                           (int32_t)swapset->info.width),
                                std::clamp((int32_t)std::round((perEye[eye].bounds.vMax - perEye[eye].bounds.vMin) *
                                                               swapset->info.height),
                                           0,
                                           (int32_t)swapset->info.height),

                            }};
                        swapset->layerIndex = (uint32_t)m_frameLayers.size();
                        goodViewsCount++;
                    }
                }
                TraceLoggingWriteTagged(local,
                                        "HmdDriver_SubmitLayer",
                                        TLArg(eye == vr::Eye_Left ? "Left" : "Right", "Eye"),
                                        TLArg(xr::ToString(layer.views[eye].pose).c_str(), "Pose"),
                                        TLArg(xr::ToString(layer.views[eye].fov).c_str(), "Fov"),
                                        TLArg(xr::ToString(layer.views[eye].subImage.imageRect).c_str(), "ImageRect"));
            }

            if (goodViewsCount == xr::StereoView::Count) {
                m_frameLayers.emplace_back(layer);
            } else {
                TraceLoggingWriteTagged(local, "HmdDriver_SubmitLayer_SwapsetNotFound");
            }

            TraceLoggingWriteStop(local, "HmdDriver_SubmitLayer");
        }

        void Present(vr::SharedTextureHandle_t syncTexture) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_Present",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLPArg((HANDLE)syncTexture, "SyncTexture"));

            // Acquire the keyed mutex passed by the compositor to signal the end of the GPU work.
            if ((HANDLE)syncTexture != m_syncTextureHandle) {
                if (FAILED(m_d3d11Device->OpenSharedResource((HANDLE)syncTexture,
                                                             IID_PPV_ARGS(m_syncTexture.ReleaseAndGetAddressOf())))) {
                    CHECK_HRCMD(m_d3d11Device->OpenSharedResource1(
                        (HANDLE)syncTexture, IID_PPV_ARGS(m_syncTexture.ReleaseAndGetAddressOf())));
                }
                m_syncTextureHandle = (HANDLE)syncTexture;
            }

            CHECK_HRCMD(m_syncTexture->QueryInterface(IID_PPV_ARGS(m_syncMutex.ReleaseAndGetAddressOf())));

            HRESULT result;
            {
                TraceLocalActivity(acquireSync);
                TraceLoggingWriteStart(acquireSync, "HmdDriver_Present_AcquireSync");
                result = m_syncMutex->AcquireSync(0, 100);
                TraceLoggingWriteStop(acquireSync, "HmdDriver_Present_AcquireSync", TLArg(result, "Result"));
            }
            CHECK_HRCMD(result);

            if (result == WAIT_TIMEOUT) {
                TraceLoggingWriteTagged(local, "HmdDriver_Present_AcquireSyncTimedout");
                m_syncMutex.Reset();
            } else if (result == WAIT_ABANDONED) {
                TraceLoggingWriteTagged(local, "HmdDriver_Present_AcquireSyncAbandoned");
                m_syncMutex->ReleaseSync(0);
                m_syncMutex.Reset();
            }

            // Read oldest timer (before we start a new measurement).
            const auto lastCopyTime = m_gpuTimerCopy[m_currentTimerIndex]->query();
            if (IsTraceEnabled()) {
                m_gpuTimerCopy[m_currentTimerIndex]->start();
            }

            // Release all swapchain images from this frame.
            ProcessFrameSwapchains();

            if (IsTraceEnabled()) {
                m_gpuTimerCopy[m_currentTimerIndex]->stop();
                m_currentTimerIndex = m_currentTimerIndex++;
                m_currentTimerIndex = m_currentTimerIndex < k_numGpuTimers ? m_currentTimerIndex : 0;
            }

            // Submit the current frame.
            XrFrameEndInfo frameInfo = {XR_TYPE_FRAME_END_INFO};
            frameInfo.displayTime = m_frameState.predictedDisplayTime;
            frameInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            std::vector<XrCompositionLayerProjection> projections;
            projections.resize(m_frameLayers.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION});
            std::vector<XrCompositionLayerBaseHeader*> layers;
            layers.reserve(m_frameLayers.size());
            for (uint32_t layerIndex = 0; layerIndex < m_frameLayers.size(); layerIndex++) {
                auto& projection = projections[layerIndex];
                projection.views = m_frameLayers[layerIndex].views;
                projection.viewCount = xr::StereoView::Count;
                projection.layerFlags = layerIndex > 0 ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                projection.space = m_referenceSpace.Get();
                layers.push_back((XrCompositionLayerBaseHeader*)&projection);
            }
            frameInfo.layers = layers.data();
            frameInfo.layerCount = (uint32_t)m_frameLayers.size();
            {
                TraceLocalActivity(endFrame);
                TraceLoggingWriteStart(endFrame,
                                       "HmdDriver_Present_EndFrame",
                                       TLArg(frameInfo.displayTime, "DisplayTime"),
                                       TLArg(frameInfo.layerCount, "LayerCount"));
                CHECK_XRCMD(xrEndFrame(m_session.Get(), &frameInfo));
                TraceLoggingWriteStop(endFrame, "HmdDriver_Present_EndFrame");
            }

            m_frameLayers.clear();

            // When using RenderDoc, signal a frame through the dummy swapchain.
            if (m_dxgiSwapchain) {
                m_dxgiSwapchain->Present(0, 0);
            }

            TraceLoggingWriteStop(
                local, "HmdDriver_Present", TLArg(m_frameTimes.size(), "Fps"), TLArg(lastCopyTime, "LastCopyTimeUs"));
        }

        void PostPresent(const Throttling_t* pThrottling) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_PostPresent", TLArg(m_deviceIndex, "ObjectId"));

            {
                // Flush and release the keyed mutex.
                auto guard = MakeScopeGuard([&]() {
                    m_d3d11Context->Flush();

                    if (m_syncMutex) {
                        if (!m_dxgiSwapchain) {
                            CHECK_HRCMD(m_syncMutex->ReleaseSync(0));
                        } else {
                            // When using RenderDoc, this call doesn't make the debugger happy. Forego error checks.
                            m_syncMutex->ReleaseSync(0);
                        }
                        m_syncMutex = nullptr;
                    }
                });

                // Kick-off the next frame.
                {
                    TraceLocalActivity(waitFrame);
                    TraceLoggingWriteStart(waitFrame, "HmdDriver_PostPresent_WaitFrame");
                    CHECK_XRCMD(xrWaitFrame(m_session.Get(), nullptr, &m_frameState));
                    TraceLoggingWriteStop(waitFrame,
                                          "HmdDriver_PostPresent_WaitFrame",
                                          TLArg(m_frameState.predictedDisplayTime, "PredictedDisplayTime"),
                                          TLArg(m_frameState.predictedDisplayPeriod, "PredictedDisplayPeriod"));
                }

                const float runningStart =
                    std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "running_start"), 0.f, 1.f);
                const auto vsyncTimeOffset = runningStart * m_frameState.predictedDisplayPeriod / 1e9f;
                TraceLoggingWriteTagged(
                    local, "HmdDriver_PostPresent_VsyncEvent", TLArg(vsyncTimeOffset, "VsyncTimeOffset"));
                vr::VRServerDriverHost()->VsyncEvent(vsyncTimeOffset);

                // Acquire swapset images for the upcoming frame.
                {
                    std::shared_lock lock(m_swapsetsMutex);

                    for (auto& it : m_swapsets) {
                        auto& swapset = *it;

                        if (swapset.acquiredIndex) {
                            continue;
                        }

                        swapset.acquiredIndex = swapset.nextIndex++;
                        swapset.nextIndex = swapset.nextIndex < 3 ? swapset.nextIndex : 0;

                        // Create a dependency on the GPU with the sync texture.
                        D3D11_BOX box = {};
                        box.right = box.bottom = box.back = 1;
                        m_d3d11Context->CopySubresourceRegion(
                            m_syncTexture.Get(), 0, 0, 0, 0, swapset.textures[*swapset.acquiredIndex].Get(), 0, &box);
                    }
                }
            }

            UpdateHeadProperties(m_frameState.predictedDisplayTime);
            {
                TraceLocalActivity(beginFrame);
                TraceLoggingWriteStart(beginFrame, "HmdDriver_PostPresent_BeginFrame");
                CHECK_XRCMD(xrBeginFrame(m_session.Get(), nullptr));
                TraceLoggingWriteStop(beginFrame, "HmdDriver_PostPresent_BeginFrame");
            }

            // Update HMD, controllers, and eye tracking.
            {
                LARGE_INTEGER nowQpc = {};
                QueryPerformanceCounter(&nowQpc);
                XrTime now = 0;
                CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(m_instance.Get(), &nowQpc, &now));

                const float headPredictionBlending =
                    std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "head_prediction_blend"), 0.f, 1.f);
                const float controllerPredictionBlending =
                    std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "controller_prediction_blend"), 0.f, 1.f);

                TraceLoggingWriteTagged(local,
                                        "HmdDriver_PostPresent_ApplyPrediction",
                                        TLArg(now, "Now"),
                                        TLArg(headPredictionBlending, "HeadPredictionBlending"),
                                        TLArg(controllerPredictionBlending, "ControllerPredictionBlending"));

                const auto applyPredictionBlending = [&](float blending) {
                    return (XrTime)(now + blending * std::max(m_frameState.predictedDisplayTime - now, 0ll));
                };

                const XrTime timeForHeadTracking = applyPredictionBlending(headPredictionBlending);
                const XrTime timeForControllerTracking = applyPredictionBlending(controllerPredictionBlending);

                UpdateTrackingState(timeForHeadTracking);
                for (uint32_t side = 0; side < 2; side++) {
                    m_controllerDriver[side]->UpdateTrackingState(timeForControllerTracking);
                    if (m_handDriver[side]) {
                        m_handDriver[side]->UpdateTrackingState(timeForControllerTracking);
                    }
                }
                if (m_hasEyeTracking) {
                    // Always use predicted display time for eye tracking.
                    UpdateEyeTrackingState(m_frameState.predictedDisplayTime);
                }

                // Update the FPS counter.
                m_frameTimes.push_back(now);
                while (now - m_frameTimes.front() >= 1'000'000'000) {
                    m_frameTimes.pop_front();
                }
            }

            // Update inputs (buttons, etc).
            {
                XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
                const XrActiveActionSet activeActionSet{m_actionSet.Get(), XR_NULL_PATH};
                syncInfo.activeActionSets = &activeActionSet;
                syncInfo.countActiveActionSets = 1;
                CHECK_XRCMD(xrSyncActions(m_session.Get(), &syncInfo));

                for (uint32_t side = 0; side < 2; side++) {
                    m_controllerDriver[side]->UpdateInputsState(m_frameState.predictedDisplayTime);
                    if (m_handDriver[side]) {
                        m_handDriver[side]->UpdateInputsState(m_frameState.predictedDisplayTime);
                    }
                }
            }

            m_sharpening = std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "sharpening"), 0.f, 1.f);

            TraceLoggingWriteStop(local, "HmdDriver_PostPresent");
        }

        void SendHapticEvent(const vr::VREvent_HapticVibration_t& data) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_SendHapticEvent",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(data.containerHandle, "Container"));

            // Dispatch events to the controllers.
            for (uint32_t side = 0; side < 2; side++) {
                if (vr::VRProperties()->TrackedDeviceToPropertyContainer(m_controllerDriver[side]->GetDeviceIndex()) ==
                    data.containerHandle) {
                    m_controllerDriver[side]->SendHapticEvent(data);
                    break;
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_SendHapticEvent");
        }

        void ApplySettingsChanges() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_ApplySettingsChanges", TLArg(m_deviceIndex, "ObjectId"));

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Dispatch settings update to the controllers.
            for (uint32_t side = 0; side < 2; side++) {
                m_controllerDriver[side]->ApplySettingsChanges();
                if (m_handDriver[side]) {
                    m_handDriver[side]->ApplySettingsChanges();
                }
            }

            // Dispatch settings to the client process.
            if (m_sharedMemory) {
                m_sharedMemory->allowOpenDashboard =
                    vr::VRSettings()->GetInt32("driver_cloudxr", "use_windows_key") >= 2;
            }

            TraceLoggingWriteStop(local, "HmdDriver_ApplySettingsChanges");
        }

        void RunFrame() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_RunFrame", TLArg(m_deviceIndex, "ObjectId"));

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Pump all events. We do nothing with them at this time.
            {
                XrEventDataBuffer buffer = {XR_TYPE_EVENT_DATA_BUFFER};
                while (xrPollEvent(m_instance.Get(), &buffer) == XR_SUCCESS) {
                }
            }

            // We defer adding the drivers here, since it cannot be done from Activate().
            if (m_isFirstFrame) {
                for (uint32_t side = 0; side < 2; side++) {
                    vr::VRServerDriverHost()->TrackedDeviceAdded(m_controllerDriver[side]->GetSerialNumber(),
                                                                 vr::TrackedDeviceClass_Controller,
                                                                 m_controllerDriver[side].get());
                    if (m_handDriver[side]) {
                        vr::VRServerDriverHost()->TrackedDeviceAdded(m_handDriver[side]->GetSerialNumber(),
                                                                     vr::TrackedDeviceClass_Controller,
                                                                     m_handDriver[side].get());
                    }
                }
            }

            m_isFirstFrame = false;

            // Handle client utility events.
            if (m_sharedMemory) {
                // See if the client process is already started.
                if (m_clientProcessInfo.dwProcessId) {
                    if (!WaitForSingleObject(m_clientProcessInfo.hProcess, 0)) {
                        CloseHandle(m_clientProcessInfo.hProcess);

                        // Mark as finished.
                        m_clientProcessInfo = {};
                    }
                }

                // Start the client process if needed.
                if (!m_clientProcessInfo.dwProcessId) {
                    HMODULE thisDll = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)&CreateHmdDriver,
                                       &thisDll);
                    WCHAR path[MAX_PATH]{};
                    GetModuleFileNameW(thisDll, path, MAX_PATH);
                    const auto root = std::filesystem::path(path).parent_path();

                    STARTUPINFO info = {sizeof(info)};
                    if (!CreateProcess((root / L"client_utility.exe").wstring().c_str(),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       FALSE,
                                       0,
                                       nullptr,
                                       root.parent_path().parent_path().c_str(),
                                       &info,
                                       &m_clientProcessInfo)) {
                        DriverLog("Failed to start client utility: %d", GetLastError());
                    }
                    CloseHandle(m_clientProcessInfo.hThread);
                }

                if (InterlockedExchange(&m_sharedMemory->sendClickEvent, 0)) {
                    vr::VRDriverInput()->UpdateBooleanComponent(m_components[ComponentSystemClick], true, 0);
                    m_inClickEvent = true;
                } else if (m_inClickEvent) {
                    vr::VRDriverInput()->UpdateBooleanComponent(m_components[ComponentSystemClick], false, 0);
                    m_inClickEvent = false;
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_RunFrame");
        }

        void UpdateHeadProperties(XrTime time) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HmdDriver_UpdateHeadProperties", TLArg(m_deviceIndex, "ObjectId"), TLArg(time, "Time"));

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            vr::VRDriverInput()->UpdateBooleanComponent(m_components[ComponentPresence], m_frameState.shouldRender, 0);

            LARGE_INTEGER nowQpc = {};
            QueryPerformanceCounter(&nowQpc);
            XrTime now = 0;
            CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(m_instance.Get(), &nowQpc, &now));

            float vsyncToPhotonsTime = (m_frameState.predictedDisplayTime - now) / 1e9f;
            float refreshRate = 1e9f / m_frameState.predictedDisplayPeriod;
#if 1
            // TODO: These values are all over the place.
            // Perform a quick heuristic to filter them out.
            // - Ignore values below 72 Hz.
            // - Detect by range.
            if (refreshRate >= 71.f) {
                if (refreshRate >= 118.f) {
                    refreshRate = 120.f;
                } else if (refreshRate >= 98.f) {
                    refreshRate = 100.f;
                } else if (refreshRate >= 94.f) {
                    refreshRate = 96.f;
                } else if (refreshRate >= 88.f) {
                    refreshRate = 90.f;
                } else if (refreshRate >= 78.f) {
                    refreshRate = 80.f;
                } else if (refreshRate >= 70.f) {
                    refreshRate = 72.f;
                }

                // Conservatively assume 1 full frame.
                vsyncToPhotonsTime = 1.f / refreshRate;
#else
            {
#endif
                if (std::abs(m_refreshRate - refreshRate) > 1.0001f) {
                    DriverLog("Detected refresh rate: %u Hz (photon time: %.3fms)",
                              (uint32_t)std::round(refreshRate),
                              vsyncToPhotonsTime * 1e3f);
                    vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, refreshRate);
                    m_refreshRate = refreshRate;
                    vr::VRProperties()->SetFloatProperty(
                        container, vr::Prop_SecondsFromVsyncToPhotons_Float, vsyncToPhotonsTime);
                    m_vsyncToPhotonsTime = vsyncToPhotonsTime;
                }
            }

            TraceLoggingWriteTagged(local,
                                    "HmdDriver_UpdateHeadProperties",
                                    TLArg(vsyncToPhotonsTime, "VsyncToPhotonsTime"),
                                    TLArg(refreshRate, "RefreshRate"));

            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.space = m_viewSpace.Get();
            locateInfo.displayTime = time;
            XrViewState viewsState = {XR_TYPE_VIEW_STATE};
            XrView views[xr::StereoView::Count] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t count = xr::StereoView::Count;
            CHECK_XRCMD(xrLocateViews(m_session.Get(), &locateInfo, &viewsState, count, &count, views));
            TraceLoggingWriteTagged(local,
                                    "HmdDriver_UpdateHeadProperties",
                                    TLArg((int)viewsState.viewStateFlags, "ViewStateFlags"),
                                    TLArg(xr::ToString(views[xr::StereoView::Left].pose).c_str(), "LeftPose"),
                                    TLArg(xr::ToString(views[xr::StereoView::Left].fov).c_str(), "LeftFov"),
                                    TLArg(xr::ToString(views[xr::StereoView::Right].pose).c_str(), "RightPose"),
                                    TLArg(xr::ToString(views[xr::StereoView::Right].fov).c_str(), "RightFov"));

            if (Pose::IsPoseValid(viewsState.viewStateFlags)) {
                vr::VRServerDriverHost()->SetDisplayEyeToHead(
                    m_deviceIndex,
                    StoreHmdMatrix34(LoadXrPose(views[xr::StereoView::Left].pose)),
                    StoreHmdMatrix34(LoadXrPose(views[xr::StereoView::Right].pose)));
            }

            const auto toProjectionRaw = [](const XrFovf& fov) -> vr::HmdRect2_t {
                return {{tan(fov.angleLeft), tan(fov.angleDown)}, {tan(fov.angleRight), tan(fov.angleUp)}};
            };
            vr::VRServerDriverHost()->SetDisplayProjectionRaw(m_deviceIndex,
                                                              toProjectionRaw(views[xr::StereoView::Left].fov),
                                                              toProjectionRaw(views[xr::StereoView::Right].fov));
            m_cachedEyeFov[xr::StereoView::Left] = views[xr::StereoView::Left].fov;
            m_cachedEyeFov[xr::StereoView::Right] = views[xr::StereoView::Right].fov;

            TraceLoggingWriteStop(local, "HmdDriver_UpdateHeadProperties");
        }

        void UpdateTrackingState(XrTime time) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HmdDriver_UpdateTrackingState", TLArg(m_deviceIndex, "ObjectId"), TLArg(time, "Time"));

            vr::DriverPose_t pose = {};
            pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1.0;
            pose.deviceIsConnected = true;
            pose.result = vr::TrackingResult_Running_OutOfRange;

            XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
            XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
            location.next = &velocity;
            CHECK_XRCMD(xrLocateSpace(m_viewSpace.Get(), m_referenceSpace.Get(), time, &location));
            TraceLoggingWriteTagged(local,
                                    "HmdDriver_UpdateTrackingState",
                                    TLArg((int)location.locationFlags, "LocationFlags"),
                                    TLArg(xr::ToString(location.pose).c_str(), "Pose"));

            pose.poseIsValid = Pose::IsPoseValid(location.locationFlags);
            if (pose.poseIsValid) {
                pose.vecPosition[0] = location.pose.position.x;
                pose.vecPosition[1] = location.pose.position.y;
                pose.vecPosition[2] = location.pose.position.z;
                pose.qRotation.x = location.pose.orientation.x;
                pose.qRotation.y = location.pose.orientation.y;
                pose.qRotation.z = location.pose.orientation.z;
                pose.qRotation.w = location.pose.orientation.w;

                if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                    pose.vecVelocity[0] = velocity.linearVelocity.x;
                    pose.vecVelocity[1] = velocity.linearVelocity.y;
                    pose.vecVelocity[2] = velocity.linearVelocity.z;
                }
                if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
                    pose.vecAngularVelocity[0] = velocity.angularVelocity.x;
                    pose.vecAngularVelocity[1] = velocity.angularVelocity.y;
                    pose.vecAngularVelocity[2] = velocity.angularVelocity.z;
                }

                LARGE_INTEGER nowQpc = {};
                QueryPerformanceCounter(&nowQpc);
                XrTime now = 0;
                CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(m_instance.Get(), &nowQpc, &now));
                pose.poseTimeOffset = (time - now) / 1e9;

                if (Pose::IsPoseTracked(location.locationFlags)) {
                    pose.result = vr::TrackingResult_Running_OK;
                }
            }
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_deviceIndex, pose, sizeof(pose));

            TraceLoggingWriteStop(local, "HmdDriver_UpdateTrackingState", TLArg(pose.poseTimeOffset, "PoseTimeOffset"));
        }

        void UpdateEyeTrackingState(XrTime time) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HmdDriver_UpdateEyeTrackingState", TLArg(m_deviceIndex, "ObjectId"), TLArg(time, "Time"));

            XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
            CHECK_XRCMD(xrLocateSpace(m_eyeGazeSpace.Get(), m_viewSpace.Get(), time, &location));
            TraceLoggingWriteTagged(local,
                                    "HmdDriver_UpdateEyeTrackingState",
                                    TLArg((int)location.locationFlags, "LocationFlags"),
                                    TLArg(xr::ToString(location.pose).c_str(), "Pose"));

            vr::VREyeTrackingData_t data = {};
            data.bValid = Pose::IsPoseValid(location.locationFlags);
            data.bTracked = Pose::IsPoseTracked(location.locationFlags);
            data.bActive = data.bValid;
            if (data.bValid) {
                const auto gaze = LoadXrPose(location.pose);
                const auto gazeProjectedPoint =
                    DirectX::XMVector3Transform(DirectX::XMVectorSet(0.f, 0.f, -1.f, 1.f), gaze);

                DirectX::XMStoreFloat3((DirectX::XMFLOAT3*)&data.vGazeTarget,
                                       DirectX::XMVector3Normalize(gazeProjectedPoint));
            } else {
                DirectX::XMStoreFloat3((DirectX::XMFLOAT3*)&data.vGazeTarget, DirectX::XMVectorSet(0, 0, -1, 1));
            }
            vr::VRDriverInput()->UpdateEyeTrackingComponent(m_components[ComponentEyeGaze], &data, 0.f);

            TraceLoggingWriteStop(local, "HmdDriver_UpdateEyeTrackingState");
        }

        const char* GetSerialNumber() const override {
            return "CLOUDXR";
        }

        vr::TrackedDeviceIndex_t GetDeviceIndex() const override {
            return m_deviceIndex;
        }

      private:
        void ProcessFrameSwapchains() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_ProcessFrameSwapchains", TLArg(m_deviceIndex, "ObjectId"));

            std::shared_lock lock(m_swapsetsMutex);

            for (auto& it : m_swapsets) {
                if (it->acquiredIndex) {
                    auto& swapset = *it;

                    // Flush the render target to the swapchain.
                    uint32_t acquiredIndex;
                    CHECK_XRCMD(xrAcquireSwapchainImage(swapset.swapchain.Get(), nullptr, &acquiredIndex));

                    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    waitInfo.timeout = XR_INFINITE_DURATION;
                    CHECK_XRCMD(xrWaitSwapchainImage(swapset.swapchain.Get(), &waitInfo));

                    ID3D11Texture2D* inputTexture = swapset.textures[*swapset.acquiredIndex].Get();
                    ID3D11Texture2D* swapchainTexture = swapset.swapchainTextures[*swapset.acquiredIndex].Get();

                    if (swapset.layerIndex == 0 && m_sharpening > 0) {
                        // Setup common resources for sharpening.
                        SharpenConstants constants = {};
                        constants.topLeft = swapset.layerRect.offset;
                        constants.extent = swapset.layerRect.extent;

                        CasSetup(constants.const0,
                                 constants.const1,
                                 m_sharpening,
                                 (AF1)swapset.layerRect.extent.width,
                                 (AF1)swapset.layerRect.extent.height,
                                 (AF1)swapset.layerRect.extent.width,
                                 (AF1)swapset.layerRect.extent.height);

                        m_d3d11Context->UpdateSubresource(m_sharpeningConstants.Get(), 0, nullptr, &constants, 0, 0);

                        ComPtr<ID3D11ShaderResourceView> srv;
                        {
                            D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
                            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            desc.Format = (DXGI_FORMAT)swapset.info.format;
                            desc.Texture2D.MipLevels = 1;
                            CHECK_HRCMD(m_d3d11Device->CreateShaderResourceView(
                                inputTexture, &desc, srv.ReleaseAndGetAddressOf()));
                        }

                        if (swapset.info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
                            // Apply sharpening via Compute Shader.
                            TraceLoggingWriteTagged(local, "HmdDriver_ProcessFrameSwapchains_SharpenCS");
                            m_d3d11Context->CSSetShader(IsSrgbFormat((DXGI_FORMAT)swapset.info.format)
                                                            ? m_sharpeningShader[1].Get()
                                                            : m_sharpeningShader[0].Get(),
                                                        nullptr,
                                                        0);
                            m_d3d11Context->CSSetConstantBuffers(0, 1, m_sharpeningConstants.GetAddressOf());
                            m_d3d11Context->CSSetShaderResources(0, 1, srv.GetAddressOf());
                            ComPtr<ID3D11UnorderedAccessView> uav;
                            {
                                D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
                                desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                                desc.Format = GetUavFormat((DXGI_FORMAT)swapset.info.format);
                                CHECK_HRCMD(m_d3d11Device->CreateUnorderedAccessView(
                                    swapchainTexture, &desc, uav.ReleaseAndGetAddressOf()));
                            }
                            m_d3d11Context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

                            const uint32_t blockWidth = 16;
                            const uint32_t blockHeight = 16;
                            m_d3d11Context->Dispatch(
                                ((swapset.layerRect.extent.width + blockWidth - 1) / blockWidth),
                                ((swapset.layerRect.extent.height + blockHeight - 1) / blockHeight),
                                1);

                            // Unbind all resources to avoid D3D validation errors.
                            {
                                m_d3d11Context->CSSetShader(nullptr, nullptr, 0);
                                ID3D11Buffer* nullCbv[] = {nullptr};
                                m_d3d11Context->CSSetConstantBuffers(0, 1, nullCbv);
                                ID3D11ShaderResourceView* nullSrv[] = {nullptr};
                                m_d3d11Context->CSSetShaderResources(0, 1, nullSrv);
                                ID3D11UnorderedAccessView* nullUav[] = {nullptr};
                                m_d3d11Context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
                            }
                        } else {
                            // Apply sharpening via Pixel Shader.
                            TraceLoggingWriteTagged(local, "HmdDriver_ProcessFrameSwapchains_SharpenPS");
                            m_d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                            m_d3d11Context->VSSetShader(m_fullscreenQuadShader.Get(), nullptr, 0);
                            m_d3d11Context->PSSetShader(m_sharpeningShaderAlt.Get(), nullptr, 0);
                            m_d3d11Context->PSSetConstantBuffers(0, 1, m_sharpeningConstants.GetAddressOf());
                            m_d3d11Context->PSSetShaderResources(0, 1, srv.GetAddressOf());
                            ComPtr<ID3D11RenderTargetView> rtv;
                            {
                                D3D11_RENDER_TARGET_VIEW_DESC desc = {};
                                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                                desc.Format = (DXGI_FORMAT)swapset.info.format;
                                CHECK_HRCMD(m_d3d11Device->CreateRenderTargetView(
                                    swapchainTexture, &desc, rtv.ReleaseAndGetAddressOf()));
                            }
                            m_d3d11Context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                            D3D11_VIEWPORT viewport{};
                            viewport.TopLeftX = (float)swapset.layerRect.offset.x;
                            viewport.TopLeftY = (float)swapset.layerRect.offset.y;
                            viewport.Width = (float)swapset.layerRect.extent.width;
                            viewport.Height = (float)swapset.layerRect.extent.height;
                            viewport.MaxDepth = 1.f;
                            m_d3d11Context->RSSetViewports(1, &viewport);
                            m_d3d11Context->OMSetDepthStencilState(m_noDepthTest.Get(), 0xff);

                            m_d3d11Context->Draw(3, 0);

                            // Unbind all resources to avoid D3D validation errors.
                            {
                                m_d3d11Context->VSSetShader(nullptr, nullptr, 0);
                                m_d3d11Context->PSSetShader(nullptr, nullptr, 0);
                                ID3D11Buffer* nullCbv[] = {nullptr};
                                m_d3d11Context->PSSetConstantBuffers(0, 1, nullCbv);
                                ID3D11ShaderResourceView* nullSrv[] = {nullptr};
                                m_d3d11Context->PSSetShaderResources(0, 1, nullSrv);
                                ID3D11RenderTargetView* nullRtv[] = {nullptr};
                                m_d3d11Context->OMSetRenderTargets(1, nullRtv, nullptr);
                            }
                        }
                    } else {
                        // Simply copy.
                        TraceLoggingWriteTagged(local, "HmdDriver_ProcessFrameSwapchains_Copy");
                        D3D11_BOX box = {};
                        box.left = swapset.layerRect.offset.x;
                        box.top = swapset.layerRect.offset.y;
                        box.right = box.left + swapset.layerRect.extent.width;
                        box.bottom = box.top + swapset.layerRect.extent.height;
                        box.back = 1;
                        m_d3d11Context->CopySubresourceRegion(swapchainTexture,
                                                              0,
                                                              swapset.layerRect.offset.x,
                                                              swapset.layerRect.offset.y,
                                                              0,
                                                              inputTexture,
                                                              0,
                                                              &box);
                    }

                    CHECK_XRCMD(xrReleaseSwapchainImage(swapset.swapchain.Get(), nullptr));
                    swapset.acquiredIndex.reset();
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_ProcessFrameSwapchains");
        }

        void InitializeSession() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_InitializeSession", TLArg(m_deviceIndex, "ObjectId"));

            const auto getAdapterByLuid = [&](const LUID& luid) {
                ComPtr<IDXGIFactory1> dxgiFactory;
                CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));

                ComPtr<IDXGIAdapter1> dxgiAdapter;
                for (UINT adapterIndex = 0;; adapterIndex++) {
                    // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to
                    // enumerate.
                    CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

                    DXGI_ADAPTER_DESC1 desc;
                    CHECK_HRCMD(dxgiAdapter->GetDesc1(&desc));
                    if (!memcmp(&desc.AdapterLuid, &luid, sizeof(LUID))) {
                        TraceLoggingWriteTagged(
                            local, "HmdDriver_InitializeSession", TLArg(desc.Description, "AdapterName"));
                        DriverLog("Using adapter: %ws", desc.Description);
                        break;
                    }
                }

                return dxgiAdapter;
            };

            XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
            sessionCreateInfo.systemId = m_system.Id;

            XrGraphicsRequirementsD3D11KHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            CHECK_XRCMD(xrGetD3D11GraphicsRequirementsKHR(m_instance.Get(), m_system.Id, &graphicsRequirements));

            const D3D_FEATURE_LEVEL featureLevel = graphicsRequirements.minFeatureLevel;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            CHECK_HRCMD(D3D11CreateDevice(getAdapterByLuid(graphicsRequirements.adapterLuid).Get(),
                                          D3D_DRIVER_TYPE_UNKNOWN,
                                          0,
                                          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                          &featureLevel,
                                          1,
                                          D3D11_SDK_VERSION,
                                          device.ReleaseAndGetAddressOf(),
                                          nullptr,
                                          context.ReleaseAndGetAddressOf()));

            XrGraphicsBindingD3D11KHR graphicsBindings = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            graphicsBindings.device = device.Get();

            CHECK_HRCMD(device->QueryInterface(IID_PPV_ARGS(m_d3d11Device.ReleaseAndGetAddressOf())));
            CHECK_HRCMD(context->QueryInterface(IID_PPV_ARGS(m_d3d11Context.ReleaseAndGetAddressOf())));
            sessionCreateInfo.next = &graphicsBindings;

            CHECK_XRCMD(xrCreateSession(m_instance.Get(), &sessionCreateInfo, m_session.Put(xrDestroySession)));
            DriverLog("Using Direct3D 11");

            // Resources for CAS.
            {
                CHECK_HRCMD(m_d3d11Device->CreateComputeShader(
                    k_SharpeningCS, sizeof(k_SharpeningCS), nullptr, m_sharpeningShader[0].ReleaseAndGetAddressOf()));
                CHECK_HRCMD(m_d3d11Device->CreateComputeShader(k_SharpeningSrgbCS,
                                                               sizeof(k_SharpeningSrgbCS),
                                                               nullptr,
                                                               m_sharpeningShader[1].ReleaseAndGetAddressOf()));
                CHECK_HRCMD(m_d3d11Device->CreateVertexShader(k_FullscreenQuadVS,
                                                              sizeof(k_FullscreenQuadVS),
                                                              nullptr,
                                                              m_fullscreenQuadShader.ReleaseAndGetAddressOf()));
                CHECK_HRCMD(m_d3d11Device->CreatePixelShader(
                    k_SharpeningPS, sizeof(k_SharpeningPS), nullptr, m_sharpeningShaderAlt.ReleaseAndGetAddressOf()));

                {
                    D3D11_BUFFER_DESC desc{};
                    desc.ByteWidth = (UINT)((sizeof(SharpenConstants) + 15) / 16) * 16;
                    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    CHECK_HRCMD(
                        m_d3d11Device->CreateBuffer(&desc, nullptr, m_sharpeningConstants.ReleaseAndGetAddressOf()));
                }
                {
                    D3D11_DEPTH_STENCIL_DESC desc{};
                    CHECK_HRCMD(m_d3d11Device->CreateDepthStencilState(&desc, m_noDepthTest.ReleaseAndGetAddressOf()));
                }
            }

            for (uint32_t i = 0; i < k_numGpuTimers; i++) {
                m_gpuTimerCopy[i] = std::make_unique<D3D11GpuTimer>(device.Get(), m_d3d11Context.Get());
            }

            // If RenderDoc is loaded, then create a DXGI swapchain to signal events. Otherwise RenderDoc will
            // not see our OpenXR frames.
            HMODULE renderdocModule;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "renderdoc.dll", &renderdocModule) &&
                renderdocModule) {
                DriverLog("Detected RenderDoc\n");

                DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
                swapchainDesc.Width = 8;
                swapchainDesc.Height = 8;
                swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                swapchainDesc.SampleDesc.Count = 1;
                swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapchainDesc.BufferCount = 3;
                swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

                ComPtr<IDXGIDevice> dxgiDevice;
                CHECK_HRCMD(m_d3d11Device->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

                ComPtr<IDXGIAdapter> dxgiAdapter;
                CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));

                ComPtr<IDXGIFactory2> dxgiFactory;
                CHECK_HRCMD(dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));
                CHECK_HRCMD(dxgiFactory->CreateSwapChainForComposition(
                    dxgiDevice.Get(), &swapchainDesc, nullptr, m_dxgiSwapchain.ReleaseAndGetAddressOf()));
            }

            // Retrieve recommended render resolution.
            {
                const auto views = xr::EnumerateViewConfigurationViews(
                    m_instance.Get(), m_system.Id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

                m_renderTargetWidth = views[xr::StereoView::Left].recommendedImageRectWidth;
                m_renderTargetHeight = views[xr::StereoView::Left].recommendedImageRectHeight;
            }

            // Create reference spaces.
            {
                XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                spaceCreateInfo.poseInReferenceSpace = Pose::Identity();
                CHECK_XRCMD(xrCreateReferenceSpace(m_session.Get(), &spaceCreateInfo, m_viewSpace.Put(xrDestroySpace)));
            }
            {
                XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                const auto availableReferenceSpaces = xr::EnumerateReferenceSpaceTypes(m_session.Get());
                spaceCreateInfo.referenceSpaceType =
                    std::find(availableReferenceSpaces.cbegin(),
                              availableReferenceSpaces.cend(),
                              XR_REFERENCE_SPACE_TYPE_STAGE) != availableReferenceSpaces.cend()
                        ? XR_REFERENCE_SPACE_TYPE_STAGE
                        : XR_REFERENCE_SPACE_TYPE_LOCAL;
                spaceCreateInfo.poseInReferenceSpace = Pose::Identity();
                CHECK_XRCMD(
                    xrCreateReferenceSpace(m_session.Get(), &spaceCreateInfo, m_referenceSpace.Put(xrDestroySpace)));
            }

            TraceLoggingWriteStop(local, "HmdDriver_InitializeSession");
        }

        void InitializeInputs() {
            {
                XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
                strcpy_s(actionSetCreateInfo.actionSetName, "steamvr_actionset");
                strcpy_s(actionSetCreateInfo.localizedActionSetName, "SteamVR ActionSet");
                actionSetCreateInfo.priority = 0;
                CHECK_XRCMD(
                    xrCreateActionSet(m_instance.Get(), &actionSetCreateInfo, m_actionSet.Put(xrDestroyActionSet)));
            }

            if (m_hasEyeTracking) {
                // Create actions and action spaces for the eye gaze.
                {
                    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
                    strcpy_s(actionCreateInfo.actionName, "steamvr_eye_tracker");
                    strcpy_s(actionCreateInfo.localizedActionName, "Eye Tracker");
                    actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
                    actionCreateInfo.countSubactionPaths = 0;
                    CHECK_XRCMD(
                        xrCreateAction(m_actionSet.Get(), &actionCreateInfo, m_eyeGazeAction.Put(xrDestroyAction)));
                }
                {
                    XrActionSpaceCreateInfo actionSpaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                    actionSpaceCreateInfo.action = m_eyeGazeAction.Get();
                    actionSpaceCreateInfo.subactionPath = XR_NULL_PATH;
                    actionSpaceCreateInfo.poseInActionSpace = Pose::Identity();
                    CHECK_XRCMD(xrCreateActionSpace(
                        m_session.Get(), &actionSpaceCreateInfo, m_eyeGazeSpace.Put(xrDestroySpace)));
                }
                {
                    XrActionSuggestedBinding binding;
                    binding.action = m_eyeGazeAction.Get();

                    XrInteractionProfileSuggestedBinding suggestedBindings{
                        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                    CHECK_XRCMD(
                        xrStringToPath(m_instance.Get(), "/user/eyes_ext/input/gaze_ext/pose", &binding.binding));
                    CHECK_XRCMD(xrStringToPath(m_instance.Get(),
                                               "/interaction_profiles/ext/eye_gaze_interaction",
                                               &suggestedBindings.interactionProfile));
                    suggestedBindings.suggestedBindings = &binding;
                    suggestedBindings.countSuggestedBindings = 1;
                    CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance.Get(), &suggestedBindings));
                }
            }

            // Create actions and action spaces for each controller.
            // Reconcile all bindings and submit them.
            {
                std::vector<XrActionSuggestedBinding> bindings =
                    m_controllerDriver[0]->CreateBindings(m_actionSet.Get());
                const auto otherSideBindings = m_controllerDriver[1]->CreateBindings(m_actionSet.Get());
                bindings.insert(bindings.end(), otherSideBindings.begin(), otherSideBindings.end());

                XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                CHECK_XRCMD(xrStringToPath(m_instance.Get(),
                                           m_controllerDriver[0]->GetInteractionProfile().c_str(),
                                           &suggestedBindings.interactionProfile));
                suggestedBindings.suggestedBindings = bindings.data();
                suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
                CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance.Get(), &suggestedBindings));
            }
            if (m_handDriver[0] && m_extensions.SupportsHandInteraction) {
                std::vector<XrActionSuggestedBinding> bindings = m_handDriver[0]->CreateBindings(m_actionSet.Get());
                const auto otherSideBindings = m_handDriver[1]->CreateBindings(m_actionSet.Get());
                bindings.insert(bindings.end(), otherSideBindings.begin(), otherSideBindings.end());

                XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                CHECK_XRCMD(xrStringToPath(m_instance.Get(),
                                           m_handDriver[0]->GetInteractionProfile().c_str(),
                                           &suggestedBindings.interactionProfile));
                suggestedBindings.suggestedBindings = bindings.data();
                suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
                CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance.Get(), &suggestedBindings));
            }
            {
                XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
                const XrActionSet actionSet = m_actionSet.Get();
                attachInfo.actionSets = &actionSet;
                attachInfo.countActionSets = 1;
                CHECK_XRCMD(xrAttachSessionActionSets(m_session.Get(), &attachInfo));
            }
        }

        struct TextureSwapset {
            std::array<HANDLE, 3> handles;
            std::optional<uint32_t> acquiredIndex;
            uint32_t nextIndex = 0;
            xr::SwapchainHandle swapchain;

            XrSwapchainCreateInfo info;
            uint32_t pid;

            std::array<ComPtr<ID3D11Texture2D>, 3> textures;
            std::array<ComPtr<ID3D11Texture2D>, 3> swapchainTextures;

            uint32_t layerIndex = 0;
            XrRect2Di layerRect = {};
        };

        struct FrameLayer {
            XrCompositionLayerProjectionView views[xr::StereoView::Count] = {
                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        };

        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

        vr::VRInputComponentHandle_t m_components[ComponentCount] = {};

        bool m_hasEyeTracking = false;
        bool m_hasHandTracking = false;
        bool m_isFovMutable = false;
        uint32_t m_renderTargetWidth = 0;
        uint32_t m_renderTargetHeight = 0;
        float m_horizontalFovTangent = 1.f;
        float m_verticalFovTangent = 1.f;
        float m_refreshRate = 0;
        float m_vsyncToPhotonsTime = 0;

        // Reminder: C++ data members are destroyed in reverse order of declaration.
        xr::InstanceHandle& m_instance;
        xr::ExtensionContext& m_extensions;
        sample::SystemContext& m_system;
        xr::SessionHandle m_session;
        LUID m_adapterLuid = {};
        ComPtr<ID3D11Device1> m_d3d11Device;
        ComPtr<ID3D11DeviceContext1> m_d3d11Context;
        ComPtr<ID3D11ComputeShader> m_sharpeningShader[2];
        ComPtr<ID3D11VertexShader> m_fullscreenQuadShader;
        ComPtr<ID3D11PixelShader> m_sharpeningShaderAlt;
        ComPtr<ID3D11Buffer> m_sharpeningConstants;
        ComPtr<ID3D11DepthStencilState> m_noDepthTest;
        ComPtr<IDXGISwapChain1> m_dxgiSwapchain;

        xr::ActionSetHandle m_actionSet;
        xr::ActionHandle m_eyeGazeAction;
        xr::SpaceHandle m_viewSpace;
        xr::SpaceHandle m_referenceSpace;
        xr::SpaceHandle m_eyeGazeSpace;

        std::unique_ptr<IControllerDriver> m_controllerDriver[2];
        std::unique_ptr<IHandDriver> m_handDriver[2];

        XrFrameState m_frameState = {XR_TYPE_FRAME_STATE};
        XrFovf m_cachedEyeFov[xr::StereoView::Count] = {};
        float m_sharpening = 0.f;

        std::shared_mutex m_swapsetsMutex;
        std::vector<std::unique_ptr<TextureSwapset>> m_swapsets;

        HANDLE m_syncTextureHandle = {};
        ComPtr<ID3D11Texture2D> m_syncTexture;
        ComPtr<IDXGIKeyedMutex> m_syncMutex;

        std::vector<FrameLayer> m_frameLayers;

        wil::unique_handle m_sharedFileHandle;
        shared::SharedMemory* m_sharedMemory = nullptr;
        bool m_inClickEvent = false;
        PROCESS_INFORMATION m_clientProcessInfo = {};

        bool m_isFirstFrame = true;

        static constexpr uint32_t k_numGpuTimers = 3;
        std::unique_ptr<D3D11GpuTimer> m_gpuTimerCopy[k_numGpuTimers];
        uint32_t m_currentTimerIndex = 0;
        std::deque<XrTime> m_frameTimes;
    };

} // namespace

namespace driver {
    std::unique_ptr<IHmdDriver> CreateHmdDriver(xr::InstanceHandle& instance,
                                                xr::ExtensionContext& extensions,
                                                sample::SystemContext& system) {
        return std::make_unique<HmdDriver>(instance, extensions, system);
    }
} // namespace driver
