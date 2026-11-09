/*
 * EnOcean actuators as Matter On/Off endpoints.
 *
 * Each actuator is taught in to one sender ID of the TCM (base ID + offset) and becomes one
 * endpoint: On/Off Light or On/Off Plug-in Unit. Supported kinds:
 *   - Eltako: switched like by a rocker (RPS press, 0x30 on / 0x10 off); confirms its state with
 *     F6 telegrams (0x30/0x70 on, 0x10/0x50 off).
 *   - D2-01: addressed VLD commands (set output, status query); answers with its status.
 *
 * A two-channel Eltako FMS61NP in series-switch mode (SS1: off, contact 1, contact 2, both) can
 * also get one endpoint per contact ("lamps"). It needs a second sender taught in as series-switch
 * pushbutton: the gateway presses it until the contacts are as wanted, and uses the central sender
 * (the actuator's offset) for all on and all off.
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

	/* Size of the first version (without mContact, mStepOffset); still loaded. */
	static constexpr size_t kV1Size = 6;

	uint32_t mId;
	uint8_t mKind;
	uint8_t mOffset;     /* sender ID = TCM base ID + offset; Eltako: central on/off */
	uint8_t mContact;    /* Eltako: 0 = the whole actuator, 1 or 2 = one contact (lamp) */
	uint8_t mStepOffset; /* Eltako lamps: sender taught in as series-switch pushbutton */
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
	/* Power of the load measured by a D2-01 actuator; reported to Matter if it changed. */
	void OnPower(uint32_t watts);

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

	/* Eltako confirmations: bit 0x40 of the F6 data picks the contact, 0x20 is on. A two-channel
	 * FMS61NP confirms both contacts on every change: 0x70/0x50 is contact 1, 0x30/0x10 contact 2. */
	static constexpr uint8_t kContact1 = 0x01;
	static constexpr uint8_t kContact2 = 0x02;

	struct DeviceInfo {
		EnOceanUserData mData;
		uint16_t mEndpoint;
		uint16_t mDeviceType;
		bool mStateKnown;
		bool mOn;
		uint8_t mContacts;     /* Eltako: contacts that are on, kContact1 | kContact2 */
		uint8_t mContactsSeen; /* Eltako: contacts that have confirmed since start */
		bool mStepping;        /* Eltako lamps: the gateway is stepping the actuator */
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
		uint8_t mRepeaters; /* repeater count of the last telegram (0 = direct) */
		uint32_t mCount;
		uint32_t mRepeatedCount; /* telegrams that came through a repeater */
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

	/* Sends on/off to an actuator (@p contact 0) or one of its lamps (1, 2); Matter is updated when
	 * it confirms. */
	CHIP_ERROR Switch(uint32_t id, bool on, uint8_t contact = 0);

	/* Sends a rocker press (F6 0x30, "on") from base ID + @p offset, without a target actuator: for
	 * teaching in a sender, or to step a series switch. */
	CHIP_ERROR Press(uint8_t offset);

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

	/* Per Eltako actuator (shared by its endpoints): contacts from the confirmations, stepping. */
	struct Actuator {
		uint32_t mId = 0;
		uint8_t mCentralOffset = 0;
		uint8_t mStepOffset = kNoStepOffset;
		uint8_t mContacts = 0;
		uint8_t mContactsSeen = 0;
		/* Stepping towards mTarget: one action (press or central on/off), then wait for both
		 * confirmations or a timeout, then the next action. */
		bool mBusy = false;
		uint8_t mTarget = 0;
		uint8_t mActions = 0;
		uint8_t mConfirms = 0;
		int64_t mSentMs = 0;
		int64_t mLastConfirmMs = 0;
	};
	static constexpr uint8_t kNoStepOffset = 0xFF;
	static constexpr uint8_t kMaxStepActions = 8;
	static constexpr uint32_t kStepTimeoutMs = 3000;
	static constexpr uint32_t kStepSettleMs = 300;

	static void OnRadio(const esp3_packet *pkt);
	static void StepTimerHandler(k_timer *timer);
	static void StepWork(intptr_t);
	static void ProcessTelegrams(intptr_t);
	static void QueryTimerHandler(k_timer *timer);
	static void QueryStates(intptr_t);

	void HandleTelegram(const Telegram &t);
	void UpdateSeen(const Telegram &t, int64_t now);
	CHIP_ERROR SendQuery(const EnOceanUserData &data);
	CHIP_ERROR SendPowerQuery(const EnOceanUserData &data);
	Device *Find(uint32_t id, uint8_t contact = 0);
	Device *FindFree();
	Actuator *FindActuator(uint32_t id);
	Actuator *AddActuator(const EnOceanUserData &data);
	void OnContact(Actuator &a, uint8_t data, int64_t now);
	void PublishContacts(const Actuator &a);
	CHIP_ERROR SetContacts(Actuator &a, uint8_t target);
	void Step(Actuator &a);
	CHIP_ERROR SendEltako(uint8_t offset, bool on);

	Device mDevices[kMaxDevices];
	Actuator mActuators[kMaxDevices];
	k_timer mStepTimer;
	SeenInfo mSeen[kMaxSeen] = {};
	/* Status queries to D2-01 actuators that haven't answered: every 30 s, doubling up to 10 min */
	static constexpr uint32_t kQueryIntervalMinS = 30;
	static constexpr uint32_t kQueryIntervalMaxS = 600;

	k_timer mQueryTimer;
	uint32_t mQueryIntervalS = kQueryIntervalMinS;
};

} /* namespace Gateway */
