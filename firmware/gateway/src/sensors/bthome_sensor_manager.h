/*
 * Bridges advertisement-only BTHome sensors to Matter.
 *
 * - New sensors are added automatically on their first BTHome packet
 *   (CONFIG_GATEWAY_BTHOME_AUTO_ADD), as a Temperature Sensor and a Humidity
 *   Sensor endpoint sharing one BTHomeSensorProvider. eQ-3 thermostats with
 *   eq3-custom-fw are added as one Thermostat endpoint.
 * - Added sensors are stored by the bridge storage and restored on boot with
 *   the same endpoint IDs, so names and rooms in the controller are kept.
 * - Removed sensors go on an ignore list so auto-add does not bring them back.
 * - A sensor is reported unreachable after CONFIG_GATEWAY_BTHOME_UNREACHABLE_TIMEOUT_S
 *   without packets.
 *
 * Unless stated otherwise, methods must be called in the Matter thread or with
 * the Matter stack lock held.
 */

#pragma once

#include "bthome_sensor_provider.h"

#include <lib/core/CHIPError.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>

namespace Gateway
{

/* Implementation specific data stored with each bridged device (BridgeStorageManager user data). */
struct StoredDeviceUserData {
	enum Kind : uint8_t { kGattDevice = 0, kBTHomeSensor = 1 };

	bt_addr_le_t mAddr;
	uint8_t mKind;
} __packed;

class BTHomeSensorManager {
public:
	static constexpr size_t kMaxSensors = CONFIG_GATEWAY_BTHOME_MAX_SENSORS;
	static constexpr size_t kMaxIgnored = CONFIG_GATEWAY_BTHOME_IGNORE_MAX;

	struct SensorInfo {
		bt_addr_le_t mAddr;
		uint16_t mTemperatureEndpoint;
		uint16_t mHumidityEndpoint;
		uint16_t mThermostatEndpoint;
		bool mReachable;
		int64_t mLastSeenMs;
		bool mHasTemperature;
		bool mHasHumidity;
		int16_t mTemperature;
		uint16_t mHumidity;
		uint8_t mBattery;
	};

	static BTHomeSensorManager &Instance()
	{
		static BTHomeSensorManager sInstance;
		return sInstance;
	}

	/* Loads the ignore list. Call before the bridge restores stored devices. */
	void Init();

	/* Starts receiving readings from the sensor scanner. Call after the bridge has been initialized. */
	void Start();

	/* Restores one stored endpoint of a BTHome sensor (called while the bridge loads stored devices). */
	CHIP_ERROR Restore(const bt_addr_le_t &addr, uint16_t deviceType, const char *uniqueID, const char *nodeLabel,
			   uint8_t index, uint16_t endpointId);

	/* Removes the sensor's endpoints and puts its address on the ignore list. */
	CHIP_ERROR Remove(const bt_addr_le_t &addr);

	/* Takes an address off the ignore list, so auto-add can add it again. */
	CHIP_ERROR Unignore(const bt_addr_le_t &addr);

	size_t GetSensors(SensorInfo *out, size_t max) const;
	size_t GetIgnored(bt_addr_le_t *out, size_t max) const;

	/* Called from the provider destructor. */
	void OnProviderDeleted(BTHomeSensorProvider *provider);

private:
	struct Sensor {
		BTHomeSensorProvider *mProvider = nullptr;
		uint16_t mTemperatureEndpoint = 0;
		uint16_t mHumidityEndpoint = 0;
		uint16_t mThermostatEndpoint = 0;
	};

	static void OnScannerReading(const bt_addr_le_t *addr, int8_t rssi, const bthome_reading *reading);
	static void ProcessEvents(intptr_t);
	static void ReachabilityTimerHandler(k_timer *timer);
	static void CheckReachability(intptr_t);

	void HandleReading(const bt_addr_le_t &addr, const bthome_reading &reading);
	CHIP_ERROR AddSensor(const bt_addr_le_t &addr, bool thermostat, Sensor *&sensor);
	Sensor *Find(const bt_addr_le_t &addr);
	Sensor *FindFree();
	bool IsIgnored(const bt_addr_le_t &addr) const;
	CHIP_ERROR SaveIgnored();

	Sensor mSensors[kMaxSensors];
	bt_addr_le_t mIgnored[kMaxIgnored];
	size_t mIgnoredCount = 0;
	bool mFullWarningLogged = false;
};

} /* namespace Gateway */
