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

#include "HmdDriver.h"
#include "Utilities.h"
#include "Tracing.h"

#include "version.h"
#include "commit.h"

using namespace util;

namespace {
    std::unique_ptr<vr::IServerTrackedDeviceProvider> thisDriver;
    std::unique_ptr<driver::IHmdDriver> hmdDriver;

    const std::vector<const char*> k_RequestedExtensions = {
        XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
        XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
        XR_KHR_VISIBILITY_MASK_EXTENSION_NAME,
        XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
        XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
        XR_EXT_HAND_TRACKING_EXTENSION_NAME,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME};
    const std::vector<XrViewConfigurationType> k_SupportedViewConfigurationTypes = {
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
    };
    const std::vector<XrEnvironmentBlendMode> k_SupportedEnvironmentBlendModes = {
        XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
    };

    class Driver : public vr::IServerTrackedDeviceProvider {
      public:
        Driver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Ctor");

            TraceLoggingWriteStop(local, "Driver_Ctor");
        }

        virtual ~Driver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Dtor");

            Cleanup();

            TraceLoggingWriteStop(local, "Driver_Dtor");
        };

        vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Init");

            VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

            if (!m_isLoaded) {
                DriverLog("Driver version: %d.%d.%d (%s)",
                          DriverVersionMajor,
                          DriverVersionMinor,
                          DriverVersionPatch,
                          DriverCommitHash);

                // Which OpenXR runtime to use?
                std::wstring runtimeJson;
                {
                    char buffer[MAX_PATH + 1] = {};
                    vr::VRSettings()->GetString("driver_cloudxr", "openxr_runtime_json", buffer, sizeof(buffer));
                    runtimeJson = xr::utf8_to_wide(buffer);
                }

                std::filesystem::path runtimeDll;
                if (runtimeJson.empty()) {
                    // With no overrides, use CloudXR shipped with the driver.
                    HMODULE thisDll = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)&driver::CreateHmdDriver,
                                       &thisDll);
                    WCHAR path[MAX_PATH]{};
                    GetModuleFileNameW(thisDll, path, MAX_PATH);
                    const auto root = std::filesystem::path(path).parent_path();
                    runtimeDll = root / "openxr_cloudxr.dll";

                    // Start the service upon request.
                    if (vr::VRSettings()->GetBool("driver_cloudxr", "start_cloudxr_service")) {
                        try {
                            StartCloudXrService();
                        } catch (std::exception& ex) {
                            DriverLog("Failed to start CloudXR service: %s", ex.what());
                        }
                    }
                } else {
                    // With override, find the full path to the desired OpenXR runtime.
                    wil::unique_hkey enumKey;
                    const LSTATUS status = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                                                        L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes",
                                                        0,
                                                        KEY_READ,
                                                        enumKey.put());

                    DWORD maxLength = 0;
                    RegQueryInfoKey(enumKey.get(),
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    &maxLength,
                                    nullptr,
                                    nullptr,
                                    nullptr);
                    maxLength++;

                    DWORD index = 0;
                    while (true) {
                        std::wstring value(maxLength, 0);

                        DWORD length = maxLength;
                        if (RegEnumValue(
                                enumKey.get(), index++, value.data(), &length, nullptr, nullptr, nullptr, nullptr)) {
                            break;
                        }
                        value.resize(length);

                        const auto pos = value.find(runtimeJson);
                        if (pos != std::wstring::npos && pos == value.size() - runtimeJson.size()) {
                            TraceLoggingWriteTagged(local, "Driver_UseRuntime", TLArg(value.c_str(), "JsonPath"));
                            DriverLog("Using OpenXR runtime: '%ws'", value.c_str());

                            // We need the DLL, not the JSON. Parse the library_path.
                            cJSON* json = nullptr;
                            try {
                                std::ifstream file(value.c_str());
                                std::stringstream buffer;
                                buffer << file.rdbuf();
                                const auto content = buffer.str();

                                json = cJSON_ParseWithLength(content.c_str(), content.size());
                                if (!json) {
                                    throw std::runtime_error("Failed to parse JSON");
                                }

                                const cJSON* runtime = cJSON_GetObjectItemCaseSensitive(json, "runtime");
                                if (!runtime) {
                                    throw std::runtime_error("Failed to get 'runtime' node");
                                }

                                const cJSON* library_path = cJSON_GetObjectItemCaseSensitive(runtime, "library_path");
                                if (!library_path) {
                                    throw std::runtime_error("Failed to get 'library_path' node");
                                }

                                const auto root = std::filesystem::path(value).parent_path();
                                runtimeDll = root / cJSON_GetStringValue(library_path);
                            } catch (std::runtime_error& exc) {
                                DriverLog("Error parsing runtime JSON %s: %s", value.c_str(), exc.what());
                            }
                            cJSON_Delete(json);
                            break;
                        }
                    }
                }

                if (!runtimeDll.empty()) {
                    try {
                        // In order to bypass all OpenXR API layers, we skip using the OpenXR Loader and negotiate our
                        // instance directly with the runtime DLL.
                        SetDllDirectory(runtimeDll.parent_path().c_str());
                        *m_runtime.put() = LoadLibrary(runtimeDll.c_str());
                        if (!m_runtime) {
                            throw std::runtime_error("Failed to load runtime DLL");
                        }

                        PFN_xrNegotiateLoaderRuntimeInterface negotiateRuntimeInterface =
                            (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m_runtime.get(),
                                                                                  "xrNegotiateLoaderRuntimeInterface");
                        XrNegotiateLoaderInfo loaderInfo = {};
                        loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
                        loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
                        loaderInfo.structSize = sizeof(XrNegotiateLoaderInfo);
                        loaderInfo.minInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
                        loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
                        loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
                        loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0, 999);
                        XrNegotiateRuntimeRequest runtimeRequest = {};
                        runtimeRequest.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
                        runtimeRequest.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
                        runtimeRequest.structSize = sizeof(XrNegotiateRuntimeRequest);
                        CHECK_XRCMD(negotiateRuntimeInterface(&loaderInfo, &runtimeRequest));

                        // Pre-populate a couple of critical function pointers.
                        CHECK_XRCMD(runtimeRequest.getInstanceProcAddr(
                            XR_NULL_HANDLE,
                            "xrEnumerateInstanceExtensionProperties",
                            (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties));
                        CHECK_XRCMD(runtimeRequest.getInstanceProcAddr(
                            XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance));

                        // Create the OpenXR instance.
                        m_extensions = xr::CreateExtensionContext(k_RequestedExtensions);
                        if (!m_extensions.SupportsD3D11 && !m_extensions.SupportsD3D12) {
                            throw std::runtime_error("Runtime does not support Direct3D!");
                        }

                        XrInstanceCreateInfo instanceCreateInfo{XR_TYPE_INSTANCE_CREATE_INFO};
                        xr::SetEnabledExtensions(instanceCreateInfo, m_extensions.EnabledExtensions);
                        xr::SetApplicationInfo(instanceCreateInfo.applicationInfo,
                                               {"OpenVR-CloudXR", 1},
                                               {"", 1},
                                               XR_MAKE_VERSION(1, 0, 0));

                        // Retry in case the service needs time to spin up.
                        XrResult result = XR_ERROR_RUNTIME_UNAVAILABLE;
                        XrInstance instance = XR_NULL_HANDLE;
                        uint32_t retries = m_service ? 5 : 1;
                        do {
                            result = xrCreateInstance(&instanceCreateInfo, &instance);
                            TraceLoggingWriteTagged(
                                local, "Driver_Init_CreateSession", TLArg(xr::ToCString(result), "Status"));
                        } while (result == XR_ERROR_RUNTIME_UNAVAILABLE && --retries && (Sleep(1000), true));
                        CHECK_XRCMD(result);

                        // Populate the rest of the function pointers.
                        xr::g_dispatchTable.Initialize(instance, runtimeRequest.getInstanceProcAddr);

                        // Don't forget to attach our smart handle now that xrDestroyInstance is resolved!
                        *m_instance.Put(xrDestroyInstance) = instance;

                        // We can now use OpenXR normally with the dispatch table!

                        XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
                        CHECK_XRCMD(xrGetInstanceProperties(m_instance.Get(), &instanceProperties));
                        DriverLog("Instance created for '%s' version %u.%u.%u",
                                  instanceProperties.runtimeName,
                                  XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                  XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                  XR_VERSION_PATCH(instanceProperties.runtimeVersion));

                        if (!PollHmd()) {
                            // Only error out if we do not own the CloudXR service.
                            if (!m_service) {
                                throw std::runtime_error("Failed to acquire headset. Is the headset connected?");
                            }

                            // Signal SteamVR to wait for connection (polled in RunFrame() below).
                            vr::VRServerDriverHost()->TrackedDeviceAdded(
                                "Wait For CloudXR Headset", vr::TrackedDeviceClass_HMD, &m_disconnectedHmdDriver);
                        }

                        m_isLoaded = true;
                    } catch (std::exception& ex) {
                        DriverLog("Failed to initialize HMD: %s", ex.what());
                    }
                } else {
                    TraceLoggingWriteTagged(local, "Driver_RuntimeNotFound", TLArg(runtimeJson.c_str(), "JsonFile"));
                    DriverLog("Could not find runtime '%ws' in HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes",
                              runtimeJson.c_str());
                }
            }

            TraceLoggingWriteStop(local, "Driver_Init", TLArg(m_isLoaded, "Loaded"));

            return m_isLoaded ? vr::VRInitError_None : vr::VRInitError_Init_HmdNotFound;
        }

        void Cleanup() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Cleanup");

            hmdDriver.reset();
            m_instance.Reset();
            m_runtime.reset();
            if (m_service) {
                StopCloudXrService();
            }

            VR_CLEANUP_SERVER_DRIVER_CONTEXT();

            TraceLoggingWriteStop(local, "Driver_Cleanup");
        }

        const char* const* GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        void RunFrame() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_RunFrame");

            if (hmdDriver) {
                vr::VREvent_t event;
                while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
                    switch (event.eventType) {
                    case vr::VREvent_Input_HapticVibration:
                        hmdDriver->SendHapticEvent(event.data.hapticVibration);
                        break;
                    case vr::VREvent_AnyDriverSettingsChanged:
                        hmdDriver->ApplySettingsChanges();
                        break;
                    }
                }

                hmdDriver->RunFrame();
            }

            if (m_service) {
                nv_cxr_event event{};
                do {
                    CHECK_CXRCMD(nv_cxr_service_poll_event(m_service, &event));

                    switch (event.type) {
                    case NV_CXR_EVENT_CLOUDXR_CLIENT_CONNECTED:
                        DriverLog("CloudXR Client connected!\n");
                        break;
                    case NV_CXR_EVENT_CLOUDXR_CLIENT_DISCONNECTED:
                        DriverLog("CloudXR Client disconnected.\n");
                        break;
                    case NV_CXR_EVENT_OPENXR_APP_CONNECTED:
                        DriverLog("CloudXR App connected!\n");
                        break;
                    case NV_CXR_EVENT_OPENXR_APP_DISCONNECTED:
                        DriverLog("CloudXR App disconnected.\n");
                        break;
                    }
                } while (event.type != NV_CXR_EVENT_NONE);

                if (!hmdDriver) {
                    PollHmd();
                }
            }

            TraceLoggingWriteStop(local, "Driver_RunFrame");
        };

        bool ShouldBlockStandbyMode() override {
            return false;
        }

        void EnterStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_EnterStandby");

            TraceLoggingWriteStop(local, "Driver_EnterStandby");
        };

        void LeaveStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_LeaveStandby");

            if (hmdDriver) {
                hmdDriver->LeaveStandby();
            }

            TraceLoggingWriteStop(local, "Driver_LeaveStandby");
        };

      private:
        class DisconnectedHmdDriver : public vr::ITrackedDeviceServerDriver {
            vr::EVRInitError Activate(uint32_t unObjectId) override {
                return vr::VRInitError_Driver_WirelessHmdNotConnected;
            }

            // clang-format off
            void Deactivate() override {}
            void EnterStandby() override {}
            void* GetComponent(const char* pchComponentNameAndVersion) override { return nullptr; }
            void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {}
            vr::DriverPose_t GetPose() override { return {}; }
            // clang-format on
        };

        bool PollHmd() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_PollHmd");

            std::optional<sample::SystemContext> system =
                sample::CreateSystemContext(m_instance.Get(),
                                            m_extensions,
                                            XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
                                            k_SupportedViewConfigurationTypes,
                                            k_SupportedEnvironmentBlendModes);
            if (system) {
                m_system = std::move(*system);
                hmdDriver = driver::CreateHmdDriver(m_instance, m_extensions, m_system);

                vr::VRServerDriverHost()->TrackedDeviceAdded(
                    hmdDriver->GetSerialNumber(), vr::TrackedDeviceClass_HMD, hmdDriver.get());
            }

            TraceLoggingWriteStop(local, "Driver_PollHmd", TLArg(!!hmdDriver, "HasHmd"));

            return !!hmdDriver;
        }

        void StartCloudXrService() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_StartCloudXrService");

            CHECK_CXRCMD(nv_cxr_service_create(&m_service));

            const auto setStringProperty = [&](const std::string& property, const std::string& value) {
                DriverLog("Setting '%s' = '%s'", property.c_str(), value.c_str());
                CHECK_CXRCMD(nv_cxr_service_set_string_property(
                    m_service, property.c_str(), property.size(), value.c_str(), value.size()));
            };
            const auto setBooleanProperty = [&](const std::string& property, const bool value) {
                DriverLog("Setting '%s' = %s", property.c_str(), value ? "true" : "false");
                CHECK_CXRCMD(nv_cxr_service_set_boolean_property(m_service, property.c_str(), property.size(), value));
            };
            const auto setInt64Property = [&](const std::string& property, const int64_t value) {
                DriverLog("Setting '%s' = %d", property.c_str(), value);
                CHECK_CXRCMD(nv_cxr_service_set_int64_property(m_service, property.c_str(), property.size(), value));
            };

            char buffer[MAX_PATH + 1] = {};
            vr::VRSettings()->GetString("driver_cloudxr", "cloudxr_profile", buffer, sizeof(buffer));
            setStringProperty("device-profile", buffer);
            setBooleanProperty("disable-alpha", true);
            setBooleanProperty("audio-streaming", vr::VRSettings()->GetBool("driver_cloudxr", "cloudxr_audio_enable"));
            if (vr::VRSettings()->GetBool("driver_cloudxr", "cloudxr_streaming_settings")) {
                setBooleanProperty("immediate-compositor",
                                   !vr::VRSettings()->GetBool("driver_cloudxr", "cloudxr_video_buffering"));
                setInt64Property("runtime-foveation-warped-width",
                                 vr::VRSettings()->GetInt32("driver_cloudxr", "cloudxr_foveation_warped_width"));
                setInt64Property("streaming-bits-per-channel",
                                 vr::VRSettings()->GetBool("driver_cloudxr", "cloudxr_use_10_bits") ? 10 : 8);
                setBooleanProperty("runtime-foveation",
                                   vr::VRSettings()->GetBool("driver_cloudxr", "cloudxr_foveation_enable"));
                setInt64Property("runtime-foveation-inset",
                                 vr::VRSettings()->GetInt32("driver_cloudxr", "cloudxr_foveation_inset"));
            }

            CHECK_CXRCMD(nv_cxr_service_start(m_service));

            TraceLoggingWriteStop(local, "Driver_StartCloudXrService");
        }

        void StopCloudXrService() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_StopCloudXrService");

            nv_cxr_service_stop(m_service);
            // TODO: This hangs sometimes. Need a watchdog of some sort.
            nv_cxr_service_join(m_service);
            nv_cxr_service_destroy(m_service);

            TraceLoggingWriteStop(local, "Driver_StopCloudXrService");
        }

        nv_cxr_service* m_service = nullptr;
        wil::unique_hmodule m_runtime;
        xr::InstanceHandle m_instance;
        xr::ExtensionContext m_extensions;
        sample::SystemContext m_system;
        DisconnectedHmdDriver m_disconnectedHmdDriver;

        bool m_isLoaded = false;
    };
} // namespace

namespace xr::detail {
    [[noreturn]] void _Throw(std::string failureMessage, const char* originator, const char* sourceLocation) {
        if (originator != nullptr) {
            failureMessage += xr::detail::_Fmt("\n    Origin: %s", originator);
        }
        if (sourceLocation != nullptr) {
            failureMessage += xr::detail::_Fmt("\n    Source: %s", sourceLocation);
        }

        DriverLog("%s", failureMessage.c_str());
        throw std::logic_error(failureMessage);
    }
} // namespace xr::detail

// Entry point for vrserver.
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName) == 0) {
        if (!thisDriver) {
            thisDriver = std::make_unique<Driver>();
        }
        return thisDriver.get();
    }
    if (pReturnCode) {
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
