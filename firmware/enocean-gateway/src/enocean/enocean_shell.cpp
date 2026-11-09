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
	static EnOceanManager::SeenInfo seen[EnOceanManager::kMaxSeen];
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
		char hops[8] = "direct";

		if (seen[i].mRepeaters) {
			snprintk(hops, sizeof(hops), "%u hop%s", seen[i].mRepeaters, seen[i].mRepeaters > 1 ? "s" : "");
		}
		shell_print(sh, "%08X  %02X %-8s  %4d dBm  %-7s  %5u telegrams (%u direct, %u repeated)  %us ago",
			    seen[i].mId, seen[i].mRorg, data, seen[i].mDbm, hops, seen[i].mCount, seen[i].mCount - seen[i].mRepeatedCount, seen[i].mRepeatedCount,
			    SecondsAgo(seen[i].mLastSeenMs));
	}
	shell_print(sh, "%u sender(s) heard; dBm and hops of the last telegram (own telegrams through repeaters "
			"are left out)",
		    unsigned(n));
	return 0;
}

int CmdList(const struct shell *sh, size_t, char **)
{
	/* Static: too big for the shell stack (shell commands run one at a time). */
	static EnOceanManager::DeviceInfo devices[EnOceanManager::kMaxDevices];
	size_t n;

	{
		chip::DeviceLayer::StackLock lock;
		n = EnOceanManager::Instance().GetDevices(devices, ARRAY_SIZE(devices));
	}
	for (size_t i = 0; i < n; i++) {
		const auto &d = devices[i];
		char extra[40] = "";
		char kind[10];
		char sender[16];

		if (d.mHasPower) {
			snprintk(extra, sizeof(extra), ", %u W", d.mPowerW);
		} else if (d.mData.mContact == 0 &&
			   d.mContactsSeen == (EnOceanManager::kContact1 | EnOceanManager::kContact2)) {
			snprintk(extra, sizeof(extra), " (contact 1 %s, 2 %s)",
				 (d.mContacts & EnOceanManager::kContact1) ? "on" : "off",
				 (d.mContacts & EnOceanManager::kContact2) ? "on" : "off");
		}
		if (d.mStepping) {
			strncat(extra, ", stepping", sizeof(extra) - strlen(extra) - 1);
		}
		if (d.mData.mContact) {
			snprintk(kind, sizeof(kind), "lamp %u", d.mData.mContact);
			snprintk(sender, sizeof(sender), "base+%u/+%u", d.mData.mOffset, d.mData.mStepOffset);
		} else {
			snprintk(kind, sizeof(kind), "%s", d.mData.mKind == EnOceanUserData::kEltako ? "eltako" : "d2");
			snprintk(sender, sizeof(sender), "base+%u", d.mData.mOffset);
		}
		shell_print(sh, "%08X  endpoint %-3u %-6s %-5s sender %-11s %s%s", d.mData.mId, d.mEndpoint, kind,
			    d.mDeviceType == MatterBridgedDevice::DeviceType::OnOffPlugInUnit ? "plug" : "light",
			    sender, d.mStateKnown ? (d.mOn ? "on" : "off") : "state unknown", extra);
	}
	shell_print(sh, "Total: %u of max %u endpoint(s)", unsigned(n), unsigned(EnOceanManager::kMaxDevices));
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

int CmdLamps(const struct shell *sh, size_t argc, char **argv)
{
	/* Static: too big for the shell stack (shell commands run one at a time). */
	static EnOceanManager::DeviceInfo devices[EnOceanManager::kMaxDevices];
	const EnOceanManager::DeviceInfo *whole = nullptr;
	char name[MatterBridgedDevice::kNodeLabelSize - 2] = "";
	uint32_t id;
	char *end;

	if (!ParseId(sh, argv[1], id)) {
		return -EINVAL;
	}
	unsigned long step = strtoul(argv[2], &end, 10);
	if (*end || step > 127) {
		shell_error(sh, "Step offset must be 0..127 (sender taught in as series-switch pushbutton)");
		return -EINVAL;
	}
	for (size_t i = 3; i < argc; i++) {
		if (i > 3) {
			strncat(name, " ", sizeof(name) - strlen(name) - 1);
		}
		strncat(name, argv[i], sizeof(name) - strlen(name) - 1);
	}

	chip::DeviceLayer::StackLock lock;
	size_t n = EnOceanManager::Instance().GetDevices(devices, ARRAY_SIZE(devices));

	for (size_t i = 0; i < n; i++) {
		if (devices[i].mData.mId == id && devices[i].mData.mContact == 0) {
			whole = &devices[i];
		}
	}
	if (!whole || whole->mData.mKind != EnOceanUserData::kEltako) {
		shell_error(sh, "Add %08X as eltako first (its offset is the central on/off sender)", id);
		return -ENOENT;
	}
	for (uint8_t contact = 1; contact <= 2; contact++) {
		EnOceanUserData data = whole->mData;
		char label[MatterBridgedDevice::kNodeLabelSize];

		data.mContact = contact;
		data.mStepOffset = uint8_t(step);
		snprintk(label, sizeof(label), "%s %u", name, contact);

		CHIP_ERROR err = EnOceanManager::Instance().Add(data, whole->mDeviceType, label);
		if (err == CHIP_ERROR_DUPLICATE_KEY_ID) {
			shell_error(sh, "Lamp %u of %08X is already added", contact, id);
			continue;
		}
		if (err != CHIP_NO_ERROR) {
			shell_error(sh, "Add failed: %" CHIP_ERROR_FORMAT, err.Format());
			return -EIO;
		}
		shell_print(sh, "Added lamp %u of %08X as \"%s\"", contact, id, label);
	}
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

int CmdSwitch(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	const bool on = strcmp(argv[2], "on") == 0;
	uint8_t contact = 0;

	if (!ParseId(sh, argv[1], id)) {
		return -EINVAL;
	}
	if (argc > 3) {
		contact = strcmp(argv[3], "1") == 0 ? 1 : strcmp(argv[3], "2") == 0 ? 2 : 0xFF;
	}
	if ((!on && strcmp(argv[2], "off") != 0) || contact == 0xFF) {
		shell_error(sh, "Usage: enocean switch <id> on|off [1|2]");
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = EnOceanManager::Instance().Switch(id, on, contact);
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Switch failed: %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}
	return 0;
}

int CmdPress(const struct shell *sh, size_t, char **argv)
{
	char *end;
	unsigned long offset = strtoul(argv[1], &end, 10);

	if (*end || offset > 127) {
		shell_error(sh, "Offset must be 0..127 (sender ID = base ID + offset)");
		return -EINVAL;
	}

	CHIP_ERROR err;
	{
		chip::DeviceLayer::StackLock lock;
		err = EnOceanManager::Instance().Press(uint8_t(offset));
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "Press failed: %" CHIP_ERROR_FORMAT, err.Format());
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
	SHELL_CMD_ARG(lamps, NULL,
		      "<id> <step offset> <name...>: one endpoint per contact of a two-channel Eltako in series-switch "
		      "mode",
		      CmdLamps, 4, 10),
	SHELL_CMD_ARG(remove, NULL, "<id>: remove an actuator (and its lamps) from Matter", CmdRemove, 2, 0),
	SHELL_CMD_ARG(switch, NULL,
		      "<id> on|off [1|2]: switch an actuator, or one of its lamps (Matter follows its confirmation)",
		      CmdSwitch, 3, 1),
	SHELL_CMD_ARG(press, NULL,
		      "<offset>: send a rocker press from base ID + offset (teach-in, stepping a series switch)",
		      CmdPress, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(enocean, &sEnOceanCommands, "EnOcean gateway", NULL);
