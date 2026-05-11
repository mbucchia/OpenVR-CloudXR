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

#include "HandDriver.h"
#include "Tracing.h"
#include "Utilities.h"

using namespace driver;
using namespace util;
using namespace xr::math;

namespace {
    enum HandSkeletonBone {
        BoneRoot = 0,
        BoneWrist,
        BoneThumb0,
        BoneThumb1,
        BoneThumb2,
        BoneThumb3,
        BoneIndexFinger0,
        BoneIndexFinger1,
        BoneIndexFinger2,
        BoneIndexFinger3,
        BoneIndexFinger4,
        BoneMiddleFinger0,
        BoneMiddleFinger1,
        BoneMiddleFinger2,
        BoneMiddleFinger3,
        BoneMiddleFinger4,
        BoneRingFinger0,
        BoneRingFinger1,
        BoneRingFinger2,
        BoneRingFinger3,
        BoneRingFinger4,
        BonePinkyFinger0,
        BonePinkyFinger1,
        BonePinkyFinger2,
        BonePinkyFinger3,
        BonePinkyFinger4,
        BoneAux_Thumb,
        BoneAux_IndexFinger,
        BoneAux_MiddleFinger,
        BoneAux_RingFinger,
        BoneAux_PinkyFinger,

        BoneCount
    };

    enum Component {
        ComponentSkeleton,
        ComponentIndexPinch,
        ComponentPinkyPinch,

        ComponentCount,
    };

    class HandDriver : public IHandDriver {
      public:
        HandDriver(xr::InstanceHandle& instance,
                   xr::SessionHandle& session,
                   xr::SpaceHandle& referenceSpace,
                   vr::ETrackedControllerRole role)
            : m_role(role), m_instance(instance), m_session(session), m_referenceSpace(referenceSpace) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HandDriver_Ctor",
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"));

            m_serialNumber = m_role == vr::TrackedControllerRole_LeftHand ? "HAND_LEFT" : "HAND_RIGHT";

            // Initial pose fields.
            m_latestPose.qWorldFromDriverRotation.w = m_latestPose.qDriverFromHeadRotation.w =
                m_latestPose.qRotation.w = 1.f;
            m_latestPose.deviceIsConnected = true;
            m_latestPose.result = vr::TrackingResult_Running_OutOfRange;

            TraceLoggingWriteStop(local, "HandDriver_Ctor");
        }

        ~HandDriver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HandDriver_Dtor", TLArg(m_serialNumber.c_str(), "SerialNumber"));

            TraceLoggingWriteStop(local, "HandDriver_Dtor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            const bool isLeft = m_role == vr::TrackedControllerRole_LeftHand;
            TraceLoggingWriteStart(
                local, "HandDriver_Activate", TLArg(unObjectId, "ObjectId"), TLArg(isLeft ? "Left" : "Right", "Role"));

            m_deviceIndex = unObjectId;

            ApplySettingsChanges();

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Fill out all the properties to use SteamLink's hand tracking profile.
            vr::VRProperties()->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, m_role);
            vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "vrlink");

            vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Hand Tracking");
            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_ControllerType_String, "svl_hand_interaction_augmented");
            vr::VRProperties()->SetStringProperty(container,
                                                  vr::Prop_ModelNumber_String,
                                                  isLeft ? "VRLink Hand Tracker (Left Hand)"
                                                         : "VRLink Hand Tracker (Right Hand)");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, GetSerialNumber());
            vr::VRProperties()->SetStringProperty(container,
                                                  vr::Prop_InputProfilePath_String,
                                                  "{vrlink}/input/svl_hand_interaction_augmented_input_profile.json");
            vr::VRProperties()->SetBoolProperty(container, vr::Prop_DeviceHasNoIMU_Bool, true);

            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_RenderModelName_String, "{vrlink}/rendermodels/shuttlecock");

            // clang-format off
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceOff_String, isLeft ? "{vrlink}/icons/left_handtracking_off.png" : "{vrlink}/icons/right_handtracking_off.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearching_String, isLeft ? "{vrlink}/icons/left_handtracking_searching.png" : "{vrlink}/icons/right_handtracking_searching.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String, isLeft ? "{vrlink}/icons/left_handtracking_ready.png" : "{vrlink}/icons/right_handtracking_ready.png");
            vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReadyAlert_String, isLeft ? "{vrlink}/icons/left_handtracking_ready_info.png" : "{vrlink}/icons/right_handtracking_ready_info.png");
            // clang-format on

            // Create all the input components.
            vr::VRDriverInput()->CreateSkeletonComponent(container,
                                                         isLeft ? "/input/skeleton/left" : "/input/skeleton/right",
                                                         isLeft ? "/skeleton/hand/left" : "/skeleton/hand/right",
                                                         "/pose/raw",
                                                         vr::VRSkeletalTracking_Full,
                                                         nullptr,
                                                         0,
                                                         &m_components[ComponentSkeleton]);
            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       "/input/index_pinch/value",
                                                       &m_components[ComponentIndexPinch],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedOneSided);
            vr::VRDriverInput()->CreateScalarComponent(container,
                                                       "/input/pinky_pinch/value",
                                                       &m_components[ComponentPinkyPinch],
                                                       vr::VRScalarType_Absolute,
                                                       vr::VRScalarUnits_NormalizedOneSided);

            // Initialize the OpenXR hand joints tracker.
            {
                XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                createInfo.hand = isLeft ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                CHECK_XRCMD(
                    xrCreateHandTrackerEXT(m_session.Get(), &createInfo, m_handTracker.Put(xrDestroyHandTrackerEXT)));
            }

            m_ready = true;

            TraceLoggingWriteStop(local, "HandDriver_Activate");

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HandDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;
            m_ready = false;

            TraceLoggingWriteStop(local, "HandDriver_Deactivate");
        }

        void EnterStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HandDriver_EnterStandby", TLArg(m_deviceIndex, "ObjectId"));

            TraceLoggingWriteStop(local, "HandDriver_EnterStandby");
        }

        void LeaveStandby() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HandDriver_LeaveStandby", TLArg(m_deviceIndex, "ObjectId"));

            TraceLoggingWriteStop(local, "HandDriver_LeaveStandby");
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
            TraceLoggingWriteStart(local, "HandDriver_CreateBindings", TLArg(isLeft ? "Left" : "Right", "Role"));

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);
            std::vector<XrActionSuggestedBinding> bindings;

            // Setup bindings for actions based on the OpenXR hand interaction profile. Caller must ensure that profile
            // is supported.
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
            addInput(ComponentIndexPinch, XR_ACTION_TYPE_FLOAT_INPUT, "pinch_ext/value", "pinch_value", "Pinch Analog");
            addInput(ComponentPinkyPinch, XR_ACTION_TYPE_FLOAT_INPUT, "grasp_ext/value", "grasp_value", "Grasp Analog");
            // clang-format on

            TraceLoggingWriteStop(local, "HandDriver_CreateBindings");

            return bindings;
        }

        std::string GetInteractionProfile() const override {
            return "/interaction_profiles/ext/hand_interaction_ext";
        }

        bool IsTracked() override {
            std::shared_lock lock(m_poseMutex);
            return m_latestPose.poseIsValid;
        }

        void ApplySettingsChanges() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HandDriver_ApplySettingsChanges",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"));

            TraceLoggingWriteStop(local, "HandDriver_ApplySettingsChanges");
        }

        void UpdateTrackingState(XrTime time) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HandDriver_UpdateTrackingState",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(m_role == vr::TrackedControllerRole_LeftHand ? "Left" : "Right", "Role"),
                                   TLArg(time, "Time"));

            vr::DriverPose_t pose = {};
            pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1.0;

            if (m_ready) {
                static constexpr auto k_TrackedJoint = XR_HAND_JOINT_WRIST_EXT;
                XrHandJointsLocateInfoEXT info = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
                info.baseSpace = m_referenceSpace.Get();
                info.time = time;
                XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
                XrHandJointVelocityEXT velocities[XR_HAND_JOINT_COUNT_EXT];
                XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
                XrHandJointVelocitiesEXT locationVelocities = {XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
                locations.next = &locationVelocities;
                locations.jointLocations = joints;
                locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
                locationVelocities.jointVelocities = velocities;
                locationVelocities.jointCount = XR_HAND_JOINT_COUNT_EXT;
                CHECK_XRCMD(xrLocateHandJointsEXT(m_handTracker.Get(), &info, &locations));
                TraceLoggingWriteTagged(local,
                                        "HandDriver_UpdateTrackingState",
                                        TLArg(!!locations.isActive, "IsActive"),
                                        TLArg((int)joints[k_TrackedJoint].locationFlags, "LocationFlags"),
                                        TLArg(xr::ToString(joints[k_TrackedJoint].pose).c_str(), "Pose"));

                // Update the root pose.
                // TODO: Fix the incorrect pose offset here.
                pose.deviceIsConnected = locations.isActive;
                pose.result = vr::TrackingResult_Running_OutOfRange;

                pose.poseIsValid = Pose::IsPoseValid(joints[k_TrackedJoint].locationFlags);
                if (pose.poseIsValid) {
                    const auto& location = joints[k_TrackedJoint];

                    pose.vecPosition[0] = location.pose.position.x;
                    pose.vecPosition[1] = location.pose.position.y;
                    pose.vecPosition[2] = location.pose.position.z;
                    pose.qRotation.x = location.pose.orientation.x;
                    pose.qRotation.y = location.pose.orientation.y;
                    pose.qRotation.z = location.pose.orientation.z;
                    pose.qRotation.w = location.pose.orientation.w;

                    const auto velocity = velocities[k_TrackedJoint];
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

                    if (Pose::IsPoseTracked(joints[k_TrackedJoint].locationFlags)) {
                        pose.result = vr::TrackingResult_Running_OK;
                    }
                }

                {
                    std::unique_lock lock(m_poseMutex);
                    m_latestPose = pose;
                }
                vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_deviceIndex, pose, sizeof(pose));

                // Update the skeleton.
                if (pose.poseIsValid) {
                    using namespace DirectX;

                    vr::VRBoneTransform_t transforms[BoneCount]{};

                    const auto StorePose = [&](vr::VRBoneTransform_t& bone, const XMMATRIX& pose) {
                        XMVECTOR position, orientation, scale;
                        XMMatrixDecompose(&scale, &orientation, &position, pose);

                        // Per ALVR:
                        // // Convert to SteamVR frame of reference
                        // let(orientation, position) = if id == *LEFT_HAND_ID{
                        // (
                        //    Quat::from_xyzw(-o.z, -o.y, -o.x, o.w),
                        //    Vec3::new(-p.z, -p.y, -p.x),
                        // )} else {
                        // (
                        //    Quat::from_xyzw(o.z, o.y, -o.x, o.w),
                        //    Vec3::new(p.z, p.y, -p.x),
                        // )};
                        //
                        // Also, HmdQuaternionf_t is w,x,y,z
                        if (m_role == vr::TrackedControllerRole_LeftHand) {
                            bone.orientation = {XMVectorGetW(orientation),
                                                -XMVectorGetZ(orientation),
                                                -XMVectorGetY(orientation),
                                                -XMVectorGetX(orientation)};
                            bone.position = {
                                -XMVectorGetZ(position), -XMVectorGetY(position), -XMVectorGetX(position), 1.f};
                        } else {
                            bone.orientation = {XMVectorGetW(orientation),
                                                XMVectorGetZ(orientation),
                                                XMVectorGetY(orientation),
                                                -XMVectorGetX(orientation)};
                            bone.position = {
                                XMVectorGetZ(position), XMVectorGetY(position), -XMVectorGetX(position), 1.f};
                        }
                    };

                    XMMATRIX openxrRootPose, openxrWristPose, openxrPreviousJointPose;
                    for (uint32_t i = 0; i < BoneCount; i++) {
                        XMMATRIX openvrPose;
                        if (i <= BonePinkyFinger4) {
                            const XMMATRIX openxrPose = LoadXrPose(joints[i].pose);

                            // Re-root the transforms according to the hierarchy at:
                            // https://github.com/ValveSoftware/openvr/wiki/Hand-Skeleton#bone-structure
                            switch (i) {
                            case BoneRoot:
                                // There is only one root bone. It is intended to not translate or rotate, so that it
                                // can be constrained to the reference pose of the controller that is animating the
                                // skeleton.
                                static_assert(k_TrackedJoint == XR_HAND_JOINT_WRIST_EXT);
                                openvrPose = XMMatrixIdentity();
                                openxrRootPose = openxrPose;
                                break;

                            case BoneWrist:
                                // Magic offset from Index Controller
                                // https://github.com/ValveSoftware/openvr/blob/f51d87ecf8f7903e859b0aa4d617ff1e5f33db5a/samples/drivers/drivers/handskeletonsimulation/src/hand_simulation.cpp#L232C1-L233C1
                                // Swizzled to map with StorePose().
                                openvrPose = m_role == vr::TrackedControllerRole_LeftHand
                                                 ? XMMatrixRotationQuaternion(
                                                       XMVectorSet(-0.379296f, 0.920279f, 0.078608f, -0.055147f))
                                                 : XMMatrixRotationQuaternion(
                                                       XMVectorSet(0.379296f, 0.920279f, -0.078608f, -0.055147f));
                                openvrPose.r[3] = m_role == vr::TrackedControllerRole_LeftHand
                                                      ? XMVectorSet(-0.164722f, -0.036503f, 0.034038f, 1.000000f)
                                                      : XMVectorSet(-0.164722f, 0.036503f, 0.034038f, 1.000000f);
                                openxrWristPose = openxrPose;
                                break;

                            case BoneThumb0:
                            case BoneIndexFinger0:
                            case BoneMiddleFinger0:
                            case BoneRingFinger0:
                            case BonePinkyFinger0:
                                // Reset to the wrist base pose for each metacarpal.
                                openxrPreviousJointPose = openxrWristPose;

                                // Magic rotation for metacarpal.
                                openxrPreviousJointPose =
                                    XMMatrixMultiply(XMMatrixRotationQuaternion(XMQuaternionRotationRollPitchYaw(
                                                         DEG_TO_RAD(90.f), DEG_TO_RAD(-90.f), DEG_TO_RAD(0.f))),
                                                     openxrPreviousJointPose);

                                // Intentional fall-through.

                            default:
                                openvrPose =
                                    XMMatrixMultiply(openxrPose, XMMatrixInverse(nullptr, openxrPreviousJointPose));
                                openxrPreviousJointPose = openxrPose;
                                break;
                            }
                        } else {
                            // The skeleton has 5 auxiliary bones ('aux bones' for short) for helping in the
                            // construction of hand poses. These bones have the same position and rotation and rotation
                            // as the last knuckle bone in each finger, but are direct children of the root bone.
                            uint32_t knuckle;
                            switch (i) {
                            case BoneAux_Thumb:
                                knuckle = BoneThumb2;
                                break;
                            case BoneAux_IndexFinger:
                                knuckle = BoneIndexFinger3;
                                break;
                            case BoneAux_MiddleFinger:
                                knuckle = BoneMiddleFinger3;
                                break;
                            case BoneAux_RingFinger:
                                knuckle = BoneRingFinger3;
                                break;
                            case BoneAux_PinkyFinger:
                                knuckle = BonePinkyFinger3;
                                break;
                            }

                            const XMMATRIX openxrKnucklePose = LoadXrPose(joints[knuckle].pose);

                            openvrPose = XMMatrixMultiply(openxrKnucklePose, XMMatrixInverse(nullptr, openxrRootPose));
                        }

                        StorePose(transforms[i], openvrPose);
                    }

                    vr::VRDriverInput()->UpdateSkeletonComponent(m_components[ComponentSkeleton],
                                                                 vr::VRSkeletalMotionRange_WithController,
                                                                 transforms,
                                                                 BoneCount);
                    vr::VRDriverInput()->UpdateSkeletonComponent(m_components[ComponentSkeleton],
                                                                 vr::VRSkeletalMotionRange_WithoutController,
                                                                 transforms,
                                                                 BoneCount);
                }
            }

            TraceLoggingWriteStop(
                local, "HandDriver_UpdateTrackingState", TLArg(pose.poseTimeOffset, "PoseTimeOffset"));
        }

        void UpdateInputsState(XrTime time) {
            TraceLocalActivity(local);
            const bool isLeft = m_role == vr::TrackedControllerRole_LeftHand;
            TraceLoggingWriteStart(local,
                                   "HandDriver_UpdateInputsState",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(isLeft ? "Left" : "Right", "Role"),
                                   TLArg(time, "Time"));

            if (m_ready) {
                const vr::PropertyContainerHandle_t container =
                    vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

                if (m_actions[ComponentIndexPinch].Get() != XR_NULL_HANDLE) {
                    const auto updateAnalog = [&](Component index) {
                        XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
                        info.action = m_actions[index].Get();
                        XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
                        CHECK_XRCMD(xrGetActionStateFloat(m_session.Get(), &info, &state));
                        TraceLoggingWriteTagged(local,
                                                "HandDriver_UpdateInputs_UpdateScalarComponent",
                                                TLArg(!!state.isActive, "IsActive"),
                                                TLArg(state.currentState, "State"));
                        vr::VRDriverInput()->UpdateScalarComponent(m_components[index], state.currentState, 0.0);
                    };

                    updateAnalog(ComponentIndexPinch);
                    updateAnalog(ComponentPinkyPinch);
                } else {
                    // The interaction profile wasn't bound (not supported by the runtime).
                    // TODO: Fallback that emulates from hand joints math.
                }
            }

            TraceLoggingWriteStop(local, "HandDriver_UpdateInputsState");
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

        xr::HandTrackerHandle m_handTracker;
        xr::ActionHandle m_actions[ComponentCount];

        DirectX::XMMATRIX m_poseOffset = DirectX::XMMatrixIdentity();

        std::shared_mutex m_poseMutex;
        vr::DriverPose_t m_latestPose = {};
    };
} // namespace

namespace driver {
    std::unique_ptr<IHandDriver> CreateHandDriver(xr::InstanceHandle& instance,
                                                  xr::SessionHandle& session,
                                                  xr::SpaceHandle& referenceSpace,
                                                  vr::ETrackedControllerRole role) {
        return std::make_unique<HandDriver>(instance, session, referenceSpace, role);
    }
} // namespace driver
