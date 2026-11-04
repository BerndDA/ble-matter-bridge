# Source this file to build and flash from a terminal:
#   source scripts/ncs-env.sh
#   west build -b nrf52840dk/nrf52840 --sysbuild -d firmware/gateway/build firmware/gateway
#   west flash -d firmware/gateway/build
#
# It uses the real install path (not the /opt/nordic symlink), because builds through
# the symlink fail to resolve some linker script includes.

NCS_ROOT="${NCS_ROOT:-/Volumes/mac-mini-external/nordic}"
NCS_VERSION="v3.4.1"
NCS_TOOLCHAIN="${NCS_ROOT}/ncs/toolchains/97226bad62"

export ZEPHYR_BASE="${NCS_ROOT}/ncs/${NCS_VERSION}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${NCS_TOOLCHAIN}/opt/zephyr-sdk"
export PATH="${NCS_TOOLCHAIN}/bin:${NCS_TOOLCHAIN}/nrfutil/bin:${PATH}"

# `west flash` needs the nrfutil "device" command, which the VS Code extension ships.
_nrf_ext=$(ls -d "${HOME}"/.vscode/extensions/nordic-semiconductor.nrf-connect-*/platform/nrfutil 2>/dev/null | tail -1)
if [ -n "${_nrf_ext}" ]; then
	export NRFUTIL_HOME="${_nrf_ext}"
else
	echo "ncs-env: nRF Connect VS Code extension not found; west flash may not work" >&2
fi
unset _nrf_ext
