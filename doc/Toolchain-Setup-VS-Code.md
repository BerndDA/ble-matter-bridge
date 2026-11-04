# Toolchain Setup (VS Code, macOS)

This is the minimum you need to build and flash firmware for the nRF52840 gateway from VS Code. Sniffer, mobile app and commissioning tools can be added later.

## What you need

- Mac with macOS, VS Code, internet access
- nRF52840 DK and a USB cable

## Steps

1. **Install SEGGER J-Link.** Download the macOS installer from SEGGER's J-Link Software page and run it. Use the J-Link version listed in the release notes for your SDK version. Nordic Academy currently names v8.76 for SDK v3.2.
2. **Install VS Code** from code.visualstudio.com.
3. **Install the extension pack.** In VS Code, open Extensions, search for **nRF Connect for VS Code Extension Pack** and install the full pack, not just the base extension. It brings in nRF Connect, DeviceTree, Kconfig, nRF Terminal, C/C++, CMake and linker map support.
4. **Install the SDK and toolchain.** In the nRF Connect panel, choose **Install SDK** → **nRF Connect SDK** (not "Bare Metal") → the latest released tag. From v3.1.0 onward, the SDK, toolchain and nRF Util come as a single package. Pin this version for the project, and don't use preview tags for real work.
5. **Check the setup.** Create a new application from a sample (for example *hello_world*), add a build configuration for board `nrf52840dk/nrf52840`, build it, connect the DK and click **Flash**. Open the serial output in nRF Terminal to confirm it runs.

Next, build the SDK's `hello_world` sample for `nrf52840dk/nrf52840`, flash it to the DK, and verify its serial output. The Matter Bridge application in NCS v3.4.1 does not list the nRF52840 DK as a supported target; its supported platforms include the nRF5340 DK and nRF54LM20 DK. To run the bridge sample, use one of those supported boards or separately validate a port to the nRF52840.

## Installing on a separate drive (macOS)

**Toolchain: not officially supported.** On macOS, the toolchain always installs to `/opt/nordic/ncs/toolchains/`. The VS Code setting `nrf-connect.toolchainManager.installDirectory`, which moves the toolchain on Windows and Linux, has no effect on macOS according to Nordic's settings reference.

**Workaround (unofficial, untested):** Replace `/opt/nordic` with a symlink to a folder on the external drive before installing. If you try it:
- Format the drive as APFS, not exFAT, so permissions and symlinks work.
- Use a path without spaces.
- The drive has to be connected whenever you build.

If anything breaks, go back to the default location first before troubleshooting.

**Your projects** are ordinary folders and can live on any drive.

## Sources

- [Nordic Academy – Installing nRF Connect SDK and VS Code (v3.2.0)](https://academy.nordicsemi.com/?p=48898)
- [Nordic docs – nrf-connect.toolchainManager.installDirectory](https://docs.nordicsemi.com/r/NexOA~Hcq7~UmFj_rM~Nsw/rtyujlvTppsbagGZD7LUiA)
- [DevZone – How to change the toolchain path in VS Code](https://devzone.nordicsemi.com/f/nordic-q-a/120705/how-to-change-the-toolchain-path-in-vscode)
