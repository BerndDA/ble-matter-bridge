/*
 * Host unit tests for the BTHome v2 parser. Build and run with `make` in this folder.
 */

#include "bthome.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(cond)) {                                                                     \
			failures++;                                                                \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
		}                                                                                  \
	} while (0)

#define CHECK_EQ(a, b)                                                                             \
	do {                                                                                       \
		long long _a = (long long)(a), _b = (long long)(b);                                \
		checks++;                                                                          \
		if (_a != _b) {                                                                    \
			failures++;                                                                \
			printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, \
			       _a, _b);                                                            \
		}                                                                                  \
	} while (0)

#define RUN(test)                                                                                  \
	do {                                                                                       \
		printf("%s\n", #test);                                                             \
		test();                                                                            \
	} while (0)

/* Example from https://bthome.io/format/: temperature 25.00 degC, humidity 50.55 %. */
static void test_spec_example(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x02, 0xC4, 0x09, 0x03, 0xBF, 0x13};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.fields, BTHOME_HAS_TEMPERATURE | BTHOME_HAS_HUMIDITY);
	CHECK_EQ(r.temperature_centi_c, 2500);
	CHECK_EQ(r.humidity_centi_pct, 5055);
	CHECK(!r.trigger_based);
	CHECK(!r.truncated);
}

/* Full advertisement as sent by pvvx firmware: flags + BTHome service data. */
static void test_pvvx_adv(void)
{
	const uint8_t adv[] = {
		0x02, 0x01, 0x06, /* flags */
		0x0E, 0x16, 0xD2, 0xFC, 0x40, /* service data, UUID, device info */
		0x00, 0x5A, /* packet id 90 */
		0x01, 0x64, /* battery 100 % */
		0x02, 0x2C, 0x08, /* temperature 20.92 degC */
		0x03, 0x6E, 0x13, /* humidity 49.74 % */
	};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_adv(adv, sizeof(adv), &r), 0);
	CHECK_EQ(r.fields, BTHOME_HAS_PACKET_ID | BTHOME_HAS_BATTERY | BTHOME_HAS_TEMPERATURE |
				   BTHOME_HAS_HUMIDITY);
	CHECK_EQ(r.packet_id, 90);
	CHECK_EQ(r.battery_pct, 100);
	CHECK_EQ(r.temperature_centi_c, 2092);
	CHECK_EQ(r.humidity_centi_pct, 4974);
	CHECK(!r.truncated);
}

/* pvvx alternates with a second packet carrying voltage and binary states. */
static void test_pvvx_voltage_packet(void)
{
	const uint8_t adv[] = {
		0x02, 0x01, 0x06,
		0x0B, 0x16, 0xD2, 0xFC, 0x40,
		0x00, 0x5B, /* packet id */
		0x0C, 0xB8, 0x0B, /* voltage 3000 mV */
		0x10, 0x00, /* power: off */
		0x11, 0x01, /* opening: open */
	};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_adv(adv, sizeof(adv), &r), 0);
	CHECK_EQ(r.fields, BTHOME_HAS_PACKET_ID | BTHOME_HAS_VOLTAGE);
	CHECK_EQ(r.voltage_mv, 3000);
	CHECK(!r.truncated);
}

static void test_negative_temperature(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x02, 0x38, 0xFF}; /* -2.00 degC */
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.temperature_centi_c, -200);
}

static void test_battery_clamped(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x01, 0x96};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.battery_pct, 100);
}

static void test_encrypted_rejected(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x41, 0x02, 0xC4, 0x09};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), BTHOME_ERR_ENCRYPTED);
}

static void test_wrong_version_rejected(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x20, 0x02, 0xC4, 0x09};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), BTHOME_ERR_INVALID);
}

static void test_wrong_uuid_rejected(void)
{
	const uint8_t sd[] = {0x1A, 0x18, 0x40, 0x02, 0xC4, 0x09};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), BTHOME_ERR_INVALID);
}

static void test_too_short_rejected(void)
{
	const uint8_t sd[] = {0xD2, 0xFC};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), BTHOME_ERR_INVALID);
	CHECK_EQ(bthome_parse_service_data(NULL, 0, &r), BTHOME_ERR_INVALID);
}

static void test_trigger_based_flag(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x44, 0x00, 0x01};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK(r.trigger_based);
}

/* Values before an unknown object ID are kept; parsing stops there. */
static void test_unknown_object_keeps_earlier_values(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x02, 0xC4, 0x09, 0xEE, 0x01, 0x03, 0xBF, 0x13};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.fields, BTHOME_HAS_TEMPERATURE);
	CHECK_EQ(r.temperature_centi_c, 2500);
	CHECK(r.truncated);
}

/* An object cut off at the end of the packet is not used. */
static void test_truncated_object_ignored(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x03, 0xBF, 0x13, 0x02, 0xC4};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.fields, BTHOME_HAS_HUMIDITY);
	CHECK(r.truncated);
}

/* Variable-length objects (text, raw) are skipped correctly. */
static void test_variable_length_objects(void)
{
	const uint8_t sd[] = {0xD2, 0xFC, 0x40, 0x02, 0xC4, 0x09,
			      0x53, 0x02, 'h',  'i', /* text "hi" */
			      0x54, 0x01, 0xFF}; /* raw, 1 byte */
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_service_data(sd, sizeof(sd), &r), 0);
	CHECK_EQ(r.temperature_centi_c, 2500);
	CHECK(!r.truncated);
}

static void test_adv_without_bthome(void)
{
	const uint8_t adv[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'A', 'T', 'C', '1'};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_adv(adv, sizeof(adv), &r), BTHOME_ERR_INVALID);
}

static void test_adv_malformed_length(void)
{
	const uint8_t adv[] = {0x02, 0x01, 0x06, 0x20, 0x16, 0xD2, 0xFC, 0x40};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_adv(adv, sizeof(adv), &r), BTHOME_ERR_INVALID);
}

/* Zero-length AD structure terminates the data; trailing padding is ignored. */
static void test_adv_zero_padding(void)
{
	const uint8_t adv[] = {0x02, 0x01, 0x06, 0x00, 0x00, 0x00};
	struct bthome_reading r;

	CHECK_EQ(bthome_parse_adv(adv, sizeof(adv), &r), BTHOME_ERR_INVALID);
}

int main(void)
{
	RUN(test_spec_example);
	RUN(test_pvvx_adv);
	RUN(test_pvvx_voltage_packet);
	RUN(test_negative_temperature);
	RUN(test_battery_clamped);
	RUN(test_encrypted_rejected);
	RUN(test_wrong_version_rejected);
	RUN(test_wrong_uuid_rejected);
	RUN(test_too_short_rejected);
	RUN(test_trigger_based_flag);
	RUN(test_unknown_object_keeps_earlier_values);
	RUN(test_truncated_object_ignored);
	RUN(test_variable_length_objects);
	RUN(test_adv_without_bthome);
	RUN(test_adv_malformed_length);
	RUN(test_adv_zero_padding);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
