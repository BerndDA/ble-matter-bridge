/*
 * Gateway log output over UDP. See net_log.h.
 */

#include "net_log.h"

#include <platform/CHIPDeviceLayer.h>

#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/udp.h>

#include <hal/nrf_ficr.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
/* No extern "C" in this header */
extern "C" {
#include <zephyr/sys/printk-hooks.h>
}
#include <zephyr/sys/ring_buffer.h>

#include <app_version.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(net_log, LOG_LEVEL_INF);

namespace Gateway::NetLog
{
namespace
{
constexpr char kPrefix[] = "blegw-log ";
constexpr uint16_t kPort = CONFIG_GATEWAY_NET_LOG_PORT;
constexpr int64_t kLeaseMs = int64_t(CONFIG_GATEWAY_NET_LOG_LEASE_S) * 1000;
constexpr uint32_t kFlushMs = 100;
constexpr size_t kDatagram = CONFIG_GATEWAY_NET_LOG_DATAGRAM;

otUdpSocket sSocket;
bool sOpen;

/* Subscriber, written in the OpenThread context and read in the flush work, both with the OT lock. */
otIp6Address sPeerAddr;
uint16_t sPeerPort;
int64_t sLeaseUntilMs;

/* Captured output: written from any context (printk), read by the flush work. */
RING_BUF_DECLARE(sRing, CONFIG_GATEWAY_NET_LOG_BUFFER);
k_spinlock sRingLock;
atomic_t sActive;
bool sDebug;
bool sAtLineStart = true;
bool sDropLine;
uint32_t sDropped;

printk_hook_fn_t sConsoleHook;
k_work_delayable sFlushWork;

int Hook(int c)
{
	const int ret = sConsoleHook ? sConsoleHook(c) : c;

	if (!atomic_get(&sActive) || c == '\r') {
		return ret;
	}

	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	if (sAtLineStart) {
		/* Minimal log mode prefixes debug lines with "D: ". */
		sDropLine = !sDebug && c == 'D';
	}
	sAtLineStart = (c == '\n');
	if (!sDropLine) {
		const uint8_t byte = uint8_t(c);
		if (ring_buf_put(&sRing, &byte, 1) != 1) {
			sDropped++;
		}
	}
	k_spin_unlock(&sRingLock, key);
	return ret;
}

void Send(const otIp6Address &addr, uint16_t port, const void *data, size_t len)
{
	otInstance *instance = openthread_get_default_instance();
	otMessage *message = otUdpNewMessage(instance, nullptr);

	if (!message) {
		return;
	}
	if (otMessageAppend(message, data, uint16_t(len)) != OT_ERROR_NONE) {
		otMessageFree(message);
		return;
	}

	otMessageInfo info = {};
	info.mPeerAddr = addr;
	info.mPeerPort = port;
	if (otUdpSend(instance, &sSocket, message, &info) != OT_ERROR_NONE) {
		otMessageFree(message);
	}
}

void SendText(const otIp6Address &addr, uint16_t port, const char *text)
{
	Send(addr, port, text, strlen(text));
}

void Reset(bool debug)
{
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	ring_buf_reset(&sRing);
	sDropped = 0;
	sAtLineStart = true;
	sDropLine = false;
	sDebug = debug;
	k_spin_unlock(&sRingLock, key);
}

void Stop()
{
	atomic_set(&sActive, 0);
	Reset(sDebug);
}

void Flush(k_work *)
{
	uint8_t buf[kDatagram];
	size_t len = 0;
	uint32_t dropped;

	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	dropped = sDropped;
	sDropped = 0;
	if (dropped) {
		len = size_t(snprintf(reinterpret_cast<char *>(buf), sizeof(buf),
				      "[net log: %u bytes dropped]\n", unsigned(dropped)));
	}
	/* Under the lock, because a new subscription resets the ring. At most one datagram, so it's short. */
	len += ring_buf_get(&sRing, buf + len, sizeof(buf) - len);
	k_spin_unlock(&sRingLock, key);

	chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
	const bool expired = k_uptime_get() >= sLeaseUntilMs;
	if (!expired && len > 0) {
		Send(sPeerAddr, sPeerPort, buf, len);
	}
	chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();

	if (expired) {
		Stop();
		LOG_INF("net log: subscription expired");
		return;
	}
	k_work_reschedule(&sFlushWork, K_MSEC(kFlushMs));
}

bool IsCommand(const char *msg, const char *command)
{
	return strcmp(msg, command) == 0;
}

/* OpenThread context, OT lock held. */
void OnReceive(void *, otMessage *message, const otMessageInfo *info)
{
	char msg[48];
	uint16_t n = otMessageRead(message, otMessageGetOffset(message), msg, sizeof(msg) - 1);

	while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) {
		n--;
	}
	msg[n] = '\0';
	if (strncmp(msg, kPrefix, sizeof(kPrefix) - 1) != 0) {
		return;
	}
	const char *command = msg + sizeof(kPrefix) - 1;

	if (IsCommand(command, "hello")) {
		char reply[96];
		snprintf(reply, sizeof(reply), "%sgateway BLEGW-%08X%08X %s %s", kPrefix,
			 unsigned(nrf_ficr_deviceid_get(NRF_FICR, 1)), unsigned(nrf_ficr_deviceid_get(NRF_FICR, 0)),
			 APP_VERSION_STRING, CONFIG_BOARD);
		SendText(info->mPeerAddr, info->mPeerPort, reply);
	} else if (IsCommand(command, "subscribe") || IsCommand(command, "subscribe debug")) {
		const bool debug = IsCommand(command, "subscribe debug");
		const bool wasActive = atomic_get(&sActive);
		const bool samePeer = wasActive && sPeerPort == info->mPeerPort &&
				      memcmp(&sPeerAddr, &info->mPeerAddr, sizeof(sPeerAddr)) == 0;

		sPeerAddr = info->mPeerAddr;
		sPeerPort = info->mPeerPort;
		sLeaseUntilMs = k_uptime_get() + kLeaseMs;
		if (!samePeer || sDebug != debug) {
			Reset(debug);
		}
		atomic_set(&sActive, 1);
		if (!samePeer) {
			char addr[OT_IP6_ADDRESS_STRING_SIZE];
			otIp6AddressToString(&info->mPeerAddr, addr, sizeof(addr));
			/* Also the receiver's first line, which shows the path works. */
			LOG_INF("net log: [%s]:%u subscribed%s", addr, info->mPeerPort, debug ? " (debug)" : "");
		}
		SendText(info->mPeerAddr, info->mPeerPort, "blegw-log ok");
		if (!wasActive) {
			k_work_reschedule(&sFlushWork, K_NO_WAIT);
		}
	} else if (IsCommand(command, "stop")) {
		Stop();
		LOG_INF("net log: unsubscribed");
		SendText(info->mPeerAddr, info->mPeerPort, "blegw-log stopped");
	}
}
} /* namespace */

void Init()
{
	if (sOpen) {
		return;
	}
	k_work_init_delayable(&sFlushWork, Flush);

	otInstance *instance = openthread_get_default_instance();
	otSockAddr name = {};
	name.mPort = kPort;

	chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
	otError err = otUdpOpen(instance, &sSocket, OnReceive, nullptr);
	if (err == OT_ERROR_NONE) {
		err = otUdpBind(instance, &sSocket, &name, OT_NETIF_THREAD);
		if (err != OT_ERROR_NONE) {
			otUdpClose(instance, &sSocket);
		}
	}
	chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();

	if (err != OT_ERROR_NONE) {
		printk("net log: cannot open UDP port %u (%d)\n", kPort, err);
		return;
	}
	sOpen = true;
	sConsoleHook = __printk_get_hook();
	__printk_hook_install(Hook);
}

} /* namespace Gateway::NetLog */
