// KRVR — Opaque data channel for haptic events.
//
// CloudXR's runtime advertises a non-public OpenXR extension
// `XR_NV_opaque_data_channel` (paired with the headset-side decoder in the
// visionOS app). The extension provides a UUID-keyed side-channel that
// piggy-backs on the existing streaming connection — perfect for sending
// small, latency-sensitive packets (haptics, custom inputs) without
// inventing a new transport.
//
// NVIDIA's own xrApplyHapticFeedback path through CloudXR is documented as
// "not fully supported yet" (per
// https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_runtime/controller_hand_tracking.html#other-considerations).
// Haptic events submitted that way reach the runtime but do not surface on
// the headset device. Until NVIDIA closes that path, we ferry the haptic
// data over the opaque channel ourselves.
//
// Wire format matches `krvr_layer.cpp`'s `HapticPacket` byte-for-byte —
// the AVP-side listener is shared between the OpenXR-direct path (the
// API layer) and the SteamVR path (this driver). UUID below is the
// agreed-upon channel ID — DO NOT CHANGE without coordinating with the
// visionOS decoder.

#pragma once

namespace krvr {

    // --- Wire packet ----------------------------------------------------------
    // MUST match krvr_layer.cpp's HapticPacket byte-for-byte. AVP decoder
    // routes by `magic`.

#pragma pack(push, 1)
    struct HapticPacket {
        uint16_t magic;       // MAGIC_HAPTIC
        uint8_t  version;     // 1
        uint8_t  hand;        // 0 = left, 1 = right
        uint64_t durationNs;  // 0 = stop
        float    frequency;
        float    amplitude;
    };
#pragma pack(pop)

    static_assert(sizeof(HapticPacket) == 20, "HapticPacket must be exactly 20 bytes");
    static constexpr uint16_t MAGIC_HAPTIC = 0x4856; // 'HV'

    // --- Singleton manager ---------------------------------------------------
    // Header-only so we don't need a new .cpp in the vcxproj. State lives in
    // function-local statics with an inline accessor — single instance at
    // process level, thread-safe init under C++11 magic statics.

    class OpaqueChannel {
    public:
        static OpaqueChannel& Get() {
            static OpaqueChannel inst;
            return inst;
        }

        bool Init(XrInstance instance, XrSystemId systemId,
                  PFN_xrGetInstanceProcAddr getProcAddr) {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_inited) return true;
            m_instance = instance;
            m_systemId = systemId;

            auto resolve = [&](const char* name, auto& out) -> bool {
                PFN_xrVoidFunction fn = nullptr;
                if (getProcAddr(instance, name, &fn) != XR_SUCCESS || !fn) {
                    return false;
                }
                out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(fn);
                return true;
            };

            if (!resolve("xrCreateOpaqueDataChannelNV", m_fnCreate)) return false;
            if (!resolve("xrDestroyOpaqueDataChannelNV", m_fnDestroy)) return false;
            if (!resolve("xrGetOpaqueDataChannelStateNV", m_fnGetState)) return false;
            if (!resolve("xrShutdownOpaqueDataChannelNV", m_fnShutdown)) return false;
            if (!resolve("xrSendOpaqueDataChannelNV", m_fnSend)) return false;

            // Same UUID as krvr_layer.cpp — the AVP listener is keyed to it.
            const GUID uuid = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}};
            XrOpaqueDataChannelCreateInfoNV ci{};
            ci.type = XR_TYPE_OPAQUE_DATA_CHANNEL_CREATE_INFO_NV;
            ci.systemId = systemId;
            memcpy(&ci.uuid, &uuid, sizeof(uuid));

            XrOpaqueDataChannelNV ch = XR_NULL_HANDLE;
            if (m_fnCreate(instance, &ci, &ch) != XR_SUCCESS) {
                return false;
            }
            m_channel = ch;
            m_inited = true;
            return true;
        }

        // Returns true if the channel is in CONNECTED state. Cheap to call
        // before every send — avoids spamming sends while disconnected.
        bool IsConnected() {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_inited || m_channel == XR_NULL_HANDLE) return false;
            XrOpaqueDataChannelStateNV state{};
            state.type = XR_TYPE_OPAQUE_DATA_CHANNEL_STATE_NV;
            if (m_fnGetState(m_channel, &state) != XR_SUCCESS) return false;
            return state.state == XR_OPAQUE_DATA_CHANNEL_STATUS_CONNECTED_NV;
        }

        void SendHaptic(uint8_t hand, uint64_t durationNs, float frequency, float amplitude) {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_inited || m_channel == XR_NULL_HANDLE) return;

            HapticPacket pkt{};
            pkt.magic = MAGIC_HAPTIC;
            pkt.version = 1;
            pkt.hand = hand;
            pkt.durationNs = durationNs;
            pkt.frequency = frequency;
            pkt.amplitude = amplitude;
            // Best-effort send. If the channel is in disconnected state the
            // runtime returns an error which we ignore — the next haptic
            // event will retry on its own.
            (void)m_fnSend(m_channel, (uint32_t)sizeof(pkt),
                           reinterpret_cast<const uint8_t*>(&pkt));
        }

        void Shutdown() {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_channel != XR_NULL_HANDLE && m_fnShutdown) {
                m_fnShutdown(m_channel);
            }
            if (m_channel != XR_NULL_HANDLE && m_fnDestroy) {
                m_fnDestroy(m_channel);
            }
            m_channel = XR_NULL_HANDLE;
            m_inited = false;
        }

    private:
        OpaqueChannel() = default;
        OpaqueChannel(const OpaqueChannel&) = delete;
        OpaqueChannel& operator=(const OpaqueChannel&) = delete;

        std::mutex m_mutex;
        bool m_inited = false;
        XrInstance m_instance = XR_NULL_HANDLE;
        XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
        XrOpaqueDataChannelNV m_channel = XR_NULL_HANDLE;

        PFN_xrCreateOpaqueDataChannelNV    m_fnCreate   = nullptr;
        PFN_xrDestroyOpaqueDataChannelNV   m_fnDestroy  = nullptr;
        PFN_xrGetOpaqueDataChannelStateNV  m_fnGetState = nullptr;
        PFN_xrShutdownOpaqueDataChannelNV  m_fnShutdown = nullptr;
        PFN_xrSendOpaqueDataChannelNV      m_fnSend     = nullptr;
    };

} // namespace krvr
