/*
 * EnOcean Serial Protocol 3 (ESP3): framing between the host and a TCM 310/515.
 *
 *   0x55 | data length (2, big endian) | optional length (1) | packet type (1) | CRC8 header
 *        | data | optional data | CRC8 data
 *
 * Both CRCs are CRC-8 with polynomial 0x07, initial value 0. The header CRC covers the four
 * bytes after 0x55; the data CRC covers data and optional data.
 *
 * Plain C without Zephyr dependencies, so it can be unit-tested on the host (tests/host).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP3_SYNC 0x55

/* Largest data + optional data this decoder accepts. Radio telegrams and the responses the
 * gateway asks for are far below this; longer packets are dropped and counted. */
#define ESP3_MAX_PAYLOAD 128

/* Packet types */
#define ESP3_RADIO_ERP1 0x01
#define ESP3_RESPONSE 0x02
#define ESP3_RADIO_SUB_TEL 0x03
#define ESP3_EVENT 0x04
#define ESP3_COMMON_COMMAND 0x05
#define ESP3_SMART_ACK_COMMAND 0x06
#define ESP3_REMOTE_MAN_COMMAND 0x07
#define ESP3_RADIO_MESSAGE 0x09
#define ESP3_RADIO_ERP2 0x0A

/* Common commands (first data byte of an ESP3_COMMON_COMMAND packet) */
#define ESP3_CO_RD_VERSION 0x03
#define ESP3_CO_RD_IDBASE 0x08

/* Return codes (first data byte of an ESP3_RESPONSE packet) */
#define ESP3_RET_OK 0x00
#define ESP3_RET_ERROR 0x01
#define ESP3_RET_NOT_SUPPORTED 0x02
#define ESP3_RET_WRONG_PARAM 0x03
#define ESP3_RET_OPERATION_DENIED 0x04

struct esp3_packet {
	uint8_t type;
	uint16_t data_len;
	uint8_t opt_len;
	/* data_len bytes of data, then opt_len bytes of optional data */
	uint8_t payload[ESP3_MAX_PAYLOAD];
};

static inline const uint8_t *esp3_data(const struct esp3_packet *p)
{
	return p->payload;
}

static inline const uint8_t *esp3_opt(const struct esp3_packet *p)
{
	return p->payload + p->data_len;
}

struct esp3_stats {
	uint32_t packets;
	uint32_t header_crc_errors;
	uint32_t data_crc_errors;
	uint32_t oversize;
	uint32_t skipped_bytes; /* bytes outside a frame (noise before a sync byte) */
};

/* Byte-wise decoder; resynchronises on the next 0x55 after any error. */
struct esp3_decoder {
	uint8_t state;
	uint8_t header[5]; /* 4 header bytes + header CRC */
	uint8_t header_len;
	uint16_t pos;
	uint16_t total;
	struct esp3_packet pkt;
	struct esp3_stats stats;
};

uint8_t esp3_crc8(const uint8_t *buf, size_t len);

void esp3_decoder_init(struct esp3_decoder *dec);

/*
 * Feed one received byte. Returns true when it completes a valid packet; the packet is then in
 * dec->pkt until the next call.
 */
bool esp3_decoder_feed(struct esp3_decoder *dec, uint8_t byte);

/*
 * Build a frame into out. Returns its length, or 0 if out_size is too small.
 */
size_t esp3_encode(uint8_t type, const uint8_t *data, uint16_t data_len, const uint8_t *opt,
		   uint8_t opt_len, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
