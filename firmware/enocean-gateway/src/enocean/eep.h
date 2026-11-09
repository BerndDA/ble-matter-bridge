/*
 * EnOcean radio telegrams (ERP1, as delivered by ESP3) and a few EnOcean Equipment Profiles.
 *
 * Which EEP a sender uses is only in the telegram for 4BS teach-in (with "LRN type" set) and
 * UTE. RPS (rockers) and 1BS (contacts) never say; the RORG alone has to do.
 *
 * Values are in hundredths (centi-degrees, centi-percent), the unit Matter uses.
 *
 * Plain C without Zephyr dependencies, so it can be unit-tested on the host (tests/host).
 */

#pragma once

#include "esp3.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Radio telegram types (RORG) */
#define EEP_RORG_RPS 0xF6 /* repeated switch: rockers, window handles */
#define EEP_RORG_1BS 0xD5 /* 1 byte: contacts */
#define EEP_RORG_4BS 0xA5 /* 4 bytes: most sensors */
#define EEP_RORG_VLD 0xD2 /* variable length */
#define EEP_RORG_UTE 0xD4 /* universal teach-in */

#define EEP_ID_BROADCAST 0xFFFFFFFFu

struct erp1 {
	uint8_t rorg;
	const uint8_t *payload; /* user data between RORG and sender ID */
	uint8_t payload_len;
	uint32_t sender;
	uint8_t status;
	/* From the optional data, if the module sent it (TCM 310/515 do) */
	bool has_opt;
	uint8_t subtel;
	uint32_t dest;
	int8_t dbm; /* received signal strength, negative */
	uint8_t security;
};

/* Split an ESP3_RADIO_ERP1 packet. False if it is something else or too short. */
bool erp1_parse(const struct esp3_packet *pkt, struct erp1 *out);

/* EEP as RORG-FUNC-TYPE, for example A5-02-05 */
struct eep {
	uint8_t rorg;
	uint8_t func;
	uint8_t type;
};

/* --- 4BS (A5) --- */

/* True if the telegram is a teach-in (LRN bit DB0.3 = 0). */
bool eep_4bs_is_teach_in(const struct erp1 *t);

/*
 * Teach-in variant 3 carries the EEP and manufacturer (LRN type DB0.7 = 1). Returns false for a
 * data telegram or a teach-in without profile (variant 2, older devices).
 */
bool eep_4bs_teach_in(const struct erp1 *t, struct eep *eep, uint16_t *manufacturer);

/* A5-02-01 .. A5-02-0B: temperature, 8 bit, ranges -40..0 up to 60..100 degC. */
bool eep_a5_02(uint8_t type, const struct erp1 *t, int16_t *centi_c);

/*
 * A5-04-01 (0..40 degC) and A5-04-02 (-20..60 degC): humidity and temperature.
 * has_temp is false when the device says it has no temperature sensor (DB0.1 = 0).
 */
bool eep_a5_04(uint8_t type, const struct erp1 *t, uint16_t *centi_rh, int16_t *centi_c,
	       bool *has_temp);

/* --- 1BS (D5) --- */

/* D5-00-01: single input contact. teach_in is true for a teach-in telegram (LRN DB0.3 = 0). */
bool eep_d5_00_01(const struct erp1 *t, bool *closed, bool *teach_in);

/* --- RPS (F6) --- */

/* F6-02-01/02: rocker switch with two rockers (A and B). */
struct eep_rocker {
	bool pressed; /* energy bow: true on press, false on release */
	/* Only for a press with NU = 1 (one or two known buttons): */
	bool known;
	uint8_t button;	 /* 0 = A I, 1 = A O, 2 = B I, 3 = B O */
	bool second;	 /* a second button is pressed too */
	uint8_t button2; /* valid if second */
	/* For a press with NU = 0: number of buttons (0 = none, 3 = three or four) */
	uint8_t count;
};

bool eep_f6_02(const struct erp1 *t, struct eep_rocker *out);

const char *eep_rocker_button_name(uint8_t button);

/* --- UTE (D4) --- */

struct eep_ute {
	uint8_t command;        /* DB6 bits 3..0: 0 = teach-in query, 1 = response */
	bool bidirectional;     /* DB6.7 */
	bool response_expected; /* DB6.6 = 0 */
	uint8_t request;        /* DB6 bits 5..4: 0 = teach-in, 1 = teach-out, 2 = either */
	uint8_t channels;      /* DB5, 0xFF = all */
	uint16_t manufacturer; /* 11 bit */
	struct eep eep;
};

bool eep_ute(const struct erp1 *t, struct eep_ute *out);

/* --- VLD (D2) --- */

/* D2-01 (electronic switches and dimmers) commands, in the low nibble of the first byte */
#define EEP_D2_01_SET_OUTPUT 0x01
#define EEP_D2_01_STATUS_QUERY 0x03
#define EEP_D2_01_STATUS_RESPONSE 0x04
#define EEP_D2_01_MEASUREMENT_QUERY 0x06
#define EEP_D2_01_MEASUREMENT_RESPONSE 0x07

#define EEP_D2_01_ALL_CHANNELS 0x1E
#define EEP_D2_01_OUTPUT_INVALID 127

/* D2-01 command 4: actuator status response */
struct eep_d2_01_status {
	uint8_t channel;
	uint8_t output; /* 0 = off, 1..100 = %, 127 = not valid */
	bool local_control;
	bool power_failure;
};

/* D2-01 command 7: actuator measurement response */
struct eep_d2_01_measurement {
	uint8_t channel;
	uint8_t unit; /* 0 = Ws, 1 = Wh, 2 = kWh, 3 = W, 4 = kW */
	uint32_t value;
};

/* Command number of a D2-01 telegram, or -1 if it is not VLD. */
int eep_d2_01_command(const struct erp1 *t);

bool eep_d2_01_status(const struct erp1 *t, struct eep_d2_01_status *out);
bool eep_d2_01_measurement(const struct erp1 *t, struct eep_d2_01_measurement *out);

const char *eep_d2_01_unit_name(uint8_t unit);

/* --- Sending --- */

/*
 * Build an ESP3 frame with an ERP1 radio telegram from sender (base ID + offset). With dest =
 * EEP_ID_BROADCAST no optional data is sent; otherwise it is addressed to dest. Returns the
 * frame length, or 0 if out_size is too small.
 */
size_t erp1_encode(uint8_t rorg, const uint8_t *payload, uint8_t len, uint32_t sender,
		   uint8_t status, uint32_t dest, uint8_t *out, size_t out_size);

/*
 * Eltako actuator taught in to a rocker: a press of the right half, 0x30 = on, 0x10 = off.
 * No release, no optional data (as enocean-mqtt did it).
 */
size_t eep_eltako_switch(uint32_t sender, bool on, uint8_t *out, size_t out_size);

/* D2-01 command 1: set output (0 = off, 1..100 = %) on a channel, addressed to dest. */
size_t eep_d2_01_set_output(uint32_t sender, uint32_t dest, uint8_t channel, uint8_t output,
			    uint8_t *out, size_t out_size);

/* D2-01 command 3: ask for the status of a channel; the actuator answers with command 4. */
size_t eep_d2_01_query_status(uint32_t sender, uint32_t dest, uint8_t channel, uint8_t *out,
			      size_t out_size);

/*
 * D2-01 command 6: ask for the power (power = true) or the energy of a channel; the actuator
 * answers with command 7.
 */
size_t eep_d2_01_query_measurement(uint32_t sender, uint32_t dest, uint8_t channel, bool power,
				   uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
