/*
 * ERP1 telegrams and EEP decoding, see eep.h.
 */

#include "eep.h"

#include <stddef.h>

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool erp1_parse(const struct esp3_packet *pkt, struct erp1 *out)
{
	const uint8_t *d = esp3_data(pkt);

	/* RORG + at least one data byte + sender ID (4) + status (1) */
	if (pkt->type != ESP3_RADIO_ERP1 || pkt->data_len < 7) {
		return false;
	}

	out->rorg = d[0];
	out->payload = &d[1];
	out->payload_len = (uint8_t)(pkt->data_len - 6);
	out->sender = be32(&d[pkt->data_len - 5]);
	out->status = d[pkt->data_len - 1];

	/* Optional data: sub-telegrams (1), destination ID (4), dBm (1), security level (1) */
	out->has_opt = pkt->opt_len >= 7;
	if (out->has_opt) {
		const uint8_t *o = esp3_opt(pkt);

		out->subtel = o[0];
		out->dest = be32(&o[1]);
		out->dbm = (int8_t)-o[5];
		out->security = o[6];
	} else {
		out->subtel = 0;
		out->dest = EEP_ID_BROADCAST;
		out->dbm = 0;
		out->security = 0;
	}
	return true;
}

/* --- 4BS --- */

/* 4BS payload: DB3 DB2 DB1 DB0 */
static bool is_4bs(const struct erp1 *t)
{
	return t->rorg == EEP_RORG_4BS && t->payload_len == 4;
}

bool eep_4bs_is_teach_in(const struct erp1 *t)
{
	return is_4bs(t) && !(t->payload[3] & 0x08);
}

bool eep_4bs_teach_in(const struct erp1 *t, struct eep *eep, uint16_t *manufacturer)
{
	const uint8_t *p = t->payload;

	if (!eep_4bs_is_teach_in(t) || !(p[3] & 0x80)) {
		return false;
	}
	eep->rorg = EEP_RORG_4BS;
	eep->func = p[0] >> 2;
	eep->type = (uint8_t)(((p[0] & 0x03) << 5) | (p[1] >> 3));
	*manufacturer = (uint16_t)(((p[1] & 0x07) << 8) | p[2]);
	return true;
}

/* Scale raw (0..raw_max) linearly onto min..max degC, in hundredths, rounded. */
static int16_t scale_centi(uint32_t raw, uint32_t raw_max, int32_t min, int32_t max)
{
	int32_t span = (max - min) * 100;
	int32_t v = (int32_t)(((int64_t)raw * span + (int64_t)raw_max / 2) / (int64_t)raw_max);

	return (int16_t)(min * 100 + v);
}

bool eep_a5_02(uint8_t type, const struct erp1 *t, int16_t *centi_c)
{
	/* A5-02-01 is -40..0 degC; each following type moves the 40 K range up by 10 K. */
	if (type < 0x01 || type > 0x0B || !is_4bs(t) || eep_4bs_is_teach_in(t)) {
		return false;
	}
	int32_t min = -40 + 10 * (type - 1);

	/* DB1 counts down: 255 = min, 0 = max */
	*centi_c = scale_centi(255u - t->payload[2], 255u, min, min + 40);
	return true;
}

bool eep_a5_04(uint8_t type, const struct erp1 *t, uint16_t *centi_rh, int16_t *centi_c,
	       bool *has_temp)
{
	int32_t min, max;

	switch (type) {
	case 0x01:
		min = 0;
		max = 40;
		break;
	case 0x02:
		min = -20;
		max = 60;
		break;
	default:
		return false;
	}
	if (!is_4bs(t) || eep_4bs_is_teach_in(t)) {
		return false;
	}

	const uint8_t *p = t->payload;

	if (p[1] > 250 || p[2] > 250) {
		return false;
	}
	*centi_rh = (uint16_t)scale_centi(p[1], 250u, 0, 100);
	*centi_c = scale_centi(p[2], 250u, min, max);
	*has_temp = (p[3] & 0x02) != 0;
	return true;
}

/* --- 1BS --- */

bool eep_d5_00_01(const struct erp1 *t, bool *closed, bool *teach_in)
{
	if (t->rorg != EEP_RORG_1BS || t->payload_len != 1) {
		return false;
	}
	*teach_in = !(t->payload[0] & 0x08);
	*closed = (t->payload[0] & 0x01) != 0;
	return true;
}

/* --- RPS --- */

#define STATUS_T21 0x20
#define STATUS_NU 0x10

bool eep_f6_02(const struct erp1 *t, struct eep_rocker *out)
{
	if (t->rorg != EEP_RORG_RPS || t->payload_len != 1) {
		return false;
	}

	uint8_t b = t->payload[0];

	out->pressed = (b & 0x10) != 0;
	out->known = (t->status & STATUS_NU) != 0;
	if (out->known) {
		out->button = b >> 5;
		out->second = (b & 0x01) != 0;
		out->button2 = (b >> 1) & 0x07;
		out->count = out->second ? 2 : 1;
	} else {
		out->button = 0;
		out->second = false;
		out->button2 = 0;
		out->count = b >> 5;
	}
	return true;
}

const char *eep_rocker_button_name(uint8_t button)
{
	static const char *const names[] = {"A I", "A O", "B I", "B O"};

	return button < 4 ? names[button] : "?";
}

/* --- UTE --- */

bool eep_ute(const struct erp1 *t, struct eep_ute *out)
{
	/* DB6 .. DB0 */
	if (t->rorg != EEP_RORG_UTE || t->payload_len != 7) {
		return false;
	}

	const uint8_t *p = t->payload;

	out->bidirectional = (p[0] & 0x80) != 0;
	out->response_expected = !(p[0] & 0x40);
	out->request = (p[0] >> 4) & 0x03;
	out->command = p[0] & 0x0F;
	out->channels = p[1];
	out->manufacturer = (uint16_t)(((p[3] & 0x07) << 8) | p[2]);
	out->eep.type = p[4];
	out->eep.func = p[5];
	out->eep.rorg = p[6];
	return true;
}

/* --- VLD: D2-01 --- */

int eep_d2_01_command(const struct erp1 *t)
{
	if (t->rorg != EEP_RORG_VLD || t->payload_len < 1) {
		return -1;
	}
	return t->payload[0] & 0x0F;
}

bool eep_d2_01_status(const struct erp1 *t, struct eep_d2_01_status *out)
{
	/* byte 0: PF, PFD, CMD; byte 1: OC, EL, I/O channel; byte 2: LC, output value */
	if (eep_d2_01_command(t) != EEP_D2_01_STATUS_RESPONSE || t->payload_len < 3) {
		return false;
	}

	const uint8_t *p = t->payload;

	out->power_failure = (p[0] & 0x80) != 0;
	out->channel = p[1] & 0x1F;
	out->local_control = (p[2] & 0x80) != 0;
	out->output = p[2] & 0x7F;
	return true;
}

bool eep_d2_01_measurement(const struct erp1 *t, struct eep_d2_01_measurement *out)
{
	/* byte 0: CMD; byte 1: unit (bits 7..5), I/O channel (bits 4..0); bytes 2..5: value */
	if (eep_d2_01_command(t) != EEP_D2_01_MEASUREMENT_RESPONSE || t->payload_len < 6) {
		return false;
	}

	const uint8_t *p = t->payload;

	out->unit = p[1] >> 5;
	out->channel = p[1] & 0x1F;
	out->value = be32(&p[2]);
	return true;
}

const char *eep_d2_01_unit_name(uint8_t unit)
{
	static const char *const names[] = {"Ws", "Wh", "kWh", "W", "kW"};

	return unit < 5 ? names[unit] : "?";
}

/* --- Sending --- */

size_t erp1_encode(uint8_t rorg, const uint8_t *payload, uint8_t len, uint32_t sender,
		   uint8_t status, uint32_t dest, uint8_t *out, size_t out_size)
{
	uint8_t data[1 + 14 + 5];
	/* Sub-telegrams 3, destination, dBm 0xFF (send), security level 0 */
	uint8_t opt[7] = {0x03, (uint8_t)(dest >> 24), (uint8_t)(dest >> 16), (uint8_t)(dest >> 8),
			  (uint8_t)dest, 0xFF, 0x00};
	size_t n = 0;

	if (len > 14) {
		return 0;
	}
	data[n++] = rorg;
	for (uint8_t i = 0; i < len; i++) {
		data[n++] = payload[i];
	}
	data[n++] = (uint8_t)(sender >> 24);
	data[n++] = (uint8_t)(sender >> 16);
	data[n++] = (uint8_t)(sender >> 8);
	data[n++] = (uint8_t)sender;
	data[n++] = status;

	if (dest == EEP_ID_BROADCAST) {
		return esp3_encode(ESP3_RADIO_ERP1, data, (uint16_t)n, NULL, 0, out, out_size);
	}
	return esp3_encode(ESP3_RADIO_ERP1, data, (uint16_t)n, opt, sizeof(opt), out, out_size);
}

size_t eep_eltako_switch(uint32_t sender, bool on, uint8_t *out, size_t out_size)
{
	const uint8_t b = on ? 0x30 : 0x10;

	/* status: T21 = 1, NU = 1 */
	return erp1_encode(EEP_RORG_RPS, &b, 1, sender, 0x30, EEP_ID_BROADCAST, out, out_size);
}

size_t eep_d2_01_set_output(uint32_t sender, uint32_t dest, uint8_t channel, uint8_t output,
			    uint8_t *out, size_t out_size)
{
	/* byte 1: dim value (bits 7..5, 0 = switch at once), I/O channel (bits 4..0) */
	const uint8_t p[3] = {EEP_D2_01_SET_OUTPUT, (uint8_t)(channel & 0x1F),
			      (uint8_t)(output & 0x7F)};

	return erp1_encode(EEP_RORG_VLD, p, sizeof(p), sender, 0x00, dest, out, out_size);
}

size_t eep_d2_01_query_status(uint32_t sender, uint32_t dest, uint8_t channel, uint8_t *out,
			      size_t out_size)
{
	const uint8_t p[2] = {EEP_D2_01_STATUS_QUERY, (uint8_t)(channel & 0x1F)};

	return erp1_encode(EEP_RORG_VLD, p, sizeof(p), sender, 0x00, dest, out, out_size);
}
