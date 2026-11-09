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

const char *ProductName(uint8_t kind)
{
	return kind == EnOceanUserData::kEltako ? "Switch actuator" : "D2-01 actuator";
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
					       ProductName(data.mKind), measuresPower);
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

	if (tcm_init(OnRadio) != 0) {
		return CHIP_ERROR_INTERNAL;
	}
	/* D2-01 actuators can be asked for their state once the base ID is known. */
	k_timer_start(&mQueryTimer, K_SECONDS(2), K_SECONDS(5));
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Restore(const EnOceanUserData &data, uint16_t deviceType, const char *uniqueID,
				   const char *nodeLabel, uint8_t index, uint16_t endpointId)
{
	VerifyOrReturnError(!Find(data.mId), CHIP_ERROR_DUPLICATE_KEY_ID);

	Device *device = FindFree();
	VerifyOrReturnError(device, CHIP_ERROR_NO_MEMORY, LOG_ERR("EnOcean: no free slot to restore %08X", data.mId));

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
	char uniqueId[9];

	VerifyOrReturnError(data.mOffset < 128, CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(data.mKind == EnOceanUserData::kEltako || data.mKind == EnOceanUserData::kD2_01,
			    CHIP_ERROR_INVALID_ARGUMENT);
	VerifyOrReturnError(!Find(data.mId), CHIP_ERROR_DUPLICATE_KEY_ID);

	Device *device = FindFree();
	VerifyOrReturnError(device, CHIP_ERROR_NO_MEMORY);

	snprintk(uniqueId, sizeof(uniqueId), "%08X", data.mId);

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
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR EnOceanManager::Remove(uint32_t id)
{
	Device *device = Find(id);
	VerifyOrReturnError(device, CHIP_ERROR_NOT_FOUND);

	uint8_t index;
	/* Deletes the provider, which clears the slot. */
	ReturnErrorOnFailure(BridgeManager::Instance().RemoveBridgedDevice(device->mEndpoint, index));
	BridgeStorageManager::Instance().RemoveBridgedDevice(index);
	return StoreDeviceIndexes();
}

CHIP_ERROR EnOceanManager::Switch(uint32_t id, bool on)
{
	Device *device = Find(id);
	VerifyOrReturnError(device, CHIP_ERROR_NOT_FOUND);
	return Send(*device->mProvider, on);
}

CHIP_ERROR EnOceanManager::Send(const EnOceanProvider &provider, bool on)
{
	const EnOceanUserData &data = provider.GetData();
	const uint32_t base = tcm_base_id();
	uint8_t frame[48];
	size_t n;

	VerifyOrReturnError(base != 0, CHIP_ERROR_INCORRECT_STATE,
			    LOG_WRN("EnOcean: TCM base ID not known, cannot switch %08X", data.mId));

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

	if (self.mStatesQueried || tcm_base_id() == 0) {
		return;
	}
	self.mStatesQueried = true;
	k_timer_stop(&self.mQueryTimer);

	for (auto &device : self.mDevices) {
		if (device.mProvider && device.mProvider->GetData().mKind == EnOceanUserData::kD2_01) {
			self.SendQuery(device.mProvider->GetData());
		}
	}
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

	Device *device = Find(t.mSender);
	if (!device) {
		return;
	}
	device->mDbm = t.mDbm;
	device->mLastSeenMs = now;

	const EnOceanUserData &data = device->mProvider->GetData();

	if (data.mKind == EnOceanUserData::kEltako) {
		if (t.mRorg == EEP_RORG_RPS && t.mLen == 1) {
			/* The "O" half of either rocker (0x30, 0x70) means on. */
			device->mProvider->OnState((t.mPayload[0] & 0x20) != 0);
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
	slot->mCount++;
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

EnOceanManager::Device *EnOceanManager::Find(uint32_t id)
{
	for (auto &device : mDevices) {
		if (device.mProvider && device.mProvider->GetData().mId == id) {
			return &device;
		}
	}
	return nullptr;
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
