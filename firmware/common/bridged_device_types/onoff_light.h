/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "matter_bridged_device.h"

#include <app/data-model/Nullable.h>

/*
 * On/Off Light (0x0100) or On/Off Plug-in Unit (0x010A): the same clusters, only the device type differs.
 * Vendor and product name are shown in the controller's device details; the strings must outlive the device.
 *
 * With measuresPower, the endpoint is also an Electrical Sensor (0x0510): Electrical Power Measurement (AC,
 * active power only) and Power Topology (node topology). The power comes from the provider as an
 * HandleAttributeChange of ActivePower (int64_t, mW); it is null until the first value.
 * RegisterPowerMeasurement() must have been called once before such a device is added.
 */
class OnOffLightDevice : public Nrf::MatterBridgedDevice {
public:
	OnOffLightDevice(const char *uniqueID, const char *nodeLabel,
			 uint16_t deviceType = Nrf::MatterBridgedDevice::DeviceType::OnOffLight,
			 const char *vendorName = "", const char *productName = "", bool measuresPower = false);

	/* Serves Electrical Power Measurement for every endpoint that has it. Call once at start. */
	static CHIP_ERROR RegisterPowerMeasurement();

	bool GetOnOff() { return mOnOff; }
	void Toggle() { mOnOff = !mOnOff; }

	uint16_t GetDeviceType() const override { return mDeviceType; }
	const char *GetVendorName() const override { return mVendorName; }
	const char *GetProductName() const override { return mProductName; }
	CHIP_ERROR HandleRead(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			      uint16_t maxReadLength) override;
	CHIP_ERROR HandleReadOnOff(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);
	CHIP_ERROR HandleReadGroups(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);
	CHIP_ERROR HandleWrite(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			       size_t size) override;
	CHIP_ERROR HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
					 size_t dataSize) override;

	static constexpr uint16_t GetOnOffClusterRevision() { return 6; }
	static constexpr uint32_t GetOnOffFeatureMap() { return 1; }
	static constexpr uint16_t GetGroupsClusterRevision() { return 4; }
	static constexpr uint32_t GetGroupsFeatureMap() { return 1; }
	static constexpr uint8_t GetGroupsNameSupportMap() { return 0; }

	bool MeasuresPower() const { return mMeasuresPower; }
	chip::app::DataModel::Nullable<int64_t> GetActivePower() const { return mActivePower; }

private:
	uint16_t mDeviceType;
	const char *mVendorName;
	const char *mProductName;
	bool mMeasuresPower;
	chip::app::DataModel::Nullable<int64_t> mActivePower; /* mW */
	bool mOnOff{ false };
	bool mGlobalSceneControl{ false };
	int16_t mOnTime{ 0 };
	int16_t mOffWaitTime{ 0 };
	chip::app::Clusters::OnOff::StartUpOnOffEnum mStartUpOnOff{
		chip::app::Clusters::OnOff::StartUpOnOffEnum::kUnknownEnumValue
	};
};
