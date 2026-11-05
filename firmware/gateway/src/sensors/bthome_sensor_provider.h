/*
 * Data provider for one advertisement-only BTHome device. It is shared by a
 * sensor's Temperature Sensor and Humidity Sensor endpoints, or serves the
 * Thermostat endpoint of an eQ-3 thermostat (eq3-custom-fw).
 *
 * All methods run in the Matter (CHIP) thread.
 */

#pragma once

#include "bridged_device_data_provider.h"
#include "bthome.h"
#include "eq3_client.h"

#include <zephyr/bluetooth/addr.h>

namespace Gateway
{

class BTHomeSensorProvider : public Nrf::BridgedDeviceDataProvider {
public:
	BTHomeSensorProvider(const bt_addr_le_t &addr, UpdateAttributeCallback updateCallback);
	~BTHomeSensorProvider() override;

	void Init() override {}
	void NotifyUpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
			       size_t dataSize) override;
	CHIP_ERROR UpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer) override;

	/* Applies a new measurement and reports changed values to Matter. */
	void OnReading(const bthome_reading &reading, int64_t nowMs);

	/* Result of an eQ-3 command sent by UpdateState; @p status is nullptr when it failed. */
	void OnThermostatCommandDone(const Eq3Status *status);

	/* Marks the sensor unreachable if nothing was received for @p timeoutMs. */
	void CheckReachable(int64_t nowMs, int64_t timeoutMs);

	/* Sets the reachable state and reports it if it changed (or if @p force). */
	void SetReachable(bool reachable, bool force = false);

	const bt_addr_le_t &GetAddress() const { return mAddr; }
	bool IsReachable() const { return mReachable; }
	int64_t GetLastSeenMs() const { return mLastSeenMs; }
	bool HasTemperature() const { return mHasTemperature; }
	bool HasHumidity() const { return mHasHumidity; }
	int16_t GetTemperature() const { return mTemperature; }
	uint16_t GetHumidity() const { return mHumidity; }
	uint8_t GetBattery() const { return mBattery; }

private:
	void UpdateBattery(uint8_t percent, bool low = false);
	void UpdateThermostat(const bthome_reading &reading);
	void ApplyTarget(int32_t halfDegrees);
	CHIP_ERROR SendTarget(uint8_t halfDegrees);
	void NotifyIfChanged(chip::ClusterId clusterId, chip::AttributeId attributeId, int32_t &last, int32_t value,
			     size_t size);

	bt_addr_le_t mAddr;
	bool mReachable = true; /* matches the default of MatterBridgedDevice */
	int64_t mLastSeenMs = 0;

	bool mHasTemperature = false;
	bool mHasHumidity = false;
	int16_t mTemperature = 0; /* 0.01 degC */
	uint16_t mHumidity = 0; /* 0.01 % */
	bool mHasBattery = false;
	uint8_t mBattery = 0; /* % */
	uint8_t mBatteryLevel = 0; /* PowerSource::BatChargeLevelEnum */

	/* Last values reported to the Thermostat cluster; kNotReported until the first one. */
	static constexpr int32_t kNotReported = INT32_MIN;
	int32_t mLocalTemperature = kNotReported;
	int32_t mSetpoint = kNotReported;
	int32_t mSystemMode = kNotReported;
	int32_t mHeatingDemand = kNotReported;
	bool mBatteryLow = false;
	/* Last target that was not "off", in half degrees: restored when Matter switches the mode back to Heat. */
	uint8_t mLastOnTarget = 40;
	/* While a command is in flight, broadcast targets are ignored: they may still show the old value. */
	bool mCommandPending = false;
};

} /* namespace Gateway */
