/*
 * Host unit tests for the ESP3 framing and EEP decoding. Build and run with `make` in this folder.
 */

#include "eep.h"
#include "esp3.h"

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

/* Feed bytes; return how many packets completed. The last one stays in dec->pkt. */
static int feed(struct esp3_decoder *dec, const uint8_t *buf, size_t len)
{
	int n = 0;

	for (size_t i = 0; i < len; i++) {
		if (esp3_decoder_feed(dec, buf[i])) {
			n++;
		}
	}
	return n;
}

/* Build an ERP1 frame as a TCM sends it: RORG, payload, sender, status + 7 bytes optional data. */
static size_t erp1_frame(uint8_t rorg, const uint8_t *payload, uint8_t len, uint32_t sender,
			 uint8_t status, uint8_t dbm, uint8_t *out, size_t out_size)
{
	uint8_t data[32];
	uint8_t opt[7] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, dbm, 0x00};
	size_t n = 0;

	data[n++] = rorg;
	memcpy(&data[n], payload, len);
	n += len;
	data[n++] = (uint8_t)(sender >> 24);
	data[n++] = (uint8_t)(sender >> 16);
	data[n++] = (uint8_t)(sender >> 8);
	data[n++] = (uint8_t)sender;
	data[n++] = status;
	return esp3_encode(ESP3_RADIO_ERP1, data, (uint16_t)n, opt, sizeof(opt), out, out_size);
}

/* Decode one ERP1 telegram through the whole chain. */
static struct erp1 telegram(uint8_t rorg, const uint8_t *payload, uint8_t len, uint8_t status)
{
	static struct esp3_decoder dec;
	uint8_t frame[64];
	struct erp1 t = {0};
	size_t n = erp1_frame(rorg, payload, len, 0x0185A1B2, status, 0x45, frame, sizeof(frame));

	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, frame, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	return t;
}

/* CRC values from the ESP3 specification and common examples. */
static void test_crc8(void)
{
	const uint8_t version_header[] = {0x00, 0x01, 0x00, 0x05};
	const uint8_t version_data[] = {ESP3_CO_RD_VERSION};
	const uint8_t erp1_header[] = {0x00, 0x07, 0x07, 0x01};

	CHECK_EQ(esp3_crc8(version_header, 4), 0x70);
	CHECK_EQ(esp3_crc8(version_data, 1), 0x09);
	CHECK_EQ(esp3_crc8(erp1_header, 4), 0x7A);
}

static void test_encode_version_request(void)
{
	const uint8_t cmd = ESP3_CO_RD_VERSION;
	const uint8_t expected[] = {0x55, 0x00, 0x01, 0x00, 0x05, 0x70, 0x03, 0x09};
	uint8_t out[16];

	CHECK_EQ(esp3_encode(ESP3_COMMON_COMMAND, &cmd, 1, NULL, 0, out, sizeof(out)), 8);
	CHECK(memcmp(out, expected, sizeof(expected)) == 0);
	CHECK_EQ(esp3_encode(ESP3_COMMON_COMMAND, &cmd, 1, NULL, 0, out, 7), 0);
}

static void test_decode_response(void)
{
	/* CO_RD_IDBASE response: RET_OK, base ID FF 80 00 00, optional: 10 write cycles left */
	const uint8_t data[] = {ESP3_RET_OK, 0xFF, 0x80, 0x00, 0x00};
	const uint8_t opt[] = {0x0A};
	uint8_t frame[32];
	struct esp3_decoder dec;
	size_t n = esp3_encode(ESP3_RESPONSE, data, sizeof(data), opt, sizeof(opt), frame,
			       sizeof(frame));

	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, frame, n), 1);
	CHECK_EQ(dec.pkt.type, ESP3_RESPONSE);
	CHECK_EQ(dec.pkt.data_len, 5);
	CHECK_EQ(dec.pkt.opt_len, 1);
	CHECK_EQ(esp3_data(&dec.pkt)[1], 0xFF);
	CHECK_EQ(esp3_opt(&dec.pkt)[0], 0x0A);
	CHECK_EQ(dec.stats.packets, 1);
}

static void test_noise_and_back_to_back(void)
{
	const uint8_t payload[] = {0x30};
	uint8_t buf[128];
	struct esp3_decoder dec;
	size_t n = 0;

	/* Noise containing a stray 0x55 whose "header" fails its CRC */
	const uint8_t noise[] = {0x00, 0x13, 0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0xAA};

	memcpy(buf, noise, sizeof(noise));
	n += sizeof(noise);
	n += erp1_frame(EEP_RORG_RPS, payload, 1, 0x01020304, 0x30, 0x40, &buf[n], sizeof(buf) - n);
	n += erp1_frame(EEP_RORG_RPS, payload, 1, 0x01020305, 0x30, 0x40, &buf[n], sizeof(buf) - n);

	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, buf, n), 2);
	CHECK(dec.stats.header_crc_errors >= 1);

	struct erp1 t;

	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK_EQ(t.sender, 0x01020305);
}

/* A frame whose header starts inside the bytes of a broken header must still be found. */
static void test_resync_inside_header(void)
{
	const uint8_t cmd = ESP3_CO_RD_VERSION;
	uint8_t buf[32] = {0x55, 0x00};
	struct esp3_decoder dec;
	size_t n = 2;

	n += esp3_encode(ESP3_COMMON_COMMAND, &cmd, 1, NULL, 0, &buf[n], sizeof(buf) - n);
	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, buf, n), 1);
	CHECK_EQ(dec.pkt.type, ESP3_COMMON_COMMAND);
}

static void test_bad_data_crc(void)
{
	const uint8_t payload[] = {0x30};
	uint8_t buf[64];
	struct esp3_decoder dec;
	size_t n = erp1_frame(EEP_RORG_RPS, payload, 1, 0x01020304, 0x30, 0x40, buf, sizeof(buf));

	buf[7] ^= 0x01;
	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, buf, n), 0);
	CHECK_EQ(dec.stats.data_crc_errors, 1);

	/* The next good frame still comes through */
	buf[7] ^= 0x01;
	CHECK_EQ(feed(&dec, buf, n), 1);
}

static void test_oversize_dropped(void)
{
	/* Header announcing 200 bytes of data */
	uint8_t hdr[6] = {0x55, 0x00, 200, 0x00, ESP3_RADIO_ERP1, 0};
	struct esp3_decoder dec;

	hdr[5] = esp3_crc8(&hdr[1], 4);
	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, hdr, sizeof(hdr)), 0);
	CHECK_EQ(dec.stats.oversize, 1);
}

static void test_erp1_fields(void)
{
	const uint8_t payload[] = {0x08};
	struct erp1 t = telegram(EEP_RORG_1BS, payload, 1, 0x00);

	CHECK_EQ(t.rorg, EEP_RORG_1BS);
	CHECK_EQ(t.payload_len, 1);
	CHECK_EQ(t.sender, 0x0185A1B2);
	CHECK(t.has_opt);
	CHECK_EQ(t.dbm, -0x45);
	CHECK_EQ(t.dest, EEP_ID_BROADCAST);
}

static void test_4bs_teach_in(void)
{
	/* A5-02-05, manufacturer 0x00B, teach-in with profile (LRN type = 1, LRN = 0) */
	const uint8_t with_profile[] = {0x08, 0x28, 0x0B, 0x80};
	const uint8_t without_profile[] = {0x00, 0x00, 0x00, 0x00};
	const uint8_t data[] = {0x00, 0x00, 0x80, 0x08};
	struct eep eep;
	uint16_t manufacturer;
	struct erp1 t = telegram(EEP_RORG_4BS, with_profile, 4, 0x00);

	CHECK(eep_4bs_is_teach_in(&t));
	CHECK(eep_4bs_teach_in(&t, &eep, &manufacturer));
	CHECK_EQ(eep.rorg, 0xA5);
	CHECK_EQ(eep.func, 0x02);
	CHECK_EQ(eep.type, 0x05);
	CHECK_EQ(manufacturer, 0x00B);

	t = telegram(EEP_RORG_4BS, without_profile, 4, 0x00);
	CHECK(eep_4bs_is_teach_in(&t));
	CHECK(!eep_4bs_teach_in(&t, &eep, &manufacturer));

	t = telegram(EEP_RORG_4BS, data, 4, 0x00);
	CHECK(!eep_4bs_is_teach_in(&t));
}

static void test_a5_02(void)
{
	const uint8_t cold[] = {0x00, 0x00, 0xFF, 0x08};
	const uint8_t warm[] = {0x00, 0x00, 0x00, 0x08};
	const uint8_t mid[] = {0x00, 0x00, 0x80, 0x08};
	int16_t c;
	struct erp1 t = telegram(EEP_RORG_4BS, cold, 4, 0x00);

	CHECK(eep_a5_02(0x05, &t, &c));
	CHECK_EQ(c, 0);
	CHECK(eep_a5_02(0x01, &t, &c));
	CHECK_EQ(c, -4000);
	CHECK(!eep_a5_02(0x0C, &t, &c));

	t = telegram(EEP_RORG_4BS, warm, 4, 0x00);
	CHECK(eep_a5_02(0x05, &t, &c));
	CHECK_EQ(c, 4000);
	CHECK(eep_a5_02(0x0B, &t, &c));
	CHECK_EQ(c, 10000);

	/* 127/255 * 40 = 19.92 */
	t = telegram(EEP_RORG_4BS, mid, 4, 0x00);
	CHECK(eep_a5_02(0x05, &t, &c));
	CHECK_EQ(c, 1992);
}

static void test_a5_04(void)
{
	const uint8_t both[] = {0x00, 125, 125, 0x0A};
	const uint8_t no_temp[] = {0x00, 250, 0, 0x08};
	const uint8_t bad[] = {0x00, 251, 0, 0x0A};
	uint16_t rh;
	int16_t c;
	bool has_temp;
	struct erp1 t = telegram(EEP_RORG_4BS, both, 4, 0x00);

	CHECK(eep_a5_04(0x01, &t, &rh, &c, &has_temp));
	CHECK_EQ(rh, 5000);
	CHECK_EQ(c, 2000);
	CHECK(has_temp);

	CHECK(eep_a5_04(0x02, &t, &rh, &c, &has_temp));
	CHECK_EQ(c, 2000); /* -20 + 125/250 * 80 */

	t = telegram(EEP_RORG_4BS, no_temp, 4, 0x00);
	CHECK(eep_a5_04(0x01, &t, &rh, &c, &has_temp));
	CHECK_EQ(rh, 10000);
	CHECK(!has_temp);

	t = telegram(EEP_RORG_4BS, bad, 4, 0x00);
	CHECK(!eep_a5_04(0x01, &t, &rh, &c, &has_temp));
	CHECK(!eep_a5_04(0x03, &t, &rh, &c, &has_temp));
}

static void test_d5_00_01(void)
{
	const uint8_t closed[] = {0x09};
	const uint8_t open[] = {0x08};
	const uint8_t learn[] = {0x00};
	bool is_closed, teach;
	struct erp1 t = telegram(EEP_RORG_1BS, closed, 1, 0x00);

	CHECK(eep_d5_00_01(&t, &is_closed, &teach));
	CHECK(is_closed);
	CHECK(!teach);

	t = telegram(EEP_RORG_1BS, open, 1, 0x00);
	CHECK(eep_d5_00_01(&t, &is_closed, &teach));
	CHECK(!is_closed);

	t = telegram(EEP_RORG_1BS, learn, 1, 0x00);
	CHECK(eep_d5_00_01(&t, &is_closed, &teach));
	CHECK(teach);
}

static void test_f6_02(void)
{
	const uint8_t press_ao[] = {0x30};
	/* R1 = B I (010), EB, R2 = A O (001), SA */
	const uint8_t press_bi_and_ao[] = {0x40 | 0x10 | 0x02 | 0x01};
	const uint8_t release[] = {0x00};
	const uint8_t three[] = {0x70};
	struct eep_rocker r;
	struct erp1 t = telegram(EEP_RORG_RPS, press_ao, 1, 0x30);

	CHECK(eep_f6_02(&t, &r));
	CHECK(r.pressed);
	CHECK(r.known);
	CHECK_EQ(r.button, 1);
	CHECK(!r.second);
	CHECK(strcmp(eep_rocker_button_name(r.button), "A O") == 0);

	t = telegram(EEP_RORG_RPS, press_bi_and_ao, 1, 0x30);
	CHECK(eep_f6_02(&t, &r));
	CHECK_EQ(r.button, 2);
	CHECK(r.second);
	CHECK_EQ(r.button2, 1);

	t = telegram(EEP_RORG_RPS, release, 1, 0x20);
	CHECK(eep_f6_02(&t, &r));
	CHECK(!r.pressed);
	CHECK(!r.known);
	CHECK_EQ(r.count, 0);

	t = telegram(EEP_RORG_RPS, three, 1, 0x20);
	CHECK(eep_f6_02(&t, &r));
	CHECK(r.pressed);
	CHECK_EQ(r.count, 3);
}

static void test_ute(void)
{
	/* Bidirectional, response expected, teach-in or out, all channels, manufacturer 0x03E,
	 * EEP D2-01-01 */
	const uint8_t payload[] = {0xA0, 0xFF, 0x3E, 0x00, 0x01, 0x01, 0xD2};
	struct eep_ute u;
	struct erp1 t = telegram(EEP_RORG_UTE, payload, 7, 0x00);

	CHECK(eep_ute(&t, &u));
	CHECK(u.bidirectional);
	CHECK(u.response_expected);
	CHECK_EQ(u.request, 2);
	CHECK_EQ(u.command, 0);
	CHECK_EQ(u.channels, 0xFF);
	CHECK_EQ(u.manufacturer, 0x03E);
	CHECK_EQ(u.eep.rorg, 0xD2);
	CHECK_EQ(u.eep.func, 0x01);
	CHECK_EQ(u.eep.type, 0x01);
}

static void test_wrong_rorg_rejected(void)
{
	const uint8_t payload[] = {0x09};
	bool a, b;
	int16_t c;
	struct eep_rocker r;
	struct erp1 t = telegram(EEP_RORG_1BS, payload, 1, 0x00);

	CHECK(!eep_f6_02(&t, &r));
	CHECK(!eep_a5_02(0x05, &t, &c));
	CHECK(!eep_4bs_is_teach_in(&t));
	t = telegram(EEP_RORG_RPS, payload, 1, 0x30);
	CHECK(!eep_d5_00_01(&t, &a, &b));
}

/* Frames enocean-mqtt sent to the Eltako actuators (log on the Pi, 2026-11-08) */
static void test_eltako_switch_matches_enocean_mqtt(void)
{
	const uint8_t flur_off[] = {0x55, 0x00, 0x07, 0x00, 0x01, 0x11, 0xF6,
				    0x10, 0xFF, 0xA0, 0x1D, 0x86, 0x30, 0x42};
	const uint8_t rechts_off[] = {0x55, 0x00, 0x07, 0x00, 0x01, 0x11, 0xF6,
				      0x10, 0xFF, 0xA0, 0x1D, 0x83, 0x30, 0x03};
	uint8_t out[32];

	CHECK_EQ(eep_eltako_switch(0xFFA01D86, false, out, sizeof(out)), sizeof(flur_off));
	CHECK(memcmp(out, flur_off, sizeof(flur_off)) == 0);
	CHECK_EQ(eep_eltako_switch(0xFFA01D83, false, out, sizeof(out)), sizeof(rechts_off));
	CHECK(memcmp(out, rechts_off, sizeof(rechts_off)) == 0);

	/* On is the same with 0x30, and decodes back */
	struct esp3_decoder dec;
	struct erp1 t;
	size_t n = eep_eltako_switch(0xFFA01D87, true, out, sizeof(out));

	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, out, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK(!t.has_opt);
	CHECK_EQ(t.payload[0], 0x30);
	CHECK_EQ(t.sender, 0xFFA01D87);
	CHECK_EQ(t.status, 0x30);
}

/* Received from kueche_radio (05017E43) by the spike: D2 076000000000 st=80 to FFA01D89 */
static void test_d2_01_measurement(void)
{
	const uint8_t zero[] = {0x07, 0x60, 0x00, 0x00, 0x00, 0x00};
	const uint8_t energy[] = {0x07, 0x41, 0x00, 0x01, 0x02, 0x03};
	struct eep_d2_01_measurement m;
	struct erp1 t = telegram(EEP_RORG_VLD, zero, sizeof(zero), 0x80);

	CHECK_EQ(eep_d2_01_command(&t), EEP_D2_01_MEASUREMENT_RESPONSE);
	CHECK(eep_d2_01_measurement(&t, &m));
	CHECK_EQ(m.unit, 3);
	CHECK(strcmp(eep_d2_01_unit_name(m.unit), "W") == 0);
	CHECK_EQ(m.channel, 0);
	CHECK_EQ(m.value, 0);

	t = telegram(EEP_RORG_VLD, energy, sizeof(energy), 0x80);
	CHECK(eep_d2_01_measurement(&t, &m));
	CHECK_EQ(m.unit, 2);
	CHECK_EQ(m.channel, 1);
	CHECK_EQ(m.value, 0x010203);

	struct eep_d2_01_status st;

	CHECK(!eep_d2_01_status(&t, &st));
}

static void test_d2_01_status(void)
{
	const uint8_t on_local[] = {0x04, 0x60, 0xE4};
	const uint8_t off[] = {0x84, 0x00, 0x00};
	struct eep_d2_01_status st;
	struct erp1 t = telegram(EEP_RORG_VLD, on_local, sizeof(on_local), 0x80);

	CHECK(eep_d2_01_status(&t, &st));
	CHECK_EQ(st.channel, 0);
	CHECK_EQ(st.output, 100);
	CHECK(st.local_control);
	CHECK(!st.power_failure);

	t = telegram(EEP_RORG_VLD, off, sizeof(off), 0x80);
	CHECK(eep_d2_01_status(&t, &st));
	CHECK_EQ(st.output, 0);
	CHECK(st.power_failure);
}

static void test_d2_01_commands(void)
{
	uint8_t out[48];
	struct esp3_decoder dec;
	struct erp1 t;
	size_t n = eep_d2_01_set_output(0xFFA01D89, 0x05017E43, 0, 100, out, sizeof(out));

	esp3_decoder_init(&dec);
	CHECK_EQ(feed(&dec, out, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK_EQ(t.rorg, EEP_RORG_VLD);
	CHECK_EQ(t.payload_len, 3);
	CHECK_EQ(t.payload[0], 0x01);
	CHECK_EQ(t.payload[1], 0x00);
	CHECK_EQ(t.payload[2], 100);
	CHECK_EQ(t.sender, 0xFFA01D89);
	CHECK(t.has_opt);
	CHECK_EQ(t.subtel, 3);
	CHECK_EQ(t.dest, 0x05017E43);

	n = eep_d2_01_query_status(0xFFA01D89, 0x05017E43, EEP_D2_01_ALL_CHANNELS, out,
				   sizeof(out));
	CHECK_EQ(feed(&dec, out, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK_EQ(t.payload_len, 2);
	CHECK_EQ(t.payload[0], 0x03);
	CHECK_EQ(t.payload[1], 0x1E);

	n = eep_d2_01_query_measurement(0xFFA01D89, 0x05017E43, 0, true, out, sizeof(out));
	CHECK_EQ(feed(&dec, out, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK_EQ(t.rorg, 0xD2);
	CHECK_EQ(t.payload_len, 2);
	CHECK_EQ(t.payload[0], 0x06);
	CHECK_EQ(t.payload[1], 0x20);
	CHECK_EQ(t.sender, 0xFFA01D89);
	CHECK_EQ(t.dest, 0x05017E43);

	n = eep_d2_01_query_measurement(0xFFA01D89, 0x05017E43, 1, false, out, sizeof(out));
	CHECK_EQ(feed(&dec, out, n), 1);
	CHECK(erp1_parse(&dec.pkt, &t));
	CHECK_EQ(t.payload[1], 0x01);
}

int main(void)
{
	RUN(test_crc8);
	RUN(test_encode_version_request);
	RUN(test_decode_response);
	RUN(test_noise_and_back_to_back);
	RUN(test_resync_inside_header);
	RUN(test_bad_data_crc);
	RUN(test_oversize_dropped);
	RUN(test_erp1_fields);
	RUN(test_4bs_teach_in);
	RUN(test_a5_02);
	RUN(test_a5_04);
	RUN(test_d5_00_01);
	RUN(test_f6_02);
	RUN(test_ute);
	RUN(test_wrong_rorg_rejected);
	RUN(test_eltako_switch_matches_enocean_mqtt);
	RUN(test_d2_01_measurement);
	RUN(test_d2_01_status);
	RUN(test_d2_01_commands);

	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
