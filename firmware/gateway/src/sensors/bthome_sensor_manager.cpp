/*
 * Bridges advertisement-only BTHome sensors to Matter.
 */

#include "bthome_sensor_manager.h"

#include "ble_bridged_device_factory.h"
#include "bridge_manager.h"
#include "bridge_storage_manager.h"
#include "sensor_scanner.h"

#include <platform/CHIPDeviceLayer.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <string.h>

LOG_MODULE_REGISTER(bthome_sensors, CONFIG_GATEWAY_SENSOR_SCANNER_LOG_LEVEL);

using namespace chip;
using Nrf::BridgeManager;
using Nrf::BridgeStorageManager;
using Nrf::MatterBridgedDevice;

namespace Gateway
{
namespace
{
constexpr char kIgnoreListKey[] = "gw/bthome_ign";
constexpr int64_t kUnreachableTimeoutMs = int64_t(CONFIG_GATEWAY_BTHOME_UNREACHABLE_TIMEOUT_S) * 1000;
constexpr uint32_t kReachabilityCheckMs = 30 * 1000;

/* Readings travel from the Bluetooth RX thread to the Matter thread through this queue. */
struct QueuedReading {
	bt_addr_le_t mAddr;
	bthome_reading mReading;
};

K_MSGQ_DEFINE(sReadingQueue, sizeof(QueuedReading), CONFIG_GATEWAY_BTHOME_QUEUE_SIZE, 4);
atomic_t sProcessScheduled;
k_timer sReachabilityTimer;

/* "A4C1380C54AA": most significant byte first, as printed on the sensor. */
void FormatUniqueId(const bt_addr_le_t &addr, char *out, size_t len)
{
	const uint8_t *a = addr.a.val;
	snprintk(out, len, "%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* "Sensor 54AA": the last two address bytes, like the name pvvx firmware advertises. */
void FormatNodeLabel(const bt_addr_le_t &addr, char *out, size_t len)
{
	snprintk(out, len, "Sensor %02X%02X", addr.a.val[1], addr.a.val[0]);
}

CHIP_ERROR StoreDevice(MatterBridgedDevice *device, const bt_addr_le_t &addr, uint8_t index)
{
	BridgeStorageManager::BridgedDevice stored;
	StoredDeviceUserData userData;

	bt_addr_le_copy(&userData.mAddr, &addr);
	userData.mKind = StoredDeviceUserData::kBTHomeSensor;

	stored.mEndpointId = device->GetEndpointId();
	stored.mDeviceType = device->GetDeviceType();
	stored.mUniqueIDLength = strnlen(device->GetUniqueID(), MatterBridgedDevice::kUniqueIDSize);
	memcpy(stored.mUniqueID, device->GetUniqueID(), stored.mUniqueIDLength);
	stored.mNodeLabelLength = strnlen(device->GetNodeLabel(), MatterBridgedDevice::kNodeLabelSize);
	memcpy(stored.mNodeLabel, device->GetNodeLabel(), stored.mNodeLabelLength);
	stored.mUserDataSize = sizeof(userData);
	stored.mUserData = reinterpret_cast<uint8_t *>(&userData);

	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevice(stored, index), CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

CHIP_ERROR StoreDeviceIndexes()
{
	uint8_t indexes[BridgeManager::kMaxBridgedDevices] = { 0 };
	uint8_t count = 0;

	ReturnErrorOnFailure(BridgeManager::Instance().GetDevicesIndexes(indexes, sizeof(indexes), count));
	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevicesIndexes(indexes, count),
			    CHIP_ERROR_INTERNAL);
	VerifyOrReturnError(BridgeStorageManager::Instance().StoreBridgedDevicesCount(count), CHIP_ERROR_INTERNAL);
	return CHIP_NO_ERROR;
}

struct IgnoreLoadContext {
	bt_addr_le_t *mList;
	size_t mMax;
	size_t *mCount;
};

int LoadIgnoreList(const char *key, size_t len, settings_read_cb readCb, void *cbArg, void *param)
{
	auto *ctx = static_cast<IgnoreLoadContext *>(param);

	/* Only the exact key, not deeper subtrees. */
	if (settings_name_next(key, nullptr) != 0) {
		return 0;
	}

	const size_t maxBytes = ctx->mMax * sizeof(bt_addr_le_t);
	const ssize_t read = readCb(cbArg, ctx->mList, MIN(len, maxBytes));

	*ctx->mCount = read > 0 ? size_t(read) / sizeof(bt_addr_le_t) : 0;
	return 0;
}
} /* namespace */

void BTHomeSensorManager::Init()
{
	IgnoreLoadContext ctx{ mIgnored, kMaxIgnored, &mIgnoredCount };

	mIgnoredCount = 0;
	const int err = settings_load_subtree_direct(kIgnoreListKey, LoadIgnoreList, &ctx);
	if (err) {
		LOG_WRN("Cannot load BTHome ignore list (err %d)", err);
	}
	LOG_INF("BTHome: %u ignored address(es)", unsigned(mIgnoredCount));
}

void BTHomeSensorManager::Start()
{
	/* Restored sensors stay unreachable until their first packet arrives. */
	for (auto &sensor : mSensors) {
		if (sensor.mProvider) {
			sensor.mProvider->SetReachable(false, true);
		}
	}

	sensor_scanner_set_listener(OnScannerReading);
	k_timer_init(&sReachabilityTimer, ReachabilityTimerHandler, nullptr);
	k_timer_start(&sReachabilityTimer, K_MSEC(kReachabilityCheckMs), K_MSEC(kReachabilityCheckMs));
}

void BTHomeSensorManager::OnScannerReading(const bt_addr_le_t *addr, int8_t, const bthome_reading *reading)
{
	/* Bluetooth RX thread: hand over to the Matter thread. */
	QueuedReading item;

	bt_addr_le_copy(&item.mAddr, addr);
	item.mReading = *reading;
	if (k_msgq_put(&sReadingQueue, &item, K_NO_WAIT) != 0) {
		LOG_DBG("Reading queue full, dropping reading");
	}

	if (atomic_cas(&sProcessScheduled, 0, 1)) {
		if (DeviceLayer::PlatformMgr().ScheduleWork(ProcessEvents) != CHIP_NO_ERROR) {
			atomic_set(&sProcessScheduled, 0);
		}
	}
}

void BTHomeSensorManager::ProcessEvents(intptr_t)
{
	/* Clear first, so readings queued while processing schedule another run. */
	atomic_set(&sProcessScheduled, 0);

	QueuedReading item;
	while (k_msgq_get(&sReadingQueue, &item, K_NO_WAIT) == 0) {
		Instance().HandleReading(item.mAddr, item.mReading);
	}
}

void BTHomeSensorManager::ReachabilityTimerHandler(k_timer *)
{
	DeviceLayer::PlatformMgr().ScheduleWork(CheckReachability);
}

void BTHomeSensorManager::CheckReachability(intptr_t)
{
	const int64_t now = k_uptime_get();

	for (auto &sensor : Instance().mSensors) {
		if (sensor.mProvider) {
			sensor.mProvider->CheckReachable(now, kUnreachableTimeoutMs);
		}
	}
}

void BTHomeSensorManager::HandleReading(const bt_addr_le_t &addr, const bthome_reading &reading)
{
	Sensor *sensor = Find(addr);

	if (!sensor) {
		if (!IS_ENABLED(CONFIG_GATEWAY_BTHOME_AUTO_ADD) || IsIgnored(addr)) {
			return;
		}
		/* pvvx alternates packets; only add a sensor once a packet with measurements arrives. */
		if (!(reading.fields & (BTHOME_HAS_TEMPERATURE | BTHOME_HAS_HUMIDITY))) {
			return;
		}
		if (AddSensor(addr, sensor) != CHIP_NO_ERROR) {
			return;
		}
	}

	sensor->mProvider->OnReading(reading, k_uptime_get());
}

CHIP_ERROR BTHomeSensorManager::AddSensor(const bt_addr_le_t &addr, Sensor *&sensor)
{
	char uniqueId[MatterBridgedDevice::kUniqueIDSize];
	char label[MatterBridgedDevice::kNodeLabelSize];
	char addrStr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&addr, addrStr, sizeof(addrStr));

	sensor = FindFree();
	if (!sensor) {
		if (!mFullWarningLogged) {
			LOG_WRN("BTHome: no free sensor slot for %s (max %u)", addrStr, unsigned(kMaxSensors));
			mFullWarningLogged = true;
		}
		return CHIP_ERROR_NO_MEMORY;
	}

	FormatUniqueId(addr, uniqueId, sizeof(uniqueId));
	FormatNodeLabel(addr, label, sizeof(label));

	auto &deviceFactory = BleBridgedDeviceFactory::GetBridgedDeviceFactory();
	MatterBridgedDevice *devices[2] = {
		deviceFactory.Create(MatterBridgedDevice::DeviceType::TemperatureSensor, uniqueId, label),
		deviceFactory.Create(MatterBridgedDevice::DeviceType::HumiditySensor, uniqueId, label),
	};
	auto *provider = Platform::New<BTHomeSensorProvider>(addr, BridgeManager::HandleUpdate);

	if (!devices[0] || !devices[1] || !provider) {
		Platform::Delete(devices[0]);
		Platform::Delete(devices[1]);
		Platform::Delete(provider);
		sensor = nullptr;
		LOG_ERR("BTHome: out of memory adding %s", addrStr);
		return CHIP_ERROR_NO_MEMORY;
	}

	/* Claim the slot before handing the provider over, so its destructor can release it on failure. */
	sensor->mProvider = provider;

	uint8_t indexes[2];
	CHIP_ERROR err = BridgeManager::Instance().AddBridgedDevices(devices, provider, 2, indexes);
	if (err != CHIP_NO_ERROR) {
		/* The bridge manager deleted the devices and the provider. */
		LOG_ERR("BTHome: cannot add %s: %" CHIP_ERROR_FORMAT, addrStr, err.Format());
		sensor = nullptr;
		return err;
	}

	sensor->mTemperatureEndpoint = devices[0]->GetEndpointId();
	sensor->mHumidityEndpoint = devices[1]->GetEndpointId();

	for (uint8_t i = 0; i < 2 && err == CHIP_NO_ERROR; i++) {
		err = StoreDevice(devices[i], addr, indexes[i]);
	}
	if (err == CHIP_NO_ERROR) {
		err = StoreDeviceIndexes();
	}
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("BTHome: %s added but not stored: %" CHIP_ERROR_FORMAT, addrStr, err.Format());
	}

	LOG_INF("BTHome: added %s as \"%s\" (endpoints %u, %u)", addrStr, label, sensor->mTemperatureEndpoint,
		sensor->mHumidityEndpoint);
	return CHIP_NO_ERROR;
}

CHIP_ERROR BTHomeSensorManager::Restore(const bt_addr_le_t &addr, uint16_t deviceType, const char *uniqueID,
					const char *nodeLabel, uint8_t index, uint16_t endpointId)
{
	Sensor *sensor = Find(addr);

	if (!sensor) {
		sensor = FindFree();
		VerifyOrReturnError(sensor, CHIP_ERROR_NO_MEMORY, LOG_ERR("BTHome: no free slot to restore sensor"));
		sensor->mProvider = Platform::New<BTHomeSensorProvider>(addr, BridgeManager::HandleUpdate);
		VerifyOrReturnError(sensor->mProvider, CHIP_ERROR_NO_MEMORY);
	}

	MatterBridgedDevice *device =
		BleBridgedDeviceFactory::GetBridgedDeviceFactory().Create(deviceType, uniqueID, nodeLabel);
	VerifyOrReturnError(device, CHIP_ERROR_NO_MEMORY);

	ReturnErrorOnFailure(
		BridgeManager::Instance().AddBridgedDevices(&device, sensor->mProvider, 1, &index, &endpointId));

	if (deviceType == MatterBridgedDevice::DeviceType::TemperatureSensor) {
		sensor->mTemperatureEndpoint = endpointId;
	} else if (deviceType == MatterBridgedDevice::DeviceType::HumiditySensor) {
		sensor->mHumidityEndpoint = endpointId;
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR BTHomeSensorManager::Remove(const bt_addr_le_t &addr)
{
	CHIP_ERROR err = CHIP_NO_ERROR;
	Sensor *sensor = Find(addr);

	if (sensor) {
		/* Copy: removing the last endpoint deletes the provider, which clears the slot. */
		const uint16_t endpoints[] = { sensor->mTemperatureEndpoint, sensor->mHumidityEndpoint };

		for (uint16_t endpoint : endpoints) {
			if (endpoint != 0) {
				CHIP_ERROR removeErr = BleBridgedDeviceFactory::RemoveDevice(endpoint);
				if (removeErr != CHIP_NO_ERROR) {
					err = removeErr;
				}
			}
		}
	}

	if (!IsIgnored(addr)) {
		VerifyOrReturnError(mIgnoredCount < kMaxIgnored, CHIP_ERROR_NO_MEMORY);
		bt_addr_le_copy(&mIgnored[mIgnoredCount++], &addr);
		ReturnErrorOnFailure(SaveIgnored());
	}
	return err;
}

CHIP_ERROR BTHomeSensorManager::Unignore(const bt_addr_le_t &addr)
{
	for (size_t i = 0; i < mIgnoredCount; i++) {
		if (bt_addr_le_eq(&mIgnored[i], &addr)) {
			mIgnored[i] = mIgnored[--mIgnoredCount];
			return SaveIgnored();
		}
	}
	return CHIP_ERROR_NOT_FOUND;
}

size_t BTHomeSensorManager::GetSensors(SensorInfo *out, size_t max) const
{
	size_t n = 0;

	for (const auto &sensor : mSensors) {
		if (!sensor.mProvider || n >= max) {
			continue;
		}
		const BTHomeSensorProvider &p = *sensor.mProvider;
		SensorInfo &info = out[n++];

		bt_addr_le_copy(&info.mAddr, &p.GetAddress());
		info.mTemperatureEndpoint = sensor.mTemperatureEndpoint;
		info.mHumidityEndpoint = sensor.mHumidityEndpoint;
		info.mReachable = p.IsReachable();
		info.mLastSeenMs = p.GetLastSeenMs();
		info.mHasTemperature = p.HasTemperature();
		info.mHasHumidity = p.HasHumidity();
		info.mTemperature = p.GetTemperature();
		info.mHumidity = p.GetHumidity();
		info.mBattery = p.GetBattery();
	}
	return n;
}

size_t BTHomeSensorManager::GetIgnored(bt_addr_le_t *out, size_t max) const
{
	const size_t n = MIN(max, mIgnoredCount);

	memcpy(out, mIgnored, n * sizeof(bt_addr_le_t));
	return n;
}

void BTHomeSensorManager::OnProviderDeleted(BTHomeSensorProvider *provider)
{
	for (auto &sensor : mSensors) {
		if (sensor.mProvider == provider) {
			sensor = Sensor{};
			mFullWarningLogged = false;
		}
	}
}

BTHomeSensorManager::Sensor *BTHomeSensorManager::Find(const bt_addr_le_t &addr)
{
	for (auto &sensor : mSensors) {
		if (sensor.mProvider && bt_addr_le_eq(&sensor.mProvider->GetAddress(), &addr)) {
			return &sensor;
		}
	}
	return nullptr;
}

BTHomeSensorManager::Sensor *BTHomeSensorManager::FindFree()
{
	for (auto &sensor : mSensors) {
		if (!sensor.mProvider) {
			return &sensor;
		}
	}
	return nullptr;
}

bool BTHomeSensorManager::IsIgnored(const bt_addr_le_t &addr) const
{
	for (size_t i = 0; i < mIgnoredCount; i++) {
		if (bt_addr_le_eq(&mIgnored[i], &addr)) {
			return true;
		}
	}
	return false;
}

CHIP_ERROR BTHomeSensorManager::SaveIgnored()
{
	const int err = settings_save_one(kIgnoreListKey, mIgnored, mIgnoredCount * sizeof(bt_addr_le_t));

	if (err) {
		LOG_ERR("Cannot save BTHome ignore list (err %d)", err);
		return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
	}
	return CHIP_NO_ERROR;
}

} /* namespace Gateway */
