/*
 * BTHome v2 advertisement parser (https://bthome.io/format/).
 */

#include "bthome.h"

#include <string.h>

#define DEVICE_INFO_ENCRYPTED 0x01
#define DEVICE_INFO_TRIGGER_BASED 0x04
#define DEVICE_INFO_VERSION_SHIFT 5
#define BTHOME_VERSION 2

#define OBJ_PACKET_ID 0x00
#define OBJ_BATTERY 0x01
#define OBJ_TEMPERATURE 0x02
#define OBJ_HUMIDITY 0x03
#define OBJ_VOLTAGE 0x0C
#define OBJ_COUNT 0x09
#define OBJ_GENERIC_BOOLEAN 0x0F
#define OBJ_BATTERY_LOW 0x15
#define OBJ_CONNECTIVITY 0x19
#define OBJ_LOCK 0x1F
#define OBJ_WINDOW 0x2D
#define OBJ_MOISTURE_8 0x2F
#define OBJ_TEXT 0x53
#define OBJ_RAW 0x54

#define LEN_VARIABLE 0xFF
#define LEN_UNKNOWN 0

/*
 * Payload length of each object ID. BTHome objects carry no length field, so an
 * unknown ID means the rest of the packet cannot be parsed. Objects are sent in
 * ascending ID order, so the values this gateway needs (IDs 0x00-0x0C) come first.
 */
static uint8_t object_length(uint8_t id)
{
	switch (id) {
	/* uint8 / sint8 / binary sensors and events */
	case 0x00: case 0x01: case 0x09: case 0x0F: case 0x10: case 0x11:
	case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A:
	case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
	case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
	case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C:
	case 0x2D: case 0x2E: case 0x2F: case 0x3A: case 0x46:
		return 1;
	/* 16-bit values */
	case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C:
	case 0x0D: case 0x0E: case 0x12: case 0x13: case 0x14: case 0x3C:
	case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x43: case 0x44:
	case 0x45: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x51:
	case 0x52:
		return 2;
	/* 24-bit values */
	case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42: case 0x4B:
		return 3;
	/* 32-bit values */
	case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50:
		return 4;
	case OBJ_TEXT: case OBJ_RAW:
		return LEN_VARIABLE;
	default:
		return LEN_UNKNOWN;
	}
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

int bthome_parse_service_data(const uint8_t *data, size_t len, struct bthome_reading *out)
{
	memset(out, 0, sizeof(*out));

	/* UUID (2) + device information (1) */
	if (data == NULL || len < 3 || get_le16(data) != BTHOME_SERVICE_UUID) {
		return BTHOME_ERR_INVALID;
	}

	const uint8_t info = data[2];

	if ((info >> DEVICE_INFO_VERSION_SHIFT) != BTHOME_VERSION) {
		return BTHOME_ERR_INVALID;
	}
	if (info & DEVICE_INFO_ENCRYPTED) {
		return BTHOME_ERR_ENCRYPTED;
	}
	out->trigger_based = (info & DEVICE_INFO_TRIGGER_BASED) != 0;

	size_t pos = 3;

	while (pos < len) {
		const uint8_t id = data[pos++];
		size_t obj_len = object_length(id);

		if (obj_len == LEN_VARIABLE) {
			if (pos >= len) {
				out->truncated = true;
				break;
			}
			obj_len = 1 + data[pos]; /* length byte + content */
		}
		if (obj_len == LEN_UNKNOWN || pos + obj_len > len) {
			out->truncated = true;
			break;
		}

		const uint8_t *v = &data[pos];

		switch (id) {
		case OBJ_PACKET_ID:
			out->packet_id = v[0];
			out->fields |= BTHOME_HAS_PACKET_ID;
			break;
		case OBJ_BATTERY:
			out->battery_pct = v[0] > 100 ? 100 : v[0];
			out->fields |= BTHOME_HAS_BATTERY;
			break;
		case OBJ_TEMPERATURE:
			/* eQ-3 sends room temperature, then target temperature, both as 0x02. */
			if (out->fields & BTHOME_HAS_TEMPERATURE) {
				out->temperature2_centi_c = (int16_t)get_le16(v);
				out->fields |= BTHOME_HAS_TEMPERATURE2;
			} else {
				out->temperature_centi_c = (int16_t)get_le16(v);
				out->fields |= BTHOME_HAS_TEMPERATURE;
			}
			break;
		case OBJ_HUMIDITY:
			out->humidity_centi_pct = get_le16(v);
			out->fields |= BTHOME_HAS_HUMIDITY;
			break;
		case OBJ_VOLTAGE:
			out->voltage_mv = get_le16(v);
			out->fields |= BTHOME_HAS_VOLTAGE;
			break;
		case OBJ_COUNT:
			out->mode = v[0];
			out->fields |= BTHOME_HAS_MODE;
			break;
		case OBJ_MOISTURE_8:
			out->valve_pct = v[0] > 100 ? 100 : v[0];
			out->fields |= BTHOME_HAS_VALVE;
			break;
		case OBJ_GENERIC_BOOLEAN:
			out->boost = v[0] != 0;
			out->fields |= BTHOME_HAS_BOOST;
			break;
		case OBJ_BATTERY_LOW:
			out->battery_low = v[0] != 0;
			out->fields |= BTHOME_HAS_BATTERY_LOW;
			break;
		case OBJ_CONNECTIVITY:
			out->connected = v[0] != 0;
			out->fields |= BTHOME_HAS_CONNECTED;
			break;
		case OBJ_LOCK:
			out->unlocked = v[0] != 0;
			out->fields |= BTHOME_HAS_UNLOCKED;
			break;
		case OBJ_WINDOW:
			out->window_open = v[0] != 0;
			out->fields |= BTHOME_HAS_WINDOW_OPEN;
			break;
		default:
			break;
		}
		pos += obj_len;
	}

	return 0;
}

int bthome_parse_adv(const uint8_t *adv, size_t len, struct bthome_reading *out)
{
	size_t pos = 0;

	memset(out, 0, sizeof(*out));
	if (adv == NULL) {
		return BTHOME_ERR_INVALID;
	}

	while (pos < len) {
		const uint8_t field_len = adv[pos];

		if (field_len == 0) {
			break; /* early termination of the advertising data */
		}
		if (pos + 1 + field_len > len) {
			return BTHOME_ERR_INVALID; /* malformed AD structure */
		}

		const uint8_t type = adv[pos + 1];
		const uint8_t *value = &adv[pos + 2];
		const size_t value_len = field_len - 1;

		if (type == BTHOME_AD_TYPE_SERVICE_DATA_UUID16 && value_len >= 2 &&
		    get_le16(value) == BTHOME_SERVICE_UUID) {
			return bthome_parse_service_data(value, value_len, out);
		}
		pos += 1 + field_len;
	}

	return BTHOME_ERR_INVALID;
}
