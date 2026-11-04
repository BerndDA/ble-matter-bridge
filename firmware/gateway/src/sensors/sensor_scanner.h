/*
 * Background passive scan for advertisement-only sensors (BTHome v2).
 *
 * The scanner runs continuously with a low duty cycle so Thread keeps enough
 * radio time. It pauses while the bridge's GATT code scans for devices or
 * creates a connection, because the Bluetooth host allows only one explicit
 * scanner and no connection setup while it runs.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>

#include "bthome.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sensor_seen {
	bt_addr_le_t addr;
	int8_t rssi;
	struct bthome_reading last; /* merged: fields from all packets received so far */
	int64_t last_seen_ms; /* k_uptime_get() of the last BTHome packet */
	uint32_t packets; /* BTHome packets received (including repeats) */
	uint32_t measurements; /* distinct packet IDs received */
	uint32_t max_gap_ms; /* longest time between two BTHome packets since boot */
};

/* Called for each new measurement (new packet ID), from the Bluetooth RX thread. Keep it short. */
typedef void (*sensor_scanner_listener_t)(const bt_addr_le_t *addr, int8_t rssi,
					  const struct bthome_reading *reading);

/* Registers the scan callback and starts background scanning. Bluetooth must be enabled. */
int sensor_scanner_init(void);

/* Sets the single listener for new measurements (NULL to remove). */
void sensor_scanner_set_listener(sensor_scanner_listener_t listener);

/* Stops background scanning for at least @p ms milliseconds (extends an active pause). */
void sensor_scanner_pause(uint32_t ms);

/* Ends a pause early and restarts background scanning. */
void sensor_scanner_resume(void);

/* Copies up to @p max entries of the "devices heard" table. Returns the number copied. */
size_t sensor_scanner_get_seen(struct sensor_seen *out, size_t max);

/* Returns true while the background scan itself is running. */
bool sensor_scanner_is_scanning(void);

#ifdef __cplusplus
}
#endif
