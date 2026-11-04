/*
 * Data provider for one advertisement-only BTHome sensor.
 */

#include "bthome_sensor_provider.h"
#include "bthome_sensor_manager.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>

using namespace chip::app;

namespace Gateway
{

BTHomeSensorProvider::BTHomeSensorProvider(const bt_addr_le_t &addr, UpdateAttributeCallback updateCallback)
	: Nrf::BridgedDeviceDataProvider(updateCallback)
{
	bt_addr_le_copy(&mAddr, &addr);
}

BTHomeSensorProvider::~BTHomeSensorProvider()
{
	/* The bridge manager owns and deletes providers (e.g. on `matter_bridge remove`). */
	BTHomeSensorManager::Instance().OnProviderDeleted(this);
}

void BTHomeSensorProvider::NotifyUpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
					     size_t dataSize)
{
	if (mUpdateAttributeCallback) {
		mUpdateAttributeCallback(*this, clusterId, attributeId, data, dataSize);
	}
}

CHIP_ERROR BTHomeSensorProvider::UpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *)
{
	/* The node label is only kept locally; the sensor itself cannot be written to. */
	if (clusterId == Clusters::BridgedDeviceBasicInformation::Id &&
	    attributeId == Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
		return CHIP_NO_ERROR;
	}
	return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

void BTHomeSensorProvider::OnReading(const bthome_reading &reading, int64_t nowMs)
{
	mLastSeenMs = nowMs;
	SetReachable(true);

	if (reading.fields & BTHOME_HAS_BATTERY) {
		mBattery = reading.battery_pct;
	}

	if ((reading.fields & BTHOME_HAS_TEMPERATURE) &&
	    (!mHasTemperature || mTemperature != reading.temperature_centi_c)) {
		mHasTemperature = true;
		mTemperature = reading.temperature_centi_c;
		NotifyUpdateState(Clusters::TemperatureMeasurement::Id,
				  Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, &mTemperature,
				  sizeof(mTemperature));
	}

	if ((reading.fields & BTHOME_HAS_HUMIDITY) && (!mHasHumidity || mHumidity != reading.humidity_centi_pct)) {
		mHasHumidity = true;
		mHumidity = reading.humidity_centi_pct;
		NotifyUpdateState(Clusters::RelativeHumidityMeasurement::Id,
				  Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &mHumidity,
				  sizeof(mHumidity));
	}
}

void BTHomeSensorProvider::CheckReachable(int64_t nowMs, int64_t timeoutMs)
{
	if (mReachable && nowMs - mLastSeenMs > timeoutMs) {
		SetReachable(false);
	}
}

void BTHomeSensorProvider::SetReachable(bool reachable, bool force)
{
	if (reachable == mReachable && !force) {
		return;
	}
	mReachable = reachable;
	NotifyUpdateState(Clusters::BridgedDeviceBasicInformation::Id,
			  Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id, &mReachable,
			  sizeof(mReachable));
}

} /* namespace Gateway */
