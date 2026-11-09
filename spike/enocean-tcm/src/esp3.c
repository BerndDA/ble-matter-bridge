/*
 * ESP3 framing, see esp3.h.
 */

#include "esp3.h"

#include <string.h>

enum {
	ST_SYNC,
	ST_HEADER,
	ST_PAYLOAD,
	ST_DATA_CRC,
};

uint8_t esp3_crc8(const uint8_t *buf, size_t len)
{
	uint8_t crc = 0;

	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

void esp3_decoder_init(struct esp3_decoder *dec)
{
	memset(dec, 0, sizeof(*dec));
	dec->state = ST_SYNC;
}

/*
 * The header failed its CRC. The 0x55 that started it was probably noise, so look for another
 * sync byte inside the five header bytes already received and continue from there, as the ESP3
 * specification suggests.
 */
static void resync_in_header(struct esp3_decoder *dec)
{
	for (uint8_t i = 0; i < dec->header_len; i++) {
		if (dec->header[i] == ESP3_SYNC) {
			uint8_t rest = (uint8_t)(dec->header_len - i - 1);

			memmove(dec->header, &dec->header[i + 1], rest);
			dec->header_len = rest;
			dec->state = ST_HEADER;
			return;
		}
	}
	dec->header_len = 0;
	dec->state = ST_SYNC;
}

static bool header_complete(struct esp3_decoder *dec)
{
	const uint8_t *h = dec->header;

	if (esp3_crc8(h, 4) != h[4]) {
		dec->stats.header_crc_errors++;
		resync_in_header(dec);
		return false;
	}

	dec->pkt.data_len = (uint16_t)((h[0] << 8) | h[1]);
	dec->pkt.opt_len = h[2];
	dec->pkt.type = h[3];
	dec->total = (uint16_t)(dec->pkt.data_len + dec->pkt.opt_len);
	dec->pos = 0;
	dec->header_len = 0;

	if (dec->total > ESP3_MAX_PAYLOAD) {
		dec->stats.oversize++;
		dec->state = ST_SYNC;
		return false;
	}
	dec->state = dec->total ? ST_PAYLOAD : ST_DATA_CRC;
	return false;
}

bool esp3_decoder_feed(struct esp3_decoder *dec, uint8_t byte)
{
	switch (dec->state) {
	case ST_SYNC:
		if (byte == ESP3_SYNC) {
			dec->header_len = 0;
			dec->state = ST_HEADER;
		} else {
			dec->stats.skipped_bytes++;
		}
		return false;

	case ST_HEADER:
		dec->header[dec->header_len++] = byte;
		/* After a resync up to 4 bytes are already buffered, never 5. */
		if (dec->header_len == sizeof(dec->header)) {
			return header_complete(dec);
		}
		return false;

	case ST_PAYLOAD:
		dec->pkt.payload[dec->pos++] = byte;
		if (dec->pos == dec->total) {
			dec->state = ST_DATA_CRC;
		}
		return false;

	case ST_DATA_CRC:
		dec->state = ST_SYNC;
		if (esp3_crc8(dec->pkt.payload, dec->total) != byte) {
			dec->stats.data_crc_errors++;
			/* A 0x55 in place of the CRC may be the next frame's start. */
			if (byte == ESP3_SYNC) {
				dec->header_len = 0;
				dec->state = ST_HEADER;
			}
			return false;
		}
		dec->stats.packets++;
		return true;
	}

	dec->state = ST_SYNC;
	return false;
}

size_t esp3_encode(uint8_t type, const uint8_t *data, uint16_t data_len, const uint8_t *opt,
		   uint8_t opt_len, uint8_t *out, size_t out_size)
{
	size_t total = 6u + data_len + opt_len + 1u;

	if (out_size < total) {
		return 0;
	}

	out[0] = ESP3_SYNC;
	out[1] = (uint8_t)(data_len >> 8);
	out[2] = (uint8_t)data_len;
	out[3] = opt_len;
	out[4] = type;
	out[5] = esp3_crc8(&out[1], 4);
	if (data_len) {
		memcpy(&out[6], data, data_len);
	}
	if (opt_len) {
		memcpy(&out[6 + data_len], opt, opt_len);
	}
	out[6 + data_len + opt_len] = esp3_crc8(&out[6], (size_t)data_len + opt_len);
	return total;
}
