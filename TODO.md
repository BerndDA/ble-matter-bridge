# TODO

What's left to do and to decide. Tick items off here; the plan behind the steps is in [doc/BLE-to-Matter-over-Thread-Gateway.md](doc/BLE-to-Matter-over-Thread-Gateway.md).

## Decisions

- [ ] **Thermostat model.** This decides step 4. For eQ-3 thermostats, Tasmota's driver documents the GATT protocol; other brands need reverse engineering.
- [ ] **Far sensors.** The sensor in the other room delivers about 1 % of its advertisements, with gaps of up to 6.5 minutes ([measurements](doc/measurements.md)). Options:
  - Place the gateway more centrally (no code change).
  - Have pvvx advertise more often, for example every 1 s (costs sensor battery).
  - Scan at a 50 % duty cycle instead of 25 % (costs Thread listening time; run the A/B test again).
- [ ] **Unreachable timeout.** 10 minutes now. Raise it if far sensors stay, or keep it once reception is better.
- [ ] **What comes next:** battery reporting, or step 4.

## Next

- [ ] Raise `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` to 1536 B (peak use 75 % of 1152 B).
- [ ] Battery for BTHome sensors: Power Source cluster on the bridged endpoints (ZAP change), with BatPercentRemaining.

## Step 4: one thermostat

- [ ] Find the GATT protocol for the chosen model.
- [ ] Connect on demand with bonding; keys stored on the gateway.
- [ ] Thermostat endpoint (cluster 0x0201): LocalTemperature, OccupiedHeatingSetpoint, SystemMode, battery.
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
- [x] Step 5, first round: RAM, flash and radio measured with two sensors
- [x] Sensors decided: pvvx firmware with BTHome v2, unencrypted
