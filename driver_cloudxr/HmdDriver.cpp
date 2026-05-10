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
#include "D3D12Utils.h"
#include "HandDriver.h"
#include "HmdDriver.h"
#include "SharedMemory.h"
#include "Tracing.h"
#include "Utilities.h"

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

            // Initial pose fields.
            m_latestPose.qWorldFromDriverRotation.w = m_latestPose.qDriverFromHeadRotation.w =
                m_latestPose.qRotation.w = 1.f;
            m_latestPose.deviceIsConnected = true;
            m_latestPose.result = vr::TrackingResult_Running_OutOfRange;

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

            {
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

            ApplySettingsChanges();

            // TODO: Better icons would be appreciated.
            // clang-format off
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceOff_String, "{oculus}/icons/quest_headset_off.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearching_String, "{oculus}/icons/quest_headset_searching.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{oculus}/icons/quest_headset_alert_searching.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String, "{oculus}/icons/quest_headset_ready.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{oculus}/icons/quest_headset_ready_alert.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceNotReady_String, "{oculus}/icons/quest_headset_error.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceStandby_String, "{oculus}/icons/quest_headset_standby.png");
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

            if (vr::VRSettings()->GetBool("driver_cloudxr", "async_tracking_updates")) {
                m_updateThreadActive = true;
                m_updateThread = std::thread(&HmdDriver::UpdateThread, this);
            }

            m_isFirstFrame = true;

            TraceLoggingWriteStop(local, "HmdDriver_Activate");

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            if (m_updateThreadActive.exchange(false) && m_updateThread.joinable()) {
                m_updateThread.join();
            }

            if (m_sharedMemory) {
                UnmapViewOfFile(m_sharedMemory);
                m_sharedMemory = nullptr;
            }
            if (m_sharedFileHandle) {
                m_sharedFileHandle.reset();
            }

            xrEndSession(m_session.Get());
            m_actionSet.Reset();
            m_eyeGazeAction.Reset();
            m_viewSpace.Reset();
            m_referenceSpace.Reset();
            m_eyeGazeSpace.Reset();
            m_session.Reset();
            m_d3d11Context.Reset();
            m_d3d11Device.Reset();
            m_d3d12Context.reset();
            m_d3d12Device.Reset();

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
            std::shared_lock lock(m_poseMutex);
            return m_latestPose;
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

            *pnWidth = (uint32_t)m_renderTargetWidth;
            *pnHeight = (uint32_t)m_renderTargetHeight;

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

            *pfLeft = tan(m_cachedEyeFov[eEye].angleLeft);
            *pfRight = tan(m_cachedEyeFov[eEye].angleRight);
            // Top and bottom are backwards per SteamVR documentation.
            *pfTop = tan(m_cachedEyeFov[eEye].angleDown);
            *pfBottom = tan(m_cachedEyeFov[eEye].angleUp);

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
                                   TLArg(unPid, "Pid"),
                                   TLArg(pSwapTextureSetDesc->nWidth, "Width"),
                                   TLArg(pSwapTextureSetDesc->nHeight, "Height"),
                                   TLArg(pSwapTextureSetDesc->nFormat, "Format"),
                                   TLArg(pSwapTextureSetDesc->nSampleCount, "SampleCount"));

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
                                            : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

            CHECK_XRCMD(xrCreateSwapchain(m_session.Get(), &swapset->info, swapset->swapchain.Put(xrDestroySwapchain)));

            uint32_t count = 0;
            CHECK_XRCMD(xrEnumerateSwapchainImages(swapset->swapchain.Get(), 0, &count, nullptr));

            if (m_d3d12Device) {
                std::vector<XrSwapchainImageD3D12KHR> images(3, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapset->swapchain.Get(),
                                                       count,
                                                       &count,
                                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

                // In order to be robust to the internals of the OpenXR runtime, we handle non-shareable textures.
                D3D12_HEAP_FLAGS heapFlags;
                CHECK_HRCMD(images[0].texture->GetHeapProperties(nullptr, &heapFlags));

                swapset->needCopy = !(heapFlags & D3D12_HEAP_FLAG_SHARED);
                // TODO: D3D12 handles are KMT handles, unless (re)shared from D3D11 or Vulkan. how to detect that?
                swapset->useNtHandles = true;
                pOutSwapTextureSet->unTextureFlags = !swapset->useNtHandles ? 0 : vr::VRSwapTextureFlag_Shared_NTHandle;

                if (swapset->useNtHandles) {
                    // This may fail if the process is closing. Let it be (the handles will not be used anyway).
                    *swapset->processHandle.put() = OpenProcess(PROCESS_ALL_ACCESS, false, unPid);
                }

                uint32_t index = 0;
                for (const auto& image : images) {
                    swapset->swapchainTextures12[index] = image.texture;

                    if (!swapset->needCopy) {
                        swapset->textures12[index] = image.texture;
                    } else {
                        // Create a shareable copy of the texture.
                        const D3D12_RESOURCE_DESC desc = images[0].texture->GetDesc();
                        D3D12_HEAP_PROPERTIES heapProperties{};
                        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
                        heapProperties.CreationNodeMask = heapProperties.VisibleNodeMask = 1;
                        CHECK_HRCMD(m_d3d12Device->CreateCommittedResource(
                            &heapProperties,
                            D3D12_HEAP_FLAG_SHARED,
                            &desc,
                            D3D12_RESOURCE_STATE_COMMON,
                            nullptr,
                            IID_PPV_ARGS(swapset->textures12[index].ReleaseAndGetAddressOf())));
                    }

                    HANDLE handle = {};
                    if (swapset->useNtHandles) {
                        wil::unique_handle ntHandle;
                        CHECK_HRCMD(m_d3d12Device->CreateSharedHandle(
                            image.texture, nullptr, GENERIC_ALL, nullptr, ntHandle.put()));

                        // This may fail if the process is closing. Let it be (the handles will not be used anyway).
                        DuplicateHandle(GetCurrentProcess(),
                                        ntHandle.get(),
                                        swapset->processHandle.get(),
                                        &handle,
                                        0,
                                        false,
                                        DUPLICATE_SAME_ACCESS);
                    } else {
                        CHECK_HRCMD(
                            m_d3d12Device->CreateSharedHandle(image.texture, nullptr, GENERIC_ALL, nullptr, &handle));
                    }
                    TraceLoggingWriteTagged(local,
                                            "HmdDriver_CreateSwapTextureSet_Texture",
                                            TLPArg(handle, "Handle"),
                                            TLArg(swapset->useNtHandles, "IsNtHandle"));
                    swapset->handles[index] = handle;
                    pOutSwapTextureSet->rSharedTextureHandles[index++] = (vr::SharedTextureHandle_t)handle;
                }
            } else {
                std::vector<XrSwapchainImageD3D11KHR> images(3, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapset->swapchain.Get(),
                                                       count,
                                                       &count,
                                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

                // In order to be robust to the internals of the OpenXR runtime, we handle non-shareable textures and we
                // handle both types of HANDLE.
                D3D11_TEXTURE2D_DESC desc{};
                images[0].texture->GetDesc(&desc);
                // TODO: Fix the logic around keyed mutexes to avoid a texture copy.
#if 0
                swapset->useKeyedMutex = desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
#endif
                swapset->needCopy = !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) && !swapset->useKeyedMutex;
                swapset->useNtHandles = desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                pOutSwapTextureSet->unTextureFlags = !swapset->useNtHandles ? 0 : vr::VRSwapTextureFlag_Shared_NTHandle;
                if (swapset->useNtHandles) {
                    // This may fail if the process is closing. Let it be (the handles will not be used anyway).
                    *swapset->processHandle.put() = OpenProcess(PROCESS_ALL_ACCESS, false, unPid);
                }

                uint32_t index = 0;
                for (const auto& image : images) {
                    swapset->swapchainTextures11[index] = image.texture;

                    if (!swapset->needCopy) {
                        swapset->textures11[index] = image.texture;
                        if (swapset->useKeyedMutex) {
                            CHECK_HRCMD(image.texture->QueryInterface(
                                IID_PPV_ARGS(swapset->keyedMutexes[index].ReleaseAndGetAddressOf())));
                        }
                    } else {
                        // Create a shareable copy of the texture.
                        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                        CHECK_HRCMD(m_d3d11Device->CreateTexture2D(
                            &desc, nullptr, swapset->textures11[index].ReleaseAndGetAddressOf()));
                    }

                    ComPtr<IDXGIResource1> resource;
                    CHECK_HRCMD(swapset->textures11[index]->QueryInterface(resource.ReleaseAndGetAddressOf()));

                    HANDLE handle = {};
                    if (swapset->useNtHandles) {
                        wil::unique_handle ntHandle;
                        CHECK_HRCMD(resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put()));

                        // This may fail if the process is closing. Let it be (the handles will not be used anyway).
                        DuplicateHandle(GetCurrentProcess(),
                                        ntHandle.get(),
                                        swapset->processHandle.get(),
                                        &handle,
                                        0,
                                        false,
                                        DUPLICATE_SAME_ACCESS);
                    } else {
                        CHECK_HRCMD(resource->GetSharedHandle(&handle));
                    }
                    TraceLoggingWriteTagged(local,
                                            "HmdDriver_CreateSwapTextureSet_Texture",
                                            TLPArg(handle, "Handle"),
                                            TLArg(swapset->useNtHandles, "IsNtHandle"));
                    swapset->handles[index] = handle;
                    pOutSwapTextureSet->rSharedTextureHandles[index++] = (vr::SharedTextureHandle_t)handle;
                }
            }

            if (swapset->needCopy) {
                static bool logged = false;
                if (!logged) {
                    DriverLog("Runtime did not distribute swapchains with shareable properties, using slow path...");
                }
                logged = true;
            }

            // Acquire the first image.
            uint32_t acquiredIndex = 0;
            CHECK_XRCMD(xrAcquireSwapchainImage(swapset->swapchain.Get(), nullptr, &acquiredIndex));
            swapset->acquiredIndex = acquiredIndex;

            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            CHECK_XRCMD(xrWaitSwapchainImage(swapset->swapchain.Get(), &waitInfo));

            if (swapset->useKeyedMutex) {
                CHECK_HRCMD(swapset->keyedMutexes[acquiredIndex]->ReleaseSync(0));
            }

            TraceLoggingWriteStop(local, "HmdDriver_CreateSwapTextureSet");
        }

        void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HmdDriver_DestroySwapTextureSet", TLPArg((HANDLE)sharedTextureHandle, "Handle"));

            std::unique_lock lock(m_swapsetsMutex);
            for (auto it = m_swapsets.begin(); it != m_swapsets.end(); it++) {
                if ((*it)->handles[0] == (HANDLE)sharedTextureHandle ||
                    (*it)->handles[1] == (HANDLE)sharedTextureHandle ||
                    (*it)->handles[2] == (HANDLE)sharedTextureHandle) {
                    m_swapsets.erase(it);
                    break;
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_DestroySwapTextureSet");
        }

        void DestroyAllSwapTextureSets(uint32_t unPid) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_DestroyAllSwapTextureSets", TLArg(unPid, "Pid"));

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
                        if (!swapset->acquiredIndex) {
                            uint32_t acquiredIndex;
                            CHECK_XRCMD(xrAcquireSwapchainImage(swapset->swapchain.Get(), nullptr, &acquiredIndex));
                            swapset->acquiredIndex = acquiredIndex;
                            (*pIndices)[eye] = acquiredIndex;

                            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                            waitInfo.timeout = XR_INFINITE_DURATION;
                            CHECK_XRCMD(xrWaitSwapchainImage(swapset->swapchain.Get(), &waitInfo));

                            if (swapset->useKeyedMutex) {
                                CHECK_HRCMD(swapset->keyedMutexes[acquiredIndex]->ReleaseSync(0));
                            }
                        } else {
                            (*pIndices)[eye] = *swapset->acquiredIndex;
                        }
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
                        layer.views[eye].subImage.imageRect = {
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
                        goodViewsCount++;
                    }
                }
                TraceLoggingWriteTagged(local,
                                        "HmdDriver_SubmitLayer",
                                        TLArg(eye == vr::Eye_Left ? "Left" : "Right", "Eye"),
                                        TLArg(xr::ToString(layer.views[eye].pose).c_str(), "Pose"),
                                        TLArg(xr::ToString(layer.views[eye].fov).c_str(), "Fov"),
                                        TLArg(xr::ToString(layer.views[eye].subImage.imageRect).c_str(), "ImageRect"));

                // TODO: Submit depth. This is only useful with OpenXR apps (low priority for this driver).
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
            ComPtr<IDXGIKeyedMutex> currentSyncMutex;
            const auto it = m_syncMutexCache.find((HANDLE)syncTexture);
            if (it != m_syncMutexCache.cend()) {
                currentSyncMutex = it->second;
            } else {
                ComPtr<ID3D11Texture2D> texture;
                if (FAILED(m_d3d11Device->OpenSharedResource((HANDLE)syncTexture,
                                                             IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())))) {
                    CHECK_HRCMD(m_d3d11Device->OpenSharedResource1((HANDLE)syncTexture,
                                                                   IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
                }

                CHECK_HRCMD(texture->QueryInterface(IID_PPV_ARGS(currentSyncMutex.ReleaseAndGetAddressOf())));
                m_syncMutexCache.insert_or_assign((HANDLE)syncTexture, currentSyncMutex);
            }

            HRESULT result = currentSyncMutex->AcquireSync(0, 100);
            CHECK_HRCMD(result);

            if (result == WAIT_TIMEOUT) {
                currentSyncMutex.Reset();
            } else if (result == WAIT_ABANDONED) {
                currentSyncMutex->ReleaseSync(0);
                currentSyncMutex.Reset();
            }

            // Release the keyed mutex when done.
            auto guard = MakeScopeGuard([&]() {
                if (currentSyncMutex) {
                    CHECK_HRCMD(currentSyncMutex->ReleaseSync(0));
                    currentSyncMutex = nullptr;
                }
            });

            // Release all swapchain images from this frame.
            {
                std::shared_lock lock(m_swapsetsMutex);

                for (auto& it : m_swapsets) {
                    if (it->acquiredIndex) {
                        auto& swapset = *it;

                        // Flush the render target to the swapchain if needed.
                        if (swapset.needCopy) {
                            // TODO: Copy only used rect (one rect per swapchain being submitted).
                            if (m_d3d12Device) {
                                auto commandList = m_d3d12Context->GetCommandList();
                                commandList.Commands->CopyResource(
                                    swapset.swapchainTextures12[*swapset.acquiredIndex].Get(),
                                    swapset.textures12[*swapset.acquiredIndex].Get());
                                m_d3d12Context->SubmitCommandList(commandList);
                            } else {
                                m_d3d11Context->CopyResource(swapset.swapchainTextures11[*swapset.acquiredIndex].Get(),
                                                             swapset.textures11[*swapset.acquiredIndex].Get());
                            }
                        } else if (swapset.useKeyedMutex) {
                            CHECK_HRCMD(swapset.keyedMutexes[*swapset.acquiredIndex]->AcquireSync(0, 0));
                        }

                        CHECK_XRCMD(xrReleaseSwapchainImage(swapset.swapchain.Get(), nullptr));
                        swapset.acquiredIndex.reset();
                    }
                }
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
                // TODO: There seems to be an alpha-blending issue with SteamVR's system layer.
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

            TraceLoggingWriteStop(local, "HmdDriver_Present");
        }

        void PostPresent(const Throttling_t* pThrottling) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_PostPresent", TLArg(m_deviceIndex, "ObjectId"));

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

            const bool useUpdateThread = m_updateThread.joinable();

            const float runningStart =
                std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "running_start"), 0.f, 1.f);
            vr::VRServerDriverHost()->VsyncEvent(runningStart * m_frameState.predictedDisplayPeriod / 1e9f);

            UpdateHeadProperties(m_frameState.predictedDisplayTime);
            {
                TraceLocalActivity(beginFrame);
                TraceLoggingWriteStart(beginFrame, "HmdDriver_PostPresent_BeginFrame");
                CHECK_XRCMD(xrBeginFrame(m_session.Get(), nullptr));
                TraceLoggingWriteStop(beginFrame, "HmdDriver_PostPresent_BeginFrame");
            }

            // Update HMD, controllers, and eye tracking.
            if (!useUpdateThread) {
                LARGE_INTEGER nowQpc = {};
                QueryPerformanceCounter(&nowQpc);
                XrTime now = 0;
                CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(m_instance.Get(), &nowQpc, &now));

                const float headPredictionBlending =
                    std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "head_prediction_blend"), -1.f, 1.f);
                const float controllerPredictionBlending =
                    std::clamp(vr::VRSettings()->GetFloat("driver_cloudxr", "controller_prediction_blend"), -1.f, 1.f);

                const auto applyPredictionBlending = [&](float blending) {
                    if (blending >= 0) {
                        return (XrTime)(now + blending * std::max(m_frameState.predictedDisplayTime - now, 0ll));
                    }
                    return (XrTime)(now + blending * m_frameState.predictedDisplayPeriod);
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
            const float vsyncToPhotonsTime = (m_frameState.predictedDisplayTime - now) / 1e9f;
            // TODO: These values are all over the place.
            vr::VRProperties()->SetFloatProperty(
                container, vr::Prop_SecondsFromVsyncToPhotons_Float, vsyncToPhotonsTime);
            m_vsyncToPhotonsTime = vsyncToPhotonsTime;

            const float refreshRate = 1e9f / m_frameState.predictedDisplayPeriod;
            if (refreshRate >= 58.f && std::abs(m_refreshRate - refreshRate) > 1.0001f) {
                // TODO: These values are all over the place.
                // DriverLog("Detected refresh rate: %u Hz", (uint32_t)std::round(refreshRate));
                vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, refreshRate);
            }
            m_refreshRate = refreshRate;

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
            {
                std::unique_lock lock(m_poseMutex);
                m_latestPose = pose;
            }
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_deviceIndex, pose, sizeof(pose));

            TraceLoggingWriteStop(local, "HmdDriver_UpdateTrackingState");
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
        void UpdateThread() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_UpdateThread", TLArg(m_deviceIndex, "ObjectId"));

            SetThreadDescription(GetCurrentThread(), L"HmdDriver_UpdateThread");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            const auto updatePeriodMs = vr::VRSettings()->GetInt32("driver_cloudxr", "async_update_period");
            DriverLog("Using asynchronous tracking updates with %dms period", updatePeriodMs);

            wil::unique_handle timer;
            *timer.put() = CreateWaitableTimer(nullptr, FALSE, nullptr);
            LARGE_INTEGER noDelay = {};
            SetWaitableTimer(timer.get(), &noDelay, updatePeriodMs, nullptr, nullptr, TRUE);

            while (m_updateThreadActive) {
                const bool waited = WaitForSingleObject(timer.get(), 100) == WAIT_OBJECT_0;

                LARGE_INTEGER nowQpc = {};
                QueryPerformanceCounter(&nowQpc);
                XrTime now = 0;
                CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(m_instance.Get(), &nowQpc, &now));

                TraceLoggingWriteTagged(local, "HmdDriver_UpdateThread", TLArg(waited, "Waited"), TLArg(now, "Now"));

                // Update HMD, controllers, and eye tracking.
                UpdateTrackingState(now);
                for (uint32_t side = 0; side < 2; side++) {
                    m_controllerDriver[side]->UpdateTrackingState(now);
                    if (m_handDriver[side]) {
                        m_handDriver[side]->UpdateTrackingState(now);
                    }
                }
                if (m_hasEyeTracking) {
                    UpdateEyeTrackingState(now);
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_UpdateThread");
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

            if (m_extensions.SupportsD3D12 && vr::VRSettings()->GetBool("driver_cloudxr", "prefer_d3d12")) {
                XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
                sessionCreateInfo.systemId = m_system.Id;

                XrGraphicsRequirementsD3D12KHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
                CHECK_XRCMD(xrGetD3D12GraphicsRequirementsKHR(m_instance.Get(), m_system.Id, &graphicsRequirements));

                m_adapterLuid = graphicsRequirements.adapterLuid;
                CHECK_HRCMD(D3D12CreateDevice(getAdapterByLuid(graphicsRequirements.adapterLuid).Get(),
                                              graphicsRequirements.minFeatureLevel,
                                              IID_PPV_ARGS(m_d3d12Device.ReleaseAndGetAddressOf())));
                m_d3d12Context = std::make_unique<D3D12Utils::CommandContext>(m_d3d12Device.Get(), L"Submission");

                XrGraphicsBindingD3D12KHR graphicsBindings = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
                graphicsBindings.device = m_d3d12Device.Get();
                graphicsBindings.queue = m_d3d12Context->GetCommandQueue();

                sessionCreateInfo.next = &graphicsBindings;

                CHECK_XRCMD(xrCreateSession(m_instance.Get(), &sessionCreateInfo, m_session.Put(xrDestroySession)));
                DriverLog("Using Direct3D 12");
            } else {
                XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
                sessionCreateInfo.systemId = m_system.Id;

                XrGraphicsRequirementsD3D11KHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
                CHECK_XRCMD(xrGetD3D11GraphicsRequirementsKHR(m_instance.Get(), m_system.Id, &graphicsRequirements));

                const D3D_FEATURE_LEVEL featureLevel = graphicsRequirements.minFeatureLevel;
                ComPtr<ID3D11Device> device;
                CHECK_HRCMD(D3D11CreateDevice(getAdapterByLuid(graphicsRequirements.adapterLuid).Get(),
                                              D3D_DRIVER_TYPE_UNKNOWN,
                                              0,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                              &featureLevel,
                                              1,
                                              D3D11_SDK_VERSION,
                                              device.ReleaseAndGetAddressOf(),
                                              nullptr,
                                              m_d3d11Context.ReleaseAndGetAddressOf()));

                XrGraphicsBindingD3D11KHR graphicsBindings = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
                graphicsBindings.device = device.Get();

                CHECK_HRCMD(device->QueryInterface(IID_PPV_ARGS(m_d3d11Device.ReleaseAndGetAddressOf())));
                sessionCreateInfo.next = &graphicsBindings;

                CHECK_XRCMD(xrCreateSession(m_instance.Get(), &sessionCreateInfo, m_session.Put(xrDestroySession)));
                DriverLog("Using Direct3D 11");
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
            ~TextureSwapset() {
                if (useNtHandles) {
                    // Close NT HANDLE in remote process.
                    for (auto& handle : handles) {
                        DuplicateHandle(
                            processHandle.get(), handle, nullptr, nullptr, 0, false, DUPLICATE_CLOSE_SOURCE);
                    }
                }
            }

            std::array<HANDLE, 3> handles;
            std::optional<uint32_t> acquiredIndex;
            xr::SwapchainHandle swapchain;

            XrSwapchainCreateInfo info;
            bool needCopy;
            bool useKeyedMutex;
            bool useNtHandles;
            wil::unique_handle processHandle;
            uint32_t pid;

            std::array<ComPtr<ID3D11Texture2D>, 3> textures11;
            std::array<ComPtr<ID3D11Texture2D>, 3> swapchainTextures11;
            std::array<ComPtr<ID3D12Resource>, 3> textures12;
            std::array<ComPtr<ID3D12Resource>, 3> swapchainTextures12;
            std::array<ComPtr<IDXGIKeyedMutex>, 3> keyedMutexes;
        };

        struct FrameLayer {
            XrCompositionLayerProjectionView views[xr::StereoView::Count] = {
                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        };

        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

        vr::VRInputComponentHandle_t m_components[ComponentCount] = {};

        bool m_hasEyeTracking = false;
        bool m_hasHandTracking = false;
        uint32_t m_renderTargetWidth = 0;
        uint32_t m_renderTargetHeight = 0;
        float m_refreshRate = 0;
        float m_vsyncToPhotonsTime = 0;

        std::shared_mutex m_poseMutex;
        vr::DriverPose_t m_latestPose = {};

        std::unique_ptr<IControllerDriver> m_controllerDriver[2];
        std::unique_ptr<IHandDriver> m_handDriver[2];

        xr::InstanceHandle& m_instance;
        xr::ExtensionContext& m_extensions;
        sample::SystemContext& m_system;
        xr::SessionHandle m_session;
        LUID m_adapterLuid = {};
        ComPtr<ID3D11Device1> m_d3d11Device;
        ComPtr<ID3D11DeviceContext> m_d3d11Context;
        ComPtr<ID3D12Device> m_d3d12Device;
        std::unique_ptr<D3D12Utils::CommandContext> m_d3d12Context;

        xr::ActionSetHandle m_actionSet;
        xr::ActionHandle m_eyeGazeAction;
        xr::SpaceHandle m_viewSpace;
        xr::SpaceHandle m_referenceSpace;
        xr::SpaceHandle m_eyeGazeSpace;

        XrFrameState m_frameState = {XR_TYPE_FRAME_STATE};
        XrFovf m_cachedEyeFov[xr::StereoView::Count] = {};

        std::shared_mutex m_swapsetsMutex;
        std::vector<std::unique_ptr<TextureSwapset>> m_swapsets;

        std::unordered_map<HANDLE, ComPtr<IDXGIKeyedMutex>> m_syncMutexCache;

        std::vector<FrameLayer> m_frameLayers;

        wil::unique_handle m_sharedFileHandle;
        shared::SharedMemory* m_sharedMemory = nullptr;
        bool m_inClickEvent = false;
        PROCESS_INFORMATION m_clientProcessInfo = {};

        std::atomic_bool m_updateThreadActive;
        std::thread m_updateThread;
        bool m_isFirstFrame = true;
    };

} // namespace

namespace driver {
    std::unique_ptr<IHmdDriver> CreateHmdDriver(xr::InstanceHandle& instance,
                                                xr::ExtensionContext& extensions,
                                                sample::SystemContext& system) {
        return std::make_unique<HmdDriver>(instance, extensions, system);
    }
} // namespace driver
