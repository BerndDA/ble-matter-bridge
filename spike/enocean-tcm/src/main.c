/*
 * EnOcean spike: a TCM 310/515 (for example on an EnOcean Pi hat) on a UART of the nRF52840.
 * Logs every radio telegram, decodes the common profiles and offers a few shell commands.
 * No Matter yet; this is to see how the devices at hand behave.
 */

#include "eep.h"
#include "esp3.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const struct device *const tcm = DEVICE_DT_GET(DT_CHOSEN(enocean_uart));

/* --- Receive: ISR into a ring buffer, a thread decodes --- */

RING_BUF_DECLARE(rx_ring, 512);
static K_SEM_DEFINE(rx_sem, 0, 1);
static uint32_t rx_overruns;

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

/* --- Transmit: one command at a time, the response is matched to it --- */

static K_MUTEX_DEFINE(tx_lock);
static volatile uint8_t pending_cmd; /* common command awaiting a response, 0 = none */

static volatile uint32_t base_id; /* from CO_RD_IDBASE at start, 0 = not read yet */

/* pending: the common command the response belongs to, 0 for a radio telegram */
static void send_frame(const uint8_t *frame, size_t n, uint8_t pending)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	pending_cmd = pending;
	for (size_t i = 0; i < n; i++) {
		uart_poll_out(tcm, frame[i]);
	}
	k_mutex_unlock(&tx_lock);
}

static int send_common_command(uint8_t cmd)
{
	uint8_t frame[16];
	size_t n = esp3_encode(ESP3_COMMON_COMMAND, &cmd, 1, NULL, 0, frame, sizeof(frame));

	send_frame(frame, n, cmd);
	return 0;
}

/* --- The actuators taught in to this TCM (from the enocean-mqtt config on the Pi) --- */

enum actuator_kind {
	ACT_ELTAKO, /* taught to a rocker; reports its state as F6 telegrams */
	ACT_D2_01,  /* VLD electronic switch, addressed commands */
};

struct actuator {
	uint32_t id;
	const char *name;
	enum actuator_kind kind;
	uint8_t offset; /* sender ID = base ID + offset, as taught in */
};

static const struct actuator actuators[] = {
	{0x0190B727, "kueche_links", ACT_ELTAKO, 7},
	{0x018B3A28, "kueche_rechts", ACT_ELTAKO, 3},
	{0x0188EA75, "flur", ACT_ELTAKO, 6},
	{0x05017E43, "kueche_radio", ACT_D2_01, 9},
};

static const struct actuator *actuator_by_id(uint32_t id)
{
	for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
		if (actuators[i].id == id) {
			return &actuators[i];
		}
	}
	return NULL;
}

static const struct actuator *actuator_by_name(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
		if (strcmp(actuators[i].name, name) == 0) {
			return &actuators[i];
		}
	}
	return NULL;
}

/* --- Devices heard, with the profile they use (learned from teach-in or set in the shell) --- */

#define MAX_DEVICES 32

struct device_entry {
	uint32_t id;
	uint8_t rorg;
	struct eep eep; /* rorg 0 = not known */
	uint32_t count;
	int8_t dbm;
	int64_t last_ms;
};

static struct device_entry devices[MAX_DEVICES];
static K_MUTEX_DEFINE(devices_lock);

/* Caller holds devices_lock. Returns NULL if the table is full. */
static struct device_entry *device_get(uint32_t id, bool create)
{
	struct device_entry *free_slot = NULL;

	for (size_t i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].id == id) {
			return &devices[i];
		}
		if (!free_slot && devices[i].id == 0) {
			free_slot = &devices[i];
		}
	}
	if (create && free_slot) {
		memset(free_slot, 0, sizeof(*free_slot));
		free_slot->id = id;
	}
	return create ? free_slot : NULL;
}

/* --- Printing ---
 *
 * Lines are assembled here and printed in one piece; printed in parts, the shell would put its
 * prompt in between. Only the receive thread and main() (before any radio traffic) use it.
 */

static char line[192];
static size_t line_len;

static void out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintk(&line[line_len], sizeof(line) - line_len, fmt, ap);
	va_end(ap);

	if (n > 0) {
		line_len = MIN(line_len + (size_t)n, sizeof(line) - 1);
	}
	if (line_len && line[line_len - 1] == '\n') {
		printk("%s", line);
		line_len = 0;
	}
}

static const char *centi(int32_t v, char *buf, size_t size)
{
	uint32_t a = (uint32_t)(v < 0 ? -v : v);

	snprintk(buf, size, "%s%u.%02u", v < 0 ? "-" : "", a / 100, a % 100);
	return buf;
}

static void print_rocker(const struct erp1 *t)
{
	struct eep_rocker r;

	if (!eep_f6_02(t, &r)) {
		return;
	}
	if (!r.pressed) {
		out(" | rocker released\n");
	} else if (r.known && r.second) {
		out(" | rocker %s + %s pressed\n", eep_rocker_button_name(r.button),
		       eep_rocker_button_name(r.button2));
	} else if (r.known) {
		out(" | rocker %s pressed\n", eep_rocker_button_name(r.button));
	} else {
		out(" | rocker: %u buttons pressed\n", r.count);
	}
}

static void print_4bs(const struct erp1 *t, const struct eep *known)
{
	char a[12], b[12];
	struct eep eep;
	uint16_t manufacturer;
	int16_t c;
	uint16_t rh;
	bool has_temp;

	if (eep_4bs_is_teach_in(t)) {
		if (eep_4bs_teach_in(t, &eep, &manufacturer)) {
			out(" | teach-in A5-%02X-%02X, manufacturer %03X\n", eep.func, eep.type,
			       manufacturer);
		} else {
			out(" | teach-in without profile (set it: tcm profile %08X A5-xx-xx)\n",
			       t->sender);
		}
		return;
	}

	if (known->rorg == EEP_RORG_4BS && known->func == 0x02 &&
	    eep_a5_02(known->type, t, &c)) {
		out(" | A5-02-%02X: %s C\n", known->type, centi(c, a, sizeof(a)));
		return;
	}
	if (known->rorg == EEP_RORG_4BS && known->func == 0x04 &&
	    eep_a5_04(known->type, t, &rh, &c, &has_temp)) {
		out(" | A5-04-%02X: %s %%", known->type, centi(rh, a, sizeof(a)));
		if (has_temp) {
			out(", %s C", centi(c, b, sizeof(b)));
		}
		out("\n");
		return;
	}
	if (known->rorg) {
		out(" | %02X-%02X-%02X not decoded yet\n", known->rorg, known->func,
		       known->type);
		return;
	}

	/* Profile not known: show the two most common readings as a hint */
	out(" | profile unknown;");
	if (eep_a5_02(0x05, t, &c)) {
		out(" as A5-02-05: %s C;", centi(c, a, sizeof(a)));
	}
	if (eep_a5_04(0x01, t, &rh, &c, &has_temp)) {
		out(" as A5-04-01: %s %%, %s C", centi(rh, a, sizeof(a)),
		       centi(c, b, sizeof(b)));
	}
	out("\n");
}

static void print_contact(const struct erp1 *t)
{
	bool closed, teach_in;

	if (!eep_d5_00_01(t, &closed, &teach_in)) {
		return;
	}
	if (teach_in) {
		out(" | contact teach-in (D5-00-01)\n");
	} else {
		out(" | contact %s\n", closed ? "closed" : "open");
	}
}

static void print_ute(const struct erp1 *t)
{
	struct eep_ute u;
	static const char *const request[] = {"teach-in", "teach-out", "teach-in/out", "?"};

	if (!eep_ute(t, &u)) {
		return;
	}
	out(" | UTE %s %s, %s, EEP %02X-%02X-%02X, manufacturer %03X, channels %u\n",
	       u.command == 0 ? "query" : "response", request[u.request],
	       u.bidirectional ? "bidirectional" : "unidirectional", u.eep.rorg, u.eep.func,
	       u.eep.type, u.manufacturer, u.channels);
}

static void print_d2_01(const struct actuator *act, const struct erp1 *t)
{
	struct eep_d2_01_status st;
	struct eep_d2_01_measurement m;

	if (eep_d2_01_status(t, &st)) {
		if (st.output == EEP_D2_01_OUTPUT_INVALID) {
			out(" | %s channel %u: output not valid\n", act->name, st.channel);
		} else {
			out(" | %s channel %u: %s (%u %%)%s%s\n", act->name, st.channel,
			    st.output ? "on" : "off", st.output,
			    st.local_control ? ", local control" : "",
			    st.power_failure ? ", power failure" : "");
		}
	} else if (eep_d2_01_measurement(t, &m)) {
		out(" | %s channel %u: %u %s\n", act->name, m.channel, m.value,
		    eep_d2_01_unit_name(m.unit));
	} else {
		out(" | %s: D2-01 command %d\n", act->name, eep_d2_01_command(t));
	}
}

static void handle_radio(const struct esp3_packet *pkt)
{
	struct erp1 t;
	struct eep known = {0};

	if (!erp1_parse(pkt, &t)) {
		out("ERP1 too short (%u bytes)\n", pkt->data_len);
		return;
	}

	k_mutex_lock(&devices_lock, K_FOREVER);
	struct device_entry *d = device_get(t.sender, true);

	if (d) {
		struct eep learned;
		uint16_t manufacturer;

		d->rorg = t.rorg;
		d->count++;
		d->dbm = t.dbm;
		d->last_ms = k_uptime_get();
		if (eep_4bs_teach_in(&t, &learned, &manufacturer)) {
			d->eep = learned;
		}
		known = d->eep;
	}
	k_mutex_unlock(&devices_lock);

	out("%08X %02X", t.sender, t.rorg);
	for (uint8_t i = 0; i < t.payload_len; i++) {
		out(i ? "%02X" : " %02X", t.payload[i]);
	}
	out(" st=%02X %ddBm", t.status, t.dbm);
	if (t.subtel > 1) {
		out(" x%u", t.subtel);
	}
	if (t.dest != EEP_ID_BROADCAST) {
		out(" to %08X", t.dest);
	}
	if (t.security) {
		out(" sec=%u", t.security);
	}

	const struct actuator *act = actuator_by_id(t.sender);

	if (act && act->kind == ACT_ELTAKO && t.rorg == EEP_RORG_RPS && t.payload_len == 1) {
		/* Eltako status: the "O" half of either rocker (0x30, 0x70) means on */
		out(" | %s %s\n", act->name, (t.payload[0] & 0x20) ? "on" : "off");
		return;
	}
	if (act && act->kind == ACT_D2_01 && t.rorg == EEP_RORG_VLD) {
		print_d2_01(act, &t);
		return;
	}

	switch (t.rorg) {
	case EEP_RORG_RPS:
		print_rocker(&t);
		break;
	case EEP_RORG_1BS:
		print_contact(&t);
		break;
	case EEP_RORG_4BS:
		print_4bs(&t, &known);
		break;
	case EEP_RORG_UTE:
		print_ute(&t);
		break;
	default:
		out("\n");
		break;
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
	if (d[0] != ESP3_RET_OK) {
		out("TCM: command %02X failed, return code %02X\n", cmd, d[0]);
		return;
	}

	switch (cmd) {
	case ESP3_CO_RD_VERSION:
		if (pkt->data_len >= 33) {
			char desc[17];

			memcpy(desc, &d[17], 16);
			desc[16] = '\0';
			out("TCM: app %u.%u.%u.%u, API %u.%u.%u.%u, chip ID %02X%02X%02X%02X, "
			       "\"%s\"\n",
			       d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11],
			       d[12], desc);
		}
		break;
	case ESP3_CO_RD_IDBASE:
		if (pkt->data_len >= 5) {
			base_id = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) |
				  ((uint32_t)d[3] << 8) | d[4];
			out("TCM: base ID %02X%02X%02X%02X", d[1], d[2], d[3], d[4]);
			if (pkt->opt_len >= 1) {
				out(", %u base ID changes left", esp3_opt(pkt)[0]);
			}
			out("\n");
		}
		break;
	default:
		out("TCM: OK\n");
		break;
	}
}

static void handle_packet(const struct esp3_packet *pkt)
{
	switch (pkt->type) {
	case ESP3_RADIO_ERP1:
		handle_radio(pkt);
		break;
	case ESP3_RESPONSE:
		handle_response(pkt);
		break;
	case ESP3_EVENT:
		out("TCM event %02X\n", pkt->data_len ? esp3_data(pkt)[0] : 0);
		break;
	default:
		out("ESP3 packet type %02X, %u + %u bytes\n", pkt->type, pkt->data_len,
		       pkt->opt_len);
		break;
	}
}

static struct esp3_decoder decoder;

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

K_THREAD_DEFINE(rx_tid, 2048, rx_thread, NULL, NULL, NULL, 5, 0, 0);

/* --- Shell --- */

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "asking the TCM for its version");
	return send_common_command(ESP3_CO_RD_VERSION);
}

static int cmd_idbase(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "asking the TCM for its base ID");
	return send_common_command(ESP3_CO_RD_IDBASE);
}

static int cmd_devices(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int64_t now = k_uptime_get();

	k_mutex_lock(&devices_lock, K_FOREVER);
	for (size_t i = 0; i < MAX_DEVICES; i++) {
		const struct device_entry *d = &devices[i];

		if (!d->id) {
			continue;
		}

		const struct actuator *act = actuator_by_id(d->id);

		if (act) {
			shell_print(sh, "%08X  %02X  %-13s %4u telegrams  %4d dBm  %us ago", d->id,
				    d->rorg, act->name, d->count, d->dbm,
				    (uint32_t)((now - d->last_ms) / 1000));
		} else if (d->eep.rorg) {
			shell_print(sh, "%08X  %02X  %02X-%02X-%02X  %4u telegrams  %4d dBm  %us ago",
				    d->id, d->rorg, d->eep.rorg, d->eep.func, d->eep.type, d->count,
				    d->dbm, (uint32_t)((now - d->last_ms) / 1000));
		} else {
			shell_print(sh, "%08X  %02X  ?           %4u telegrams  %4d dBm  %us ago",
				    d->id, d->rorg, d->count, d->dbm, (uint32_t)((now - d->last_ms) / 1000));
		}
	}
	k_mutex_unlock(&devices_lock);
	return 0;
}

static int cmd_profile(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char *end;
	uint32_t id = strtoul(argv[1], &end, 16);
	unsigned int rorg, func, type;

	if (*end || !id || sscanf(argv[2], "%x-%x-%x", &rorg, &func, &type) != 3 || rorg > 0xFF ||
	    func > 0xFF || type > 0xFF) {
		shell_error(sh, "usage: tcm profile <sender id, hex> <EEP, e.g. A5-02-05>");
		return -EINVAL;
	}

	k_mutex_lock(&devices_lock, K_FOREVER);
	struct device_entry *d = device_get(id, true);

	if (d) {
		d->eep = (struct eep){.rorg = rorg, .func = func, .type = type};
	}
	k_mutex_unlock(&devices_lock);

	if (!d) {
		shell_error(sh, "device table full");
		return -ENOMEM;
	}
	shell_print(sh, "%08X uses %02X-%02X-%02X", id, rorg, func, type);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Counters are only written by the receive thread; a slightly stale copy is fine. */
	struct esp3_stats s = decoder.stats;

	shell_print(sh, "packets %u, header CRC errors %u, data CRC errors %u, oversize %u, "
			"skipped bytes %u, UART overruns %u",
		    s.packets, s.header_crc_errors, s.data_crc_errors, s.oversize,
		    s.skipped_bytes, rx_overruns);
	return 0;
}

static int cmd_switch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const struct actuator *act = actuator_by_name(argv[1]);
	bool on = strcmp(argv[2], "on") == 0;
	bool status = strcmp(argv[2], "status") == 0;
	uint8_t frame[48];
	size_t n;

	if (!act || (!on && !status && strcmp(argv[2], "off") != 0)) {
		shell_error(sh, "usage: tcm switch <name> on|off|status");
		for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
			shell_print(sh, "  %s", actuators[i].name);
		}
		return -EINVAL;
	}
	if (!base_id) {
		shell_error(sh, "base ID not known yet; run tcm idbase");
		return -EAGAIN;
	}

	uint32_t sender = base_id + act->offset;

	if (act->kind == ACT_ELTAKO) {
		if (status) {
			shell_error(sh, "Eltako actuators report only when they switch");
			return -ENOTSUP;
		}
		n = eep_eltako_switch(sender, on, frame, sizeof(frame));
	} else if (status) {
		n = eep_d2_01_query_status(sender, act->id, EEP_D2_01_ALL_CHANNELS, frame,
					   sizeof(frame));
	} else {
		n = eep_d2_01_set_output(sender, act->id, 0, on ? 100 : 0, frame, sizeof(frame));
	}

	shell_print(sh, "%s %s from %08X", act->name, argv[2], sender);
	send_frame(frame, n, 0);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	tcm_cmds, SHELL_CMD(version, NULL, "Read the TCM's firmware version", cmd_version),
	SHELL_CMD(idbase, NULL, "Read the TCM's base ID (for sending)", cmd_idbase),
	SHELL_CMD(devices, NULL, "Senders heard, with their profile", cmd_devices),
	SHELL_CMD_ARG(profile, NULL, "<id> <EEP>: set a sender's profile, e.g. A5-02-05",
		      cmd_profile, 3, 0),
	SHELL_CMD_ARG(switch, NULL, "<name> on|off|status: switch a taught-in actuator",
		      cmd_switch, 3, 0),
	SHELL_CMD(stats, NULL, "ESP3 decoder counters", cmd_stats), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tcm, &tcm_cmds, "EnOcean TCM", NULL);

int main(void)
{
	if (!device_is_ready(tcm)) {
		out("TCM UART not ready\n");
		return 0;
	}

	esp3_decoder_init(&decoder);
	uart_irq_callback_user_data_set(tcm, uart_isr, NULL);
	uart_irq_rx_enable(tcm);

	out("EnOcean spike: listening on %s\n", tcm->name);

	/* The TCM needs about 100 ms after power-up before it answers. */
	k_msleep(200);
	send_common_command(ESP3_CO_RD_VERSION);
	k_msleep(200);
	send_common_command(ESP3_CO_RD_IDBASE);
	return 0;
}
