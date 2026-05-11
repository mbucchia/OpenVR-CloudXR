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

TRACELOGGING_DECLARE_PROVIDER(TraceProvider);

#define IsTraceEnabled() TraceLoggingProviderEnabled(TraceProvider, 0, 0)

#define TraceLocalActivity(activity) TraceLoggingActivity<TraceProvider> activity;

#define TLArg(var, ...) TraceLoggingValue(var, ##__VA_ARGS__)
#define TLPArg(var, ...) TraceLoggingPointer(var, ##__VA_ARGS__)

namespace {
    void DriverLogVarArgs(const char* pMsgFormat, va_list args) {
        char buf[1024];
        vsnprintf_s(buf, sizeof(buf), pMsgFormat, args);

        TraceLoggingWrite(TraceProvider, "DriverLog", TLArg(buf, "Message"));
        vr::VRDriverLog()->Log(buf);
    }
} // namespace

static inline void DriverLog(const char* pMsgFormat, ...) {
    va_list args;
    va_start(args, pMsgFormat);

    DriverLogVarArgs(pMsgFormat, args);

    va_end(args);
}

namespace xr::detail {
    // Add new macro for CloudXR results.
#define CHECK_CXRCMD(cmd) xr::detail::_CheckCxrResult(cmd, #cmd, FILE_AND_LINE)

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
} // namespace xr::detail
