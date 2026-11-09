/*
 * EnOcean-to-Matter-over-Thread gateway.
 * Based on the nRF Connect SDK Matter Bridge application
 * (Copyright (c) 2023 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause).
 */

#include "app_task.h"
#include "enocean_manager.h"
#ifdef CONFIG_GATEWAY_IDENTITY_FROM_CHIP_ID
#include "identity/chip_id_identity.h"
#endif
#ifdef CONFIG_GATEWAY_NET_LOG
#include "netlog/net_log.h"
#endif
#ifdef CONFIG_GATEWAY_WEB
#include "web/web_server.h"
#include "web_status.h"
#endif

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "bridge_manager.h"
#include "bridge_storage_manager.h"
#include "clusters/identify.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{

constexpr EndpointId kBridgeEndpointId = 1;

#ifdef CONFIG_GATEWAY_WEB
/* src/web/index.html, gzip-compressed at build time (CMakeLists.txt) */
const uint8_t sWebPage[] = {
#include "index.html.gz.inc"
};
#endif

/* Trigger effect is enabled for the bridge device */
Nrf::Matter::IdentifyCluster sIdentifyCluster(kBridgeEndpointId, true);

#ifndef CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS
void AppFactoryResetHandler(const ChipDeviceEvent *event, intptr_t /* unused */)
{
	switch (event->Type) {
	case DeviceEventType::kFactoryReset:
		Nrf::BridgeStorageManager::Instance().FactoryReset();
		break;
	default:
		break;
	}
}
#endif

#ifdef CONFIG_GATEWAY_IDENTITY_FROM_CHIP_ID
Gateway::ChipIdFactoryDataProvider sChipIdFactoryDataProvider;
#endif
} /* namespace */

CHIP_ERROR AppTask::RestoreBridgedDevices()
{
	uint8_t count;
	uint8_t indexes[Nrf::BridgeManager::kMaxBridgedDevices] = { 0 };
	size_t indexesCount = 0;

	if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevicesCount(count)) {
		LOG_INF("No bridged devices to load from the storage.");
		return CHIP_NO_ERROR;
	}

	if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevicesIndexes(
		    indexes, Nrf::BridgeManager::kMaxBridgedDevices, indexesCount)) {
		return CHIP_NO_ERROR;
	}

	for (size_t i = 0; i < indexesCount; i++) {
		Nrf::BridgeStorageManager::BridgedDevice device;
		Gateway::EnOceanUserData userData{};

		device.mUserData = reinterpret_cast<uint8_t *>(&userData);
		device.mUserDataSize = sizeof(userData);

		if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevice(device, indexes[i])) {
			return CHIP_ERROR_NOT_FOUND;
		}
		if (device.mUserDataSize != sizeof(userData)) {
			LOG_WRN("Stored device %u is not an EnOcean actuator, skipped", indexes[i]);
			continue;
		}

		CHIP_ERROR err = Gateway::EnOceanManager::Instance().Restore(userData, device.mDeviceType,
									     device.mUniqueID, device.mNodeLabel,
									     indexes[i], device.mEndpointId);
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("Cannot restore device %u: %" CHIP_ERROR_FORMAT, indexes[i], err.Format());
		}
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{
#ifdef CONFIG_GATEWAY_IDENTITY_FROM_CHIP_ID
		.mFactoryDataProvider = &sChipIdFactoryDataProvider,
#endif
		.mPostServerInitClbk = [] {
		CHIP_ERROR err = Nrf::BridgeManager::Instance().Init(RestoreBridgedDevices);
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("BridgeManager initialization failed");
			return err;
		}

		/* A missing or unwired TCM is not fatal: Matter keeps running, sending fails. */
		if (Gateway::EnOceanManager::Instance().Start() != CHIP_NO_ERROR) {
			LOG_ERR("EnOcean TCM initialization failed");
		}
#ifdef CONFIG_GATEWAY_NET_LOG
		Gateway::NetLog::Init();
#endif
#ifdef CONFIG_GATEWAY_WEB
		Gateway::Web::Init(sWebPage, sizeof(sWebPage), Gateway::WriteWebStatus);
#endif
		return CHIP_NO_ERROR;
	} }));

	if (!Nrf::GetBoard().Init()) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter
	 * network state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

#ifndef CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS
	/* The whole settings partition is not erased on factory reset; clean up the bridge storage. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(AppFactoryResetHandler, 0));
#endif

	ReturnErrorOnFailure(sIdentifyCluster.Init());

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
