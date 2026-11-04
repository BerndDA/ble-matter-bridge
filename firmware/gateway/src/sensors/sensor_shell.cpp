/*
 * Shell commands for BTHome sensors:
 *   gateway seen                       BTHome sensors heard by the background scan
 *   gateway sensors                    sensors bridged to Matter
 *   gateway remove <addr> [type]       remove a bridged sensor and ignore it from now on
 *   gateway unignore <addr> [type]     allow an ignored sensor to be auto-added again
 *   gateway ignored                    list ignored addresses
 *   gateway pause <seconds>            pause the background scan (0 resumes), for measurements
 *
 * <addr> is written as printed by `gateway seen`, e.g. A4:C1:38:0C:54:AA.
 * [type] is "public" (default) or "random".
 */

#include <stdlib.h>

#include <platform/CHIPDeviceLayer.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "bthome_sensor_manager.h"
#include "sensor_scanner.h"

using Gateway::BTHomeSensorManager;

namespace
{

void FormatCenti(char *buf, size_t len, int32_t centi)
{
	const char *sign = centi < 0 ? "-" : "";
	const int32_t a = abs(centi);

	snprintk(buf, len, "%s%d.%02d", sign, a / 100, a % 100);
}

bool ParseAddress(const struct shell *sh, size_t argc, char **argv, bt_addr_le_t &addr)
{
	const char *type = argc > 2 ? argv[2] : "public";

	if (bt_addr_le_from_str(argv[1], type, &addr) != 0) {
		shell_error(sh, "Invalid address \"%s\" (type \"%s\")", argv[1], type);
		return false;
	}
	return true;
}

/* Flag value as "0"/"1", or "-" when not received yet. */
const char *Flag(const bthome_reading &r, uint32_t field, bool value)
{
	return (r.fields & field) ? (value ? "1" : "0") : "-";
}

/* Second line for an eQ-3 thermostat (eq3-custom-fw); the Temp C column above is the room temperature. */
void PrintThermostat(const struct shell *sh, const bthome_reading &r)
{
	static const char *const kModes[] = { "auto", "manual", "holiday" };
	char target[12] = "-";
	char valve[6] = "-";
	char mode[8] = "-";

	if (r.fields & BTHOME_HAS_TEMPERATURE2) {
		FormatCenti(target, sizeof(target), r.temperature2_centi_c);
	}
	if (r.fields & BTHOME_HAS_VALVE) {
		snprintk(valve, sizeof(valve), "%u", r.valve_pct);
	}
	if (r.fields & BTHOME_HAS_MODE) {
		if (r.mode < ARRAY_SIZE(kModes)) {
			snprintk(mode, sizeof(mode), "%s", kModes[r.mode]);
		} else {
			snprintk(mode, sizeof(mode), "%u", r.mode);
		}
	}

	shell_print(sh, "  thermostat: target %s C, valve %s %%, mode %s, boost %s, window %s, locked %s, "
			"battery low %s, connected %s",
		    target, valve, mode, Flag(r, BTHOME_HAS_BOOST, r.boost),
		    Flag(r, BTHOME_HAS_WINDOW_OPEN, r.window_open), Flag(r, BTHOME_HAS_UNLOCKED, !r.unlocked),
		    Flag(r, BTHOME_HAS_BATTERY_LOW, r.battery_low), Flag(r, BTHOME_HAS_CONNECTED, r.connected));
}

int CmdSeen(const struct shell *sh, size_t, char **)
{
	static sensor_seen list[CONFIG_GATEWAY_SENSOR_SEEN_MAX];
	const size_t n = sensor_scanner_get_seen(list, ARRAY_SIZE(list));
	const int64_t now = k_uptime_get();

	shell_print(sh, "Background scan: %s", sensor_scanner_is_scanning() ? "running" : "paused/idle");
	shell_print(sh, "%-30s %5s %6s %8s %7s %5s %6s %6s %7s %7s", "Address", "RSSI", "Age s", "Temp C", "Hum %",
		    "Bat%", "mV", "Meas", "Pkts", "MaxGap");

	for (size_t i = 0; i < n; i++) {
		const sensor_seen &s = list[i];
		char addr[BT_ADDR_LE_STR_LEN];
		char temp[12] = "-";
		char hum[12] = "-";
		char bat[6] = "-";
		char mv[8] = "-";

		bt_addr_le_to_str(&s.addr, addr, sizeof(addr));
		if (s.last.fields & BTHOME_HAS_TEMPERATURE) {
			FormatCenti(temp, sizeof(temp), s.last.temperature_centi_c);
		}
		if (s.last.fields & BTHOME_HAS_HUMIDITY) {
			FormatCenti(hum, sizeof(hum), s.last.humidity_centi_pct);
		}
		if (s.last.fields & BTHOME_HAS_BATTERY) {
			snprintk(bat, sizeof(bat), "%u", s.last.battery_pct);
		}
		if (s.last.fields & BTHOME_HAS_VOLTAGE) {
			snprintk(mv, sizeof(mv), "%u", s.last.voltage_mv);
		}

		shell_print(sh, "%-30s %5d %6lld %8s %7s %5s %6s %6u %7u %6us", addr, s.rssi,
			    (now - s.last_seen_ms) / 1000, temp, hum, bat, mv, s.measurements, s.packets,
			    s.max_gap_ms / 1000);
		if (s.last.fields & BTHOME_THERMOSTAT_FIELDS) {
			PrintThermostat(sh, s.last);
		}
	}
	shell_print(sh, "Total: %u sensor(s)", unsigned(n));
	return 0;
}

int CmdSensors(const struct shell *sh, size_t, char **)
{
	static BTHomeSensorManager::SensorInfo list[BTHomeSensorManager::kMaxSensors];
	size_t n;
	{
		chip::DeviceLayer::StackLock lock;
		n = BTHomeSensorManager::Instance().GetSensors(list, ARRAY_SIZE(list));
	}
	const int64_t now = k_uptime_get();

	shell_print(sh, "Auto-add: %s", IS_ENABLED(CONFIG_GATEWAY_BTHOME_AUTO_ADD) ? "on" : "off");
	shell_print(sh, "%-30s %9s %9s %6s %8s %7s %5s", "Address", "Endpoints", "Reachable", "Age s", "Temp C",
		    "Hum %", "Bat%");

	for (size_t i = 0; i < n; i++) {
		const auto &s = list[i];
		char addr[BT_ADDR_LE_STR_LEN];
		char endpoints[12];
		char age[12] = "-";
		char temp[12] = "-";
		char hum[12] = "-";

		bt_addr_le_to_str(&s.mAddr, addr, sizeof(addr));
		if (s.mThermostatEndpoint != 0) {
			snprintk(endpoints, sizeof(endpoints), "%u (T)", s.mThermostatEndpoint);
		} else {
			snprintk(endpoints, sizeof(endpoints), "%u,%u", s.mTemperatureEndpoint, s.mHumidityEndpoint);
		}
		if (s.mLastSeenMs > 0) {
			snprintk(age, sizeof(age), "%lld", (now - s.mLastSeenMs) / 1000);
		}
		if (s.mHasTemperature) {
			FormatCenti(temp, sizeof(temp), s.mTemperature);
		}
		if (s.mHasHumidity) {
			FormatCenti(hum, sizeof(hum), s.mHumidity);
		}

		shell_print(sh, "%-30s %9s %9s %6s %8s %7s %5u", addr, endpoints, s.mReachable ? "yes" : "no", age,
			    temp, hum, s.mBattery);
	}
	shell_print(sh, "Total: %u of max %u sensor(s)", unsigned(n), unsigned(BTHomeSensorManager::kMaxSensors));
	return 0;
}

int CmdRemove(const struct shell *sh, size_t argc, char **argv)
{
	bt_addr_le_t addr;

	if (!ParseAddress(sh, argc, argv, addr)) {
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = BTHomeSensorManager::Instance().Remove(addr);
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Remove failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	shell_print(sh, "Removed and ignored %s", argv[1]);
	return 0;
}

int CmdUnignore(const struct shell *sh, size_t argc, char **argv)
{
	bt_addr_le_t addr;

	if (!ParseAddress(sh, argc, argv, addr)) {
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = BTHomeSensorManager::Instance().Unignore(addr);
	}
	if (err == CHIP_ERROR_NOT_FOUND) {
		shell_error(sh, "%s is not on the ignore list", argv[1]);
		return -ENOENT;
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Unignore failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	shell_print(sh, "%s can be auto-added again", argv[1]);
	return 0;
}

int CmdIgnored(const struct shell *sh, size_t, char **)
{
	bt_addr_le_t list[BTHomeSensorManager::kMaxIgnored];
	size_t n;
	{
		chip::DeviceLayer::StackLock lock;
		n = BTHomeSensorManager::Instance().GetIgnored(list, ARRAY_SIZE(list));
	}

	for (size_t i = 0; i < n; i++) {
		char addr[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&list[i], addr, sizeof(addr));
		shell_print(sh, "%s", addr);
	}
	shell_print(sh, "Total: %u of max %u ignored", unsigned(n), unsigned(BTHomeSensorManager::kMaxIgnored));
	return 0;
}

int CmdPause(const struct shell *sh, size_t, char **argv)
{
	const long seconds = strtol(argv[1], nullptr, 10);

	if (seconds <= 0) {
		sensor_scanner_resume();
		shell_print(sh, "Background scan resumed");
	} else {
		sensor_scanner_pause(uint32_t(seconds) * 1000);
		shell_print(sh, "Background scan paused for %ld s", seconds);
	}
	return 0;
}

} /* namespace */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_gateway, SHELL_CMD_ARG(seen, NULL, "List BTHome sensors heard by the background scan.", CmdSeen, 1, 0),
	SHELL_CMD_ARG(sensors, NULL, "List BTHome sensors bridged to Matter.", CmdSensors, 1, 0),
	SHELL_CMD_ARG(remove, NULL,
		      "Remove a bridged sensor and ignore it from now on.\n"
		      "Usage: remove <addr> [public|random]",
		      CmdRemove, 2, 1),
	SHELL_CMD_ARG(unignore, NULL,
		      "Allow an ignored sensor to be auto-added again.\n"
		      "Usage: unignore <addr> [public|random]",
		      CmdUnignore, 2, 1),
	SHELL_CMD_ARG(ignored, NULL, "List ignored sensor addresses.", CmdIgnored, 1, 0),
	SHELL_CMD_ARG(pause, NULL,
		      "Pause the background scan (for measurements); 0 resumes.\n"
		      "Usage: pause <seconds>",
		      CmdPause, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gateway, &sub_gateway, "BLE gateway commands", NULL);
