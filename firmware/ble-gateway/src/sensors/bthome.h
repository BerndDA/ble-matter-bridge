/*
 * BTHome v2 advertisement parser (https://bthome.io/format/).
 *
 * Plain C without Zephyr dependencies, so it can be unit-tested on the host
 * (see tests/host/bthome).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16-bit service UUID assigned to BTHome. */
#define BTHOME_SERVICE_UUID 0xFCD2

/* Bluetooth AD type "Service Data - 16-bit UUID". */
#define BTHOME_AD_TYPE_SERVICE_DATA_UUID16 0x16

enum bthome_field {
	BTHOME_HAS_PACKET_ID = 1u << 0,
	BTHOME_HAS_BATTERY = 1u << 1,
	BTHOME_HAS_TEMPERATURE = 1u << 2,
	BTHOME_HAS_HUMIDITY = 1u << 3,
	BTHOME_HAS_VOLTAGE = 1u << 4,
	/* Objects sent by eQ-3 thermostats with eq3-custom-fw (see PROTOCOL.md there). */
	BTHOME_HAS_TEMPERATURE2 = 1u << 5, /* a second 0x02 object; eQ-3: target temperature */
	BTHOME_HAS_VALVE = 1u << 6, /* 0x2F (BTHome: moisture %); eQ-3: valve opening */
	BTHOME_HAS_MODE = 1u << 7, /* 0x09 (BTHome: count); eQ-3: 0 auto, 1 manual, 2 holiday */
	BTHOME_HAS_BOOST = 1u << 8, /* 0x0F (generic boolean) */
	BTHOME_HAS_BATTERY_LOW = 1u << 9, /* 0x15 */
	BTHOME_HAS_CONNECTED = 1u << 10, /* 0x19 connectivity */
	BTHOME_HAS_UNLOCKED = 1u << 11, /* 0x1F lock: 1 means unlocked */
	BTHOME_HAS_WINDOW_OPEN = 1u << 12, /* 0x2D */
};

/* Fields only an eQ-3 thermostat sends; a reading with any of them is not a plain sensor. */
#define BTHOME_THERMOSTAT_FIELDS (BTHOME_HAS_TEMPERATURE2 | BTHOME_HAS_VALVE | BTHOME_HAS_MODE)

struct bthome_reading {
	uint32_t fields; /* Bitmask of enum bthome_field: which values below are valid. */
	bool trigger_based; /* Device sends on change instead of at a regular interval. */
	bool truncated; /* Parsing stopped at an unknown or malformed object (values before it are valid). */
	uint8_t packet_id; /* Increments per new measurement; repeated advertisements share it. */
	uint8_t battery_pct; /* 0..100 % */
	int16_t temperature_centi_c; /* 0.01 degC, same unit as Matter TemperatureMeasurement */
	uint16_t humidity_centi_pct; /* 0.01 %, same unit as Matter RelativeHumidityMeasurement */
	uint16_t voltage_mv; /* battery voltage */
	int16_t temperature2_centi_c; /* second 0x02 object, 0.01 degC */
	uint8_t valve_pct; /* 0..100 % */
	uint8_t mode;
	/* Binary states, valid when the matching BTHOME_HAS_* bit is set. */
	bool boost;
	bool battery_low;
	bool connected;
	bool unlocked;
	bool window_open;
};

/* Error codes (negative return values). */
#define BTHOME_ERR_INVALID (-1) /* not BTHome, too short, or wrong version */
#define BTHOME_ERR_ENCRYPTED (-2) /* encrypted payload, not supported */

/*
 * Parses BTHome service data.
 * @p data starts after the AD type byte, i.e. with the UUID 0xFCD2 in little endian (D2 FC).
 * Returns 0 on success or a BTHOME_ERR_* code.
 */
int bthome_parse_service_data(const uint8_t *data, size_t len, struct bthome_reading *out);

/*
 * Searches raw advertising data (a sequence of length/type/value AD structures)
 * for BTHome service data and parses it.
 * Returns 0 on success, BTHOME_ERR_INVALID if no BTHome data is present, or another BTHOME_ERR_* code.
 */
int bthome_parse_adv(const uint8_t *adv, size_t len, struct bthome_reading *out);

/*
 * Copies the device name from raw advertising or scan response data into @p out (NUL-terminated,
 * truncated to fit). Prefers the complete name over the shortened one.
 * Returns the copied length, or -1 if the data holds no name or is malformed.
 */
int adv_find_name(const uint8_t *adv, size_t len, char *out, size_t out_size);

/*
 * Turns the advertised name of a renamed eQ-3 thermostat ("eQ3-Dusche", see eq3-custom-fw
 * PROTOCOL.md section 2) into a Matter node label ("Dusche"), truncated to fit @p out on a UTF-8
 * character boundary. Returns false for the default name "CC-RT-BLE" or any other name.
 */
bool eq3_label_from_name(const char *name, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
