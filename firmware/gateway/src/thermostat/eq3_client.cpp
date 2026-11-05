/*
 * On-demand GATT client for eQ-3 CC-RT-BLE thermostats.
 *
 * Bluetooth callbacks only record an event and submit one work item; the state
 * machine runs in that work item (system work queue), so steps never race.
 */

#include "eq3_client.h"

#include "sensor_scanner.h"

#include <bluetooth/gatt_dm.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(eq3_client, CONFIG_GATEWAY_EQ3_LOG_LEVEL);

namespace Gateway
{
namespace Eq3Client
{
namespace
{

/* 3e135142-654f-9090-134a-a6ff5bb77046 */
const bt_uuid_128 kServiceUuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x3e135142, 0x654f, 0x9090, 0x134a, 0xa6ff5bb77046));
/* 3fa4585a-ce4a-3bad-db4b-b8df8179ea09 */
const bt_uuid_128 kCommandUuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x3fa4585a, 0xce4a, 0x3bad, 0xdb4b, 0xb8df8179ea09));
/* d0e8434d-cd29-0996-af41-6c90f4e0eb2a */
const bt_uuid_128 kReplyUuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xd0e8434d, 0xcd29, 0x0996, 0xaf41, 0x6c90f4e0eb2a));

constexpr uint8_t kMaxAttempts = CONFIG_GATEWAY_EQ3_ATTEMPTS;
constexpr k_timeout_t kAttemptTimeout = K_SECONDS(CONFIG_GATEWAY_EQ3_ATTEMPT_TIMEOUT_S);
constexpr uint8_t kMaxCmdLen = 4;

struct Request {
	bt_addr_le_t mAddr;
	uint8_t mCmd[kMaxCmdLen];
	uint8_t mLen;
};

K_MSGQ_DEFINE(sQueue, sizeof(Request), CONFIG_GATEWAY_EQ3_QUEUE_SIZE, 4);

enum Event : uint32_t {
	kEvtStart = BIT(0),
	kEvtConnected = BIT(1),
	kEvtConnectFailed = BIT(2),
	kEvtDiscovered = BIT(3),
	kEvtFailed = BIT(4), /* discovery, subscribe or write failed */
	kEvtSubscribed = BIT(5),
	kEvtReply = BIT(6),
	kEvtDisconnected = BIT(7),
	kEvtTimeout = BIT(8),
};

enum class State : uint8_t { Idle, Connecting, Discovering, Subscribing, Writing, Disconnecting };

Eq3Callback sCallback;
atomic_t sEvents;
k_work sWork;
k_work_delayable sTimeoutWork;

/* Owned by the work item. */
State sState = State::Idle;
Request sRequest;
bool sHaveRequest;
uint8_t sAttempt;
bool sSucceeded;
bt_conn *sConn;

/* Written by callbacks before their event is raised, read by the work item afterwards. */
uint16_t sCommandHandle;
bt_gatt_subscribe_params sSubscribeParams;
bt_gatt_write_params sWriteParams;
Eq3Status sStatus;

void Raise(uint32_t event)
{
	atomic_or(&sEvents, event);
	k_work_submit(&sWork);
}

/* --- Bluetooth callbacks (BT RX thread / GATT DM work queue) ------------------------------------------------- */

void OnConnected(bt_conn *conn, uint8_t err)
{
	if (conn != sConn) {
		return;
	}
	if (err) {
		LOG_INF("Connect failed (HCI 0x%02x)", err);
	}
	Raise(err ? kEvtConnectFailed : kEvtConnected);
}

void OnDisconnected(bt_conn *conn, uint8_t reason)
{
	if (conn != sConn) {
		return;
	}
	LOG_DBG("Disconnected (reason 0x%02x)", reason);
	Raise(kEvtDisconnected);
}

BT_CONN_CB_DEFINE(eq3_conn_callbacks) = {
	.connected = OnConnected,
	.disconnected = OnDisconnected,
};

void OnDiscoveryCompleted(bt_gatt_dm *dm, void *)
{
	const bt_gatt_dm_attr *cmd = bt_gatt_dm_char_by_uuid(dm, &kCommandUuid.uuid);
	const bt_gatt_dm_attr *reply = bt_gatt_dm_char_by_uuid(dm, &kReplyUuid.uuid);
	const bt_gatt_dm_attr *cmdValue = cmd ? bt_gatt_dm_desc_by_uuid(dm, cmd, &kCommandUuid.uuid) : nullptr;
	const bt_gatt_dm_attr *replyValue = reply ? bt_gatt_dm_desc_by_uuid(dm, reply, &kReplyUuid.uuid) : nullptr;
	const bt_gatt_dm_attr *ccc = reply ? bt_gatt_dm_desc_by_uuid(dm, reply, BT_UUID_GATT_CCC) : nullptr;

	if (cmdValue && replyValue && ccc) {
		sCommandHandle = cmdValue->handle;
		sSubscribeParams.value_handle = replyValue->handle;
		sSubscribeParams.ccc_handle = ccc->handle;
	}
	bt_gatt_dm_data_release(dm);

	if (cmdValue && replyValue && ccc) {
		Raise(kEvtDiscovered);
	} else {
		LOG_WRN("Thermostat characteristics not found");
		Raise(kEvtFailed);
	}
}

void OnServiceNotFound(bt_conn *, void *)
{
	LOG_WRN("Thermostat service not found");
	Raise(kEvtFailed);
}

void OnDiscoveryError(bt_conn *, int err, void *)
{
	LOG_WRN("Discovery failed (%d)", err);
	Raise(kEvtFailed);
}

const bt_gatt_dm_cb kDiscoveryCallbacks = {
	.completed = OnDiscoveryCompleted,
	.service_not_found = OnServiceNotFound,
	.error_found = OnDiscoveryError,
};

uint8_t OnNotify(bt_conn *, bt_gatt_subscribe_params *, const void *data, uint16_t length)
{
	if (!data) {
		return BT_GATT_ITER_STOP; /* unsubscribed */
	}

	const uint8_t *d = static_cast<const uint8_t *>(data);

	/* Only the status shape is of interest: 02 01 <status> <valve%> <ui state> <target> */
	if (length >= 6 && d[0] == 0x02 && d[1] == 0x01) {
		sStatus.mMode = d[2] & 0x03;
		sStatus.mBoost = d[2] & BIT(2);
		sStatus.mWindowOpen = d[2] & BIT(4);
		sStatus.mLocked = d[2] & BIT(5);
		sStatus.mBatteryLow = d[2] & BIT(7);
		sStatus.mValvePct = d[3];
		sStatus.mTargetHalfDegrees = d[5];
		Raise(kEvtReply);
	}
	return BT_GATT_ITER_CONTINUE;
}

void OnSubscribed(bt_conn *, uint8_t err, bt_gatt_subscribe_params *)
{
	if (err) {
		LOG_WRN("Subscribe failed (ATT 0x%02x)", err);
	}
	Raise(err ? kEvtFailed : kEvtSubscribed);
}

void OnWritten(bt_conn *, uint8_t err, bt_gatt_write_params *)
{
	if (err) {
		LOG_WRN("Write failed (ATT 0x%02x)", err);
		Raise(kEvtFailed);
	}
	/* Success is signalled by the status reply. */
}

void OnTimeout(k_work *)
{
	Raise(kEvtTimeout);
}

/* --- State machine (system work queue) ------------------------------------------------------------------------ */

void Disconnect()
{
	k_work_cancel_delayable(&sTimeoutWork);
	sState = State::Disconnecting;
	if (bt_conn_disconnect(sConn, BT_HCI_ERR_REMOTE_USER_TERM_CONN) != 0) {
		/* Not connected (e.g. connection creation still pending): there will be no disconnected callback. */
		Raise(kEvtDisconnected);
	}
}

void StartAttempt()
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&sRequest.mAddr, addr, sizeof(addr));
	sAttempt++;
	LOG_INF("Command 0x%02x to %s, attempt %u", sRequest.mCmd[0], addr, sAttempt);

	/* The Bluetooth host allows no connection setup while the background scan runs. */
	sensor_scanner_pause(CONFIG_GATEWAY_EQ3_ATTEMPT_TIMEOUT_S * 1000 + 1000);

	/* 50 % duty while connecting: the thermostat advertises about once a second. */
	bt_conn_le_create_param createParam = BT_CONN_LE_CREATE_PARAM_INIT(
		BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW);
	createParam.timeout = CONFIG_GATEWAY_EQ3_CONNECT_TIMEOUT_MS / 10;
	const bt_le_conn_param connParam = BT_LE_CONN_PARAM_INIT(
		CONFIG_BRIDGE_BT_CONNECTION_INTERVAL_MIN, CONFIG_BRIDGE_BT_CONNECTION_INTERVAL_MAX,
		CONFIG_BRIDGE_BT_CONNECTION_LATENCY, CONFIG_BRIDGE_BT_CONNECTION_TIMEOUT);

	memset(&sSubscribeParams, 0, sizeof(sSubscribeParams));
	sConn = nullptr;
	sState = State::Connecting;
	k_work_reschedule(&sTimeoutWork, kAttemptTimeout);

	const int err = bt_conn_le_create(&sRequest.mAddr, &createParam, &connParam, &sConn);
	if (err) {
		LOG_WRN("Connection create failed (%d)", err);
		sConn = nullptr;
		Raise(kEvtConnectFailed);
	}
}

void FinishAttempt()
{
	k_work_cancel_delayable(&sTimeoutWork);
	if (sConn) {
		bt_conn_unref(sConn);
		sConn = nullptr;
	}
	sState = State::Idle;

	if (!sSucceeded && sAttempt < kMaxAttempts) {
		StartAttempt();
		return;
	}

	sensor_scanner_resume();
	if (!sSucceeded) {
		LOG_WRN("Command 0x%02x failed after %u attempts", sRequest.mCmd[0], sAttempt);
	}
	sHaveRequest = false;
	if (sCallback) {
		sCallback(sRequest.mAddr, sSucceeded, sSucceeded ? &sStatus : nullptr);
	}
	Raise(kEvtStart); /* next request, if any */
}

void HandleEvents(uint32_t events)
{
	if (sState == State::Idle) {
		if (!sHaveRequest && k_msgq_get(&sQueue, &sRequest, K_NO_WAIT) == 0) {
			sHaveRequest = true;
			sAttempt = 0;
			sSucceeded = false;
			StartAttempt();
		}
		return;
	}

	if (events & (kEvtDisconnected | kEvtConnectFailed)) {
		/* Ends every attempt, whatever state it was in. A connection cancelled while still being created
		 * ends with a failed connect rather than a disconnect. */
		FinishAttempt();
		return;
	}
	if (sState == State::Disconnecting) {
		return;
	}
	if (events & (kEvtTimeout | kEvtFailed)) {
		if (events & kEvtTimeout) {
			LOG_WRN("Attempt timed out (state %u)", unsigned(sState));
		}
		Disconnect();
		return;
	}

	switch (sState) {
	case State::Connecting:
		if (events & kEvtConnected) {
			sState = State::Discovering;
			const int err = bt_gatt_dm_start(sConn, &kServiceUuid.uuid, &kDiscoveryCallbacks, nullptr);
			if (err) {
				LOG_WRN("Discovery start failed (%d)", err);
				Disconnect();
			}
		}
		break;
	case State::Discovering:
		if (events & kEvtDiscovered) {
			sState = State::Subscribing;
			sSubscribeParams.notify = OnNotify;
			sSubscribeParams.subscribe = OnSubscribed;
			sSubscribeParams.value = BT_GATT_CCC_NOTIFY;
			const int err = bt_gatt_subscribe(sConn, &sSubscribeParams);
			if (err) {
				LOG_WRN("Subscribe start failed (%d)", err);
				Disconnect();
			}
		}
		break;
	case State::Subscribing:
		if (events & kEvtSubscribed) {
			sState = State::Writing;
			sWriteParams.func = OnWritten;
			sWriteParams.handle = sCommandHandle;
			sWriteParams.offset = 0;
			sWriteParams.data = sRequest.mCmd;
			sWriteParams.length = sRequest.mLen;
			const int err = bt_gatt_write(sConn, &sWriteParams);
			if (err) {
				LOG_WRN("Write start failed (%d)", err);
				Disconnect();
			}
		}
		break;
	case State::Writing:
		if (events & kEvtReply) {
			LOG_INF("Reply: mode %u, target %u.%u C, valve %u %%", sStatus.mMode,
				sStatus.mTargetHalfDegrees / 2, (sStatus.mTargetHalfDegrees % 2) * 5, sStatus.mValvePct);
			sSucceeded = true;
			Disconnect();
		}
		break;
	default:
		break;
	}
}

void WorkHandler(k_work *)
{
	uint32_t events;

	while ((events = atomic_clear(&sEvents)) != 0) {
		HandleEvents(events);
	}
}

} /* namespace */

void Init(Eq3Callback callback)
{
	sCallback = callback;
	k_work_init(&sWork, WorkHandler);
	k_work_init_delayable(&sTimeoutWork, OnTimeout);
}

int Send(const bt_addr_le_t &addr, const uint8_t *cmd, size_t len)
{
	Request request = {};

	if (len == 0 || len > kMaxCmdLen) {
		return -EINVAL;
	}
	bt_addr_le_copy(&request.mAddr, &addr);
	memcpy(request.mCmd, cmd, len);
	request.mLen = uint8_t(len);

	if (k_msgq_put(&sQueue, &request, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}
	Raise(kEvtStart);
	return 0;
}

int SetTarget(const bt_addr_le_t &addr, uint8_t halfDegrees)
{
	const uint8_t target = CLAMP(halfDegrees, kMinTargetHalfDegrees, kMaxTargetHalfDegrees);
	uint8_t cmd[2];

	if (IS_ENABLED(CONFIG_GATEWAY_EQ3_TARGET_SETS_MANUAL)) {
		cmd[0] = kCmdSetMode;
		cmd[1] = uint8_t(kModeManual << 6 | target);
	} else {
		cmd[0] = kCmdSetTarget;
		cmd[1] = target;
	}
	return Send(addr, cmd, sizeof(cmd));
}

bool IsBusy()
{
	return sHaveRequest || k_msgq_num_used_get(&sQueue) > 0;
}

} /* namespace Eq3Client */
} /* namespace Gateway */
