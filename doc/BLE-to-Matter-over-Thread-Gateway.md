# BLE-to-Matter-over-Thread Gateway (nRF52840)

Nov 3, 2026 · @Bernd

## Goal

A mains-powered nRF52840 gateway brings existing BLE thermometer/humidity sensors and BLE thermostats into a Matter smart home over Thread. It is for home use, so Matter certification is not a concern.

## Architecture

The nRF52840 runs the nRF Connect SDK Matter bridge: it listens to sensors, connects to thermostats and exposes each BLE device as a Matter endpoint over Thread.

*(The original artifact has an architecture diagram that is not included in this export. Its content is summarized below.)*

On the single nRF52840 radio, four blocks run side by side:

- **BLE central:** scans the sensors (advertising only) and connects to the thermostats (GATT connection).
- **Matter bridge:** exposes one endpoint per BLE device.
- **Thread node:** joins the mesh over IEEE 802.15.4.
- **BLE peripheral:** a separate BLE role from the central, used for Matter commissioning only.

The Thread node talks to a border router (Home Assistant OTBR, or HomePod / Nest Hub), which links the mesh to the Matter controller (Home Assistant, Apple or Google Home).

Sensors are only listened to, thermostats are connected to, and the border router carries Matter traffic between the Thread mesh and the controller. Commissioning uses the separate BLE peripheral role at setup: the controller connects to the gateway as a peripheral, while the central role handles the sensors and thermostats.

## Device handling

Sensors only need scan airtime, while thermostats need BLE connections, so the two types load the gateway very differently.

| Device | BLE method | Matter mapping | Main work |
| --- | --- | --- | --- |
| Thermometer/humidity sensors | Passive scan of advertisements, no connection | Temperature Sensor and Humidity Sensor, one endpoint per sensor | Parse the vendor's advertisement format (check for encryption or bind keys), match by MAC, mark the endpoint unreachable after a timeout, rate-limit updates |
| Thermostats | Connection required, connect-on-demand: on each write and a poll about every 5 minutes, one thermostat at a time | Thermostat cluster (local temperature, heating setpoint, system mode) | Reverse-engineer or find the GATT protocol, store bonding keys, handle write latency and retries |

## Reference implementation: Tasmota (thermostats)

Tasmota on ESP32 already bridges Eqiva/eQ-3 Bluetooth radiator valves (TRVs) over BLE and has a generic Matter thermostat endpoint. The code is GPL-3.0, so use it as a protocol and design reference, not as code to copy. Links are pinned to commit `568220d` (2026-11-02).

**BLE thermostat driver:** [`xdrv_85_esp32_ble_eq3_trv.ino`](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino)
- [GATT service and characteristic UUIDs](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L299-L301): the service, the write characteristic and the notify characteristic
- [Operation queue](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L447-L535): one connection at a time, with retries. This is the same connect-on-demand pattern planned here.
- [Status response parsing](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L537-L714): temperature, valve position, mode, boost, window, lock and battery
- [Command encoding](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L1220-L1529): byte layouts for set temperature, mode, boost, lock, offset, profiles and so on. For example, the set-temperature command is `0x41` followed by the temperature × 2.
- [Polling loop](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L991-L1054): a periodic state request that runs only while the queue is idle
- [BTHome advertisement parsing](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/tasmota/tasmota_xdrv_driver/xdrv_85_esp32_ble_eq3_trv.ino#L747-L902): only for TRVs running the [eq3-custom-fw](https://github.com/dbuezas/eq3-custom-fw). It lets the gateway read state without connecting, much like the sensors.

**Matter thermostat endpoint:** [`Matter_Plugin_2_Thermostat.be`](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_2_Thermostat.be) shows a minimal Thermostat (0x0301) device. It exposes cluster 0x0201 with LocalTemperature, the heating and cooling setpoints, SystemMode, ControlSequenceOfOperation and RunningState. It is a useful checklist for the attributes the bridge endpoint needs. The bridged sensor equivalents, [Temperature](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_8_Bridge_Sensor_Temp.be) and [Humidity](https://github.com/arendst/Tasmota/blob/568220d493f13cadec7c82c66500f71190790aae/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_8_Bridge_Sensor_Humidity.be), are in the same folder.

**Documentation:** [Tasmota docs – Eqiva eQ-3 TRV](https://tasmota.github.io/docs/EQ3-TRV/) covers commands, MQTT responses and setup. Setup includes a one-time BLE pairing step (`BLEPair`), which matches the plan to store bonding keys on the gateway.

This reference only helps if your thermostats are eQ-3 models (see Open items). For other brands, the GATT protocol still has to be found or reverse-engineered.

## Constraints and risks

The main risk is resources: Matter, Thread and a BLE central together on the nRF52840's 256 KB RAM is tight.

- Measure RAM, flash and radio load after the first thermostat works, before scaling up.
- Sensors cost scan airtime but no connections. Thermostats are polled one at a time, so one central link is enough. Configure 2 to 3 links in total so Matter commissioning can overlap, queue controller commands ahead of routine polls, and cap each connection attempt with a timeout.
- Use a conservative scan duty cycle (scan window well below the scan interval) so Thread traffic isn't starved.
- Confirm the nRF52840 is supported by the Matter bridge sample in the chosen SDK version. The nRF5340 is the fallback if resources run out.
- The sample handles only a few BLE services, so custom GATT-to-cluster mapping is needed for your devices.

## Toolchain

Everything runs in VS Code with Nordic's free nRF Connect tooling.

- VS Code with the nRF Connect extension pack
- nRF Connect SDK, installed through the extension and pinned to one version per project
- SEGGER J-Link drivers, with an nRF52840 DK for flashing and debugging
- nRF Connect for Mobile, and a second nRF52840 as a Wireshark sniffer, for inspecting advertisements and thermostat traffic
- Home Assistant or `chip-tool` for commissioning and testing

## Plan

Build up in five steps, proving the stock sample first and the hardest device type last.

1. Set up the toolchain and run the stock Matter bridge sample on a DK.
2. Commission it into your controller and confirm the demo endpoints work.
3. Add advertisement-based sensors (parse, map, handle offline state).
4. Add one thermostat with connect-on-demand and bonding.
5. Measure RAM, flash and radio load, then scale up or move to the nRF5340.

## Open items

Two facts decide how much custom work this is.

- [ ] Exact sensor and thermostat models (these set the advertisement format and the GATT work)
- [x] Whether the sensors can run open firmware such as BTHome (yes: pvvx firmware with BTHome v2)

## Future ideas

- **Thread network map from the gateway.** Apple Home (HomePod as border router) shows nothing about the mesh, and ready-made topology views need an OpenThread Border Router or Home Assistant. The gateway is a full Thread router, so it can query the mesh itself: add the OpenThread shell and mesh diagnostics (`ot meshdiag topology`) to the diag build only, then a script like `scripts/diag-capture.py` reads it over the serial port and draws routers, children and link quality in both directions (Graphviz or a small HTML page). Would help with the far sensors. Open: whether the HomePods answer the diagnostic queries (they run OpenThread, so likely). For a quick look without firmware changes, the Eve app shows Thread devices and their roles.
