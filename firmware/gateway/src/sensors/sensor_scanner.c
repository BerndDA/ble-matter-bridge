/*
 * Background passive scan for advertisement-only sensors (BTHome v2).
 */

#include "sensor_scanner.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

LOG_MODULE_REGISTER(sensor_scanner, CONFIG_GATEWAY_SENSOR_SCANNER_LOG_LEVEL);

#define RETRY_INTERVAL K_MSEC(CONFIG_GATEWAY_SENSOR_SCAN_RETRY_MS)

/* Protects scan state (scanning, pause_until). Taken in thread context only. */
static K_MUTEX_DEFINE(scan_mutex);
static bool scanning;
static int64_t pause_until;
static struct k_work_delayable scan_work;

/* Protects the seen table; also taken in the Bluetooth RX thread. */
static struct k_spinlock seen_lock;
static struct sensor_seen seen[CONFIG_GATEWAY_SENSOR_SEEN_MAX];
static size_t seen_count;

static sensor_scanner_listener_t listener;

static void merge_reading(struct bthome_reading *dst, const struct bthome_reading *src)
{
	if (src->fields & BTHOME_HAS_PACKET_ID) {
		dst->packet_id = src->packet_id;
	}
	if (src->fields & BTHOME_HAS_BATTERY) {
		dst->battery_pct = src->battery_pct;
	}
	if (src->fields & BTHOME_HAS_TEMPERATURE) {
		dst->temperature_centi_c = src->temperature_centi_c;
	}
	if (src->fields & BTHOME_HAS_HUMIDITY) {
		dst->humidity_centi_pct = src->humidity_centi_pct;
	}
	if (src->fields & BTHOME_HAS_VOLTAGE) {
		dst->voltage_mv = src->voltage_mv;
	}
	if (src->fields & BTHOME_HAS_TEMPERATURE2) {
		dst->temperature2_centi_c = src->temperature2_centi_c;
	}
	if (src->fields & BTHOME_HAS_VALVE) {
		dst->valve_pct = src->valve_pct;
	}
	if (src->fields & BTHOME_HAS_MODE) {
		dst->mode = src->mode;
	}
	if (src->fields & BTHOME_HAS_BOOST) {
		dst->boost = src->boost;
	}
	if (src->fields & BTHOME_HAS_BATTERY_LOW) {
		dst->battery_low = src->battery_low;
	}
	if (src->fields & BTHOME_HAS_CONNECTED) {
		dst->connected = src->connected;
	}
	if (src->fields & BTHOME_HAS_UNLOCKED) {
		dst->unlocked = src->unlocked;
	}
	if (src->fields & BTHOME_HAS_WINDOW_OPEN) {
		dst->window_open = src->window_open;
	}
	dst->fields |= src->fields;
	dst->trigger_based = src->trigger_based;
	dst->truncated = src->truncated;
}

/* Returns the table entry for @p addr, creating it (or reusing the oldest entry) if needed. */
static struct sensor_seen *seen_entry(const bt_addr_le_t *addr)
{
	struct sensor_seen *oldest = NULL;

	for (size_t i = 0; i < seen_count; i++) {
		if (bt_addr_le_eq(&seen[i].addr, addr)) {
			return &seen[i];
		}
		if (oldest == NULL || seen[i].last_seen_ms < oldest->last_seen_ms) {
			oldest = &seen[i];
		}
	}

	struct sensor_seen *e = seen_count < ARRAY_SIZE(seen) ? &seen[seen_count++] : oldest;

	memset(e, 0, sizeof(*e));
	bt_addr_le_copy(&e->addr, addr);
	return e;
}

static void on_scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	struct bthome_reading r;

	if (bthome_parse_adv(buf->data, buf->len, &r) != 0) {
		return;
	}

	bool new_measurement;
	k_spinlock_key_t key = k_spin_lock(&seen_lock);
	struct sensor_seen *e = seen_entry(info->addr);

	/* Sensors repeat each measurement in several advertisements with the same packet ID. */
	new_measurement = !(r.fields & BTHOME_HAS_PACKET_ID) || e->measurements == 0 ||
			  !(e->last.fields & BTHOME_HAS_PACKET_ID) || e->last.packet_id != r.packet_id;
	const int64_t now = k_uptime_get();

	if (e->packets > 0 && now - e->last_seen_ms > e->max_gap_ms) {
		e->max_gap_ms = (uint32_t)(now - e->last_seen_ms);
	}
	merge_reading(&e->last, &r);
	e->rssi = info->rssi;
	e->last_seen_ms = now;
	e->packets++;
	if (new_measurement) {
		e->measurements++;
	}
	sensor_scanner_listener_t l = listener;
	k_spin_unlock(&seen_lock, key);

	if (new_measurement && l != NULL) {
		l(info->addr, info->rssi, &r);
	}
}

static struct bt_le_scan_cb scan_cb = {
	.recv = on_scan_recv,
};

static void scan_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&scan_mutex, K_FOREVER);

	const int64_t now = k_uptime_get();

	if (now < pause_until) {
		k_work_reschedule(&scan_work, K_MSEC(pause_until - now));
		k_mutex_unlock(&scan_mutex);
		return;
	}

	if (!scanning) {
		const struct bt_le_scan_param param = {
			.type = BT_LE_SCAN_TYPE_PASSIVE,
			/* No duplicate filter: every advertisement carries a new reading. */
			.options = BT_LE_SCAN_OPT_NONE,
			.interval = CONFIG_GATEWAY_SENSOR_SCAN_INTERVAL,
			.window = CONFIG_GATEWAY_SENSOR_SCAN_WINDOW,
		};
		const int err = bt_le_scan_start(&param, NULL);

		if (err == 0) {
			scanning = true;
			LOG_INF("Background scan started");
		} else if (err == -EALREADY) {
			/* Another scan (e.g. the bridge's GATT discovery) is running; its reports reach us too. */
			LOG_DBG("Another scan is active, retrying later");
		} else {
			LOG_WRN("Background scan failed to start (err %d)", err);
		}
	}

	/* Keep checking: another user may stop our scan or occupy the scanner. */
	k_work_reschedule(&scan_work, RETRY_INTERVAL);
	k_mutex_unlock(&scan_mutex);
}

int sensor_scanner_init(void)
{
	k_work_init_delayable(&scan_work, scan_work_handler);

	const int err = bt_le_scan_cb_register(&scan_cb);

	if (err != 0 && err != -EEXIST) {
		LOG_ERR("Failed to register scan callback (err %d)", err);
		return err;
	}

	k_work_reschedule(&scan_work, K_NO_WAIT);
	return 0;
}

void sensor_scanner_set_listener(sensor_scanner_listener_t l)
{
	k_spinlock_key_t key = k_spin_lock(&seen_lock);

	listener = l;
	k_spin_unlock(&seen_lock, key);
}

void sensor_scanner_pause(uint32_t ms)
{
	k_mutex_lock(&scan_mutex, K_FOREVER);

	const int64_t until = k_uptime_get() + ms;

	if (until > pause_until) {
		pause_until = until;
	}
	if (scanning) {
		const int err = bt_le_scan_stop();

		if (err != 0 && err != -EALREADY) {
			LOG_WRN("Background scan failed to stop (err %d)", err);
		}
		scanning = false;
		LOG_DBG("Background scan paused for %u ms", ms);
	}
	k_work_reschedule(&scan_work, K_MSEC(pause_until - k_uptime_get()));

	k_mutex_unlock(&scan_mutex);
}

void sensor_scanner_resume(void)
{
	k_mutex_lock(&scan_mutex, K_FOREVER);
	pause_until = 0;
	k_work_reschedule(&scan_work, K_NO_WAIT);
	k_mutex_unlock(&scan_mutex);
}

size_t sensor_scanner_get_seen(struct sensor_seen *out, size_t max)
{
	k_spinlock_key_t key = k_spin_lock(&seen_lock);
	const size_t n = MIN(max, seen_count);

	memcpy(out, seen, n * sizeof(*out));
	k_spin_unlock(&seen_lock, key);
	return n;
}

bool sensor_scanner_is_scanning(void)
{
	return scanning;
}
