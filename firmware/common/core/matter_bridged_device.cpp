/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "matter_bridged_device.h"
#include <app-common/zap-generated/callback.h>
#include <app/AttributeValueDecoder.h>
#include <app/clusters/identify-server/IdentifyCluster.h>
#include <app/clusters/power-source-server/power-source-server.h>
#include <app/data-model/Encode.h>
#include <lib/core/TLVReader.h>
#include <lib/core/TLVWriter.h>
#include <lib/support/ZclString.h>

using namespace ::chip;
using namespace ::chip::app;

namespace Nrf
{

CHIP_ERROR MatterBridgedDevice::CopyAttribute(const void *attribute, size_t attributeSize, void *buffer,
					      uint16_t maxBufferSize)
{
	if (maxBufferSize < attributeSize) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	memcpy(buffer, attribute, attributeSize);

	return CHIP_NO_ERROR;
}

void MatterBridgedDevice::SetNodeLabel(void *data, size_t size)
{
	/* Clean the buffer and fill it with the new data. */
	memset(mNodeLabel, 0, sizeof(mNodeLabel));
	memcpy(mNodeLabel, data, size);
}

CHIP_ERROR MatterBridgedDevice::HandleWriteDeviceBasicInformation(chip::ClusterId clusterId,
								  chip::AttributeId attributeId, void *data,
								  size_t dataSize)
{
	switch (attributeId) {
	case Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id:
		if (data && dataSize == sizeof(bool)) {
			SetIsReachable(*reinterpret_cast<bool *>(data));
			return CHIP_NO_ERROR;
		}
	case Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id:
		SetNodeLabel(data, dataSize);
		return CHIP_NO_ERROR;

	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MatterBridgedDevice::HandleReadBridgedDeviceBasicInformation(AttributeId attributeId, uint8_t *buffer,
									uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id: {
		bool isReachable = GetIsReachable();
		return CopyAttribute(&isReachable, sizeof(isReachable), buffer, maxReadLength);
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Id:
	case Clusters::BridgedDeviceBasicInformation::Attributes::SerialNumber::Id: {
		MutableByteSpan zclUniqueIDSpan(buffer, maxReadLength);
		return MakeZclCharString(zclUniqueIDSpan, GetUniqueID());
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::VendorName::Id: {
		MutableByteSpan span(buffer, maxReadLength);
		return MakeZclCharString(span, GetVendorName());
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::ProductName::Id: {
		MutableByteSpan span(buffer, maxReadLength);
		return MakeZclCharString(span, GetProductName());
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id: {
		MutableByteSpan zclNodeLabelSpan(buffer, maxReadLength);
		return MakeZclCharString(zclNodeLabelSpan, GetNodeLabel());
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::ClusterRevision::Id: {
		uint16_t clusterRevision = GetBridgedDeviceBasicInformationClusterRevision();
		return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
	}
	case Clusters::BridgedDeviceBasicInformation::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetBridgedDeviceBasicInformationFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MatterBridgedDevice::HandleWriteIdentify(chip::AttributeId attributeId, void *data, size_t dataSize)
{
	switch (attributeId) {
	case Clusters::Identify::Attributes::IdentifyTime::Id:
		if (data && dataSize == sizeof(uint16_t)) {
			uint16_t identifyTime = *reinterpret_cast<uint16_t *>(data);

			uint8_t tlvBuffer[64];
			TLV::TLVWriter writer;
			writer.Init(tlvBuffer, sizeof(tlvBuffer));

			TLV::TLVType outerContainerType;
			ReturnErrorOnFailure(writer.StartContainer(TLV::AnonymousTag(), TLV::kTLVType_Structure,
								   outerContainerType));
			ReturnErrorOnFailure(DataModel::Encode(writer, TLV::ContextTag(1), identifyTime));
			ReturnErrorOnFailure(writer.EndContainer(outerContainerType));
			ReturnErrorOnFailure(writer.Finalize());

			TLV::TLVReader reader;
			reader.Init(tlvBuffer, writer.GetLengthWritten());
			ReturnErrorOnFailure(reader.Next());
			ReturnErrorOnFailure(reader.EnterContainer(outerContainerType));
			ReturnErrorOnFailure(reader.Next());

			Access::SubjectDescriptor subjectDescriptor;
			subjectDescriptor.fabricIndex = kUndefinedFabricIndex;
			subjectDescriptor.authMode = Access::AuthMode::kNone;

			AttributeValueDecoder decoder(reader, subjectDescriptor);

			DataModel::WriteAttributeRequest request;
			request.path.mEndpointId = mEndpointId;
			request.path.mClusterId = Clusters::Identify::Id;
			request.path.mAttributeId = attributeId;
			request.subjectDescriptor = &subjectDescriptor;

			DataModel::ActionReturnStatus status = mIdentifyCluster.Cluster().WriteAttribute(request, decoder);
			if (!status.IsSuccess()) {
				return status.GetUnderlyingError();
			}
			return CHIP_NO_ERROR;
		}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MatterBridgedDevice::HandleReadIdentify(chip::AttributeId attributeId, uint8_t *buffer,
						   uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::Identify::Attributes::ClusterRevision::Id: {
		uint16_t clusterRevision = GetIdentifyClusterRevision();
		return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
	}
	case Clusters::Identify::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetIdentifyClusterFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	case Clusters::Identify::Attributes::IdentifyType::Id: {
		MatterBridgedDevice::IdentifyType type = mIdentifyCluster.Cluster().GetIdentifyType();
		return CopyAttribute(&type, sizeof(type), buffer, maxReadLength);
	}
	case Clusters::Identify::Attributes::IdentifyTime::Id: {
		uint16_t identifyTime = mIdentifyCluster.Cluster().GetIdentifyTime();
		return CopyAttribute(&identifyTime, sizeof(identifyTime), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MatterBridgedDevice::HandleReadPowerSource(AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength)
{
	namespace PS = Clusters::PowerSource;

	switch (attributeId) {
	case PS::Attributes::Status::Id: {
		uint8_t value = to_underlying(PS::PowerSourceStatusEnum::kActive);
		return CopyAttribute(&value, sizeof(value), buffer, maxReadLength);
	}
	case PS::Attributes::Order::Id: {
		uint8_t value = 0;
		return CopyAttribute(&value, sizeof(value), buffer, maxReadLength);
	}
	case PS::Attributes::Description::Id: {
		MutableByteSpan span(buffer, maxReadLength);
		return MakeZclCharString(span, "Battery");
	}
	case PS::Attributes::BatPercentRemaining::Id:
		return CopyAttribute(&mBatPercentRemaining, sizeof(mBatPercentRemaining), buffer, maxReadLength);
	case PS::Attributes::BatChargeLevel::Id:
		return CopyAttribute(&mBatChargeLevel, sizeof(mBatChargeLevel), buffer, maxReadLength);
	case PS::Attributes::BatReplacementNeeded::Id:
		return CopyAttribute(&mBatReplacementNeeded, sizeof(mBatReplacementNeeded), buffer, maxReadLength);
	case PS::Attributes::BatReplaceability::Id: {
		uint8_t value = to_underlying(PS::BatReplaceabilityEnum::kUserReplaceable);
		return CopyAttribute(&value, sizeof(value), buffer, maxReadLength);
	}
	case PS::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetPowerSourceFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		/* EndpointList and ClusterRevision are served by the SDK's Power Source server. */
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MatterBridgedDevice::HandleWritePowerSource(AttributeId attributeId, void *data, size_t dataSize)
{
	namespace PS = Clusters::PowerSource;

	VerifyOrReturnError(mHasPowerSource && data && dataSize == 1, CHIP_ERROR_INVALID_ARGUMENT);

	switch (attributeId) {
	case PS::Attributes::BatPercentRemaining::Id:
		mBatPercentRemaining = *static_cast<uint8_t *>(data);
		return CHIP_NO_ERROR;
	case PS::Attributes::BatChargeLevel::Id:
		mBatChargeLevel = *static_cast<uint8_t *>(data);
		return CHIP_NO_ERROR;
	case PS::Attributes::BatReplacementNeeded::Id:
		mBatReplacementNeeded = *static_cast<bool *>(data);
		return CHIP_NO_ERROR;
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

void MatterBridgedDevice::SetPowerSourceEndpointList()
{
	/* The battery powers this endpoint only. The server copies the list. */
	CHIP_ERROR err = Clusters::PowerSourceServer::Instance().SetEndpointList(mEndpointId,
										  Span<EndpointId>(&mEndpointId, 1));
	if (err != CHIP_NO_ERROR) {
		ChipLogError(DeviceLayer, "Failed to set Power Source endpoint list: %" CHIP_ERROR_FORMAT, err.Format());
	}
}

void MatterBridgedDevice::ClearPowerSourceEndpointList()
{
	Clusters::PowerSourceServer::Instance().SetEndpointList(mEndpointId, Span<EndpointId>());
}

} /* namespace Nrf */
