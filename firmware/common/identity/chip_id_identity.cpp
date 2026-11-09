/*
 * Matter identity derived from the nRF52840's chip ID. See chip_id_identity.h.
 */

#include "chip_id_identity.h"

#include <lib/support/CodeUtils.h>
#ifdef CONFIG_CHIP_CRYPTO_PSA_MIGRATE_DAC_PRIV_KEY
#include <crypto/PSAKeyAllocator.h>
#include <platform/nrfconnect/FactoryDataParser.h>

#include <psa/crypto.h>
#endif

#include <hal/nrf_ficr.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(chip_id_identity, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;

namespace Gateway
{
namespace
{
constexpr uint32_t kMaxPasscode = 99999998;

bool IsForbiddenPasscode(uint32_t code)
{
	/* Matter core spec 5.1.7.1 */
	static constexpr uint32_t kForbidden[] = { 0,	     11111111, 22222222, 33333333, 44444444, 55555555,
						   66666666, 77777777, 88888888, 99999999, 12345678, 87654321 };
	for (uint32_t forbidden : kForbidden) {
		if (code == forbidden) {
			return true;
		}
	}
	return false;
}

/* SHA-256("blegw-identity-v1:<label>:<chip ID>") */
CHIP_ERROR Hash(const char *label, const char *chipId, uint8_t (&out)[Crypto::kSHA256_Hash_Length])
{
	char input[64];
	const int len = snprintf(input, sizeof(input), "blegw-identity-v1:%s:%s", label, chipId);

	VerifyOrReturnError(len > 0 && size_t(len) < sizeof(input), CHIP_ERROR_BUFFER_TOO_SMALL);
	return Crypto::Hash_SHA256(reinterpret_cast<const uint8_t *>(input), size_t(len), out);
}

uint32_t BigEndian(const uint8_t *p, size_t n)
{
	uint32_t v = 0;
	for (size_t i = 0; i < n; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}
#ifdef CONFIG_CHIP_CRYPTO_PSA_MIGRATE_DAC_PRIV_KEY
/*
 * The SDK moves the DAC private key from the factory data into the PSA key storage at the first
 * start and zeroes it in the factory data. If a key is already stored, it keeps that one and
 * zeroes the new key. But a key in the factory data means the factory data was just written (a
 * flash or a UF2 copy), and the stored key may belong to other factory data: the BLE and EnOcean
 * gateways have different DACs, and a board switched over UF2 keeps the settings area, key storage
 * included. The device then signs with the wrong key; Apple Home calls it uncertified and fails
 * the pairing at the CSR. So a key in the factory data replaces the stored one.
 */
void ReplaceStoredDacKeyIfNew()
{
	DeviceLayer::InternalFlashFactoryData flash;
	FactoryData factoryData = {};
	uint8_t *data = nullptr;
	size_t size = 0;
	static const uint8_t kZeroKey[32] = {};

	if (flash.GetFactoryDataPartition(data, size) != CHIP_NO_ERROR ||
	    !ParseFactoryData(data, static_cast<uint16_t>(size), &factoryData)) {
		return;
	}
	if (factoryData.dac_priv_key.len != sizeof(kZeroKey) ||
	    memcmp(factoryData.dac_priv_key.data, kZeroKey, sizeof(kZeroKey)) == 0) {
		/* Already moved to the key storage: the normal start. */
		return;
	}
	psa_crypto_init();
	if (psa_destroy_key(to_underlying(Crypto::KeyIdBase::DACPrivKey)) == PSA_SUCCESS) {
		LOG_INF("New factory data: replacing the stored DAC private key");
	}
}
#endif

} /* namespace */

CHIP_ERROR ChipIdFactoryDataProvider::Init()
{
#ifdef CONFIG_CHIP_CRYPTO_PSA_MIGRATE_DAC_PRIV_KEY
	ReplaceStoredDacKeyIfNew();
#endif
	ReturnErrorOnFailure(Base::Init());

	snprintf(mChipId, sizeof(mChipId), "%08X%08X", unsigned(nrf_ficr_deviceid_get(NRF_FICR, 1)),
		 unsigned(nrf_ficr_deviceid_get(NRF_FICR, 0)));
	snprintf(mSerial, sizeof(mSerial), "%s%s", kSerialPrefix, mChipId);

	uint8_t h[Crypto::kSHA256_Hash_Length];

	ReturnErrorOnFailure(Hash("discriminator", mChipId, h));
	mDiscriminator = uint16_t(BigEndian(h, 2) & 0xFFF);

	for (unsigned n = 0; mPasscode == 0 || IsForbiddenPasscode(mPasscode); n++) {
		char label[16];
		snprintf(label, sizeof(label), "passcode%u", n);
		ReturnErrorOnFailure(Hash(label, mChipId, h));
		mPasscode = 1 + BigEndian(h, 4) % kMaxPasscode;
	}

	ReturnErrorOnFailure(Hash("salt", mChipId, mSalt));
	ReturnErrorOnFailure(Hash("rd_uid", mChipId, h));
	memcpy(mRotatingId, h, sizeof(mRotatingId));

	/* The verifier depends on passcode, salt and the iteration count from the factory data. */
	uint32_t iterations;
	ReturnErrorOnFailure(Base::GetSpake2pIterationCount(iterations));

	Crypto::Spake2pVerifier verifier;
	const int64_t start = k_uptime_get();
	ReturnErrorOnFailure(verifier.Generate(iterations, ByteSpan(mSalt), mPasscode));
	MutableByteSpan out(mVerifier);
	ReturnErrorOnFailure(verifier.Serialize(out));
	mVerifierLength = out.size();

	LOG_INF("Identity from chip ID %s: serial %s, discriminator 0x%03X (verifier in %lld ms)", mChipId, mSerial,
		mDiscriminator, k_uptime_get() - start);
	return CHIP_NO_ERROR;
}

CHIP_ERROR ChipIdFactoryDataProvider::GetSetupDiscriminator(uint16_t &setupDiscriminator)
{
	setupDiscriminator = mDiscriminator;
	return CHIP_NO_ERROR;
}

CHIP_ERROR ChipIdFactoryDataProvider::GetSpake2pSalt(MutableByteSpan &saltBuf)
{
	return CopySpanToMutableSpan(ByteSpan(mSalt), saltBuf);
}

CHIP_ERROR ChipIdFactoryDataProvider::GetSpake2pVerifier(MutableByteSpan &verifierBuf, size_t &verifierLen)
{
	VerifyOrReturnError(verifierBuf.size() >= mVerifierLength, CHIP_ERROR_BUFFER_TOO_SMALL);
	memcpy(verifierBuf.data(), mVerifier, mVerifierLength);
	verifierLen = mVerifierLength;
	verifierBuf.reduce_size(mVerifierLength);
	return CHIP_NO_ERROR;
}

CHIP_ERROR ChipIdFactoryDataProvider::GetSetupPasscode(uint32_t &setupPasscode)
{
	setupPasscode = mPasscode;
	return CHIP_NO_ERROR;
}

CHIP_ERROR ChipIdFactoryDataProvider::GetSerialNumber(char *buf, size_t bufSize)
{
	VerifyOrReturnError(bufSize > strlen(mSerial), CHIP_ERROR_BUFFER_TOO_SMALL);
	strcpy(buf, mSerial);
	return CHIP_NO_ERROR;
}

CHIP_ERROR ChipIdFactoryDataProvider::GetRotatingDeviceIdUniqueId(MutableByteSpan &uniqueIdSpan)
{
	return CopySpanToMutableSpan(ByteSpan(mRotatingId), uniqueIdSpan);
}

} /* namespace Gateway */
