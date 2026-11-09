/*
 * Read-only status page over HTTP. See web_server.h.
 *
 * All OpenThread calls and the connection table are under the OpenThread lock: the TCP callbacks
 * run in the OpenThread context with it held, the work items take it.
 */

#include "web_server.h"

#include <platform/CHIPDeviceLayer.h>

#include <openthread/tcp.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(web, LOG_LEVEL_INF);

namespace Gateway::Web
{
namespace
{
constexpr int64_t kTimeoutMs = 15000;
constexpr uint32_t kSweepMs = 2000;

enum class State : uint8_t {
	kFree,
	kReading,  /* waiting for the end of the request headers */
	kBuilding, /* the status JSON is being written on the Matter thread */
	kSending,  /* response queued, end of stream sent */
	kClosed,   /* disconnected, to be deinitialized */
};

struct Connection {
	otTcpEndpoint mEndpoint;
	uint8_t mReceiveBuffer[CONFIG_GATEWAY_WEB_RECEIVE_BUFFER];
	State mState;
	uint8_t mGeneration; /* tells a reused slot from the one a JSON build was started for */
	bool mHoldsJson;
	int64_t mStartMs;
	char mRequestLine[96];
	size_t mRequestLineLength;
	bool mRequestLineDone;
	uint32_t mLastBytes; /* the last 4 bytes received, to find "\r\n\r\n" */
	char mHead[224];
	otLinkedBuffer mHeadBuffer;
	otLinkedBuffer mBodyBuffer;
};

/* The app's src/web/index.html, gzip-compressed at build time (common.cmake) */
const uint8_t sPage[] = {
#include "index.html.gz.inc"
};

Connection sConnections[CONFIG_GATEWAY_WEB_CONNECTIONS];
otTcpListener sListener;
StatusWriter sWriter;

/* One JSON buffer: a second status request while it is in use gets 503 (the page retries). */
char sJson[CONFIG_GATEWAY_WEB_JSON_BUFFER];
bool sJsonBusy;

k_work_delayable sSweepWork;

class OtLock {
public:
	OtLock() { chip::DeviceLayer::ThreadStackMgr().LockThreadStack(); }
	~OtLock() { chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack(); }
};

void ReleaseJson(Connection &c)
{
	if (c.mHoldsJson) {
		c.mHoldsJson = false;
		sJsonBusy = false;
	}
}

void Send(Connection &c, const char *status, const char *type, const char *extraHeaders, const void *body,
	  size_t length)
{
	int n = snprintf(c.mHead, sizeof(c.mHead),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%sConnection: close\r\n\r\n",
			 status, type, unsigned(length), extraHeaders);

	/* One call per buffer: OpenThread takes no chains (mNext must be null). */
	c.mHeadBuffer = { nullptr, reinterpret_cast<const uint8_t *>(c.mHead), size_t(MIN(n, int(sizeof(c.mHead) - 1))) };
	c.mBodyBuffer = { nullptr, static_cast<const uint8_t *>(body), length };
	c.mState = State::kSending;
	if (otTcpSendByReference(&c.mEndpoint, &c.mHeadBuffer, length ? OT_TCP_SEND_MORE_TO_COME : 0) != OT_ERROR_NONE ||
	    (length && otTcpSendByReference(&c.mEndpoint, &c.mBodyBuffer, 0) != OT_ERROR_NONE) ||
	    otTcpSendEndOfStream(&c.mEndpoint) != OT_ERROR_NONE) {
		otTcpAbort(&c.mEndpoint);
		ReleaseJson(c);
		c.mState = State::kClosed;
		k_work_reschedule(&sSweepWork, K_NO_WAIT);
	}
}

void SendText(Connection &c, const char *status)
{
	Send(c, status, "text/plain", "", status, strlen(status));
}

/* Matter thread: write the JSON, then answer the connection it was asked for. */
void BuildStatus(intptr_t arg)
{
	const size_t index = size_t(arg) & 0xFF;
	const uint8_t generation = uint8_t(size_t(arg) >> 8);
	bool busy;
	size_t length = 0;

	{
		OtLock lock;
		busy = sJsonBusy;
		sJsonBusy = true;
	}
	if (!busy) {
		length = sWriter(sJson, sizeof(sJson));
	}

	OtLock lock;
	Connection &c = sConnections[index];

	if (c.mState != State::kBuilding || c.mGeneration != generation) {
		/* The connection went away meanwhile. */
		if (!busy) {
			sJsonBusy = false;
		}
		return;
	}
	if (busy) {
		SendText(c, "503 Service Unavailable");
	} else if (length == 0) {
		sJsonBusy = false;
		LOG_WRN("web: status JSON does not fit in %u bytes", unsigned(sizeof(sJson)));
		SendText(c, "500 Internal Server Error");
	} else {
		c.mHoldsJson = true;
		Send(c, "200 OK", "application/json", "Cache-Control: no-store\r\n", sJson, length);
	}
}

void Respond(Connection &c)
{
	char *line = c.mRequestLine;
	char *path = strchr(line, ' ');

	if (!path || strncmp(line, "GET ", 4) != 0) {
		SendText(c, "405 Method Not Allowed");
		return;
	}
	path++;
	path[strcspn(path, " ?")] = '\0';

	if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
		Send(c, "200 OK", "text/html; charset=utf-8", "Content-Encoding: gzip\r\nCache-Control: no-cache\r\n",
		     sPage, sizeof(sPage));
	} else if (strcmp(path, "/api/status") == 0) {
		c.mState = State::kBuilding;
		const intptr_t arg = intptr_t((size_t(c.mGeneration) << 8) | size_t(&c - sConnections));
		if (chip::DeviceLayer::PlatformMgr().ScheduleWork(BuildStatus, arg) != CHIP_NO_ERROR) {
			SendText(c, "503 Service Unavailable");
		}
	} else {
		SendText(c, "404 Not Found");
	}
}

/* --- OpenThread callbacks (OpenThread context, lock held) --- */

void OnReceiveAvailable(otTcpEndpoint *endpoint, size_t, bool endOfStream, size_t)
{
	Connection &c = *static_cast<Connection *>(otTcpEndpointGetContext(endpoint));
	const otLinkedBuffer *buffer;
	size_t total = 0;

	if (otTcpReceiveByReference(endpoint, &buffer) != OT_ERROR_NONE) {
		return;
	}
	bool headersDone = false;

	for (; buffer; buffer = buffer->mNext) {
		for (size_t i = 0; i < buffer->mLength; i++) {
			const char ch = char(buffer->mData[i]);

			if (!c.mRequestLineDone) {
				if (ch == '\r' || ch == '\n') {
					c.mRequestLineDone = true;
				} else if (c.mRequestLineLength < sizeof(c.mRequestLine) - 1) {
					c.mRequestLine[c.mRequestLineLength++] = ch;
				}
			}
			c.mLastBytes = (c.mLastBytes << 8) | uint8_t(ch);
			headersDone = headersDone || c.mLastBytes == 0x0D0A0D0A;
		}
		total += buffer->mLength;
	}
	/* Everything is read: closing with unread data would reset the connection. */
	otTcpCommitReceive(endpoint, total, 0);

	if (c.mState != State::kReading) {
		return;
	}
	if (headersDone) {
		c.mRequestLine[c.mRequestLineLength] = '\0';
		Respond(c);
	} else if (endOfStream) {
		otTcpAbort(endpoint);
		c.mState = State::kClosed;
		k_work_reschedule(&sSweepWork, K_NO_WAIT);
	}
}

void OnSendDone(otTcpEndpoint *endpoint, otLinkedBuffer *data)
{
	Connection &c = *static_cast<Connection *>(otTcpEndpointGetContext(endpoint));

	if (data == &c.mBodyBuffer) {
		ReleaseJson(c);
	}
}

void OnDisconnected(otTcpEndpoint *endpoint, otTcpDisconnectedReason)
{
	Connection &c = *static_cast<Connection *>(otTcpEndpointGetContext(endpoint));

	ReleaseJson(c);
	c.mState = State::kClosed;
	/* Deinitialize outside the callback. */
	k_work_reschedule(&sSweepWork, K_NO_WAIT);
}

otTcpIncomingConnectionAction OnAcceptReady(otTcpListener *, const otSockAddr *, otTcpEndpoint **acceptInto)
{
	for (auto &c : sConnections) {
		if (c.mState != State::kFree) {
			continue;
		}
		otTcpEndpointInitializeArgs args = {};

		args.mContext = &c;
		args.mSendDoneCallback = OnSendDone;
		args.mReceiveAvailableCallback = OnReceiveAvailable;
		args.mDisconnectedCallback = OnDisconnected;
		args.mReceiveBuffer = c.mReceiveBuffer;
		args.mReceiveBufferSize = sizeof(c.mReceiveBuffer);
		if (otTcpEndpointInitialize(openthread_get_default_instance(), &c.mEndpoint, &args) != OT_ERROR_NONE) {
			return OT_TCP_INCOMING_CONNECTION_ACTION_REFUSE;
		}
		c.mState = State::kReading;
		c.mGeneration++;
		c.mHoldsJson = false;
		c.mStartMs = k_uptime_get();
		c.mRequestLineLength = 0;
		c.mRequestLineDone = false;
		c.mLastBytes = 0;
		*acceptInto = &c.mEndpoint;
		k_work_reschedule(&sSweepWork, K_MSEC(kSweepMs));
		return OT_TCP_INCOMING_CONNECTION_ACTION_ACCEPT;
	}
	/* All busy: ignore the SYN, the client sends it again a moment later. */
	return OT_TCP_INCOMING_CONNECTION_ACTION_DEFER;
}

void OnAcceptDone(otTcpListener *, otTcpEndpoint *, const otSockAddr *) {}

/* System work queue: free closed connections, drop the ones that take too long. */
void Sweep(k_work *)
{
	const int64_t now = k_uptime_get();
	bool active = false;
	OtLock lock;

	for (auto &c : sConnections) {
		if (c.mState == State::kFree) {
			continue;
		}
		if (c.mState != State::kClosed && now - c.mStartMs > kTimeoutMs) {
			otTcpAbort(&c.mEndpoint);
			ReleaseJson(c);
			c.mState = State::kClosed;
		}
		if (c.mState == State::kClosed) {
			otTcpEndpointDeinitialize(&c.mEndpoint);
			c.mState = State::kFree;
		} else {
			active = true;
		}
	}
	if (active) {
		k_work_reschedule(&sSweepWork, K_MSEC(kSweepMs));
	}
}
} /* namespace */

void Init(StatusWriter writer)
{
	static bool sStarted;

	if (sStarted) {
		return;
	}
	sWriter = writer;
	k_work_init_delayable(&sSweepWork, Sweep);

	otTcpListenerInitializeArgs args = {};
	otSockAddr name = {};
	otError err;

	args.mAcceptReadyCallback = OnAcceptReady;
	args.mAcceptDoneCallback = OnAcceptDone;
	name.mPort = CONFIG_GATEWAY_WEB_PORT;
	{
		OtLock lock;
		err = otTcpListenerInitialize(openthread_get_default_instance(), &sListener, &args);
		if (err == OT_ERROR_NONE) {
			err = otTcpListen(&sListener, &name);
		}
	}
	if (err != OT_ERROR_NONE) {
		LOG_ERR("web: cannot listen on TCP port %u (%d)", CONFIG_GATEWAY_WEB_PORT, err);
		return;
	}
	sStarted = true;
	LOG_INF("web: status page on TCP port %u", CONFIG_GATEWAY_WEB_PORT);
}

} /* namespace Gateway::Web */
