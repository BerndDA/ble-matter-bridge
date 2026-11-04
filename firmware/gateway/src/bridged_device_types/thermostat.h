/*
 * Bridged Matter Thermostat (heating only), for eQ-3 radiator thermostats.
 *
 * Values come from the thermostat's BTHome broadcasts through the data provider.
 * Writes from controllers (setpoint, mode) are not forwarded yet and are refused.
 */

#pragma once

#include "matter_bridged_device.h"

#include <app-common/zap-generated/cluster-enums.h>

class ThermostatDevice : public Nrf::MatterBridgedDevice {
public:
	/* eQ-3 range: 5.0..29.5 degC, plus "off" (4.5) and "on" (30.0). */
	static constexpr int16_t kMinHeatSetpoint = 500;
	static constexpr int16_t kMaxHeatSetpoint = 3000;
	static constexpr int16_t kLocalTemperatureNull = INT16_MIN;

	ThermostatDevice(const char *uniqueID, const char *nodeLabel);

	uint16_t GetDeviceType() const override { return Nrf::MatterBridgedDevice::DeviceType::Thermostat; }
	CHIP_ERROR HandleRead(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			      uint16_t maxReadLength) override;
	CHIP_ERROR HandleWrite(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			       size_t size) override;
	CHIP_ERROR HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
					 size_t dataSize) override;

	static constexpr uint16_t GetThermostatClusterRevision() { return 9; }
	static constexpr uint32_t GetThermostatFeatureMap() { return 0x1; /* HEAT */ }

private:
	CHIP_ERROR HandleReadThermostat(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);

	int16_t mLocalTemperature = kLocalTemperatureNull;
	int16_t mOccupiedHeatingSetpoint = 2000;
	uint8_t mPIHeatingDemand = 0;
	uint8_t mSystemMode = chip::to_underlying(chip::app::Clusters::Thermostat::SystemModeEnum::kHeat);
};
