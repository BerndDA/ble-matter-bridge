/*
 * Shell subcommands shared by the gateway apps, for their `gateway` command:
 *   gateway identity     chip ID, serial number and Matter setup code (also in release builds)
 *   gateway bootloader   restart into the UF2 bootloader (Pro Micro UF2 builds only,
 *                        CONFIG_GATEWAY_UF2_BOOTLOADER_CMD); make flash BOARD=promicro-uf2 uses it
 */

#pragma once

#include <zephyr/shell/shell.h>

namespace Gateway::IdentityShell
{

int CmdIdentity(const struct shell *sh, size_t argc, char **argv);

#if defined(CONFIG_GATEWAY_UF2_BOOTLOADER_CMD)
int CmdBootloader(const struct shell *sh, size_t argc, char **argv);
#endif

} /* namespace Gateway::IdentityShell */
