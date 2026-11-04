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
};

struct bthome_reading {
	uint32_t fields; /* Bitmask of enum bthome_field: which values below are valid. */
	bool trigger_based; /* Device sends on change instead of at a regular interval. */
	bool truncated; /* Parsing stopped at an unknown or malformed object (values before it are valid). */
	uint8_t packet_id; /* Increments per new measurement; repeated advertisements share it. */
	uint8_t battery_pct; /* 0..100 % */
	int16_t temperature_centi_c; /* 0.01 degC, same unit as Matter TemperatureMeasurement */
	uint16_t humidity_centi_pct; /* 0.01 %, same unit as Matter RelativeHumidityMeasurement */
	uint16_t voltage_mv; /* battery voltage */
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

#ifdef __cplusplus
}
#endif
