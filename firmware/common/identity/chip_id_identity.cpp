/*
 * Matter identity derived from the nRF52840's chip ID. See chip_id_identity.h.
 */

#include "chip_id_identity.h"

#include <lib/support/CodeUtils.h>

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
} /* namespace */

CHIP_ERROR ChipIdFactoryDataProvider::Init()
{
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
