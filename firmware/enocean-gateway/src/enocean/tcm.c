/*
 * EnOcean TCM on a UART, see tcm.h.
 */

#include "tcm.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(tcm, CONFIG_GATEWAY_ENOCEAN_LOG_LEVEL);

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(enocean_uart));

static tcm_radio_cb radio_cb;
static struct esp3_decoder decoder;

RING_BUF_DECLARE(rx_ring, 512);
static K_SEM_DEFINE(rx_sem, 0, 1);
static uint32_t rx_overruns;

static K_MUTEX_DEFINE(tx_lock);
/* Common command the next response belongs to; 0 for a radio telegram. */
static volatile uint8_t pending_cmd;

static K_MUTEX_DEFINE(info_lock);
static struct tcm_info info = {.base_id_changes_left = 0xFF};

/* --- Receive --- */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}

		uint8_t buf[32];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n > 0) {
			if (ring_buf_put(&rx_ring, buf, n) < (uint32_t)n) {
				rx_overruns++;
			}
			k_sem_give(&rx_sem);
		}
	}
}

static void handle_response(const struct esp3_packet *pkt)
{
	const uint8_t *d = esp3_data(pkt);
	uint8_t cmd = pending_cmd;

	pending_cmd = 0;
	if (pkt->data_len < 1) {
		return;
	}

	k_mutex_lock(&info_lock, K_FOREVER);
	if (d[0] != ESP3_RET_OK) {
		info.send_errors++;
		k_mutex_unlock(&info_lock);
		LOG_WRN("TCM: command %02X failed, return code %02X", cmd, d[0]);
		return;
	}

	/*
	 * Answers are matched to the last command sent, but an answer can arrive after the next
	 * command went out (the version answer after the base ID query, seen after a restart).
	 * The two startup answers differ in length: the version answer has 33 data bytes, the
	 * base ID answer 5.
	 */
	if (pkt->data_len >= 33) {
		cmd = ESP3_CO_RD_VERSION;
	} else if (cmd == ESP3_CO_RD_VERSION) {
		cmd = 0;
	}

	switch (cmd) {
	case ESP3_CO_RD_VERSION:
		if (pkt->data_len >= 33) {
			memcpy(info.app_version, &d[1], 4);
			memcpy(info.api_version, &d[5], 4);
			info.chip_id = ((uint32_t)d[9] << 24) | ((uint32_t)d[10] << 16) |
				       ((uint32_t)d[11] << 8) | d[12];
			memcpy(info.description, &d[17], 16);
			info.description[16] = '\0';
			LOG_INF("TCM: app %u.%u.%u.%u, chip ID %08X, \"%s\"", d[1], d[2], d[3], d[4],
				info.chip_id, info.description);
		}
		break;
	case ESP3_CO_RD_IDBASE:
		if (pkt->data_len == 5) {
			const uint32_t id = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) |
					    ((uint32_t)d[3] << 8) | d[4];

			/* Base IDs are FF800000..FFFFFF80 (a range of 128 sender IDs). */
			if (id < 0xFF800000u || id > 0xFFFFFF80u) {
				LOG_WRN("TCM: base ID answer %08X out of range, ignored", id);
				break;
			}
			info.base_id = id;
			if (pkt->opt_len >= 1) {
				info.base_id_changes_left = esp3_opt(pkt)[0];
			}
			LOG_INF("TCM: base ID %08X", info.base_id);
		}
		break;
	default:
		break;
	}
	k_mutex_unlock(&info_lock);
}

static void handle_packet(const struct esp3_packet *pkt)
{
	switch (pkt->type) {
	case ESP3_RADIO_ERP1:
		if (radio_cb) {
			radio_cb(pkt);
		}
		break;
	case ESP3_RESPONSE:
		handle_response(pkt);
		break;
	case ESP3_EVENT:
		LOG_INF("TCM: event %02X", pkt->data_len ? esp3_data(pkt)[0] : 0);
		break;
	default:
		LOG_DBG("TCM: packet type %02X ignored", pkt->type);
		break;
	}
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint8_t buf[64];
		uint32_t n;

		k_sem_take(&rx_sem, K_FOREVER);
		while ((n = ring_buf_get(&rx_ring, buf, sizeof(buf))) > 0) {
			for (uint32_t i = 0; i < n; i++) {
				if (esp3_decoder_feed(&decoder, buf[i])) {
					handle_packet(&decoder.pkt);
				}
			}
		}
	}
}

K_THREAD_STACK_DEFINE(rx_stack, CONFIG_GATEWAY_ENOCEAN_RX_STACK_SIZE);
static struct k_thread rx_thread_data;

/* --- Transmit --- */

static int send_frame(const uint8_t *frame, size_t len, uint8_t pending)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	pending_cmd = pending;
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart, frame[i]);
	}
	k_mutex_unlock(&tx_lock);
	return 0;
}

int tcm_send(const uint8_t *frame, size_t len)
{
	int err = send_frame(frame, len, 0);

	if (!err) {
		k_mutex_lock(&info_lock, K_FOREVER);
		info.sent++;
		k_mutex_unlock(&info_lock);
	}
	return err;
}

static void send_common_command(uint8_t cmd)
{
	uint8_t frame[8];
	size_t n = esp3_encode(ESP3_COMMON_COMMAND, &cmd, 1, NULL, 0, frame, sizeof(frame));

	send_frame(frame, n, cmd);
}

/*
 * The module needs about 100 ms after power-up. Ask for the version, then for the base ID
 * until it answers: without the base ID the gateway cannot send.
 */
static void startup_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(startup_work, startup_work_handler);
static uint8_t startup_step;

static void startup_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (startup_step == 0) {
		send_common_command(ESP3_CO_RD_VERSION);
		startup_step = 1;
		k_work_reschedule(&startup_work, K_MSEC(200));
		return;
	}
	if (tcm_base_id() == 0) {
		if (startup_step == 3) {
			LOG_WRN("TCM: no base ID yet; check the wiring (UART TX/RX)");
		}
		if (startup_step < 255) {
			startup_step++;
		}
		send_common_command(ESP3_CO_RD_IDBASE);
		k_work_reschedule(&startup_work, K_SECONDS(5));
	}
}

/* --- Public --- */

int tcm_init(tcm_radio_cb cb)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("TCM: UART not ready");
		return -ENODEV;
	}

	radio_cb = cb;
	esp3_decoder_init(&decoder);

	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "tcm_rx");

	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);

	k_work_reschedule(&startup_work, K_MSEC(200));
	return 0;
}

uint32_t tcm_base_id(void)
{
	k_mutex_lock(&info_lock, K_FOREVER);
	uint32_t id = info.base_id;
	k_mutex_unlock(&info_lock);
	return id;
}

bool tcm_is_own_id(uint32_t id)
{
	uint32_t base = tcm_base_id();

	return base && id >= base && id < base + 128;
}

void tcm_get_info(struct tcm_info *out)
{
	k_mutex_lock(&info_lock, K_FOREVER);
	*out = info;
	k_mutex_unlock(&info_lock);
	/* Written only by the receive thread; a slightly stale copy is fine. */
	out->stats = decoder.stats;
	out->rx_overruns = rx_overruns;
}
