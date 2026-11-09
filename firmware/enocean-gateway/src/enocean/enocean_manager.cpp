/*
 * EnOcean actuators as Matter On/Off endpoints, see enocean_manager.h.
 */

#include "enocean_manager.h"

#include "bridge_manager.h"
#include "bridge_storage_manager.h"
#include "eep.h"
#include "onoff_light.h"
#include "tcm.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <platform/CHIPDeviceLayer.h>

#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(enocean, CONFIG_GATEWAY_ENOCEAN_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace Nrf;

namespace Gateway
{

namespace
{
K_MSGQ_DEFINE(sTelegramQueue, sizeof(EnOceanManager::Telegram), CONFIG_GATEWAY_ENOCEAN_QUEUE_SIZE, 4);
atomic_t sProcessScheduled;
uint32_t sDroppedTelegrams;

const char *VendorName(uint8_t kind)
{
	return kind == EnOceanUserData::kEltako ? "Eltako" : "EnOcean";
}

const char *ProductName(const EnOceanUserData &data)
{
	if (data.mKind != EnOceanUserData::kEltako) {
		return "D2-01 actuator";
	}
	return data.mContact == 1 ? "Switch actuator contact 1"
	       : data.mContact == 2 ? "Switch actuator contact 2"
				    : "Switch actuator";
}

uint8_t ContactBit(uint8_t contact)
{
	return contact == 1 ? EnOceanManager::kContact1 : contact == 2 ? EnOceanManager::kContact2 : 0;
}

CHIP_ERROR StoreDevice(MatterBridgedDevice *device, const EnOceanUserData &data, uint8_t index)
{
	BridgeStorageManager::BridgedDevice stored;
	EnOceanUserData userData = data;

	stored.mEndpointId = device->GetEndpointId();
	stored.mDeviceType = device->GetDeviceType();
	stored.mUniqueIDLength = strnlen(device->GetUniqueID(), MatterBridgedDevice::kUniqueIDSize);
	memcpy(stored.mUniqueID, device->GetUniqueID(), stored.mUniqueIDLength);
	stored.mNodeLabelLength = strnlen(device->GetNodeLabel(), MatterBridgedDevice::kNodeLabelSize);
	memcpy(stored.mNodeLabel, device->GetNodeLabel(), stored.mNodeLabelLength);
	stored.mUserDataSize = sizeof(userData);
	stored.mUserData = reinterpret_cast<uint8_t *>(&userData);

	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevice(stored, index), CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

CHIP_ERROR StoreDeviceIndexes()
{
	uint8_t indexes[BridgeManager::kMaxBridgedDevices] = { 0 };
	uint8_t count = 0;

	ReturnErrorOnFailure(BridgeManager::Instance().GetDevicesIndexes(indexes, sizeof(indexes), count));
	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevicesIndexes(indexes, count),
			    CHIP_ERROR_INTERNAL);
	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevicesCount(count), CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

MatterBridgedDevice *CreateDevice(const EnOceanUserData &data, uint16_t deviceType, const char *uniqueID,
				  const char *label)
{
	/* D2-01 actuators can measure the power of their load. */
	const bool measuresPower = data.mKind == EnOceanUserData::kD2_01;

	if (measuresPower && OnOffLightDevice::RegisterPowerMeasurement() != CHIP_NO_ERROR) {
		LOG_ERR("EnOcean: cannot register the power measurement");
	}
	return Platform::New<OnOffLightDevice>(uniqueID, label, deviceType, VendorName(data.mKind),
					       ProductName(data), measuresPower);
}
} /* namespace */

/* --- Provider --- */

EnOceanProvider::EnOceanProvider(const EnOceanUserData &data, UpdateAttributeCallback updateCallback)
	: BridgedDeviceDataProvider(updateCallback), mData(data)
{
}

EnOceanProvider::~EnOceanProvider()
{
	/* The bridge manager owns and deletes providers (e.g. on removal). */
	EnOceanManager::Instance().OnProviderDeleted(this);
}

void EnOceanProvider::NotifyUpdateState(ClusterId clusterId, AttributeId attributeId, void *data, size_t dataSize)
{
	if (mUpdateAttributeCallback) {
		mUpdateAttributeCallback(*this, clusterId, attributeId, data, dataSize);
	}
}

CHIP_ERROR EnOceanProvider::UpdateState(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer)
{
	/* The node label is only kept locally; the actuator has no name. */
	if (clusterId == Clusters::BridgedDeviceBasicInformation::Id &&
	    attributeId == Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
		return CHIP_NO_ERROR;
	}
	if (clusterId != Clusters::OnOff::Id || attributeId != Clusters::OnOff::Attributes::OnOff::Id || !buffer) {
		return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
	}

	const bool on = buffer[0] != 0;

	ReturnErrorOnFailure(EnOceanManager::Instance().Send(*this, on));
	/* The Matter attribute is already set; take it as the state until the actuator confirms. */
	mOnOff = on;
	return CHIP_NO_ERROR;
}

void EnOceanProvider::OnState(bool on)
{
	if (mStateKnown && on == mOnOff) {
		return;
	}
	mStateKnown = true;
	mOnOff = on;
	NotifyUpdateState(Clusters::OnOff::Id, Clusters::OnOff::Attributes::OnOff::Id, &mOnOff, sizeof(mOnOff));
}

void EnOceanProvider::OnPower(uint32_t watts)
{
	int64_t milliwatts = int64_t(watts) * 1000;

	/* The device reports only changes. */
	NotifyUpdateState(Clusters::ElectricalPowerMeasurement::Id,
			  Clusters::ElectricalPowerMeasurement::Attributes::ActivePower::Id, &milliwatts,
			  sizeof(milliwatts));
}

/* --- Manager --- */

CHIP_ERROR EnOceanManager::Start()
{
	k_timer_init(&mQueryTimer, QueryTimerHandler, nullptr);
	k_timer_init(&mStepTimer, StepTimerHandler, nullptr);

	if (tcm_init(OnRadio) != 0) {
		return CHIP_ERROR_INTERNAL;
	}
	/* D2-01 actuators can be asked for their state once the base ID is known. */
	k_timer_start(&mQueryTimer, K_SECONDS(2), K_NO_WAIT);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Restore(const EnOceanUserData &data, uint16_t deviceType, const char *uniqueID,
				   const char *nodeLabel, uint8_t index, uint16_t endpointId)
{
	VerifyOrReturnError(!Find(data.mId, data.mContact), CHIP_ERROR_DUPLICATE_KEY_ID);

	Device *device = FindFree();
	VerifyOrReturnError(device, CHIP_ERROR_NO_MEMORY, LOG_ERR("EnOcean: no free slot to restore %08X", data.mId));
	if (data.mKind == EnOceanUserData::kEltako) {
		VerifyOrReturnError(AddActuator(data), CHIP_ERROR_NO_MEMORY);
	}

	MatterBridgedDevice *matterDevice = CreateDevice(data, deviceType, uniqueID, nodeLabel);
	auto *provider = Platform::New<EnOceanProvider>(data, BridgeManager::HandleUpdate);

	if (!matterDevice || !provider) {
		Platform::Delete(matterDevice);
		Platform::Delete(provider);
		return CHIP_ERROR_NO_MEMORY;
	}

	device->mProvider = provider;
	CHIP_ERROR err = BridgeManager::Instance().AddBridgedDevices(&matterDevice, provider, 1, &index, &endpointId);
	if (err != CHIP_NO_ERROR) {
		/* The bridge manager deleted the device and the provider. */
		return err;
	}
	device->mEndpoint = endpointId;
	LOG_INF("EnOcean: restored %08X \"%s\" on endpoint %u", data.mId, nodeLabel, endpointId);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Add(const EnOceanUserData &data, uint16_t deviceType, const char *label)
{
	char uniqueId[12];

	VerifyOrReturnError(data.mOffset < 128, CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(data.mKind == EnOceanUserData::kEltako || data.mKind == EnOceanUserData::kD2_01,
			    CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(data.mContact <= 2, CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(data.mContact == 0 || (data.mKind == EnOceanUserData::kEltako && data.mStepOffset < 128),
			    CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(!Find(data.mId, data.mContact), CHIP_ERROR_DUPLICATE_KEY_ID);

	Device *device = FindFree();
	VerifyOrReturnError(device, CHIP_ERROR_NO_MEMORY);
	if (data.mKind == EnOceanUserData::kEltako) {
		VerifyOrReturnError(AddActuator(data), CHIP_ERROR_NO_MEMORY);
	}

	if (data.mContact) {
		snprintk(uniqueId, sizeof(uniqueId), "%08X-%u", data.mId, data.mContact);
	} else {
		snprintk(uniqueId, sizeof(uniqueId), "%08X", data.mId);
	}

	MatterBridgedDevice *matterDevice = CreateDevice(data, deviceType, uniqueId, label);
	auto *provider = Platform::New<EnOceanProvider>(data, BridgeManager::HandleUpdate);

	if (!matterDevice || !provider) {
		Platform::Delete(matterDevice);
		Platform::Delete(provider);
		return CHIP_ERROR_NO_MEMORY;
	}

	/* Claim the slot before handing the provider over, so its destructor can release it on failure. */
	device->mProvider = provider;

	uint8_t index;
	CHIP_ERROR err = BridgeManager::Instance().AddBridgedDevices(&matterDevice, provider, 1, &index);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("EnOcean: cannot add %08X: %" CHIP_ERROR_FORMAT, data.mId, err.Format());
		return err;
	}
	device->mEndpoint = matterDevice->GetEndpointId();

	err = StoreDevice(matterDevice, data, index);
	if (err == CHIP_NO_ERROR) {
		err = StoreDeviceIndexes();
	}
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("EnOcean: %08X added but not stored: %" CHIP_ERROR_FORMAT, data.mId, err.Format());
	}

	LOG_INF("EnOcean: added %08X \"%s\" on endpoint %u", data.mId, label, device->mEndpoint);
	if (data.mKind == EnOceanUserData::kD2_01) {
		SendQuery(data);
		/* Ask again if it doesn't answer. */
		mQueryIntervalS = kQueryIntervalMinS;
		k_timer_start(&mQueryTimer, K_SECONDS(mQueryIntervalS), K_NO_WAIT);
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Remove(uint32_t id)
{
	bool found = false;

	/* The whole actuator and its lamps. */
	for (auto &device : mDevices) {
		if (!device.mProvider || device.mProvider->GetData().mId != id) {
			continue;
		}
		uint8_t index;
		/* Deletes the provider, which clears the slot. */
		ReturnErrorOnFailure(BridgeManager::Instance().RemoveBridgedDevice(device.mEndpoint, index));
		BridgeStorageManager::Instance().RemoveBridgedDevice(index);
		found = true;
	}
	VerifyOrReturnError(found, CHIP_ERROR_NOT_FOUND);
	if (Actuator *a = FindActuator(id)) {
		*a = Actuator{};
	}
	return StoreDeviceIndexes();
}

CHIP_ERROR EnOceanManager::Switch(uint32_t id, bool on, uint8_t contact)
{
	Device *device = Find(id, contact);
	VerifyOrReturnError(device, CHIP_ERROR_NOT_FOUND);
	return Send(*device->mProvider, on);
}

CHIP_ERROR EnOceanManager::SendEltako(uint8_t offset, bool on)
{
	const uint32_t base = tcm_base_id();
	uint8_t frame[48];

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE);
	size_t n = eep_eltako_switch(base + offset, on, frame, sizeof(frame));
	VerifyOrReturnError(n > 0 && tcm_send(frame, n) == 0, CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::SetContacts(Actuator &a, uint8_t target)
{
	a.mTarget = target;
	if (!a.mBusy) {
		a.mBusy = true;
		a.mActions = 0;
		Step(a);
	}
	/* Otherwise the running stepping picks up the new target after the next confirmations. */
	return CHIP_NO_ERROR;
}

/*
 * One action towards the target. The sequence in SS1 is off -> 1 -> 2 -> both -> off: each press
 * of the step sender moves one on, and the actuator confirms both contacts after each step.
 * All off and all on go through the central sender, which is one telegram from any state.
 */
void EnOceanManager::Step(Actuator &a)
{
	const uint8_t all = kContact1 | kContact2;

	if (a.mContactsSeen == all && a.mContacts == a.mTarget) {
		a.mBusy = false;
		PublishContacts(a);
		return;
	}
	if (a.mActions++ >= kMaxStepActions) {
		LOG_WRN("EnOcean: %08X did not reach the wanted contacts, giving up", a.mId);
		a.mBusy = false;
		PublishContacts(a);
		return;
	}

	CHIP_ERROR err;

	if (a.mTarget == 0 || a.mTarget == all) {
		err = SendEltako(a.mCentralOffset, a.mTarget == all);
	} else if (a.mContactsSeen != all) {
		/* Contacts not known (e.g. after a restart): all off first, then step from there. */
		err = SendEltako(a.mCentralOffset, false);
	} else {
		err = SendEltako(a.mStepOffset, true);
	}
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("EnOcean: %08X cannot send: %" CHIP_ERROR_FORMAT, a.mId, err.Format());
		a.mBusy = false;
		PublishContacts(a);
		return;
	}
	a.mConfirms = 0;
	a.mSentMs = k_uptime_get();
	k_timer_start(&mStepTimer, K_MSEC(100), K_MSEC(100));
}

void EnOceanManager::StepTimerHandler(k_timer *)
{
	DeviceLayer::PlatformMgr().ScheduleWork(StepWork);
}

void EnOceanManager::StepWork(intptr_t)
{
	EnOceanManager &self = Instance();
	const int64_t now = k_uptime_get();
	bool busy = false;

	for (auto &a : self.mActuators) {
		if (!a.mBusy) {
			continue;
		}
		/* Both contacts confirmed (and nothing more for a moment), or no answer in time. */
		if ((a.mConfirms >= 2 && now - a.mLastConfirmMs >= kStepSettleMs) ||
		    now - a.mSentMs >= kStepTimeoutMs) {
			self.Step(a);
		}
		busy = busy || a.mBusy;
	}
	if (!busy) {
		k_timer_stop(&self.mStepTimer);
	}
}

void EnOceanManager::OnContact(Actuator &a, uint8_t data, int64_t now)
{
	const uint8_t contact = (data & 0x40) ? kContact1 : kContact2;

	a.mContactsSeen |= contact;
	if (data & 0x20) {
		a.mContacts |= contact;
	} else {
		a.mContacts &= ~contact;
	}
	a.mConfirms++;
	a.mLastConfirmMs = now;
	/* While stepping, Matter gets the result at the end instead of every step on the way. */
	if (!a.mBusy) {
		PublishContacts(a);
	}
}

void EnOceanManager::PublishContacts(const Actuator &a)
{
	for (auto &device : mDevices) {
		if (!device.mProvider || device.mProvider->GetData().mId != a.mId) {
			continue;
		}
		const uint8_t contact = device.mProvider->GetData().mContact;

		if (contact == 0) {
			/* The whole actuator is on while any contact is on. */
			device.mProvider->OnState(a.mContacts != 0);
		} else if (a.mContactsSeen & ContactBit(contact)) {
			device.mProvider->OnState((a.mContacts & ContactBit(contact)) != 0);
		}
	}
}

CHIP_ERROR EnOceanManager::Press(uint8_t offset)
{
	const uint32_t base = tcm_base_id();

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE, LOG_WRN("EnOcean: TCM base ID not known"));
	ReturnErrorOnFailure(SendEltako(offset, true));
	LOG_INF("EnOcean: press from %08X", base + offset);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Send(const EnOceanProvider &provider, bool on)
{
	const EnOceanUserData &data = provider.GetData();
	const uint32_t base = tcm_base_id();
	uint8_t frame[48];
	size_t n;

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE,
			    LOG_WRN("EnOcean: TCM base ID not known, cannot switch %08X", data.mId));

	/* An actuator with lamps: every endpoint sets a target for both contacts. */
	Actuator *a = data.mKind == EnOceanUserData::kEltako ? FindActuator(data.mId) : nullptr;
	if (a && a->mStepOffset != kNoStepOffset) {
		const uint8_t all = kContact1 | kContact2;
		/* Start from the target of a stepping in progress, so quick writes (scenes) add up. */
		uint8_t target = a->mBusy ? a->mTarget : (a->mContactsSeen == all ? a->mContacts : 0);
		const uint8_t bit = data.mContact ? ContactBit(data.mContact) : all;

		target = on ? (target | bit) : (target & ~bit);
		LOG_INF("EnOcean: %08X contacts -> %s%s", data.mId, (target & kContact1) ? "1" : "-",
			(target & kContact2) ? "2" : "-");
		return SetContacts(*a, target);
	}

	if (data.mKind == EnOceanUserData::kEltako) {
		n = eep_eltako_switch(base + data.mOffset, on, frame, sizeof(frame));
	} else {
		n = eep_d2_01_set_output(base + data.mOffset, data.mId, 0, on ? 100 : 0, frame, sizeof(frame));
	}
	VerifyOrReturnError(n > 0, CHIP_ERROR_INTERNAL);
	VerifyOrReturnError(tcm_send(frame, n) == 0, CHIP_ERROR_INTERNAL);

	LOG_INF("EnOcean: %08X %s (from %08X)", data.mId, on ? "on" : "off", base + data.mOffset);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::SendQuery(const EnOceanUserData &data)
{
	const uint32_t base = tcm_base_id();
	uint8_t frame[48];

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE);
	size_t n = eep_d2_01_query_status(base + data.mOffset, data.mId, EEP_D2_01_ALL_CHANNELS, frame,
					  sizeof(frame));
	VerifyOrReturnError(n > 0 && tcm_send(frame, n) == 0, CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::SendPowerQuery(const EnOceanUserData &data)
{
	const uint32_t base = tcm_base_id();
	uint8_t frame[48];

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE);
	size_t n = eep_d2_01_query_measurement(base + data.mOffset, data.mId, 0, true, frame, sizeof(frame));
	VerifyOrReturnError(n > 0 && tcm_send(frame, n) == 0, CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

void EnOceanManager::QueryTimerHandler(k_timer *)
{
	DeviceLayer::PlatformMgr().ScheduleWork(QueryStates);
}

void EnOceanManager::QueryStates(intptr_t)
{
	EnOceanManager &self = Instance();
	size_t pending = 0;

	if (tcm_base_id() == 0) {
		/* The TCM hasn't told its base ID yet: nothing can be sent. */
		k_timer_start(&self.mQueryTimer, K_SECONDS(5), K_NO_WAIT);
		return;
	}

	/* Ask every D2-01 actuator whose state is still unknown (no answer yet, e.g. out of range or
	 * unplugged) until it answers; the interval doubles up to kQueryIntervalMaxS. */
	for (auto &device : self.mDevices) {
		if (device.mProvider && device.mProvider->GetData().mKind == EnOceanUserData::kD2_01 &&
		    !device.mProvider->IsStateKnown()) {
			self.SendQuery(device.mProvider->GetData());
			pending++;
		}
	}
	if (pending == 0) {
		self.mQueryIntervalS = kQueryIntervalMinS;
		return;
	}
	LOG_DBG("EnOcean: asked %u actuator(s) for their state, next in %u s", unsigned(pending),
		self.mQueryIntervalS);
	k_timer_start(&self.mQueryTimer, K_SECONDS(self.mQueryIntervalS), K_NO_WAIT);
	self.mQueryIntervalS = MIN(self.mQueryIntervalS * 2, kQueryIntervalMaxS);
}

/* Runs in the TCM receive thread: copy the telegram and let the Matter thread handle it. */
void EnOceanManager::OnRadio(const esp3_packet *pkt)
{
	erp1 t;
	Telegram out = {};

	if (!erp1_parse(pkt, &t)) {
		return;
	}
	out.mSender = t.sender;
	out.mRorg = t.rorg;
	out.mStatus = t.status;
	out.mDbm = t.dbm;
	out.mLen = MIN(t.payload_len, sizeof(out.mPayload));
	memcpy(out.mPayload, t.payload, out.mLen);

	if (k_msgq_put(&sTelegramQueue, &out, K_NO_WAIT) != 0) {
		sDroppedTelegrams++;
	}
	if (atomic_cas(&sProcessScheduled, 0, 1)) {
		DeviceLayer::PlatformMgr().ScheduleWork(ProcessTelegrams);
	}
}

void EnOceanManager::ProcessTelegrams(intptr_t)
{
	Telegram t;

	atomic_set(&sProcessScheduled, 0);
	while (k_msgq_get(&sTelegramQueue, &t, K_NO_WAIT) == 0) {
		Instance().HandleTelegram(t);
	}
	if (sDroppedTelegrams) {
		LOG_WRN("EnOcean: %u telegrams dropped (queue full)", sDroppedTelegrams);
		sDroppedTelegrams = 0;
	}
}

void EnOceanManager::HandleTelegram(const Telegram &t)
{
	const int64_t now = k_uptime_get();

	/* Our own telegrams come back through repeaters. */
	if (tcm_is_own_id(t.mSender)) {
		return;
	}
	UpdateSeen(t, now);

	Device *device = nullptr;

	for (auto &d : mDevices) {
		if (d.mProvider && d.mProvider->GetData().mId == t.mSender) {
			d.mDbm = t.mDbm;
			d.mLastSeenMs = now;
			device = device ? device : &d;
		}
	}
	if (!device) {
		return;
	}

	const EnOceanUserData &data = device->mProvider->GetData();

	if (data.mKind == EnOceanUserData::kEltako) {
		Actuator *a = FindActuator(t.mSender);

		if (a && t.mRorg == EEP_RORG_RPS && t.mLen == 1) {
			LOG_INF("EnOcean: %08X F6 %02X, %d dBm", t.mSender, t.mPayload[0], t.mDbm);
			OnContact(*a, t.mPayload[0], now);
		}
		return;
	}

	/* D2-01 */
	erp1 e = {};
	e.rorg = t.mRorg;
	e.payload = t.mPayload;
	e.payload_len = t.mLen;
	e.sender = t.mSender;
	e.status = t.mStatus;

	struct eep_d2_01_status status;
	struct eep_d2_01_measurement measurement;

	if (eep_d2_01_status(&e, &status)) {
		if (status.output != EEP_D2_01_OUTPUT_INVALID) {
			device->mProvider->OnState(status.output > 0);
		}
		/* The actuator answers a switch command or a status query with its status: the load may have
		 * changed, ask for its power. */
		SendPowerQuery(data);
	} else if (eep_d2_01_measurement(&e, &measurement)) {
		/* Units 0..2 are energy (Ws, Wh, kWh), 3 and 4 power (W, kW). */
		if (measurement.unit == 3 || measurement.unit == 4) {
			const uint32_t watts = measurement.unit == 4 ? measurement.value * 1000 : measurement.value;

			device->mHasPower = true;
			device->mPowerW = watts;
			device->mProvider->OnPower(watts);
		}
		LOG_DBG("EnOcean: %08X %u %s", t.mSender, measurement.value,
			eep_d2_01_unit_name(measurement.unit));
	}
}

void EnOceanManager::UpdateSeen(const Telegram &t, int64_t now)
{
	SeenInfo *slot = nullptr;

	for (auto &seen : mSeen) {
		if (seen.mId == t.mSender) {
			slot = &seen;
			break;
		}
		/* Free slot, or else the one heard longest ago */
		if (!slot || (slot->mId != 0 && (seen.mId == 0 || seen.mLastSeenMs < slot->mLastSeenMs))) {
			slot = &seen;
		}
	}
	if (slot->mId != t.mSender) {
		*slot = SeenInfo{};
		slot->mId = t.mSender;
	}
	slot->mRorg = t.mRorg;
	slot->mLen = MIN(t.mLen, sizeof(slot->mPayload));
	memcpy(slot->mPayload, t.mPayload, slot->mLen);
	slot->mDbm = t.mDbm;
	/* Status bits 3..0: how many repeaters the telegram passed (0..2); 0xF = "do not repeat", so direct */
	slot->mRepeaters = (t.mStatus & 0x0F) == 0x0F ? 0 : (t.mStatus & 0x0F);
	slot->mCount++;
	if (slot->mRepeaters) {
		slot->mRepeatedCount++;
	}
	slot->mLastSeenMs = now;
}

size_t EnOceanManager::GetDevices(DeviceInfo *out, size_t max) const
{
	size_t n = 0;

	for (const auto &device : mDevices) {
		if (!device.mProvider || n >= max) {
			continue;
		}
		uint8_t index;
		MatterBridgedDevice *matterDevice = BridgeManager::Instance().GetDevice(device.mEndpoint, index);

		out[n].mData = device.mProvider->GetData();
		out[n].mEndpoint = device.mEndpoint;
		out[n].mDeviceType = matterDevice ? matterDevice->GetDeviceType() : 0;
		out[n].mStateKnown = device.mProvider->IsStateKnown();
		out[n].mOn = device.mProvider->GetOnOff();
		const Actuator *a = nullptr;
		for (const auto &actuator : mActuators) {
			if (actuator.mId && actuator.mId == out[n].mData.mId) {
				a = &actuator;
			}
		}
		out[n].mContacts = a ? a->mContacts : 0;
		out[n].mContactsSeen = a ? a->mContactsSeen : 0;
		out[n].mStepping = a && a->mBusy;
		out[n].mHasPower = device.mHasPower;
		out[n].mPowerW = device.mPowerW;
		out[n].mDbm = device.mDbm;
		out[n].mLastSeenMs = device.mLastSeenMs;
		n++;
	}
	return n;
}

size_t EnOceanManager::GetSeen(SeenInfo *out, size_t max) const
{
	size_t n = 0;

	for (const auto &seen : mSeen) {
		if (seen.mId != 0 && n < max) {
			out[n++] = seen;
		}
	}
	return n;
}

void EnOceanManager::OnProviderDeleted(EnOceanProvider *provider)
{
	for (auto &device : mDevices) {
		if (device.mProvider == provider) {
			device = Device{};
		}
	}
}

EnOceanManager::Device *EnOceanManager::Find(uint32_t id, uint8_t contact)
{
	for (auto &device : mDevices) {
		if (device.mProvider && device.mProvider->GetData().mId == id &&
		    device.mProvider->GetData().mContact == contact) {
			return &device;
		}
	}
	return nullptr;
}

EnOceanManager::Actuator *EnOceanManager::FindActuator(uint32_t id)
{
	for (auto &a : mActuators) {
		if (a.mId == id) {
			return &a;
		}
	}
	return nullptr;
}

EnOceanManager::Actuator *EnOceanManager::AddActuator(const EnOceanUserData &data)
{
	Actuator *a = FindActuator(data.mId);

	if (!a) {
		a = FindActuator(0);
		VerifyOrReturnValue(a, nullptr);
		*a = Actuator{};
		a->mId = data.mId;
		a->mCentralOffset = data.mOffset;
	}
	if (data.mContact) {
		a->mCentralOffset = data.mOffset;
		a->mStepOffset = data.mStepOffset;
	}
	return a;
}

EnOceanManager::Device *EnOceanManager::FindFree()
{
	for (auto &device : mDevices) {
		if (!device.mProvider) {
			return &device;
		}
	}
	return nullptr;
}

} /* namespace Gateway */
