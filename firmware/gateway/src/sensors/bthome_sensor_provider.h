/*
 * Data provider for one advertisement-only BTHome sensor. It is shared by the
 * sensor's Temperature Sensor and Humidity Sensor endpoints.
 *
 * All methods run in the Matter (CHIP) thread.
 */

#pragma once

#include "bridged_device_data_provider.h"
#include "bthome.h"

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
	bt_addr_le_t mAddr;
	bool mReachable = true; /* matches the default of MatterBridgedDevice */
	int64_t mLastSeenMs = 0;

	bool mHasTemperature = false;
	bool mHasHumidity = false;
	int16_t mTemperature = 0; /* 0.01 degC */
	uint16_t mHumidity = 0; /* 0.01 % */
	uint8_t mBattery = 0;
};

} /* namespace Gateway */
