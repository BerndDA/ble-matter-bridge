/*
 * Status JSON of the EnOcean gateway for the web page (src/web/index.html). See web_status.h.
 */

#include "web_status.h"

#include "bridge_manager.h"
#include "enocean_manager.h"
#include "matter_bridged_device.h"
#include "tcm.h"

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DiagnosticDataProvider.h>

#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <zephyr/kernel.h>
#include <zephyr/net/openthread.h>

#include <app_version.h>
#include <ncs_version.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using namespace chip;
using Gateway::EnOceanManager;
using Gateway::EnOceanUserData;
using Nrf::MatterBridgedDevice;

namespace
{

/* Appends to a fixed buffer; remembers an overflow instead of failing on every call. */
class Json {
public:
	Json(char *buf, size_t size) : mBuf(buf), mSize(size) {}

	void Raw(const char *fmt, ...)
	{
		if (mOverflow) {
			return;
		}
		va_list args;
		va_start(args, fmt);
		const int n = vsnprintf(mBuf + mLength, mSize - mLength, fmt, args);
		va_end(args);
		if (n < 0 || size_t(n) >= mSize - mLength) {
			mOverflow = true;
			return;
		}
		mLength += size_t(n);
	}

	/* "key": with a comma before it unless it opens the object or array. */
	void Key(const char *key)
	{
		Comma();
		Raw("\"%s\":", key);
		mFirst = true;
	}

	void Str(const char *key, const char *value, size_t length = SIZE_MAX)
	{
		if (key) {
			Key(key);
		}
		Comma();
		Raw("\"");
		for (size_t i = 0; i < length && value[i]; i++) {
			const unsigned char c = value[i];
			if (c == '"' || c == '\\') {
				Raw("\\%c", c);
			} else if (c < 0x20) {
				Raw("\\u%04x", c);
			} else {
				Raw("%c", c);
			}
		}
		Raw("\"");
		mFirst = false;
	}

	void Int(const char *key, long long value)
	{
		Key(key);
		Raw("%lld", value);
		mFirst = false;
	}

	void Bool(const char *key, bool value)
	{
		Key(key);
		Raw(value ? "true" : "false");
		mFirst = false;
	}

	void Null(const char *key)
	{
		Key(key);
		Raw("null");
		mFirst = false;
	}

	/* Opens an object or array; @p key is null inside an array. */
	void Open(const char *key, char bracket)
	{
		if (key) {
			Key(key);
		}
		Comma();
		Raw("%c", bracket);
		mFirst = true;
	}

	void Close(char bracket)
	{
		Raw("%c", bracket);
		mFirst = false;
	}

	size_t Done() const { return mOverflow ? 0 : mLength; }

private:
	void Comma()
	{
		if (!mFirst) {
			Raw(",");
		}
		mFirst = true;
	}

	char *mBuf;
	size_t mSize;
	size_t mLength = 0;
	bool mOverflow = false;
	bool mFirst = true;
};

const char *BootReason(DeviceLayer::BootReasonType reason)
{
	using Reason = DeviceLayer::BootReasonType;

	switch (reason) {
	case Reason::kPowerOnReboot:
		return "power on";
	case Reason::kBrownOutReset:
		return "brown-out";
	case Reason::kSoftwareWatchdogReset:
		return "software watchdog";
	case Reason::kHardwareWatchdogReset:
		return "hardware watchdog";
	case Reason::kSoftwareUpdateCompleted:
		return "software update";
	case Reason::kSoftwareReset:
		return "software reset";
	default:
		return "unknown";
	}
}

void WriteSystem(Json &j)
{
	auto &diag = DeviceLayer::GetDiagnosticDataProvider();
	uint64_t heapFree = 0, heapUsed = 0, heapHigh = 0;
	uint16_t rebootCount = 0;
	DeviceLayer::BootReasonType reason = DeviceLayer::BootReasonType::kUnspecified;

	diag.GetCurrentHeapFree(heapFree);
	diag.GetCurrentHeapUsed(heapUsed);
	diag.GetCurrentHeapHighWatermark(heapHigh);
	diag.GetRebootCount(rebootCount);
	diag.GetBootReason(reason);

	j.Open("system", '{');
	j.Str("version", APP_VERSION_STRING);
	j.Str("sdk", NCS_VERSION_STRING);
	j.Str("board", CONFIG_BOARD);
	j.Int("uptimeS", k_uptime_get() / 1000);
	j.Str("bootReason", BootReason(reason));
	j.Int("rebootCount", rebootCount);
	j.Int("heapFree", heapFree);
	j.Int("heapUsed", heapUsed);
	j.Int("heapHigh", heapHigh);
	j.Close('}');
}

void WriteThread(Json &j)
{
	otInstance *ot = openthread_get_default_instance();
	char text[OT_IP6_ADDRESS_STRING_SIZE];

	DeviceLayer::ThreadStackMgr().LockThreadStack();
	const otDeviceRole role = otThreadGetDeviceRole(ot);

	j.Open("thread", '{');
	j.Str("role", otThreadDeviceRoleToString(role));
	j.Str("network", otThreadGetNetworkName(ot));
	j.Int("channel", otLinkGetChannel(ot));
	snprintf(text, sizeof(text), "%04x", otLinkGetPanId(ot));
	j.Str("panId", text);
	snprintf(text, sizeof(text), "%04x", otThreadGetRloc16(ot));
	j.Str("rloc16", text);
	snprintf(text, sizeof(text), "%08x", unsigned(otThreadGetPartitionId(ot)));
	j.Str("partition", text);

	int8_t rssi;
	otRouterInfo parent;

	if (role == OT_DEVICE_ROLE_CHILD && otThreadGetParentAverageRssi(ot, &rssi) == OT_ERROR_NONE &&
	    otThreadGetParentInfo(ot, &parent) == OT_ERROR_NONE) {
		j.Int("parentRssi", rssi);
		j.Int("parentLqi", parent.mLinkQualityIn);
		snprintf(text, sizeof(text), "%04x", parent.mRloc16);
		j.Str("parentRloc16", text);
	} else {
		j.Null("parentRssi");
	}

	/* A router has no parent: its neighbors and the strongest link instead. */
	otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
	otNeighborInfo neighbor;
	int neighbors = 0;
	int8_t best = -128;

	while (otThreadGetNextNeighborInfo(ot, &it, &neighbor) == OT_ERROR_NONE) {
		neighbors++;
		best = MAX(best, neighbor.mAverageRssi);
	}
	j.Int("neighbors", neighbors);
	if (neighbors) {
		j.Int("bestRssi", best);
	} else {
		j.Null("bestRssi");
	}

	/* The addresses a browser can use: not link-local, mesh-local or RLOC. */
	j.Open("addresses", '[');
	for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a; a = a->mNext) {
		if (a->mRloc || a->mMeshLocal || (a->mAddress.mFields.m8[0] == 0xFE && (a->mAddress.mFields.m8[1] & 0xC0) == 0x80)) {
			continue;
		}
		otIp6AddressToString(&a->mAddress, text, sizeof(text));
		j.Str(nullptr, text);
	}
	j.Close(']');
	j.Close('}');
	DeviceLayer::ThreadStackMgr().UnlockThreadStack();
}

void WriteMatter(Json &j)
{
	Server &server = Server::GetInstance();
	char text[20];

	j.Open("matter", '{');
	j.Bool("commissioningOpen", server.GetCommissioningWindowManager().IsCommissioningWindowOpen());
	j.Int("subscriptions", app::InteractionModelEngine::GetInstance()->GetNumActiveReadHandlers(
				       app::ReadHandler::InteractionType::Subscribe));
	j.Open("fabrics", '[');
	for (const auto &fabric : server.GetFabricTable()) {
		const CharSpan label = fabric.GetFabricLabel();

		j.Open(nullptr, '{');
		j.Int("index", fabric.GetFabricIndex());
		j.Int("vendorId", int(fabric.GetVendorId()));
		snprintf(text, sizeof(text), "%08X%08X", unsigned(fabric.GetNodeId() >> 32), unsigned(fabric.GetNodeId()));
		j.Str("nodeId", text);
		j.Str("label", label.data(), label.size());
		j.Close('}');
	}
	j.Close(']');
	j.Close('}');
}

void WriteTcm(Json &j)
{
	tcm_info info;
	char text[16];

	tcm_get_info(&info);
	j.Open("tcm", '{');
	j.Bool("ok", info.base_id != 0);
	snprintf(text, sizeof(text), "%u.%u.%u.%u", info.app_version[0], info.app_version[1], info.app_version[2],
		 info.app_version[3]);
	j.Str("app", text);
	snprintf(text, sizeof(text), "%08X", unsigned(info.base_id));
	j.Str("baseId", text);
	j.Int("changesLeft", info.base_id_changes_left);
	j.Int("received", info.stats.packets);
	j.Int("crcErrors", info.stats.header_crc_errors + info.stats.data_crc_errors);
	j.Int("overruns", info.rx_overruns);
	j.Int("sent", info.sent);
	j.Int("sendErrors", info.send_errors);
	j.Close('}');
}

void WriteDevices(Json &j)
{
	static EnOceanManager::DeviceInfo devices[EnOceanManager::kMaxDevices];
	const size_t n = EnOceanManager::Instance().GetDevices(devices, ARRAY_SIZE(devices));
	const int64_t now = k_uptime_get();
	char text[12];

	j.Open("devices", '[');
	for (size_t i = 0; i < n; i++) {
		const auto &d = devices[i];
		uint8_t index;
		const MatterBridgedDevice *device = Nrf::BridgeManager::Instance().GetDevice(d.mEndpoint, index);

		j.Open(nullptr, '{');
		snprintf(text, sizeof(text), "%08X", unsigned(d.mData.mId));
		j.Str("id", text);
		j.Int("endpoint", d.mEndpoint);
		j.Str("name", device ? device->GetNodeLabel() : "");
		j.Str("kind", d.mData.mKind == EnOceanUserData::kEltako ? "eltako" : "d2");
		j.Str("type", d.mDeviceType == MatterBridgedDevice::DeviceType::OnOffPlugInUnit ? "plug" : "light");
		j.Int("lamp", d.mData.mContact);
		j.Int("offset", d.mData.mOffset);
		if (d.mData.mContact) {
			j.Int("stepOffset", d.mData.mStepOffset);
		}
		if (d.mStateKnown) {
			j.Bool("on", d.mOn);
		} else {
			j.Null("on");
		}
		j.Int("contacts", d.mContacts);
		j.Int("contactsSeen", d.mContactsSeen);
		j.Bool("stepping", d.mStepping);
		if (d.mHasPower) {
			j.Int("powerW", d.mPowerW);
		} else {
			j.Null("powerW");
		}
		if (d.mLastSeenMs) {
			j.Int("dbm", d.mDbm);
			j.Int("lastSeenS", (now - d.mLastSeenMs) / 1000);
		} else {
			j.Null("lastSeenS");
		}
		j.Close('}');
	}
	j.Close(']');
}

void WriteSeen(Json &j)
{
	static EnOceanManager::SeenInfo seen[EnOceanManager::kMaxSeen];
	const size_t n = EnOceanManager::Instance().GetSeen(seen, ARRAY_SIZE(seen));
	const int64_t now = k_uptime_get();
	char text[12];

	j.Open("seen", '[');
	for (size_t i = 0; i < n; i++) {
		const auto &s = seen[i];

		j.Open(nullptr, '{');
		snprintf(text, sizeof(text), "%08X", unsigned(s.mId));
		j.Str("id", text);
		snprintf(text, sizeof(text), "%02X", s.mRorg);
		j.Str("rorg", text);
		for (uint8_t b = 0; b < s.mLen && b < 4; b++) {
			snprintf(&text[2 * b], sizeof(text) - 2 * b, "%02X", s.mPayload[b]);
		}
		j.Str("data", s.mLen ? text : "");
		j.Int("dbm", s.mDbm);
		j.Int("hops", s.mRepeaters);
		j.Int("count", s.mCount);
		j.Int("repeated", s.mRepeatedCount);
		j.Int("agoS", (now - s.mLastSeenMs) / 1000);
		j.Close('}');
	}
	j.Close(']');
}

} /* namespace */

namespace Gateway
{

size_t WriteWebStatus(char *buf, size_t size)
{
	Json j(buf, size);

	j.Open(nullptr, '{');
	WriteSystem(j);
	WriteThread(j);
	WriteMatter(j);
	WriteTcm(j);
	WriteDevices(j);
	WriteSeen(j);
	j.Close('}');
	return j.Done();
}

} /* namespace Gateway */
