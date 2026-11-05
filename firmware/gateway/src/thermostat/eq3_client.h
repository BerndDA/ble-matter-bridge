/*
 * On-demand GATT client for eQ-3 CC-RT-BLE thermostats (stock command set,
 * tested with eq3-custom-fw 2.02; protocol: PROTOCOL.md in that repository).
 *
 * Each command is one short connection: connect, discover the thermostat
 * service, subscribe to replies, write the command, wait for the status reply,
 * disconnect. Commands run one at a time and are retried a few times. The
 * background sensor scan pauses while a connection is being set up.
 *
 * The pairing PIN must be off on the thermostat (the default on eq3-custom-fw).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>

namespace Gateway
{

/* The status reply: 02 01 <status> <valve%> <ui state> <target> ... */
struct Eq3Status {
	uint8_t mMode; /* 0 auto, 1 manual, 2 holiday */
	bool mBoost;
	bool mWindowOpen;
	bool mLocked;
	bool mBatteryLow;
	uint8_t mValvePct;
	uint8_t mTargetHalfDegrees; /* 9 = 4.5 degC (off) .. 60 = 30.0 degC (on) */
};

/*
 * Called once per command from the system work queue: @p ok with the status the
 * thermostat answered, or !ok (status nullptr) after all attempts failed.
 */
using Eq3Callback = void (*)(const bt_addr_le_t &addr, bool ok, const Eq3Status *status);

namespace Eq3Client
{

constexpr uint8_t kCmdSetTarget = 0x41; /* 41 <temp x 2>: target only, mode unchanged */
constexpr uint8_t kCmdSetMode = 0x40; /* 40 <mode << 6 | temp x 2>: mode and target in one byte */
constexpr uint8_t kModeManual = 1;
constexpr uint8_t kMinTargetHalfDegrees = 9;
constexpr uint8_t kMaxTargetHalfDegrees = 60;

/* Sets the callback for finished commands. Call once before Send. */
void Init(Eq3Callback callback);

/*
 * Queues a command (at most 4 bytes, every argument included). Returns 0, or
 * -ENOMEM if the queue is full, -EINVAL for a bad length.
 */
int Send(const bt_addr_le_t &addr, const uint8_t *cmd, size_t len);

/*
 * Sets the target. With CONFIG_GATEWAY_EQ3_TARGET_SETS_MANUAL it also switches
 * the thermostat to manual mode (40 <0x40 | halfDegrees>), otherwise it keeps
 * the mode (41 <halfDegrees>; in auto mode the target then holds only until the
 * schedule's next switch point).
 */
int SetTarget(const bt_addr_le_t &addr, uint8_t halfDegrees);

/* True while a command is queued or running. */
bool IsBusy();

} /* namespace Eq3Client */
} /* namespace Gateway */
