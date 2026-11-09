/*
 * Background scan for advertisement-only sensors (BTHome v2). Passive, except while
 * a newly heard thermostat's name is wanted (sensor_scanner_set_active).
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

/* Longest advertised name kept, plus the terminating NUL (a name fills at most 29 bytes of an AD structure). */
#define SENSOR_NAME_SIZE 30

struct sensor_seen {
	bt_addr_le_t addr;
	char name[SENSOR_NAME_SIZE]; /* advertised name, empty until heard (usually only in scan responses) */
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

/*
 * Switches between passive and active scanning. Active scanning also requests scan responses,
 * which is where eQ-3 thermostats send their name; it costs a little more radio time, so keep it
 * on only while waiting for a name.
 */
void sensor_scanner_set_active(bool active);

/* Copies the advertised name of @p addr into @p out. Returns false if none has been heard yet. */
bool sensor_scanner_get_name(const bt_addr_le_t *addr, char *out, size_t out_size);

/* Returns true while the background scan itself is running. */
bool sensor_scanner_is_scanning(void);

/* Returns true if the running background scan is an active one. */
bool sensor_scanner_is_active(void);

#ifdef __cplusplus
}
#endif
