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
#include "Tracing.h"
#include "Utilities.h"

using namespace driver;
using namespace util;
using namespace xr::math;

namespace {
    enum MotionControllerPersonality {
        MotionControllerPersonalityPsvr2Sense = 0,
        MotionControllerPersonalityOculusTouch,
        MotionControllerPersonalityCount
    };

    enum Component {
        ComponentTrigger,
        ComponentTriggerClick,
        ComponentTriggerTouch,
        ComponentGrip,
        ComponentGripClick,
        ComponentThumbstickX,
        ComponentThumbstickY,
        ComponentThumbstickClick,
        ComponentThumbstickTouch,
        ComponentThumbrestTouch,
        ComponentButton1,
        ComponentButton1Touch,
        ComponentButton2,
        ComponentButton2Touch,
        ComponentMenu,

        ComponentHaptics,

        ComponentCount,
    };

    struct PersonalityProfile {
        const char* trackingSystemName;
        const char* manufacturerName;
        const char* prettyName;
        const char* rendermodelPath;
        const char* rendermodel[2];
        const char* controllerType;
        const char* interactionProfile;
        const char* iconPathDeviceOff[2];
        const char* iconPathDeviceSearching[2];
        const char* iconPathDeviceSearchingAlert[2];
        const char* iconPathDeviceReady[2];
        const char* iconPathDeviceReadyAlert[2];
        const char* iconPathDeviceNotReady[2];
        const char* iconPathDeviceAlertLow[2];
        const char* triggerPath[2];
        const char* gripPath[2];
        const char* thumbstickPath[2];
        const char* thumbrestPath[2];
        const char* button1Path[2];
        const char* button2Path[2];
        const char* menuPath[2];
    };

    // clang-format off
    const PersonalityProfile k_MotionControllerPersonality[MotionControllerPersonalityCount] = {
        // PSVR2 Sense
        {
            /* trackingSystemName */ "playstation_vr2",
            /* manufacturerName */ "SIE",
            /* prettyName */ "PlayStation VR2 Sense",
            /* rendermodelPath */ "{cloudxr}/rendermodels/",
            /* rendermodel */ {"playstation_vr2_sense_left", "playstation_vr2_sense_right"},
            /* controllerType */ "playstation_vr2_sense",
            /* interactionProfile */ "{cloudxr}/input/playstation_vr2_sense_controller_profile.json",
            /* iconPathDeviceOff */ {"{cloudxr}/icons/left_controller_status_off.png", "{cloudxr}/icons/right_controller_status_off.png"},
            /* iconPathDeviceSearching */ {"{cloudxr}/icons/left_controller_status_searching.png", "{cloudxr}/icons/right_controller_status_searching.png"},
            /* iconPathDeviceSearchingAlert */ {"{cloudxr}/icons/left_controller_status_searching_alert.png", "{cloudxr}/icons/right_controller_status_searching_alert.png"},
            /* iconPathDeviceReady */ {"{cloudxr}/icons/left_controller_status_ready.png", "{cloudxr}/icons/right_controller_status_ready.png"},
            /* iconPathDeviceReadyAlert */ {"{cloudxr}/icons/left_controller_status_ready_alert.png", "{cloudxr}/icons/right_controller_status_ready_alert.png"},
            /* iconPathDeviceNotReady */ {"{cloudxr}/icons/left_controller_status_error.png", "{cloudxr}/icons/right_controller_status_error.png"},
            /* iconPathDeviceAlertLow */ {"{cloudxr}/icons/left_controller_status_ready_low.png" , "{cloudxr}/icons/right_controller_status_ready_low.png"},
            /* triggerPath */ {"/input/l2", "/input/r2"},
            /* gripPath */ {"/input/l1", "/input/r1"},
            /* thumbstickPath */ {"/input/left_stick", "/input/right_stick"},
            /* thumbrestPath */ {"/input/left_ps", "/input/right_ps"},
            /* button1Path */ {"/input/triangle", "/input/circle"},
            /* button2Path */ {"/input/square", "/input/cross"},
            /* menuPath */ "/input/create",

        },
        // Oculus Touch
        {
            /* trackingSystemName */ "oculus",
            /* manufacturerName */ "Oculus",
            /* prettyName */ "Oculus Quest2",
            /* rendermodelPath */ "rendermodels/",
            /* rendermodel */ {"oculus_quest2_controller_left", "oculus_quest2_controller_right"},
            /* controllerType */ "oculus_touch",
            /* interactionProfile */ "{oculus}/input/touch_profile.json",
            /* iconPathDeviceOff */ {"{oculus}/icons/rifts_left_controller_off.png", "{oculus}/icons/rifts_right_controller_off.png"},
            /* iconPathDeviceSearching */ {"{oculus}/icons/rifts_left_controller_searching.png", "{oculus}/icons/rifts_right_controller_searching.png"},
            /* iconPathDeviceSearchingAlert */ {"{oculus}/icons/rifts_left_controller_searching_alert.png", "{oculus}/icons/rifts_right_controller_searching_alert.png"},
            /* iconPathDeviceReady */ {"{oculus}/icons/rifts_left_controller_ready.png", "{oculus}/icons/rifts_right_controller_ready.png"},
            /* iconPathDeviceReadyAlert */ {"{oculus}/icons/rifts_left_controller_ready_alert.png", "{oculus}/icons/rifts_right_controller_ready_alert.png"},
            /* iconPathDeviceNotReady */ {"{oculus}/icons/rifts_left_controller_error.png", "{oculus}/icons/rifts_right_controller_error.png"},
            /* iconPathDeviceAlertLow */ {"{oculus}/icons/rifts_left_controller_ready_low.png" , "{oculus}/icons/rifts_right_controller_ready_low.png"},
            /* triggerPath */ {"/input/trigger", "/input/trigger"},
            /* gripPath */ {"/input/grip", "/input/grip"},
            /* thumbstickPath */ {"/input/joystick", "/input/joystick"},
            /* thumbrestPath */ {"/input/thumbrest", "/input/thumbrest"},
            /* button1Path */ {"/input/y", "/input/b"},
            /* button2Path */ {"/input/x", "/input/a"},
            /* menuPath */ "/input/system",
        },
    };
    // clang-format on

    class ControllerDriver : public IControllerDriver {
      public:
        ControllerDriver(xr::InstanceHandle& instance,
                         xr::SessionHandle& session,
                         xr::SpaceHandle& referenceSpace,
                         vr::ETrackedControllerRole role)
            : m_role(role), m_instance(instance), m_session(session), m_referenceSpace(referenceSpace) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_Ctor",
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"));

            m_serialNumber = m_role == vr::TrackedControllerRole_LeftHand ? "CLOUDXR_LEFT" : "CLOUDXR_RIGHT";

            m_sidePath =
                xr::StringToPath(m_instance.Get(),
                                 m_role == vr::TrackedControllerRole_LeftHand ? "/user/hand/left" : "/user/hand/right");

            // Initial pose fields.
            m_latestPose.qWorldFromDriverRotation.w = m_latestPose.qDriverFromHeadRotation.w =
                m_latestPose.qRotation.w = 1.f;
            m_latestPose.deviceIsConnected = true;
            m_latestPose.result = vr::TrackingResult_Running_OutOfRange;

            TraceLoggingWriteStop(local, "ControllerDriver_Ctor");
        }

        ~ControllerDriver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "ControllerDriver_Dtor", TLArg(m_serialNumber.c_str(), "SerialNumber"));

            TraceLoggingWriteStop(local, "ControllerDriver_Dtor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            const bool isLeft = m_role == vr::TrackedControllerRole_LeftHand;
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_Activate",
                                   TLArg(unObjectId, "ObjectId"),
                                   TLArg(isLeft ? "Left" : "Right", "Role"));

            m_deviceIndex = unObjectId;

            ApplySettingsChanges();

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Fill out all the properties based on the selected controller personality.
            const MotionControllerPersonality personalityIndex = (MotionControllerPersonality)std::clamp(
                vr::VRSettings()->GetInt32("driver_cloudxr", "motion_controllers_personality"),
                0,
                (int32_t)MotionControllerPersonalityCount);
            const auto& personality = k_MotionControllerPersonality[personalityIndex];

            vr::VRProperties()->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, m_role);
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_TrackingSystemName_String, personality.trackingSystemName);
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_ManufacturerName_String, personality.manufacturerName);
            vr::VRProperties()->SetStringProperty(
                container,
                vr::Prop_ModelNumber_String,
                (std::string(personality.prettyName) + (isLeft ? " (Left Controller)" : " (Right Controller)"))
                    .c_str());
            vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, GetSerialNumber());
            vr::VRProperties()->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 1);
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_InputProfilePath_String, personality.interactionProfile);
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_ControllerType_String, personality.controllerType);

            // Setup the render model.
            {
                using namespace DirectX;

                std::filesystem::path renderModelPath = personality.rendermodelPath;
                const std::string renderModel = personality.rendermodel[isLeft ? 0 : 1];

                renderModelPath /= renderModel;
                const auto renderModelPathStr = renderModelPath.string();

                vr::VRProperties()->SetStringProperty(container,
                                                      vr::Prop_RenderModelName_String,
                                                      renderModelPathStr.c_str()[0] == '{' ? renderModelPathStr.c_str()
                                                                                           : renderModel.c_str());

                XMMATRIX originToGrip = XMMatrixIdentity();

                // SteamVR expects the pose at the origin of the render model.
                // Retrieve the grip pose transform from the rendermodel.
                const uint32_t length = vr::VRResources()->LoadSharedResource(
                    (renderModelPath / (renderModel + ".json")).string().c_str(), nullptr, 0);
                std::string content;
                content.resize(length);
                vr::VRResources()->LoadSharedResource(
                    (renderModelPath / (renderModel + ".json")).string().c_str(), content.data(), length);

                cJSON* json = nullptr;
                try {
                    json = cJSON_ParseWithLength(content.c_str(), length);
                    if (!json) {
                        throw std::runtime_error("Failed to parse JSON");
                    }

                    const cJSON* components = cJSON_GetObjectItemCaseSensitive(json, "components");
                    if (!components) {
                        throw std::runtime_error("Failed to get components");
                    }

                    const auto getTransform = [&](const char* component) {
                        const cJSON* entry = cJSON_GetObjectItemCaseSensitive(components, component);
                        if (!entry) {
                            throw std::runtime_error("Failed to get component");
                        }

                        const cJSON* component_local = cJSON_GetObjectItemCaseSensitive(entry, "component_local");
                        if (!component_local) {
                            throw std::runtime_error("Failed to get component_local");
                        }
                        const cJSON* origin = cJSON_GetObjectItemCaseSensitive(component_local, "origin");
                        const cJSON* rotation = cJSON_GetObjectItemCaseSensitive(component_local, "rotate_xyz");

                        const auto getVector = [](const cJSON* values) {
                            XMVECTOR vec = {};
                            for (int i = 0; i < 3; i++) {
                                const cJSON* value = nullptr;
                                value = cJSON_GetArrayItem(values, i);
                                if (value) {
                                    vec = XMVectorSetByIndex(vec, (float)value->valuedouble, i);
                                }
                            }
                            return vec;
                        };

                        XMMATRIX transform = XMMatrixIdentity();

                        if (rotation) {
                            const XMVECTOR v = getVector(rotation);
                            transform = XMMatrixRotationRollPitchYaw((float)(XMVectorGetX(v) * M_PI / 180),
                                                                     (float)(XMVectorGetY(v) * M_PI / 180),
                                                                     (float)(XMVectorGetZ(v) * M_PI / 180));
                        }
                        if (origin) {
                            XMVECTOR v = getVector(origin);
                            v = XMVectorSetW(v, 1.f);
                            transform.r[3] = v;
                        }

                        return transform;
                    };

                    m_poseOffset = XMMatrixInverse(nullptr, getTransform("openxr_aim"));

                } catch (std::runtime_error& exc) {
                    DriverLog("Error parsing render model %s: %s", renderModel.c_str(), exc.what());
                }
                cJSON_Delete(json);
            }

            // clang-format off
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceOff_String, personality.iconPathDeviceOff[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearching_String, personality.iconPathDeviceSearching[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, personality.iconPathDeviceSearchingAlert[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String, personality.iconPathDeviceReady[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReadyAlert_String, personality.iconPathDeviceReadyAlert[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceNotReady_String, personality.iconPathDeviceNotReady[isLeft ? 0 : 1]);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceAlertLow_String, personality.iconPathDeviceAlertLow[isLeft ? 0 : 1]);
            // clang-format on

            // Create all the input components.
            const auto getPath = [isLeft](const char* const root[2], const char* leaf) -> std::string {
                if (!root[isLeft ? 0 : 1]) {
                    return "";
                }
                return std::string(root[isLeft ? 0 : 1]) + leaf;
            };
            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       getPath(personality.triggerPath, "/value").c_str(),
                                                       &m_components[ComponentTrigger],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedOneSided);
            vr::VRProperties()->SetInt32Property(
                container, vr::Prop_Axis1Type_Int32, vr::EVRControllerAxisType::k_eControllerAxis_Trigger);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.triggerPath, "/click").c_str(), &m_components[ComponentTriggerClick]);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.triggerPath, "/touch").c_str(), &m_components[ComponentTriggerTouch]);
            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       getPath(personality.gripPath, "/value").c_str(),
                                                       &m_components[ComponentGrip],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedOneSided);
            vr::VRProperties()->SetInt32Property(
                container, vr::Prop_Axis3Type_Int32, vr::EVRControllerAxisType::k_eControllerAxis_Trigger);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.gripPath, "/click").c_str(), &m_components[ComponentGripClick]);

            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       getPath(personality.thumbstickPath, "/x").c_str(),
                                                       &m_components[ComponentThumbstickX],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedTwoSided);
            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       getPath(personality.thumbstickPath, "/y").c_str(),
                                                       &m_components[ComponentThumbstickY],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedTwoSided);
            vr::VRProperties()->SetInt32Property(
                container, vr::Prop_Axis2Type_Int32, vr::EVRControllerAxisType::k_eControllerAxis_Joystick);
            vr::VRDriverInput()->CreateBooleanComponent(container,
                                                        getPath(personality.thumbstickPath, "/click").c_str(),
                                                        &m_components[ComponentThumbstickClick]);
            vr::VRDriverInput()->CreateBooleanComponent(container,
                                                        getPath(personality.thumbstickPath, "/touch").c_str(),
                                                        &m_components[ComponentThumbstickTouch]);
            vr::VRDriverInput()->CreateBooleanComponent(container,
                                                        getPath(personality.thumbrestPath, "/touch").c_str(),
                                                        &m_components[ComponentThumbrestTouch]);

            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.button1Path, "/click").c_str(), &m_components[ComponentButton1]);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.button1Path, "/touch").c_str(), &m_components[ComponentButton1Touch]);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.button2Path, "/click").c_str(), &m_components[ComponentButton2]);
            vr::VRDriverInput()->CreateBooleanComponent(
                container, getPath(personality.button2Path, "/touch").c_str(), &m_components[ComponentButton2Touch]);
            if (isLeft) {
                // TODO: CloudXR does not seem to send the action for /input/menu.
                vr::VRDriverInput()->CreateBooleanComponent(
                    container, getPath(personality.menuPath, "/click").c_str(), &m_components[ComponentMenu]);
            }
            vr::VRDriverInput()->CreateHapticComponent(container, "/output/haptic", &m_components[ComponentHaptics]);

            m_ready = true;

            TraceLoggingWriteStop(local, "ControllerDriver_Activate");

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "ControllerDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;
            m_ready = false;

            TraceLoggingWriteStop(local, "ControllerDriver_Deactivate");
        }

        void EnterStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "ControllerDriver_EnterStandby", TLArg(m_deviceIndex, "ObjectId"));

            TraceLoggingWriteStop(local, "ControllerDriver_EnterStandby");
        }

        void LeaveStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "ControllerDriver_LeaveStandby", TLArg(m_deviceIndex, "ObjectId"));

            TraceLoggingWriteStop(local, "ControllerDriver_LeaveStandby");
        }

        void* GetComponent(const char* pchComponentNameAndVersion) override {
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

        std::vector<XrActionSuggestedBinding> CreateBindings(XrActionSet actionSet) override {
            TraceLocalActivity(local);
            const bool isLeft = m_role == vr::TrackedControllerRole_LeftHand;
            TraceLoggingWriteStart(local, "ControllerDriver_CreateBindings", TLArg(isLeft ? "Left" : "Right", "Role"));

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);
            std::vector<XrActionSuggestedBinding> bindings;

            // Binding for the pose. We use the aim pose.
            {
                XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
                strcpy_s(actionCreateInfo.actionName,
                         isLeft ? "steamvr_left_controller_pose" : "steamvr_right_controller_pose");
                strcpy_s(actionCreateInfo.localizedActionName,
                         isLeft ? "Left Controller Pose" : "Right Controller Pose");
                actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
                actionCreateInfo.countSubactionPaths = 0;
                CHECK_XRCMD(xrCreateAction(actionSet, &actionCreateInfo, m_trackingPoseAction.Put(xrDestroyAction)));

                XrActionSpaceCreateInfo actionSpaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                actionSpaceCreateInfo.action = m_trackingPoseAction.Get();
                actionSpaceCreateInfo.subactionPath = XR_NULL_PATH;
                actionSpaceCreateInfo.poseInActionSpace = Pose::Identity();
                CHECK_XRCMD(xrCreateActionSpace(
                    m_session.Get(), &actionSpaceCreateInfo, m_trackingPoseSpace.Put(xrDestroySpace)));

                auto& binding = bindings.emplace_back();
                binding.action = m_trackingPoseAction.Get();
                binding.binding = xr::StringToPath(
                    m_instance.Get(), isLeft ? "/user/hand/left/input/aim/pose" : "/user/hand/right/input/aim/pose");
            }

            // Binding for haptics.
            {
                XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
                strcpy_s(actionCreateInfo.actionName, isLeft ? "steamvr_left_haptics" : "steamvr_right_haptics");
                strcpy_s(actionCreateInfo.localizedActionName,
                         isLeft ? "Left Controller Haptics" : "Right Controller Haptics");
                actionCreateInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                actionCreateInfo.countSubactionPaths = 0;
                CHECK_XRCMD(
                    xrCreateAction(actionSet, &actionCreateInfo, m_actions[ComponentHaptics].Put(xrDestroyAction)));

                auto& binding = bindings.emplace_back();
                binding.action = m_actions[ComponentHaptics].Get();
                binding.binding = xr::StringToPath(
                    m_instance.Get(), isLeft ? "/user/hand/left/output/haptic" : "/user/hand/right/output/haptic");
            }

            // Bindings for the triggers, buttons and joysticks.
            const auto addInput = [&](Component index,
                                      XrActionType actionType,
                                      const char* path,
                                      const char* id,
                                      const char* prettyName) {
                XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
                strcpy_s(actionCreateInfo.actionName,
                         xr::detail::_Fmt("steamvr_%s_%s", isLeft ? "left" : "right", id).c_str());
                strcpy_s(actionCreateInfo.localizedActionName,
                         xr::detail::_Fmt("%s Controller %s", isLeft ? "Left" : "Right", prettyName).c_str());
                actionCreateInfo.actionType = actionType;
                actionCreateInfo.countSubactionPaths = 0;
                CHECK_XRCMD(xrCreateAction(actionSet, &actionCreateInfo, m_actions[index].Put(xrDestroyAction)));

                auto& binding = bindings.emplace_back();
                binding.action = m_actions[index].Get();
                binding.binding = xr::StringToPath(
                    m_instance.Get(),
                    xr::detail::_Fmt("/user/hand/%s/input/%s", isLeft ? "left" : "right", path).c_str());
            };

            // clang-format off
            addInput(ComponentTrigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger/value", "trigger_value", "Trigger Analog");
            addInput(ComponentTriggerClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger/value", "trigger_click", "Trigger");
            addInput(ComponentTriggerTouch, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger/touch", "trigger_touch", "Trigger Touch");
            addInput(ComponentGrip, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze/value", "squeeze_value", "Squeeze Analog");
            addInput(ComponentGripClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "squeeze/value", "squeeze_click", "Squeeze");
            addInput(ComponentThumbstickX, XR_ACTION_TYPE_FLOAT_INPUT, "thumbstick/x", "thumbstick_x_value", "Thumbstick X Analog");
            addInput(ComponentThumbstickY, XR_ACTION_TYPE_FLOAT_INPUT, "thumbstick/y", "thumbstick_y_value", "Thumbstick Y Analog");
            addInput(ComponentThumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick/click", "thumbstick_click", "Thumbstick");
            addInput(ComponentThumbstickTouch, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick/touch", "thumbstick_touch", "Thumbstick Touch");
            addInput(ComponentThumbrestTouch, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbrest/touch", "thumbrest_touch", "Thumbrest Touch");
            if (isLeft) {
                addInput(ComponentMenu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu/click", "menu_click", "Menu");
                addInput(ComponentButton1, XR_ACTION_TYPE_BOOLEAN_INPUT, "y/click", "y_click", "Y");
                addInput(ComponentButton1Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "y/touch", "y_touch", "Y Touch");
                addInput(ComponentButton2, XR_ACTION_TYPE_BOOLEAN_INPUT, "x/click", "x_click", "X");
                addInput(ComponentButton2Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "x/touch", "x_touch", "X Touch");
            } else {
                addInput(ComponentButton1, XR_ACTION_TYPE_BOOLEAN_INPUT, "b/click", "b_click", "B");
                addInput(ComponentButton1Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "b/touch", "b_touch", "B Touch");
                addInput(ComponentButton2, XR_ACTION_TYPE_BOOLEAN_INPUT, "a/click", "a_click", "A");
                addInput(ComponentButton2Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "a/touch", "a_touch", "A Touch");
            }
            // clang-format on

            TraceLoggingWriteStop(local, "ControllerDriver_CreateBindings");

            return bindings;
        }

        std::string GetInteractionProfile() const override {
            // We always feed off of the runtime's Oculus Touch interaction profile.
            return "/interaction_profiles/oculus/touch_controller";
        }

        void SendHapticEvent(const vr::VREvent_HapticVibration_t& data) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_SendHapticEvent",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"),
                                   TLArg(data.fAmplitude, "Amplitude"),
                                   TLArg(data.fFrequency, "Frequency"),
                                   TLArg(data.fDurationSeconds, "Duration"));

            if (m_ready) {
                XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
                info.action = m_actions[ComponentHaptics].Get();
                XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
                vibration.amplitude = data.fAmplitude;
                vibration.frequency = data.fFrequency > 0 ? data.fFrequency : XR_FREQUENCY_UNSPECIFIED;
                vibration.duration =
                    std::max((XrDuration)(data.fDurationSeconds / 1e9f), (XrDuration)XR_MIN_HAPTIC_DURATION);
                CHECK_XRCMD(xrApplyHapticFeedback(m_session.Get(), &info, (XrHapticBaseHeader*)&vibration));
            }

            TraceLoggingWriteStop(local, "ControllerDriver_SendHapticEvent");
        }

        bool IsTracked() override {
            std::shared_lock lock(m_poseMutex);
            return m_latestPose.poseIsValid;
        }

        void ApplySettingsChanges() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_ApplySettingsChanges",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"));

            const auto useSystemButton = vr::VRSettings()->GetInt32("driver_cloudxr", "use_windows_key");
            m_useSystemButton = useSystemButton == 1 || useSystemButton == 3;

            TraceLoggingWriteStop(local, "ControllerDriver_ApplySettingsChanges");
        }

        void UpdateTrackingState(XrTime time) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_UpdateTrackingState",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"),
                                   TLArg(time, "Time"));

            vr::DriverPose_t pose = {};
            pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1.0;

            if (m_ready) {
                // Determine connectivity based on the interaction profile reported.
                XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE};
                CHECK_XRCMD(xrGetCurrentInteractionProfile(m_session.Get(), m_sidePath, &state));
                TraceLoggingWriteTagged(local,
                                        "ControllerDriver_UpdateTrackingState",
                                        TLArg(state.interactionProfile != XR_NULL_PATH
                                                  ? xr::PathToString(m_instance.Get(), state.interactionProfile).c_str()
                                                  : "",
                                              "InteractionProfile"));

                pose.deviceIsConnected = state.interactionProfile != XR_NULL_PATH;
                pose.result = vr::TrackingResult_Running_OutOfRange;

                // Locate and convert the pose/velocity information.
                XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
                XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
                location.next = &velocity;
                CHECK_XRCMD(xrLocateSpace(m_trackingPoseSpace.Get(), m_referenceSpace.Get(), time, &location));
                TraceLoggingWriteTagged(local,
                                        "ControllerDriver_UpdateTrackingState",
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

                    DirectX::XMVECTOR position, orientation, scale;
                    DirectX::XMMatrixDecompose(&scale, &orientation, &position, m_poseOffset);
                    pose.vecDriverFromHeadTranslation[0] = DirectX::XMVectorGetX(position);
                    pose.vecDriverFromHeadTranslation[1] = DirectX::XMVectorGetY(position);
                    pose.vecDriverFromHeadTranslation[2] = DirectX::XMVectorGetZ(position);
                    pose.qDriverFromHeadRotation.x = DirectX::XMVectorGetX(orientation);
                    pose.qDriverFromHeadRotation.y = DirectX::XMVectorGetY(orientation);
                    pose.qDriverFromHeadRotation.z = DirectX::XMVectorGetZ(orientation);
                    pose.qDriverFromHeadRotation.w = DirectX::XMVectorGetW(orientation);

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
            }

            TraceLoggingWriteStop(
                local, "ControllerDriver_UpdateTrackingState", TLArg(pose.poseTimeOffset, "PoseTimeOffset"));
        }

        void UpdateInputsState(XrTime time) {
            TraceLocalActivity(local);
            const bool isLeft = m_role == vr::TrackedControllerRole_LeftHand;
            TraceLoggingWriteStart(local,
                                   "ControllerDriver_UpdateInputsState",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(isLeft ? "Left" : "Right", "Role"),
                                   TLArg(time, "Time"));

            if (m_ready) {
                const vr::PropertyContainerHandle_t container =
                    vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

                const auto updateButton = [&](Component index) {
                    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
                    info.action = m_actions[index].Get();
                    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    CHECK_XRCMD(xrGetActionStateBoolean(m_session.Get(), &info, &state));
                    TraceLoggingWriteTagged(local,
                                            "ControllerDriver_UpdateInputs_UpdateBooleanComponent",
                                            TLArg(!!state.isActive, "IsActive"),
                                            TLArg(!!state.currentState, "State"));
                    vr::VRDriverInput()->UpdateBooleanComponent(m_components[index], state.currentState, 0.0);
                };
                const auto updateAnalog = [&](Component index) {
                    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
                    info.action = m_actions[index].Get();
                    XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
                    CHECK_XRCMD(xrGetActionStateFloat(m_session.Get(), &info, &state));
                    TraceLoggingWriteTagged(local,
                                            "ControllerDriver_UpdateInputs_UpdateScalarComponent",
                                            TLArg(!!state.isActive, "IsActive"),
                                            TLArg(state.currentState, "State"));
                    vr::VRDriverInput()->UpdateScalarComponent(m_components[index], state.currentState, 0.0);
                };

                updateAnalog(ComponentTrigger);
                updateButton(ComponentTriggerClick);
                updateButton(ComponentTriggerTouch);
                updateAnalog(ComponentGrip);
                updateButton(ComponentGripClick);
                updateAnalog(ComponentThumbstickX);
                updateAnalog(ComponentThumbstickY);
                updateButton(ComponentThumbstickClick);
                updateButton(ComponentThumbstickTouch);
                updateButton(ComponentThumbrestTouch);
                if (isLeft) {
                    updateButton(ComponentMenu);
                }
                updateButton(ComponentButton1);
                updateButton(ComponentButton1Touch);
                updateButton(ComponentButton2);
                updateButton(ComponentButton2Touch);
            }

            TraceLoggingWriteStop(local, "ControllerDriver_UpdateInputsState");
        }

        vr::TrackedDeviceIndex_t GetDeviceIndex() const {
            return m_deviceIndex;
        }

        const char* GetSerialNumber() const {
            return m_serialNumber.c_str();
        }

      private:
        const vr::ETrackedControllerRole m_role;
        xr::InstanceHandle& m_instance;
        xr::SessionHandle& m_session;
        xr::SpaceHandle& m_referenceSpace;

        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        vr::VRInputComponentHandle_t m_components[ComponentCount] = {};

        std::string m_serialNumber;

        bool m_ready = false;

        XrPath m_sidePath = XR_NULL_PATH;
        xr::ActionHandle m_actions[ComponentCount];
        xr::ActionHandle m_trackingPoseAction;
        xr::SpaceHandle m_trackingPoseSpace;

        DirectX::XMMATRIX m_poseOffset = DirectX::XMMatrixIdentity();

        std::shared_mutex m_poseMutex;
        vr::DriverPose_t m_latestPose = {};

        bool m_useSystemButton = true;
    };
} // namespace

namespace driver {
    std::unique_ptr<IControllerDriver> CreateControllerDriver(xr::InstanceHandle& instance,
                                                              xr::SessionHandle& session,
                                                              xr::SpaceHandle& referenceSpace,
                                                              vr::ETrackedControllerRole role) {
        return std::make_unique<ControllerDriver>(instance, session, referenceSpace, role);
    }
} // namespace driver
