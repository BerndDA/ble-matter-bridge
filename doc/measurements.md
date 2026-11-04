# Measurements

## 2026-11-04: nRF52840 DK, two BTHome sensors, Thread router

Setup:
- `firmware/gateway` built with [diag.conf](../firmware/gateway/diag.conf) (stack, heap and thread statistics).
- Commissioned into Apple Home; a HomePod is the Thread border router. The gateway is a Thread router.
- Two pvvx sensors (BTHome v2, advertising every 2.5 s, measuring every 10 s): `A4:C1:38:0C:54:AA` near the DK at about -30 dBm, `A4:C1:38:57:96:28` in another room at about -80 dBm.
- Background scan: window 25 ms every 100 ms (25 % duty cycle).
- A/B capture of 60 minutes, sampled every minute: 30 minutes with the background scan running (A), then 30 minutes with it paused (B).

Reproduce (close nRF Terminal first):

```sh
source scripts/ncs-env.sh
west build -b nrf52840dk/nrf52840 --sysbuild -d firmware/gateway/build-diag firmware/gateway -- -DEXTRA_CONF_FILE=diag.conf
west flash -d firmware/gateway/build-diag
python3 scripts/diag-capture.py --minutes 60 --interval 60 --ab
python3 scripts/diag-summary.py measurements/diag-<time>.log
```

### Memory

| | Used | Free |
| --- | --- | --- |
| Flash, app (release build) | 844 KB of 975 KB | 131 KB |
| Flash, app (diagnostics build) | 905 KB of 975 KB | 70 KB |
| RAM, reserved at build time | 208 KB of 256 KB | 48 KB |
| Matter heap, peak | 4.1 KB | |
| Zephyr system heap | 0 B of 516 B used | never used |

Peak stack use (since boot, 62 minutes):

| Thread | Peak use |
| --- | --- |
| CHIP (Matter) | 3640 of 10240 B (35 %) |
| openthread | 1860 of 4096 B (45 %) |
| main | 2280 of 6144 B (37 %) |
| BT RX WQ | 512 of 1216 B (42 %) |
| bt_tx_processor | 476 of 768 B (61 %) |
| **sysworkq** | **864 of 1152 B (75 %)**, the only one above 70 % |

The CPU was 98 % idle.

### Radio: Thread with and without the BLE scan

| | A: scan running | B: scan paused |
| --- | --- | --- |
| Thread role | Router, no role, parent or partition changes | Same |
| 802.15.4 frames received per minute | 625 | 806 |
| Receive errors "other" | 560 | 85 |
| Unicast frames sent (ack requested) | 121 | 12 |
| Retries | 48 (40 %) | 10 (83 %) |
| Frames dropped after all retries | 0 | 0 |
| CCA failures | 1 | 0 |

The scan costs Thread about a quarter of its listening time. With the scan running, the gateway received 22 % fewer 802.15.4 frames, which matches the 25 % scan duty cycle. Most of the receive errors marked "other" (receptions cut off) disappeared when the scan was paused.

The retry rate can't be blamed on the scan from this data. Almost all unicast traffic is Matter reports of new sensor values, so it stopped together with the scan. The 12 frames sent in phase B are too few to compare, and they were retried even more often. No frame was lost in either phase, and the Thread link stayed stable.

### BLE reception

| Sensor | Advertisements received (A) | New measurements per minute | Longest gap |
| --- | --- | --- | --- |
| Near, -30 dBm | 3.8 per minute, 16 % of those sent | 1.8 | 122 s |
| Far, -77 to -83 dBm | 0.3 per minute, 1 % | 0.2 | 393 s |

At a 25 % duty cycle, at most about 25 % of the advertisements can be received. The near sensor gets 16 %, which is enough for temperature and humidity: a new value about every 30 s. The far sensor is barely within range. Its longest gap of 6.5 minutes is close to the 10-minute unreachable timeout.

### Conclusions

- The nRF52840 has room for the planned scale: 131 KB flash and 48 KB RAM are free, and the CPU is almost idle. There is no reason to move to the nRF5340 yet.
- Raise `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` (Zephyr default 1152 B) before adding more work, for example to 1536 B.
- Thread is stable as a router while scanning. The cost is about 25 % less listening time for Thread, which shows up as cut-off receptions but not as lost frames.
- Sensors far from the gateway are the weak point. Options: place the gateway more centrally; let pvvx sensors advertise more often (for example every 1 s); or try a 50 % scan duty cycle and run this A/B test again.
- Measure again after step 4, because connecting to a thermostat adds BLE connection events.
