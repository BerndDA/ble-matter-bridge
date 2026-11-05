# TODO

What's left to do and to decide. Tick items off here; the plan behind the steps is in [doc/BLE-to-Matter-over-Thread-Gateway.md](doc/BLE-to-Matter-over-Thread-Gateway.md).

## Decisions

- [ ] **Far sensors.** The sensor in the other room delivers about 1 % of its advertisements, with gaps of up to 6.5 minutes ([measurements](doc/measurements.md)). Options:
  - Place the gateway more centrally (no code change).
  - Have pvvx advertise more often, for example every 1 s (costs sensor battery).
  - Scan at a 50 % duty cycle instead of 25 % (costs Thread listening time; run the A/B test again).
- [ ] **Unreachable timeout.** 10 minutes now. Raise it if far sensors stay, or keep it once reception is better.

## Next

- [ ] Step 4: measure again with thermostat commands (step 5), then decide on the eQ-3 modes.

## Step 4: one thermostat

- [x] eQ-3 CC-RT-BLE with [eq3-custom-fw](https://github.com/dbuezas/eq3-custom-fw): values come from BTHome broadcasts, control over GATT ([spike/eq3-flash](spike/eq3-flash/README.md), protocol in the firmware's PROTOCOL.md).
- [x] Thermostat endpoint (cluster 0x0201), read-only: room temperature, target, valve as heating demand, Off below 4.75 °C, battery from voltage and low flag.
- [x] Checked in Apple Home.
- [x] Setpoint and Off/Heat from Apple Home: one short GATT connection per command, 7–13 s until the thermostat has it. Needs the thermostat's PIN off ("Ask for a PIN" in the web app, or `PIn CodE` in its Bluetooth menu).
- [ ] Decide whether the PIN should be on, with the gateway pairing once using the code from the display.
- [ ] Connect sooner: many connect attempts miss the advertisement or drop at once (HCI 0x3E); try connecting right after the scanner hears the thermostat.
- [ ] Map eQ-3 modes (auto, manual, holiday, boost) and decide what Apple Home shows for them.
- [ ] Command queue ahead of routine polls, with a timeout per connection attempt.
- [ ] Measure again (step 5) with the thermostat connected.

## Later

- [ ] Own Matter credentials and DAC instead of the test ones; own MCUboot signing key instead of the SDK's debug key.
- [ ] OTA updates over Matter.
- [ ] Encrypted BTHome (bindkey per sensor).
- [ ] Decide on the nRF5340 only if a later measurement runs out of headroom.

## Done

- [x] Step 1: toolchain and the bridge on the nRF52840 DK
- [x] Step 2: commissioned into Apple Home (HomePod as border router)
- [x] Step 3: BTHome sensors with auto-add, remove/ignore, temperature and humidity
- [x] Sensors restore after a reboot with the same endpoints and names in Apple Home
- [x] Battery level of BTHome sensors in Apple Home (Power Source cluster)
- [x] System work queue stack raised to 1536 B
- [x] Step 5, first round: RAM, flash and radio measured with two sensors
- [x] Sensors decided: pvvx firmware with BTHome v2, unencrypted
- [x] Thermostat decided: eQ-3 CC-RT-BLE with eq3-custom-fw
