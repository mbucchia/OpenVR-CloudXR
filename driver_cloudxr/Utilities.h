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

#pragma once

#define DEG_TO_RAD(degrees) ((degrees) * (float)M_PI / 180.f)
#define RAD_TO_DEG(radians) ((radians) * 180.f / (float)M_PI)

namespace util {

    static inline bool IsDepthFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return true;
        }
        return false;
    }

    static inline DXGI_FORMAT GetTypedFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_D16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        }
        return format;
    }

    static inline DirectX::XMMATRIX LoadHmdMatrix34(const vr::HmdMatrix34_t& matrix) {
        return {matrix.m[0][0],
                matrix.m[1][0],
                matrix.m[2][0],
                0.f,
                matrix.m[0][1],
                matrix.m[1][1],
                matrix.m[2][1],
                0.f,
                matrix.m[0][2],
                matrix.m[1][2],
                matrix.m[2][2],
                0.f,
                matrix.m[0][3],
                matrix.m[1][3],
                matrix.m[2][3],
                1.f};
    }

    static inline DirectX::XMMATRIX LoadHmdMatrix44(const vr::HmdMatrix44_t& matrix) {
        return {matrix.m[0][0],
                matrix.m[1][0],
                matrix.m[2][0],
                matrix.m[3][0],
                matrix.m[0][1],
                matrix.m[1][1],
                matrix.m[2][1],
                matrix.m[3][1],
                matrix.m[0][2],
                matrix.m[1][2],
                matrix.m[2][2],
                matrix.m[3][2],
                matrix.m[0][3],
                matrix.m[1][3],
                matrix.m[2][3],
                matrix.m[3][3]};
    }

    static inline vr::HmdMatrix34_t StoreHmdMatrix34(const DirectX::XMMATRIX& matrix) {
        DirectX::XMFLOAT4X3 temp;
        DirectX::XMStoreFloat4x3(&temp, matrix);
        return {temp.m[0][0],
                temp.m[1][0],
                temp.m[2][0],
                temp.m[3][0],
                temp.m[0][1],
                temp.m[1][1],
                temp.m[2][1],
                temp.m[3][1],
                temp.m[0][2],
                temp.m[1][2],
                temp.m[2][2],
                temp.m[3][2]};
    }

    static inline vr::HmdMatrix44_t StoreHmdMatrix44(const DirectX::XMMATRIX& matrix) {
        DirectX::XMFLOAT4X4 temp;
        DirectX::XMStoreFloat4x4(&temp, matrix);
        return {temp.m[0][0],
                temp.m[1][0],
                temp.m[2][0],
                temp.m[3][0],
                temp.m[0][1],
                temp.m[1][1],
                temp.m[2][1],
                temp.m[3][1],
                temp.m[0][2],
                temp.m[1][2],
                temp.m[2][2],
                temp.m[3][2],
                temp.m[0][3],
                temp.m[1][3],
                temp.m[2][3],
                temp.m[3][3]};
    }

} // namespace util

#define CHECK_CXRCMD(cmd) xr::detail::_CheckCxrResult(cmd, #cmd, FILE_AND_LINE)

namespace xr {
    namespace detail {
        [[noreturn]] inline void _ThrowCxrResult(nv_cxr_result res,
                                                 const char* originator = nullptr,
                                                 const char* sourceLocation = nullptr) {
            xr::detail::_Throw(_Fmt("nv_cxr_result failure [%d]", res), originator, sourceLocation);
        }

        inline nv_cxr_result _CheckCxrResult(nv_cxr_result res,
                                             const char* originator = nullptr,
                                             const char* sourceLocation = nullptr) {
            if (XR_FAILED(res)) {
                xr::detail::_ThrowCxrResult(res, originator, sourceLocation);
            }

            return res;
        }
    } // namespace detail

    static inline std::string ToString(const XrPosef& pose) {
        return xr::detail::_Fmt("p: (%.3f, %.3f, %.3f), o:(%.3f, %.3f, %.3f, %.3f)",
                                pose.position.x,
                                pose.position.y,
                                pose.position.z,
                                pose.orientation.x,
                                pose.orientation.y,
                                pose.orientation.z,
                                pose.orientation.w);
    }

    static inline std::string ToString(const XrFovf& fov) {
        return xr::detail::_Fmt(
            "(u: %.3f, d: %.3f, l:%.3f, r:%.3f)", fov.angleUp, fov.angleDown, fov.angleLeft, fov.angleRight);
    }

    static inline std::string ToString(const XrVector3f& vec) {
        return xr::detail::_Fmt("(%.3f, %.3f, %.3f)", vec.x, vec.y, vec.z);
    }

    static inline std::string ToString(const XrVector2f& vec) {
        return xr::detail::_Fmt("(%.3f, %.3f)", vec.x, vec.y);
    }

    static inline std::string ToString(const XrRect2Di& rect) {
        return xr::detail::_Fmt(
            "((%.3f, %.3f), (%.3f, %.3f))", rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
    }
} // namespace xr