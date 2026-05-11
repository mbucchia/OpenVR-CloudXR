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

#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#define NOMINMAX
#include <windows.h>

#include <TraceLoggingActivity.h>
#include <TraceLoggingProvider.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;
#include <wil/resource.h>

#include <array>
#include <atomic>
#include <chrono>
#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <openvr_driver.h>

#include <cxrServiceAPI.h>

#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <DirectXMath.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#define ENABLE_GLOBAL_XR_DISPATCH_TABLE
#define NO_THROW_HANDLER
#include <XrUtility/XrDispatchTable.h>
#include <XrUtility/XrEnumerate.h>
#include <XrUtility/XrError.h>
#include <XrUtility/XrExtensionContext.h>
#include <XrUtility/XrMath.h>
#include <XrUtility/XrStereoView.h>
#include <XrUtility/XrToString.h>
#include <SampleShared/ScopeGuard.h>
#include <SampleShared/XrInstanceContext.h>
#include <SampleShared/XrSessionContext.h>
#include <SampleShared/XrSystemContext.h>

#include <cJSON.h>
