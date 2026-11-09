/*
 * EnOcean actuators as Matter On/Off endpoints.
 *
 * Each actuator is taught in to one sender ID of the TCM (base ID + offset) and becomes one
 * endpoint: On/Off Light or On/Off Plug-in Unit. Supported kinds:
 *   - Eltako: switched like by a rocker (RPS press, 0x30 on / 0x10 off); confirms its state with
 *     F6 telegrams (0x30/0x70 on, 0x10/0x50 off).
 *   - D2-01: addressed VLD commands (set output, status query); answers with its status.
 *
 * Actuators are added in the shell (`enocean add`) and stored with the bridged devices, so they
 * keep their endpoints across restarts. All methods run on the Matter thread (shell: StackLock).
 */

#pragma once

#include "bridged_device_data_provider.h"

#include <lib/core/CHIPError.h>
#include <zephyr/kernel.h>

struct esp3_packet;

namespace Gateway
{

/* Implementation specific data stored with each bridged device (BridgeStorageManager user data). */
struct EnOceanUserData {
	enum Kind : uint8_t { kEltako = 1, kD2_01 = 2 };

	uint32_t mId;
	uint8_t mKind;
	uint8_t mOffset; /* sender ID = TCM base ID + offset */
} __packed;

class EnOceanProvider : public Nrf::BridgedDeviceDataProvider {
public:
	EnOceanProvider(const EnOceanUserData &data, UpdateAttributeCallback updateCallback);
	~EnOceanProvider() override;

	void Init() override {}
	void NotifyUpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
			       size_t dataSize) override;
	/* A write from Matter: sends the new on/off state to the actuator. */
	CHIP_ERROR UpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer) override;

	/* State reported by the actuator; reported to Matter if it changed (or the first time). */
	void OnState(bool on);

	const EnOceanUserData &GetData() const { return mData; }
	bool IsStateKnown() const { return mStateKnown; }
	bool GetOnOff() const { return mOnOff; }

private:
	EnOceanUserData mData;
	bool mOnOff = false;
	bool mStateKnown = false;
};

class EnOceanManager {
public:
	static constexpr size_t kMaxDevices = CONFIG_GATEWAY_ENOCEAN_MAX_DEVICES;
	static constexpr size_t kMaxSeen = CONFIG_GATEWAY_ENOCEAN_SEEN_MAX;

	struct DeviceInfo {
		EnOceanUserData mData;
		uint16_t mEndpoint;
		uint16_t mDeviceType;
		bool mStateKnown;
		bool mOn;
		bool mHasPower;
		uint32_t mPowerW;
		int8_t mDbm;
		int64_t mLastSeenMs; /* 0 = not heard since start */
	};

	struct SeenInfo {
		uint32_t mId;
		uint8_t mRorg;
		uint8_t mLen;
		uint8_t mPayload[4]; /* first bytes of the last telegram */
		int8_t mDbm;
		uint32_t mCount;
		int64_t mLastSeenMs;
	};

	/* A radio telegram, copied out of the TCM receive thread. */
	struct Telegram {
		uint32_t mSender;
		uint8_t mRorg;
		uint8_t mStatus;
		uint8_t mLen;
		int8_t mDbm;
		uint8_t mPayload[14];
	};

	static EnOceanManager &Instance()
	{
		static EnOceanManager sInstance;
		return sInstance;
	}

	/* Starts the TCM. Call after the bridge has restored the stored devices. */
	CHIP_ERROR Start();

	/* Restores one stored actuator (called while the bridge loads stored devices). */
	CHIP_ERROR Restore(const EnOceanUserData &data, uint16_t deviceType, const char *uniqueID,
			   const char *nodeLabel, uint8_t index, uint16_t endpointId);

	/* @p deviceType: MatterBridgedDevice::DeviceType::OnOffLight or OnOffPlugInUnit. */
	CHIP_ERROR Add(const EnOceanUserData &data, uint16_t deviceType, const char *label);
	CHIP_ERROR Remove(uint32_t id);

	/* Sends on/off to an actuator; Matter is updated when it confirms. */
	CHIP_ERROR Switch(uint32_t id, bool on);

	/* Sends on/off for a provider (from a Matter write). */
	CHIP_ERROR Send(const EnOceanProvider &provider, bool on);

	size_t GetDevices(DeviceInfo *out, size_t max) const;
	size_t GetSeen(SeenInfo *out, size_t max) const;

	/* Called from the provider destructor. */
	void OnProviderDeleted(EnOceanProvider *provider);

private:
	struct Device {
		EnOceanProvider *mProvider = nullptr;
		uint16_t mEndpoint = 0;
		bool mHasPower = false;
		uint32_t mPowerW = 0;
		int8_t mDbm = 0;
		int64_t mLastSeenMs = 0;
	};

	static void OnRadio(const esp3_packet *pkt);
	static void ProcessTelegrams(intptr_t);
	static void QueryTimerHandler(k_timer *timer);
	static void QueryStates(intptr_t);

	void HandleTelegram(const Telegram &t);
	void UpdateSeen(const Telegram &t, int64_t now);
	CHIP_ERROR SendQuery(const EnOceanUserData &data);
	Device *Find(uint32_t id);
	Device *FindFree();

	Device mDevices[kMaxDevices];
	SeenInfo mSeen[kMaxSeen] = {};
	k_timer mQueryTimer;
	bool mStatesQueried = false;
};

} /* namespace Gateway */
