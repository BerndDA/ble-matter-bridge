/*
 * Bridged Matter Thermostat (heating only), for eQ-3 radiator thermostats.
 */

#include "thermostat.h"

#include <app-common/zap-generated/cluster-enums.h>

namespace
{
DESCRIPTOR_CLUSTER_ATTRIBUTES(descriptorAttrs);
BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_ATTRIBUTES(bridgedDeviceBasicAttrs);
POWER_SOURCE_BATTERY_CLUSTER_ATTRIBUTES(powerSourceAttrs);
}; /* namespace */

using namespace ::chip;
using namespace ::chip::app;
using namespace Nrf;

namespace TA = Clusters::Thermostat::Attributes;

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TA::LocalTemperature::Id, INT16S, 2, ZAP_ATTRIBUTE_MASK(NULLABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::AbsMinHeatSetpointLimit::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::AbsMaxHeatSetpointLimit::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::PIHeatingDemand::Id, INT8U, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::OccupiedHeatingSetpoint::Id, INT16S, 2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::MinHeatSetpointLimit::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::MaxHeatSetpointLimit::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::ControlSequenceOfOperation::Id, ENUM8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::SystemMode::Id, ENUM8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(TA::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* SetpointRaiseLower is mandatory; the SDK's thermostat server handles it through attribute writes. */
static constexpr CommandId kThermostatAcceptedCommands[] = {
	Clusters::Thermostat::Commands::SetpointRaiseLower::Id,
	kInvalidCommandId,
};

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedThermostatClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER), kThermostatAcceptedCommands,
			nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::PowerSource::Id, powerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedThermostatEndpoint, bridgedThermostatClusters);

static constexpr uint8_t kThermostatDeviceTypeVersion = 4;

static constexpr EmberAfDeviceType kBridgedThermostatDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::Thermostat), kThermostatDeviceTypeVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr uint8_t kThermostatDataVersionSize = MATTER_ARRAY_SIZE(bridgedThermostatClusters);

ThermostatDevice::ThermostatDevice(const char *uniqueID, const char *nodeLabel) : MatterBridgedDevice(uniqueID, nodeLabel)
{
	mDataVersionSize = kThermostatDataVersionSize;
	mEp = &bridgedThermostatEndpoint;
	mDeviceTypeList = kBridgedThermostatDeviceTypes;
	mDeviceTypeListSize = ARRAY_SIZE(kBridgedThermostatDeviceTypes);
	EnablePowerSource();
	mDataVersion = static_cast<DataVersion *>(chip::Platform::MemoryAlloc(sizeof(DataVersion) * mDataVersionSize));
}

CHIP_ERROR ThermostatDevice::HandleRead(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer,
					uint16_t maxReadLength)
{
	if (clusterId == Clusters::Thermostat::Id) {
		return HandleReadThermostat(attributeId, buffer, maxReadLength);
	}
	return CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR ThermostatDevice::HandleReadThermostat(AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength)
{
	int16_t i16;
	uint8_t u8;

	switch (attributeId) {
	case TA::LocalTemperature::Id:
		i16 = mLocalTemperature; /* INT16_MIN is the null value of a nullable int16 */
		return CopyAttribute(&i16, sizeof(i16), buffer, maxReadLength);
	case TA::AbsMinHeatSetpointLimit::Id:
	case TA::MinHeatSetpointLimit::Id:
		i16 = kMinHeatSetpoint;
		return CopyAttribute(&i16, sizeof(i16), buffer, maxReadLength);
	case TA::AbsMaxHeatSetpointLimit::Id:
	case TA::MaxHeatSetpointLimit::Id:
		i16 = kMaxHeatSetpoint;
		return CopyAttribute(&i16, sizeof(i16), buffer, maxReadLength);
	case TA::PIHeatingDemand::Id:
		return CopyAttribute(&mPIHeatingDemand, sizeof(mPIHeatingDemand), buffer, maxReadLength);
	case TA::OccupiedHeatingSetpoint::Id:
		return CopyAttribute(&mOccupiedHeatingSetpoint, sizeof(mOccupiedHeatingSetpoint), buffer, maxReadLength);
	case TA::ControlSequenceOfOperation::Id:
		u8 = to_underlying(Clusters::Thermostat::ControlSequenceOfOperationEnum::kHeatingOnly);
		return CopyAttribute(&u8, sizeof(u8), buffer, maxReadLength);
	case TA::SystemMode::Id:
		return CopyAttribute(&mSystemMode, sizeof(mSystemMode), buffer, maxReadLength);
	case TA::ClusterRevision::Id: {
		uint16_t revision = GetThermostatClusterRevision();
		return CopyAttribute(&revision, sizeof(revision), buffer, maxReadLength);
	}
	case TA::FeatureMap::Id: {
		uint32_t featureMap = GetThermostatFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR ThermostatDevice::HandleWrite(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer, size_t size)
{
	if (clusterId == Clusters::BridgedDeviceBasicInformation::Id &&
	    attributeId == Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, buffer, size);
	}
	if (clusterId != Clusters::Thermostat::Id) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
	switch (attributeId) {
	case TA::OccupiedHeatingSetpoint::Id:
	case TA::SystemMode::Id:
		/* Stored now; the provider then sends it to the thermostat (BridgeManager::HandleWrite). */
		return HandleAttributeChange(clusterId, attributeId, buffer, size);
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR ThermostatDevice::HandleAttributeChange(ClusterId clusterId, AttributeId attributeId, void *data,
						   size_t dataSize)
{
	if (!data) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	switch (clusterId) {
	case Clusters::BridgedDeviceBasicInformation::Id:
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, data, dataSize);
	case Clusters::Identify::Id:
		return HandleWriteIdentify(attributeId, data, dataSize);
	case Clusters::Thermostat::Id:
		break;
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	switch (attributeId) {
	case TA::LocalTemperature::Id:
		return CopyAttribute(data, dataSize, &mLocalTemperature, sizeof(mLocalTemperature));
	case TA::OccupiedHeatingSetpoint::Id:
		return CopyAttribute(data, dataSize, &mOccupiedHeatingSetpoint, sizeof(mOccupiedHeatingSetpoint));
	case TA::PIHeatingDemand::Id:
		return CopyAttribute(data, dataSize, &mPIHeatingDemand, sizeof(mPIHeatingDemand));
	case TA::SystemMode::Id:
		return CopyAttribute(data, dataSize, &mSystemMode, sizeof(mSystemMode));
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}
