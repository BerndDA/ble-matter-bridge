/*
 * Data provider for one advertisement-only BTHome sensor.
 */

#include "bthome_sensor_provider.h"
#include "bthome_sensor_manager.h"
#include "eq3_client.h"
#include "thermostat.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/cluster-enums.h>

#include <zephyr/sys/util.h>

#include <string.h>

using namespace chip::app;

namespace Gateway
{
namespace
{
constexpr int16_t kThermostatMinSetpoint = ThermostatDevice::kMinHeatSetpoint;
constexpr int16_t kThermostatMaxSetpoint = ThermostatDevice::kMaxHeatSetpoint;
constexpr int16_t kThermostatOffBelow = 475;
/* Two alkaline AA cells. */
constexpr int32_t kBatteryFullMv = 3000;
constexpr int32_t kBatteryEmptyMv = 2200;
} /* namespace */

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

CHIP_ERROR BTHomeSensorProvider::UpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer)
{
	namespace TA = Clusters::Thermostat::Attributes;

	/* The node label is only kept locally; the sensor itself cannot be written to. */
	if (clusterId == Clusters::BridgedDeviceBasicInformation::Id &&
	    attributeId == Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
		return CHIP_NO_ERROR;
	}
	if (clusterId != Clusters::Thermostat::Id || !buffer) {
		return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
	}

	if (attributeId == TA::OccupiedHeatingSetpoint::Id) {
		int16_t centi;
		memcpy(&centi, buffer, sizeof(centi));
		/* eQ-3 steps are 0.5 degC. */
		return SendTarget(uint8_t((CLAMP(centi, kThermostatMinSetpoint, kThermostatMaxSetpoint) + 25) / 50));
	}
	if (attributeId == TA::SystemMode::Id) {
		const auto mode = static_cast<Clusters::Thermostat::SystemModeEnum>(buffer[0]);
		return SendTarget(mode == Clusters::Thermostat::SystemModeEnum::kOff ? Eq3Client::kMinTargetHalfDegrees
										: mLastOnTarget);
	}
	return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR BTHomeSensorProvider::SendTarget(uint8_t halfDegrees)
{
	const int err = Eq3Client::SetTarget(mAddr, halfDegrees);
	if (err) {
		return CHIP_ERROR_BUSY;
	}
	mCommandPending = true;
	return CHIP_NO_ERROR;
}

void BTHomeSensorProvider::OnThermostatCommandDone(const Eq3Status *status)
{
	namespace TA = Clusters::Thermostat::Attributes;

	mCommandPending = false;
	if (!status) {
		/* Report the thermostat's real values again with the next broadcast, replacing the written ones. */
		mSetpoint = kNotReported;
		mSystemMode = kNotReported;
		return;
	}
	/* The written value may have been rounded or clamped: report what the thermostat took. */
	mSetpoint = kNotReported;
	mSystemMode = kNotReported;
	ApplyTarget(status->mTargetHalfDegrees * 50);
	NotifyIfChanged(Clusters::Thermostat::Id, TA::PIHeatingDemand::Id, mHeatingDemand, status->mValvePct,
			sizeof(uint8_t));
}

void BTHomeSensorProvider::ApplyTarget(int32_t target)
{
	namespace TA = Clusters::Thermostat::Attributes;
	using Clusters::Thermostat::SystemModeEnum;

	/* eQ-3 "off" is a target of 4.5 degC, "on" 30.0 degC; Matter shows off as SystemMode Off. */
	const bool off = target < kThermostatOffBelow;
	const int32_t mode = chip::to_underlying(off ? SystemModeEnum::kOff : SystemModeEnum::kHeat);

	NotifyIfChanged(Clusters::Thermostat::Id, TA::SystemMode::Id, mSystemMode, mode, sizeof(uint8_t));
	if (!off) {
		mLastOnTarget = uint8_t(target / 50);
		NotifyIfChanged(Clusters::Thermostat::Id, TA::OccupiedHeatingSetpoint::Id, mSetpoint,
				CLAMP(target, kThermostatMinSetpoint, kThermostatMaxSetpoint), sizeof(int16_t));
	}
}

void BTHomeSensorProvider::OnReading(const bthome_reading &reading, int64_t nowMs)
{
	mLastSeenMs = nowMs;
	SetReachable(true);

	if (reading.fields & BTHOME_THERMOSTAT_FIELDS) {
		UpdateThermostat(reading);
		return;
	}

	if (reading.fields & BTHOME_HAS_BATTERY) {
		UpdateBattery(reading.battery_pct);
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

void BTHomeSensorProvider::UpdateBattery(uint8_t percent, bool low)
{
	namespace PS = Clusters::PowerSource;

	const bool first = !mHasBattery;
	mHasBattery = true;

	if (first || percent != mBattery) {
		mBattery = percent;
		/* Matter counts in half percent. */
		uint8_t halfPercent = uint8_t(percent * 2);
		NotifyUpdateState(PS::Id, PS::Attributes::BatPercentRemaining::Id, &halfPercent, sizeof(halfPercent));
	}

	PS::BatChargeLevelEnum chargeLevel = PS::BatChargeLevelEnum::kOk;
	if (low || percent <= CONFIG_GATEWAY_BTHOME_BATTERY_CRITICAL_PCT) {
		chargeLevel = PS::BatChargeLevelEnum::kCritical;
	} else if (percent <= CONFIG_GATEWAY_BTHOME_BATTERY_WARNING_PCT) {
		chargeLevel = PS::BatChargeLevelEnum::kWarning;
	}

	const uint8_t level = chip::to_underlying(chargeLevel);
	if (first || level != mBatteryLevel) {
		mBatteryLevel = level;
		NotifyUpdateState(PS::Id, PS::Attributes::BatChargeLevel::Id, &mBatteryLevel, sizeof(mBatteryLevel));

		bool replace = chargeLevel == PS::BatChargeLevelEnum::kCritical;
		NotifyUpdateState(PS::Id, PS::Attributes::BatReplacementNeeded::Id, &replace, sizeof(replace));
	}
}

void BTHomeSensorProvider::NotifyIfChanged(chip::ClusterId clusterId, chip::AttributeId attributeId, int32_t &last,
					   int32_t value, size_t size)
{
	if (value == last) {
		return;
	}
	last = value;
	/* Little endian: the low bytes of the int32 are the int16 / uint8 value. */
	NotifyUpdateState(clusterId, attributeId, &value, size);
}

void BTHomeSensorProvider::UpdateThermostat(const bthome_reading &reading)
{
	namespace TA = Clusters::Thermostat::Attributes;

	/* The eQ-3 sends two alternating sets (PROTOCOL.md of eq3-custom-fw); each update uses only what it carries. */
	if (reading.fields & BTHOME_HAS_TEMPERATURE) {
		mHasTemperature = true; /* for `gateway sensors` */
		mTemperature = reading.temperature_centi_c;
		NotifyIfChanged(Clusters::Thermostat::Id, TA::LocalTemperature::Id, mLocalTemperature,
				reading.temperature_centi_c, sizeof(int16_t));
	}
	if ((reading.fields & BTHOME_HAS_TEMPERATURE2) && !mCommandPending) {
		ApplyTarget(reading.temperature2_centi_c);
	}
	if (reading.fields & BTHOME_HAS_VALVE) {
		NotifyIfChanged(Clusters::Thermostat::Id, TA::PIHeatingDemand::Id, mHeatingDemand, reading.valve_pct,
				sizeof(uint8_t));
	}

	/* No battery percentage: estimate it from the voltage of the two AA cells, and take the thermostat's own
	 * low battery flag as critical. */
	if (reading.fields & BTHOME_HAS_BATTERY_LOW) {
		mBatteryLow = reading.battery_low;
	}
	if (reading.fields & BTHOME_HAS_VOLTAGE) {
		const int32_t mv = reading.voltage_mv;
		const int32_t pct = (mv - kBatteryEmptyMv) * 100 / (kBatteryFullMv - kBatteryEmptyMv);
		UpdateBattery(uint8_t(CLAMP(pct, 0, 100)), mBatteryLow);
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
