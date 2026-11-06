/*
 * Matter identity derived from the nRF52840's chip ID (FICR DEVICEID).
 *
 * One firmware image for every board, yet each board has its own serial number,
 * discriminator, setup code, SPAKE2+ salt and verifier, and rotating device ID.
 * Everything else (certificates, vendor, product) still comes from the factory data.
 *
 * The derivation equals scripts/make-uf2.py (derive_identity), so `make identity`
 * prints the same codes the board uses:
 *   chip ID  = DEVICEID as 16 hex digits, most significant first ("%08X%08X" of
 *              DEVICEID[1], DEVICEID[0]); the UF2 bootloader's USB serial number
 *   h(label) = SHA-256("blegw-identity-v1:<label>:<chip ID>")
 *   serial number   "BLEGW-<chip ID>"
 *   discriminator   first 2 bytes of h("discriminator"), big endian, & 0xFFF
 *   setup code      1 + (first 4 bytes of h("passcode<n>"), big endian) % 99999998,
 *                   n = 0, 1, ... until it isn't one of the codes Matter forbids
 *   SPAKE2+ salt    h("salt")
 *   rotating ID     first 16 bytes of h("rd_uid")
 */

#pragma once

#include <crypto/CHIPCryptoPAL.h>
#include <platform/nrfconnect/FactoryDataProvider.h>

namespace Gateway
{

class ChipIdFactoryDataProvider
	: public chip::DeviceLayer::FactoryDataProvider<chip::DeviceLayer::InternalFlashFactoryData> {
public:
	using Base = chip::DeviceLayer::FactoryDataProvider<chip::DeviceLayer::InternalFlashFactoryData>;

	static constexpr size_t kChipIdLength = 16;

	CHIP_ERROR Init() override;

	CHIP_ERROR GetSetupDiscriminator(uint16_t &setupDiscriminator) override;
	CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &saltBuf) override;
	CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf, size_t &verifierLen) override;
	CHIP_ERROR GetSetupPasscode(uint32_t &setupPasscode) override;
	CHIP_ERROR GetSerialNumber(char *buf, size_t bufSize) override;
	CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &uniqueIdSpan) override;

	/* The chip ID as printed by the UF2 bootloader, NUL-terminated. Valid after Init(). */
	const char *GetChipId() const { return mChipId; }

private:
	static constexpr char kSerialPrefix[] = "BLEGW-";

	char mChipId[kChipIdLength + 1] = {};
	char mSerial[sizeof(kSerialPrefix) + kChipIdLength] = {};
	uint16_t mDiscriminator = 0;
	uint32_t mPasscode = 0;
	uint8_t mSalt[chip::Crypto::kSHA256_Hash_Length] = {};
	uint8_t mRotatingId[16] = {};
	uint8_t mVerifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
	size_t mVerifierLength = 0;
};

} /* namespace Gateway */
