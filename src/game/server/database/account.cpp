#include <game/server/database/account.h>

#include <base/system.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
const int SALT_LEN = 16;
const int HASH_LEN = 32; // SHA-256
const int ITERATIONS = 100000;

void ToHex(const unsigned char *pData, int Len, char *pOut)
{
	static const char aHex[] = "0123456789abcdef";
	for(int i = 0; i < Len; i++)
	{
		pOut[i * 2] = aHex[pData[i] >> 4];
		pOut[i * 2 + 1] = aHex[pData[i] & 0x0F];
	}
	pOut[Len * 2] = '\0';
}

int FromHexNibble(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool FromHex(const char *pHex, int Len, unsigned char *pOut)
{
	for(int i = 0; i < Len; i++)
	{
		const int Hi = FromHexNibble(pHex[i * 2]);
		const int Lo = FromHexNibble(pHex[i * 2 + 1]);
		if(Hi < 0 || Lo < 0)
			return false;
		pOut[i] = (unsigned char) ((Hi << 4) | Lo);
	}
	return true;
}
} // anonymous namespace

void HashPassword(char *pOut, int Size, const char *pPassword)
{
	unsigned char aSalt[SALT_LEN];
	unsigned char aHash[HASH_LEN];
	RAND_bytes(aSalt, SALT_LEN);
	PKCS5_PBKDF2_HMAC(pPassword, str_length(pPassword), aSalt, SALT_LEN, ITERATIONS, EVP_sha256(), HASH_LEN, aHash);

	// "salt_hex$hash_hex"
	char aSaltHex[SALT_LEN * 2 + 1];
	char aHashHex[HASH_LEN * 2 + 1];
	ToHex(aSalt, SALT_LEN, aSaltHex);
	ToHex(aHash, HASH_LEN, aHashHex);
	str_format(pOut, Size, "%s$%s", aSaltHex, aHashHex);
}

bool VerifyPassword(const char *pStored, const char *pPassword)
{
	if(!pStored || !pStored[0])
		return false;

	const char *pDollar = str_find(pStored, "$");
	if(!pDollar)
		return false;

	const int SaltHexLen = (int) (pDollar - pStored);
	const int HashHexLen = str_length(pDollar + 1);
	if(SaltHexLen != SALT_LEN * 2 || HashHexLen != HASH_LEN * 2)
		return false;

	unsigned char aSalt[SALT_LEN];
	unsigned char aStoredHash[HASH_LEN];
	if(!FromHex(pStored, SALT_LEN, aSalt) || !FromHex(pDollar + 1, HASH_LEN, aStoredHash))
		return false;

	unsigned char aDerived[HASH_LEN];
	PKCS5_PBKDF2_HMAC(pPassword, str_length(pPassword), aSalt, SALT_LEN, ITERATIONS, EVP_sha256(), HASH_LEN, aDerived);

	return CRYPTO_memcmp(aStoredHash, aDerived, HASH_LEN) == 0;
}
