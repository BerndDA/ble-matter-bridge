/*
 * Shared sections of the status JSON. See status_json.h.
 */

#include "status_json.h"

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

#include <string.h>

using namespace chip;

namespace Gateway::Web
{
namespace
{

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

} /* namespace */

void WriteCommonStatus(Json &j)
{
	WriteSystem(j);
	WriteThread(j);
	WriteMatter(j);
}

} /* namespace Gateway::Web */
