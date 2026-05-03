// MIT License
//
// Copyright(c) 2026- Matthieu Bucchianeri
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

#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <string>

#include <cxrServiceAPI.h>
#pragma comment(lib, "cloudxr.lib")

int main() {
    nv_cxr_result result;
    nv_cxr_service* service = nullptr;

    result = nv_cxr_service_create(&service);
    if (result) {
        fprintf(stderr, "Error creating service: %d\n", result);
        return 1;
    }

    const auto setStringProperty = [&](const std::string& property, const std::string& value) {
        const nv_cxr_result result =
            nv_cxr_service_set_string_property(service, property.c_str(), property.size(), value.c_str(), value.size());
        if (result) {
            fprintf(stderr, "Error setting property '%s': %d\n", property.c_str(), result);
        }
    };
    const auto setBooleanProperty = [&](const std::string& property, const bool value) {
        const nv_cxr_result result =
            nv_cxr_service_set_boolean_property(service, property.c_str(), property.size(), value);
        if (result) {
            fprintf(stderr, "Error setting property '%s': %d\n", property.c_str(), result);
        }
    };
    const auto setInt64Property = [&](const std::string& property, const int64_t value) {
        const nv_cxr_result result =
            nv_cxr_service_set_int64_property(service, property.c_str(), property.size(), value);
        if (result) {
            fprintf(stderr, "Error setting property '%s': %d\n", property.c_str(), result);
        }
    };

    setStringProperty("device-profile", "auto-webrtc");
    setBooleanProperty("disable-alpha", true);
    setBooleanProperty("audio-streaming", false);
    setBooleanProperty("runtime-foveation", true);
    //setInt64Property("runtime-foveation-unwarped-width", 2048);
    //setInt64Property("runtime-foveation-warped-width", 1280);
    //setInt64Property("runtime-foveation-inset", 40);

    result = nv_cxr_service_start(service);
    if (result) {
        fprintf(stderr, "Error starting service: %d\n", result);
        return 1;
    }

    printf("Service is running!\n\n");

    printf("=============================================================\n");
    printf("==================== PRESS ENTER TO EXIT ====================\n");
    printf("=============================================================\n\n");

    nv_cxr_event event;
    while (true) {
        if (_kbhit() && getchar() == '\n') {
            break;
        }

        do {
            result = nv_cxr_service_poll_event(service, &event);
            if (result) {
                fprintf(stderr, "Error polling for events: %d\n", result);
            }

            switch (event.type) {
            case NV_CXR_EVENT_CLOUDXR_CLIENT_CONNECTED:
                printf("Client connected!\n");
                break;
            case NV_CXR_EVENT_CLOUDXR_CLIENT_DISCONNECTED:
                printf("Client disconnected.\n");
                break;
            case NV_CXR_EVENT_OPENXR_APP_CONNECTED:
                printf("App connected!\n");
                break;
            case NV_CXR_EVENT_OPENXR_APP_DISCONNECTED:
                printf("App disconnected!\n");
                break;
            }
        } while (!result && event.type != NV_CXR_EVENT_NONE);

        Sleep(100);
    }

    printf("Exiting...\n");

    nv_cxr_service_stop(service);
    nv_cxr_service_join(service);
    nv_cxr_service_destroy(service);

    return 0;
}
