/*
 * Status JSON of the BLE gateway for the web page (src/web/index.html). See web_status.h.
 */

#include "web_status.h"

#include "bridge_manager.h"
#include "bthome_sensor_manager.h"
#include "sensor_scanner.h"
#include "web/status_json.h"

#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>

#include <stdio.h>
#include <string.h>

using Gateway::BTHomeSensorManager;
using Gateway::Web::Json;

namespace
{

/* "A4:C1:38:0C:54:AA" and "public"/"random" from Zephyr's "A4:C1:38:0C:54:AA (public)". */
void WriteAddress(Json &j, const bt_addr_le_t &addr)
{
	char text[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&addr, text, sizeof(text));
	j.Str("address", text, 17);
	j.Str("addressType", addr.type == BT_ADDR_LE_PUBLIC ? "public" : "random");
}

void WriteSensors(Json &j)
{
	static BTHomeSensorManager::SensorInfo sensors[BTHomeSensorManager::kMaxSensors];
	const size_t n = BTHomeSensorManager::Instance().GetSensors(sensors, ARRAY_SIZE(sensors));
	const int64_t now = k_uptime_get();

	j.Open("sensors", '[');
	for (size_t i = 0; i < n; i++) {
		const auto &s = sensors[i];
		const uint16_t endpoint = s.mThermostatEndpoint ? s.mThermostatEndpoint : s.mTemperatureEndpoint;
		const char *label = Nrf::BridgeManager::Instance().GetNodeLabel(endpoint);

		j.Open(nullptr, '{');
		WriteAddress(j, s.mAddr);
		j.Str("name", label ? label : "");
		j.Bool("thermostat", s.mThermostatEndpoint != 0);
		j.Int("endpoint", endpoint);
		if (!s.mThermostatEndpoint) {
			j.Int("humidityEndpoint", s.mHumidityEndpoint);
		}
		j.Bool("reachable", s.mReachable);
		if (s.mLastSeenMs > 0) {
			j.Int("lastSeenS", (now - s.mLastSeenMs) / 1000);
		} else {
			j.Null("lastSeenS");
		}
		if (s.mHasTemperature) {
			j.Int("temperature", s.mTemperature);
		} else {
			j.Null("temperature");
		}
		if (s.mHasHumidity) {
			j.Int("humidity", s.mHumidity);
		} else {
			j.Null("humidity");
		}
		j.Int("battery", s.mBattery);
		j.Close('}');
	}
	j.Close(']');
}

/* A reading's value, or null if the sensor hasn't sent it. */
void Field(Json &j, const bthome_reading &r, uint32_t field, const char *key, long long value)
{
	if (r.fields & field) {
		j.Int(key, value);
	} else {
		j.Null(key);
	}
}

void FlagField(Json &j, const bthome_reading &r, uint32_t field, const char *key, bool value)
{
	if (r.fields & field) {
		j.Bool(key, value);
	} else {
		j.Null(key);
	}
}

void WriteSeen(Json &j)
{
	static sensor_seen seen[CONFIG_GATEWAY_SENSOR_SEEN_MAX];
	const size_t n = sensor_scanner_get_seen(seen, ARRAY_SIZE(seen));
	const int64_t now = k_uptime_get();

	j.Open("scan", '{');
	j.Bool("running", sensor_scanner_is_scanning());
	j.Bool("active", sensor_scanner_is_active());
	j.Close('}');

	j.Open("seen", '[');
	for (size_t i = 0; i < n; i++) {
		const sensor_seen &s = seen[i];
		const bthome_reading &r = s.last;

		j.Open(nullptr, '{');
		WriteAddress(j, s.addr);
		j.Str("name", s.name);
		j.Int("rssi", s.rssi);
		j.Int("agoS", (now - s.last_seen_ms) / 1000);
		j.Int("packets", s.packets);
		j.Int("measurements", s.measurements);
		j.Int("maxGapS", s.max_gap_ms / 1000);
		Field(j, r, BTHOME_HAS_TEMPERATURE, "temperature", r.temperature_centi_c);
		Field(j, r, BTHOME_HAS_HUMIDITY, "humidity", r.humidity_centi_pct);
		Field(j, r, BTHOME_HAS_BATTERY, "battery", r.battery_pct);
		Field(j, r, BTHOME_HAS_VOLTAGE, "voltage", r.voltage_mv);
		if (r.fields & BTHOME_THERMOSTAT_FIELDS) {
			/* eQ-3 with eq3-custom-fw: the temperature above is the room temperature. */
			j.Open("thermostat", '{');
			Field(j, r, BTHOME_HAS_TEMPERATURE2, "target", r.temperature2_centi_c);
			Field(j, r, BTHOME_HAS_VALVE, "valve", r.valve_pct);
			Field(j, r, BTHOME_HAS_MODE, "mode", r.mode);
			FlagField(j, r, BTHOME_HAS_BOOST, "boost", r.boost);
			FlagField(j, r, BTHOME_HAS_WINDOW_OPEN, "windowOpen", r.window_open);
			FlagField(j, r, BTHOME_HAS_UNLOCKED, "locked", !r.unlocked);
			FlagField(j, r, BTHOME_HAS_BATTERY_LOW, "batteryLow", r.battery_low);
			FlagField(j, r, BTHOME_HAS_CONNECTED, "connected", r.connected);
			j.Close('}');
		}
		j.Close('}');
	}
	j.Close(']');
}

} /* namespace */

namespace Gateway
{

size_t WriteWebStatus(char *buf, size_t size)
{
	Json j(buf, size);

	j.Open(nullptr, '{');
	Gateway::Web::WriteCommonStatus(j);
	WriteSensors(j);
	WriteSeen(j);
	j.Close('}');
	return j.Done();
}

} /* namespace Gateway */
