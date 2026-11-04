/*
 * Shell commands for the sensor scanner:
 *   gateway seen    list BTHome sensors heard by the background scan
 */

#include <stdlib.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "sensor_scanner.h"

static void format_centi(char *buf, size_t len, int32_t centi)
{
	const char *sign = centi < 0 ? "-" : "";
	const int32_t a = abs(centi);

	snprintk(buf, len, "%s%d.%02d", sign, a / 100, a % 100);
}

static int cmd_seen(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static struct sensor_seen list[CONFIG_GATEWAY_SENSOR_SEEN_MAX];
	const size_t n = sensor_scanner_get_seen(list, ARRAY_SIZE(list));
	const int64_t now = k_uptime_get();

	shell_print(sh, "Background scan: %s", sensor_scanner_is_scanning() ? "running" : "paused/idle");
	shell_print(sh, "%-30s %5s %6s %8s %7s %5s %6s %6s", "Address", "RSSI", "Age s", "Temp C",
		    "Hum %", "Bat%", "mV", "Meas");

	for (size_t i = 0; i < n; i++) {
		const struct sensor_seen *s = &list[i];
		char addr[BT_ADDR_LE_STR_LEN];
		char temp[12] = "-";
		char hum[12] = "-";
		char bat[6] = "-";
		char mv[8] = "-";

		bt_addr_le_to_str(&s->addr, addr, sizeof(addr));
		if (s->last.fields & BTHOME_HAS_TEMPERATURE) {
			format_centi(temp, sizeof(temp), s->last.temperature_centi_c);
		}
		if (s->last.fields & BTHOME_HAS_HUMIDITY) {
			format_centi(hum, sizeof(hum), s->last.humidity_centi_pct);
		}
		if (s->last.fields & BTHOME_HAS_BATTERY) {
			snprintk(bat, sizeof(bat), "%u", s->last.battery_pct);
		}
		if (s->last.fields & BTHOME_HAS_VOLTAGE) {
			snprintk(mv, sizeof(mv), "%u", s->last.voltage_mv);
		}

		shell_print(sh, "%-30s %5d %6lld %8s %7s %5s %6s %6u", addr, s->rssi,
			    (now - s->last_seen_ms) / 1000, temp, hum, bat, mv, s->measurements);
	}
	shell_print(sh, "Total: %u sensor(s)", (unsigned int)n);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_gateway,
			       SHELL_CMD_ARG(seen, NULL, "List BTHome sensors heard by the background scan.",
					     cmd_seen, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gateway, &sub_gateway, "BLE gateway commands", NULL);
