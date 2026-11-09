/*
 * Shell commands of the EnOcean gateway: `enocean ...`.
 */

#include "enocean_manager.h"
#include "matter_bridged_device.h"
#include "tcm.h"

#include <platform/CHIPDeviceLayer.h>
#include <zephyr/shell/shell.h>

#include <stdlib.h>
#include <string.h>

using namespace Gateway;
using Nrf::MatterBridgedDevice;

namespace
{

bool ParseId(const struct shell *sh, const char *arg, uint32_t &id)
{
	char *end;
	unsigned long v = strtoul(arg, &end, 16);

	if (*end || v == 0 || v > 0xFFFFFFFFul || strlen(arg) > 8) {
		shell_error(sh, "Invalid EnOcean ID: %s (8 hex digits, e.g. 0188EA75)", arg);
		return false;
	}
	id = uint32_t(v);
	return true;
}

uint32_t SecondsAgo(int64_t ms)
{
	return uint32_t((k_uptime_get() - ms) / 1000);
}

int CmdInfo(const struct shell *sh, size_t, char **)
{
	tcm_info info;

	tcm_get_info(&info);
	if (info.base_id) {
		shell_print(sh, "TCM:        app %u.%u.%u.%u, API %u.%u.%u.%u, chip ID %08X, \"%s\"", info.app_version[0],
			    info.app_version[1], info.app_version[2], info.app_version[3], info.api_version[0],
			    info.api_version[1], info.api_version[2], info.api_version[3], info.chip_id,
			    info.description);
		shell_print(sh, "Base ID:    %08X (senders %08X..%08X), %u changes left", info.base_id, info.base_id,
			    info.base_id + 127, info.base_id_changes_left);
	} else {
		shell_print(sh, "TCM:        no answer yet (base ID unknown, the gateway cannot send)");
	}
	shell_print(sh, "Received:   %u packets, CRC errors %u + %u, oversize %u, skipped bytes %u, overruns %u",
		    info.stats.packets, info.stats.header_crc_errors, info.stats.data_crc_errors, info.stats.oversize,
		    info.stats.skipped_bytes, info.rx_overruns);
	shell_print(sh, "Sent:       %u telegrams, %u errors", info.sent, info.send_errors);
	return 0;
}

int CmdSeen(const struct shell *sh, size_t, char **)
{
	EnOceanManager::SeenInfo seen[EnOceanManager::kMaxSeen];
	size_t n;

	{
		chip::DeviceLayer::StackLock lock;
		n = EnOceanManager::Instance().GetSeen(seen, ARRAY_SIZE(seen));
	}
	for (size_t i = 0; i < n; i++) {
		char data[9] = "";

		for (uint8_t j = 0; j < seen[i].mLen; j++) {
			snprintk(&data[2 * j], sizeof(data) - 2 * j, "%02X", seen[i].mPayload[j]);
		}
		shell_print(sh, "%08X  %02X %-8s  %4d dBm  %5u telegrams  %us ago", seen[i].mId, seen[i].mRorg, data,
			    seen[i].mDbm, seen[i].mCount, SecondsAgo(seen[i].mLastSeenMs));
	}
	shell_print(sh, "%u sender(s) heard (own telegrams through repeaters are left out)", unsigned(n));
	return 0;
}

int CmdList(const struct shell *sh, size_t, char **)
{
	EnOceanManager::DeviceInfo devices[EnOceanManager::kMaxDevices];
	size_t n;

	{
		chip::DeviceLayer::StackLock lock;
		n = EnOceanManager::Instance().GetDevices(devices, ARRAY_SIZE(devices));
	}
	for (size_t i = 0; i < n; i++) {
		const auto &d = devices[i];
		char power[16] = "";

		if (d.mHasPower) {
			snprintk(power, sizeof(power), ", %u W", d.mPowerW);
		}
		shell_print(sh, "%08X  endpoint %-3u %-6s %-6s sender base+%-3u  %s%s", d.mData.mId, d.mEndpoint,
			    d.mData.mKind == EnOceanUserData::kEltako ? "eltako" : "d2",
			    d.mDeviceType == MatterBridgedDevice::DeviceType::OnOffPlugInUnit ? "plug" : "light",
			    d.mData.mOffset, d.mStateKnown ? (d.mOn ? "on" : "off") : "state unknown", power);
	}
	shell_print(sh, "Total: %u of max %u actuator(s)", unsigned(n), unsigned(EnOceanManager::kMaxDevices));
	return 0;
}

int CmdAdd(const struct shell *sh, size_t argc, char **argv)
{
	EnOceanUserData data = {};
	uint16_t deviceType;
	char *end;
	char label[MatterBridgedDevice::kNodeLabelSize] = "";

	uint32_t id;

	if (!ParseId(sh, argv[1], id)) {
		return -EINVAL;
	}
	data.mId = id;
	if (strcmp(argv[2], "eltako") == 0) {
		data.mKind = EnOceanUserData::kEltako;
	} else if (strcmp(argv[2], "d2") == 0) {
		data.mKind = EnOceanUserData::kD2_01;
	} else {
		shell_error(sh, "Kind must be eltako or d2");
		return -EINVAL;
	}
	unsigned long offset = strtoul(argv[3], &end, 10);
	if (*end || offset > 127) {
		shell_error(sh, "Offset must be 0..127 (sender ID = base ID + offset, as taught in)");
		return -EINVAL;
	}
	data.mOffset = uint8_t(offset);
	if (strcmp(argv[4], "light") == 0) {
		deviceType = MatterBridgedDevice::DeviceType::OnOffLight;
	} else if (strcmp(argv[4], "plug") == 0) {
		deviceType = MatterBridgedDevice::DeviceType::OnOffPlugInUnit;
	} else {
		shell_error(sh, "Type must be light or plug");
		return -EINVAL;
	}
	/* The rest is the name, spaces included. */
	for (size_t i = 5; i < argc; i++) {
		if (i > 5) {
			strncat(label, " ", sizeof(label) - strlen(label) - 1);
		}
		strncat(label, argv[i], sizeof(label) - strlen(label) - 1);
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = EnOceanManager::Instance().Add(data, deviceType, label);
	}
	if (err == CHIP_ERROR_DUPLICATE_KEY_ID) {
		shell_error(sh, "%08X is already added", data.mId);
		return -EEXIST;
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Add failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	shell_print(sh, "Added %08X as \"%s\"", data.mId, label);
	return 0;
}

int CmdRemove(const struct shell *sh, size_t, char **argv)
{
	uint32_t id;

	if (!ParseId(sh, argv[1], id)) {
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = EnOceanManager::Instance().Remove(id);
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Remove failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	shell_print(sh, "Removed %08X", id);
	return 0;
}

int CmdSwitch(const struct shell *sh, size_t, char **argv)
{
	uint32_t id;
	const bool on = strcmp(argv[2], "on") == 0;

	if (!ParseId(sh, argv[1], id)) {
		return -EINVAL;
	}
	if (!on && strcmp(argv[2], "off") != 0) {
		shell_error(sh, "Usage: enocean switch <id> on|off");
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = EnOceanManager::Instance().Switch(id, on);
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Switch failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	return 0;
}

} /* namespace */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sEnOceanCommands, SHELL_CMD(info, NULL, "TCM version, base ID and counters", CmdInfo),
	SHELL_CMD(seen, NULL, "Senders heard since start", CmdSeen),
	SHELL_CMD(list, NULL, "Actuators bridged to Matter", CmdList),
	SHELL_CMD_ARG(add, NULL,
		      "<id> eltako|d2 <offset> light|plug <name...>: bridge an actuator taught in to base ID + offset",
		      CmdAdd, 6, 10),
	SHELL_CMD_ARG(remove, NULL, "<id>: remove an actuator from Matter", CmdRemove, 2, 0),
	SHELL_CMD_ARG(switch, NULL, "<id> on|off: switch an actuator (Matter follows its confirmation)", CmdSwitch,
		      3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(enocean, &sEnOceanCommands, "EnOcean gateway", NULL);
