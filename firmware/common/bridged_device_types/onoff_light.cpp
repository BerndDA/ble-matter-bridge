/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "onoff_light.h"

#include "bridge_manager.h"

#include <zephyr/logging/log.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <app/AttributeAccessInterface.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <clusters/ElectricalPowerMeasurement/Metadata.h>
#include <clusters/PowerTopology/Metadata.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
DESCRIPTOR_CLUSTER_ATTRIBUTES(descriptorAttrs);
BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_ATTRIBUTES(bridgedDeviceBasicAttrs);
}; /* namespace */

using namespace ::chip;
using namespace ::chip::app;
using namespace Nrf;

namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;
namespace PowerTopology = chip::app::Clusters::PowerTopology;

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::GlobalSceneControl::Id, BOOLEAN, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OnTime::Id, INT16U, 2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OffWaitTime::Id, INT16U, 2,
				  ZAP_ATTRIBUTE_MASK(WRITABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::StartUpOnOff::Id, ENUM8, 1,
				  ZAP_ATTRIBUTE_MASK(WRITABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId onOffIncomingCommands[] = {
	app::Clusters::OnOff::Commands::Off::Id,
	app::Clusters::OnOff::Commands::On::Id,
	app::Clusters::OnOff::Commands::Toggle::Id,
	app::Clusters::OnOff::Commands::OffWithEffect::Id,
	app::Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
	app::Clusters::OnOff::Commands::OnWithTimedOff::Id,
	kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(groupsAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Groups::Attributes::NameSupport::Id, BITMAP8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Groups::Attributes::FeatureMap::Id, BITMAP32, 4, 1),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId groupsIncomingCommands[] = {
	app::Clusters::Groups::Commands::AddGroup::Id,
	app::Clusters::Groups::Commands::ViewGroup::Id,
	app::Clusters::Groups::Commands::GetGroupMembership::Id,
	app::Clusters::Groups::Commands::RemoveGroup::Id,
	app::Clusters::Groups::Commands::RemoveAllGroups::Id,
	app::Clusters::Groups::Commands::AddGroupIfIdentifying::Id,
	kInvalidCommandId,
};

constexpr CommandId groupsGeneratedCommands[] = {
	app::Clusters::Groups::Commands::AddGroupResponse::Id,
	app::Clusters::Groups::Commands::RemoveGroupResponse::Id,
	app::Clusters::Groups::Commands::ViewGroupResponse::Id,
	app::Clusters::Groups::Commands::GetGroupMembershipResponse::Id,
	kInvalidCommandId,
};

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedLightClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands,
				groupsGeneratedCommands),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedLightEndpoint, bridgedLightClusters);

/* Served by PowerMeasurementAccess below, not by HandleRead. */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(powerMeasurementAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(EPM::Attributes::PowerMode::Id, ENUM8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(EPM::Attributes::NumberOfMeasurementTypes::Id, INT8U, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(EPM::Attributes::Accuracy::Id, ARRAY, 0, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(EPM::Attributes::ActivePower::Id, INT64S, 8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
	DECLARE_DYNAMIC_ATTRIBUTE(EPM::Attributes::FeatureMap::Id, BITMAP32, 4, 0), DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(powerTopologyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerTopology::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedPowerClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands,
				groupsGeneratedCommands),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(EPM::Id, powerMeasurementAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, powerTopologyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedPowerEndpoint, bridgedPowerClusters);

static constexpr uint8_t kBridgedOnOffEndpointVersion = 3;

static constexpr EmberAfDeviceType kBridgedOnOffDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::OnOffLight), kBridgedOnOffEndpointVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr EmberAfDeviceType kBridgedPlugInUnitDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::OnOffPlugInUnit),
	  kBridgedOnOffEndpointVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr uint8_t kElectricalSensorVersion = 1;

static constexpr EmberAfDeviceType kBridgedOnOffPowerDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::OnOffLight), kBridgedOnOffEndpointVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::ElectricalSensor), kElectricalSensorVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr EmberAfDeviceType kBridgedPlugInUnitPowerDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::OnOffPlugInUnit),
	  kBridgedOnOffEndpointVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::ElectricalSensor), kElectricalSensorVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr uint8_t kLightDataVersionSize = MATTER_ARRAY_SIZE(bridgedLightClusters);
static constexpr uint8_t kPowerDataVersionSize = MATTER_ARRAY_SIZE(bridgedPowerClusters);

namespace
{
/* Largest value in the Accuracy attribute: 16 A at 230 V */
constexpr int64_t kMaxActivePowerMw = 3680000;
/* The actuators report whole watts */
constexpr uint64_t kActivePowerResolutionMw = 1000;

/*
 * Electrical Power Measurement of all endpoints that have it (the SDK only asks for clusters an endpoint
 * declares, so every endpoint seen here belongs to an OnOffLightDevice with measuresPower).
 */
class PowerMeasurementAccess : public AttributeAccessInterface {
public:
	PowerMeasurementAccess() : AttributeAccessInterface(NullOptional, EPM::Id) {}

	CHIP_ERROR Read(const ConcreteReadAttributePath &aPath, AttributeValueEncoder &aEncoder) override
	{
		switch (aPath.mAttributeId) {
		case EPM::Attributes::FeatureMap::Id:
			return aEncoder.Encode(static_cast<uint32_t>(EPM::Feature::kAlternatingCurrent));
		case EPM::Attributes::ClusterRevision::Id:
			return aEncoder.Encode(static_cast<uint16_t>(EPM::kRevision));
		case EPM::Attributes::PowerMode::Id:
			return aEncoder.Encode(EPM::PowerModeEnum::kAc);
		case EPM::Attributes::NumberOfMeasurementTypes::Id:
			return aEncoder.Encode(static_cast<uint8_t>(1));
		case EPM::Attributes::Accuracy::Id:
			return aEncoder.EncodeList([](const auto &encoder) -> CHIP_ERROR {
				EPM::Structs::MeasurementAccuracyRangeStruct::Type range;
				range.rangeMin = 0;
				range.rangeMax = kMaxActivePowerMw;
				range.fixedMax.SetValue(kActivePowerResolutionMw);

				EPM::Structs::MeasurementAccuracyStruct::Type accuracy;
				accuracy.measurementType = EPM::MeasurementTypeEnum::kActivePower;
				accuracy.measured = true;
				accuracy.minMeasuredValue = 0;
				accuracy.maxMeasuredValue = kMaxActivePowerMw;
				accuracy.accuracyRanges = DataModel::List<const EPM::Structs::MeasurementAccuracyRangeStruct::Type>(
					&range, 1);
				return encoder.Encode(accuracy);
			});
		case EPM::Attributes::ActivePower::Id: {
			uint8_t index;
			auto *device = static_cast<OnOffLightDevice *>(
				BridgeManager::Instance().GetDevice(aPath.mEndpointId, index));

			VerifyOrReturnError(device && device->MeasuresPower(), CHIP_ERROR_NOT_FOUND);
			return aEncoder.Encode(device->GetActivePower());
		}
		default:
			/* Not encoded: the SDK falls back to the attribute storage. */
			return CHIP_NO_ERROR;
		}
	}
};

PowerMeasurementAccess sPowerMeasurementAccess;
} /* namespace */

CHIP_ERROR OnOffLightDevice::RegisterPowerMeasurement()
{
	static bool sRegistered;

	if (!sRegistered) {
		VerifyOrReturnError(AttributeAccessInterfaceRegistry::Instance().Register(&sPowerMeasurementAccess),
				    CHIP_ERROR_INCORRECT_STATE);
		sRegistered = true;
	}
	return CHIP_NO_ERROR;
}

OnOffLightDevice::OnOffLightDevice(const char *uniqueID, const char *nodeLabel, uint16_t deviceType,
				   const char *vendorName, const char *productName, bool measuresPower)
	: MatterBridgedDevice(uniqueID, nodeLabel), mDeviceType(deviceType), mVendorName(vendorName),
	  mProductName(productName), mMeasuresPower(measuresPower)
{
	mDataVersionSize = measuresPower ? kPowerDataVersionSize : kLightDataVersionSize;
	mEp = measuresPower ? &bridgedPowerEndpoint : &bridgedLightEndpoint;
	if (deviceType == MatterBridgedDevice::DeviceType::OnOffPlugInUnit) {
		mDeviceTypeList = measuresPower ? kBridgedPlugInUnitPowerDeviceTypes : kBridgedPlugInUnitDeviceTypes;
		mDeviceTypeListSize = measuresPower ? ARRAY_SIZE(kBridgedPlugInUnitPowerDeviceTypes)
						    : ARRAY_SIZE(kBridgedPlugInUnitDeviceTypes);
	} else {
		mDeviceType = MatterBridgedDevice::DeviceType::OnOffLight;
		mDeviceTypeList = measuresPower ? kBridgedOnOffPowerDeviceTypes : kBridgedOnOffDeviceTypes;
		mDeviceTypeListSize =
			measuresPower ? ARRAY_SIZE(kBridgedOnOffPowerDeviceTypes) : ARRAY_SIZE(kBridgedOnOffDeviceTypes);
	}
	mDataVersion = static_cast<DataVersion *>(chip::Platform::MemoryAlloc(sizeof(DataVersion) * mDataVersionSize));
}

CHIP_ERROR OnOffLightDevice::HandleRead(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer,
					uint16_t maxReadLength)
{
	switch (clusterId) {
	case Clusters::OnOff::Id:
		return HandleReadOnOff(attributeId, buffer, maxReadLength);
	case Clusters::Groups::Id:
		return HandleReadGroups(attributeId, buffer, maxReadLength);
	case PowerTopology::Id:
		if (attributeId == PowerTopology::Attributes::FeatureMap::Id) {
			uint32_t featureMap = static_cast<uint32_t>(PowerTopology::Feature::kNodeTopology);
			return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
		}
		if (attributeId == PowerTopology::Attributes::ClusterRevision::Id) {
			uint16_t clusterRevision = static_cast<uint16_t>(PowerTopology::kRevision);
			return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
		}
		return CHIP_ERROR_INVALID_ARGUMENT;
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR OnOffLightDevice::HandleReadOnOff(AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::OnOff::Attributes::OnOff::Id: {
		bool onOff = GetOnOff();
		return CopyAttribute(&onOff, sizeof(onOff), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::ClusterRevision::Id: {
		uint16_t clusterRevision = GetOnOffClusterRevision();
		return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetOnOffFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::OnTime::Id: {
		return CopyAttribute(&mOnTime, sizeof(mOnTime), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::OffWaitTime::Id: {
		return CopyAttribute(&mOffWaitTime, sizeof(mOffWaitTime), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::StartUpOnOff::Id: {
		return CopyAttribute(&mStartUpOnOff, sizeof(mStartUpOnOff), buffer, maxReadLength);
	}
	case Clusters::OnOff::Attributes::GlobalSceneControl::Id: {
		return CopyAttribute(&mGlobalSceneControl, sizeof(mGlobalSceneControl), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR OnOffLightDevice::HandleReadGroups(AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::Groups::Attributes::NameSupport::Id: {
		uint8_t nameSupportMap = GetGroupsNameSupportMap();
		return CopyAttribute(&nameSupportMap, sizeof(nameSupportMap), buffer, maxReadLength);
	}
	case Clusters::Groups::Attributes::ClusterRevision::Id: {
		uint16_t clusterRevision = GetGroupsClusterRevision();
		return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
	}
	case Clusters::Groups::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetGroupsFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR OnOffLightDevice::HandleWrite(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer, size_t size)
{
	if (clusterId != Clusters::OnOff::Id && clusterId != Clusters::BridgedDeviceBasicInformation::Id) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	switch (attributeId) {
	case Clusters::OnOff::Attributes::OnOff::Id:
		mOnOff = *buffer;
		break;
	case Clusters::OnOff::Attributes::OnTime::Id:
		mOnTime = *buffer;
		break;
	case Clusters::OnOff::Attributes::OffWaitTime::Id:
		mOffWaitTime = *buffer;
		break;
	case Clusters::OnOff::Attributes::StartUpOnOff::Id:
		mStartUpOnOff = static_cast<Clusters::OnOff::StartUpOnOffEnum>(*buffer);
		break;
	case Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id:
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, buffer, size);
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR OnOffLightDevice::HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
						   size_t dataSize)
{
	CHIP_ERROR err = CHIP_NO_ERROR;
	if (!data) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	switch (clusterId) {
	case Clusters::BridgedDeviceBasicInformation::Id:
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, data, dataSize);
	case Clusters::Identify::Id:
		return HandleWriteIdentify(attributeId, data, dataSize);
	case EPM::Id: {
		/* Only the provider calls this; the value is not writable from Matter. */
		VerifyOrReturnError(mMeasuresPower && attributeId == EPM::Attributes::ActivePower::Id,
				    CHIP_ERROR_INVALID_ARGUMENT);

		int64_t milliwatts;

		ReturnErrorOnFailure(CopyAttribute(data, dataSize, &milliwatts, sizeof(milliwatts)));
		if (!mActivePower.IsNull() && mActivePower.Value() == milliwatts) {
			/* Unchanged: no report */
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
		mActivePower.SetNonNull(milliwatts);
		break;
	}
	case Clusters::OnOff::Id: {
		switch (attributeId) {
		case Clusters::OnOff::Attributes::OnOff::Id: {
			bool value;

			err = CopyAttribute(data, dataSize, &value, sizeof(value));

			if (err != CHIP_NO_ERROR) {
				return err;
			}
			mOnOff = value;
			break;
		}
		default:
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
		break;
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	return err;
}
