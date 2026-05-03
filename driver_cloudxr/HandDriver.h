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

namespace driver {

    struct IHandDriver : public vr::ITrackedDeviceServerDriver {
        virtual ~IHandDriver() = default;
        virtual std::vector<XrActionSuggestedBinding> CreateBindings(XrActionSet actionSet) = 0;
        virtual std::string GetInteractionProfile() const = 0;
        virtual bool IsTracked() = 0;
        virtual void ApplySettingsChanges() = 0;
        virtual void UpdateTrackingState(XrTime time) = 0;
        virtual void UpdateInputsState(XrTime time) = 0;
        virtual void LeaveStandby() = 0;
        virtual const char* GetSerialNumber() const = 0;
        virtual vr::TrackedDeviceIndex_t GetDeviceIndex() const = 0;
    };

    std::unique_ptr<IHandDriver> CreateHandDriver(xr::InstanceHandle& instance,
                                                  xr::SessionHandle& session,
                                                  xr::SpaceHandle& referenceSpace,
                                                  vr::ETrackedControllerRole role);

} // namespace driver
