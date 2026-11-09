/*
 * EnOcean TCM 310/515 on a UART (devicetree chosen node "enocean,uart"), ESP3 at 57600 baud.
 *
 * A receive thread decodes ESP3 packets and hands radio telegrams to a callback, in that
 * thread. Responses to the module's own commands (version, base ID) are handled here.
 */

#pragma once

#include "esp3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called in the TCM receive thread for every ESP3_RADIO_ERP1 packet. */
typedef void (*tcm_radio_cb)(const struct esp3_packet *pkt);

/* Starts the UART and the receive thread, then reads the version and base ID. */
int tcm_init(tcm_radio_cb cb);

/* Sends one ESP3 frame. Safe from any thread; returns -ENODEV if the UART is not ready. */
int tcm_send(const uint8_t *frame, size_t len);

/* Base ID of the module (FF800000 .. FFFFFF80), 0 until the module has answered. */
uint32_t tcm_base_id(void);

/* True for the 128 sender IDs the gateway may send with (base ID .. base ID + 127). */
bool tcm_is_own_id(uint32_t id);

struct tcm_info {
	uint32_t base_id;
	uint8_t base_id_changes_left; /* 0xFF = not known */
	uint8_t app_version[4];
	uint8_t api_version[4];
	uint32_t chip_id;
	char description[17];
	struct esp3_stats stats;
	uint32_t rx_overruns;
	uint32_t sent;
	uint32_t send_errors; /* responses other than RET_OK */
};

void tcm_get_info(struct tcm_info *out);

#ifdef __cplusplus
}
#endif
