/*
 * `gateway ...` shell commands of the EnOcean gateway (shared with the BLE gateway,
 * firmware/common/identity/identity_shell.h):
 *   gateway identity     chip ID, serial number and Matter setup code
 *   gateway bootloader   restart into the UF2 bootloader (Pro Micro UF2 builds)
 */

#include "identity/identity_shell.h"

#include <zephyr/shell/shell.h>

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_gateway,
	SHELL_CMD_ARG(identity, NULL, "Show chip ID, serial number and the Matter setup code.",
		      Gateway::IdentityShell::CmdIdentity, 1, 0),
	SHELL_COND_CMD_ARG(IS_ENABLED(CONFIG_GATEWAY_UF2_BOOTLOADER_CMD), bootloader, NULL,
			   "Restart into the UF2 bootloader, which then shows up as a USB drive.",
			   COND_CODE_1(CONFIG_GATEWAY_UF2_BOOTLOADER_CMD, (Gateway::IdentityShell::CmdBootloader),
				       (NULL)),
			   1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gateway, &sub_gateway, "Gateway identity", NULL);
