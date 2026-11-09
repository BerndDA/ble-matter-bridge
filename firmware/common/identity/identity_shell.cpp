/*
 * `gateway identity` and `gateway bootloader`. See identity_shell.h.
 */

#include "identity_shell.h"

#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <hal/nrf_ficr.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_GATEWAY_UF2_BOOTLOADER_CMD)
#include <hal/nrf_power.h>
#endif

namespace Gateway::IdentityShell
{

int CmdIdentity(const struct shell *sh, size_t, char **)
{
	char qr[128];
	char manual[32];
	char serial[48] = "?";
	chip::MutableCharSpan qrSpan(qr);
	chip::MutableCharSpan manualSpan(manual);
	const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);

	chip::DeviceLayer::PlatformMgr().LockChipStack();
	const CHIP_ERROR qrErr = GetQRCode(qrSpan, flags);
	const CHIP_ERROR manualErr = GetManualPairingCode(manualSpan, flags);
	auto *info = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
	if (info) {
		info->GetSerialNumber(serial, sizeof(serial));
	}
	chip::DeviceLayer::PlatformMgr().UnlockChipStack();

	/* Same format as the UF2 bootloader's USB serial number and src/identity/chip_id_identity.h */
	shell_print(sh, "Chip ID:       %08X%08X", unsigned(nrf_ficr_deviceid_get(NRF_FICR, 1)),
		    unsigned(nrf_ficr_deviceid_get(NRF_FICR, 0)));
	shell_print(sh, "Serial number: %s", serial);
	if (qrErr != CHIP_NO_ERROR || manualErr != CHIP_NO_ERROR) {
		shell_error(sh, "Setup code not available (%" CHIP_ERROR_FORMAT ")",
			    (qrErr != CHIP_NO_ERROR ? qrErr : manualErr).Format());
		return -EIO;
	}
	shell_print(sh, "Setup code:    %.*s", int(manualSpan.size()), manualSpan.data());
	shell_print(sh, "QR code:       %.*s", int(qrSpan.size()), qrSpan.data());
	return 0;
}

#if defined(CONFIG_GATEWAY_UF2_BOOTLOADER_CMD)
/* Adafruit_nRF52_Bootloader: GPREGRET value that keeps it in UF2 mode after a reset (DFU_MAGIC_UF2_RESET). */
constexpr uint32_t kUf2BootloaderMagic = 0x57;

int CmdBootloader(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "Restarting into the UF2 bootloader");
	/* Let the line reach the host before USB goes away. */
	k_sleep(K_MSEC(200));
	nrf_power_gpregret_set(NRF_POWER, 0, kUf2BootloaderMagic);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
#endif

} /* namespace Gateway::IdentityShell */
