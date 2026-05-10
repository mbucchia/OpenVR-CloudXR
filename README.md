# CloudXR driver for SteamVR

This driver for SteamVR lets you use SteamVR (and SteamVR applications, for example games built with OpenVR) with
CloudXR (or any OpenXR runtime, if this is something you are looking to do).

It works by bridging the OpenXR frame management, rendering and input/output subsystems into a SteamVR ["direct mode
component"](https://github.com/ValveSoftware/openvr/wiki/Driver-direct-mode) driver.

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

# Supported features

- Headset tracking
- Immersive VR display (streaming)
- Motion Controllers tracking
- Motion Controllers inputs (eg: triggers, buttons...)
- Motion Controllers haptics [NOT SUPPORTED BY CLOUDXR as of May 2026]
- Hand joints tracking [WORK IN PROGRESS]
- Eye tracking [NOT SUPPORTED BY CLOUDXR as of May 2026]
- Visibility mask (aka "hidden area mesh") [NOT SUPPORTED BY CLOUDXR as of May 2026]

# Usage

## For end-users

CloudXR is NOT an AR/VR streaming solution that is usable "out-of-the-box" for end-users. Instead, it is a developer
platform on top of which clients (streaming applications running on the headset) can be developed, and a service
infrastructure must be set-up.

**On its own, the OpenVR-CloudXR driver cannot be used.**

Instead, OpenVR-CloudXR is meant to enable developers to build on top of CloudXR solutions while preserving the ability
to offer SteamVR support.

## For CloudXR developers

### Building OpenVR-CloudXR

1. Use Visual Studio 2022-2026 (not Visual Studio Code).

1. Remember to fetch the Git submodules! `git submodule update --init` will do the trick!

1. After you build the Visual Studio solution, the driver is located under `bin\distribution`.

1. Register the driver with SteamVR by invoking (as regular user) the driver with
   `bin\distribution\Register-Driver.bat`.

### Setting up a CloudXR client

Follow the official CloudXR instructions. I cannot speak for Apple Vision Pro native clients, but for WebXR clients, use
[CloudXR.js](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_js/index.html).

The instructions are quite complex to follow and overwhelming. Here is a quick rundown to test on a Meta Quest.

1. Install [Node.js](https://nodejs.org/en/download/current). I personally just get a prebuilt version from that page.

1. Checkout the [CloudXR.js samples](https://github.com/NVIDIA/cloudxr-js-samples).

1. Build the `simple` sample. [These
   instructions](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_js/sample_webgl.html) are actually
   reasonable to follow. Prefer "Option B", since the Docker route has issues on Windows due to line-endings.

1. You should be up to the point of running `npm run dev-server`.

1. On the Quest, add your dev server URL to the list of trusted sites. It is explained
   [here](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_js/client_setup.html#option-1-configure-insecure-origins-http-mode),
   however at step "4. Apply configuration", you will want to do a full reboot of the Quest, not just "Relaunch".

1. At this point, you should be able to open the WebXR client on your Quest.

### Running the driver

By default, the driver will attempt to use a running CloudXR Service on your machine. This is a good scenario in case
you are spinning up and configuring the service yourself.

If you wish to have the driver spin up the service, start SteamVR, then head to _Settings_ -> _Cloud XR_, and toggle on
"Start Built-in CloudXR Service". Once you restart SteamVR, the driver will now wait for incoming client connections
(SteamVR will display the "Awaiting Wireless Connection" dialog). It's time to start/connect your CloudXR client, and
SteamVR will then continue booting.

Note that you do not need at any point to set the OpenXR `ActiveRuntime` on your system: OpenVR-CloudXR directly loads
the CloudXR OpenXR runtime built into the driver's folder.

## For other developers

If you wish to use OpenVR-CloudXR with an OpenXR platform other than CloudXR, you can set the `openxr_runtime_json`
driver setting to the name of an OpenXR runtime JSON file that is registered on your system through the
`AvailableRuntimes` registry key (`HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes`).

You can easily do that with SteamVR's `vrcmd` utility (SteamVR must be running, even without a headset connected), or
you can modify the `steamvr.vrsettings` file by hand (not recommended!):

```
"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrcmd.exe" --set-settings-string driver_cloudxr.openxr_runtime_json virtualdesktop-openxr.json
```
