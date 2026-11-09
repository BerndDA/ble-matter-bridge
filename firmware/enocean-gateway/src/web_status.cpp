/*
 * Status JSON of the EnOcean gateway for the web page (src/web/index.html). See web_status.h.
 */

#include "web_status.h"

#include "bridge_manager.h"
#include "enocean_manager.h"
#include "matter_bridged_device.h"
#include "tcm.h"
#include "web/status_json.h"

#include <platform/CHIPDeviceLayer.h>

#include <zephyr/kernel.h>

#include <stdio.h>
#include <string.h>

using namespace chip;
using Gateway::Web::Json;
using Gateway::EnOceanManager;
using Gateway::EnOceanUserData;
using Nrf::MatterBridgedDevice;

namespace
{

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
	Gateway::Web::WriteCommonStatus(j);
	WriteTcm(j);
	WriteDevices(j);
	WriteSeen(j);
	j.Close('}');
	return j.Done();
}

} /* namespace Gateway */
