/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SMP cryptographic functions for LE Secure Connections.
 * Implements AES-128, AES-CMAC, and the BT SMP f4/f5/f6/g2 functions.
 * ECDH P-256 uses a standalone constant-time implementation (p256.cpp).
 */

#include "SmpManager.h"
#include "p256.h"

#include <string.h>

#include <btDebug.h>

#ifdef _KERNEL_MODE
#include <util/Random.h>
#endif


/*
 * Minimal AES-128 implementation (single block).
 *
 * NOTE: This is a reference implementation for the SMP protocol.
 * A production build should use hardware AES or a well-tested library.
 */

/* AES S-Box */
static const uint8 sAesSBox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8 sAesRcon[11] = {
	0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};


static uint8
_GfMul2(uint8 x)
{
	return (x << 1) ^ ((x >> 7) * 0x1b);
}


static void
_AesSubBytes(uint8 state[16])
{
	for (int i = 0; i < 16; i++)
		state[i] = sAesSBox[state[i]];
}


static void
_AesShiftRows(uint8 state[16])
{
	uint8 tmp;

	tmp = state[1]; state[1] = state[5]; state[5] = state[9];
	state[9] = state[13]; state[13] = tmp;

	tmp = state[2]; state[2] = state[10]; state[10] = tmp;
	tmp = state[6]; state[6] = state[14]; state[14] = tmp;

	tmp = state[15]; state[15] = state[11]; state[11] = state[7];
	state[7] = state[3]; state[3] = tmp;
}


static void
_AesMixColumns(uint8 state[16])
{
	for (int i = 0; i < 4; i++) {
		uint8 a = state[i * 4 + 0];
		uint8 b = state[i * 4 + 1];
		uint8 c = state[i * 4 + 2];
		uint8 d = state[i * 4 + 3];

		uint8 e = a ^ b ^ c ^ d;

		state[i * 4 + 0] ^= e ^ _GfMul2(a ^ b);
		state[i * 4 + 1] ^= e ^ _GfMul2(b ^ c);
		state[i * 4 + 2] ^= e ^ _GfMul2(c ^ d);
		state[i * 4 + 3] ^= e ^ _GfMul2(d ^ a);
	}
}


static void
_AesAddRoundKey(uint8 state[16], const uint8 roundKey[16])
{
	for (int i = 0; i < 16; i++)
		state[i] ^= roundKey[i];
}


static void
_AesKeyExpansion(const uint8 key[16], uint8 expandedKey[176])
{
	memcpy(expandedKey, key, 16);

	for (int i = 4; i < 44; i++) {
		uint8 temp[4];
		memcpy(temp, expandedKey + (i - 1) * 4, 4);

		if (i % 4 == 0) {
			/* Rotate */
			uint8 t = temp[0];
			temp[0] = sAesSBox[temp[1]] ^ sAesRcon[i / 4];
			temp[1] = sAesSBox[temp[2]];
			temp[2] = sAesSBox[temp[3]];
			temp[3] = sAesSBox[t];
		}

		for (int j = 0; j < 4; j++)
			expandedKey[i * 4 + j] = expandedKey[(i - 4) * 4 + j] ^ temp[j];
	}
}


status_t
smp_aes128(const uint8 key[16], const uint8 plaintext[16],
	uint8 encrypted[16])
{
	uint8 expandedKey[176];
	_AesKeyExpansion(key, expandedKey);

	memcpy(encrypted, plaintext, 16);

	_AesAddRoundKey(encrypted, expandedKey);

	for (int round = 1; round < 10; round++) {
		_AesSubBytes(encrypted);
		_AesShiftRows(encrypted);
		_AesMixColumns(encrypted);
		_AesAddRoundKey(encrypted, expandedKey + round * 16);
	}

	_AesSubBytes(encrypted);
	_AesShiftRows(encrypted);
	_AesAddRoundKey(encrypted, expandedKey + 160);

	return B_OK;
}


/*
 * AES-CMAC as defined in RFC 4493.
 */
status_t
smp_aes_cmac(const uint8 key[16], const uint8* message,
	uint32 messageLength, uint8 mac[16])
{
	uint8 L[16], K1[16], K2[16];
	uint8 zero[16];
	memset(zero, 0, 16);

	/* Step 1: Generate subkeys */
	smp_aes128(key, zero, L);

	/* Derive K1 */
	uint8 carry = (L[0] >> 7) & 1;
	for (int i = 0; i < 15; i++)
		K1[i] = (L[i] << 1) | (L[i + 1] >> 7);
	K1[15] = L[15] << 1;
	if (carry)
		K1[15] ^= 0x87;

	/* Derive K2 */
	carry = (K1[0] >> 7) & 1;
	for (int i = 0; i < 15; i++)
		K2[i] = (K1[i] << 1) | (K1[i + 1] >> 7);
	K2[15] = K1[15] << 1;
	if (carry)
		K2[15] ^= 0x87;

	/* Step 2-4: Process message blocks */
	uint32 n = (messageLength + 15) / 16;
	bool complete;

	if (n == 0) {
		n = 1;
		complete = false;
	} else {
		complete = (messageLength % 16 == 0);
	}

	uint8 X[16];
	memset(X, 0, 16);

	for (uint32 i = 0; i < n - 1; i++) {
		uint8 Y[16];
		for (int j = 0; j < 16; j++)
			Y[j] = X[j] ^ message[i * 16 + j];
		smp_aes128(key, Y, X);
	}

	/* Last block */
	uint8 lastBlock[16];
	if (complete) {
		for (int j = 0; j < 16; j++)
			lastBlock[j] = message[(n - 1) * 16 + j] ^ K1[j];
	} else {
		uint32 remaining = messageLength - (n - 1) * 16;
		memset(lastBlock, 0, 16);
		memcpy(lastBlock, message + (n - 1) * 16, remaining);
		lastBlock[remaining] = 0x80; /* Padding */
		for (int j = 0; j < 16; j++)
			lastBlock[j] ^= K2[j];
	}

	uint8 Y[16];
	for (int j = 0; j < 16; j++)
		Y[j] = X[j] ^ lastBlock[j];
	smp_aes128(key, Y, mac);

	return B_OK;
}


/*
 * BT SMP f4: confirm value generation.
 * f4(U, V, X, Z) = AES-CMAC_X(U || V || Z)
 */
status_t
smp_f4(const uint8 u[32], const uint8 v[32], const uint8 x[16],
	uint8 z, uint8 result[16])
{
	uint8 message[65]; /* 32 + 32 + 1 */
	memcpy(message, u, 32);
	memcpy(message + 32, v, 32);
	message[64] = z;

	return smp_aes_cmac(x, message, 65, result);
}


/*
 * BT SMP f5: key derivation function.
 * f5(DHKey, N1, N2, A1, A2) produces MacKey and LTK.
 */
status_t
smp_f5(const uint8 dhkey[32], const uint8 n1[16], const uint8 n2[16],
	const uint8 a1[7], const uint8 a2[7], uint8 mackey[16], uint8 ltk[16])
{
	/* Salt for f5 */
	static const uint8 salt[16] = {
		0x6C, 0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38,
		0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE
	};

	/* T = AES-CMAC_salt(DHKey) */
	uint8 T[16];
	smp_aes_cmac(salt, dhkey, 32, T);

	/* keyID = "btle" */
	static const uint8 keyID[4] = { 0x62, 0x74, 0x6c, 0x65 };

	/* Length = 256 (0x0100) in big-endian */
	uint8 length[2] = { 0x01, 0x00 };

	/* m = Counter || keyID || N1 || N2 || A1 || A2 || Length */
	uint8 m[53]; /* 1 + 4 + 16 + 16 + 7 + 7 + 2 */

	/* Counter = 0: MacKey */
	m[0] = 0x00;
	memcpy(m + 1, keyID, 4);
	memcpy(m + 5, n1, 16);
	memcpy(m + 21, n2, 16);
	memcpy(m + 37, a1, 7);
	memcpy(m + 44, a2, 7);
	memcpy(m + 51, length, 2);

	smp_aes_cmac(T, m, 53, mackey);

	/* Counter = 1: LTK */
	m[0] = 0x01;
	smp_aes_cmac(T, m, 53, ltk);

	return B_OK;
}


/*
 * BT SMP f6: check value generation.
 * f6(W, N1, N2, R, IOcap, A1, A2) = AES-CMAC_W(N1 || N2 || R || IOcap || A1 || A2)
 */
status_t
smp_f6(const uint8 mackey[16], const uint8 n1[16], const uint8 n2[16],
	const uint8 r[16], const uint8 iocap[3], const uint8 a1[7],
	const uint8 a2[7], uint8 result[16])
{
	uint8 m[65]; /* 16 + 16 + 16 + 3 + 7 + 7 */
	memcpy(m, n1, 16);
	memcpy(m + 16, n2, 16);
	memcpy(m + 32, r, 16);
	memcpy(m + 48, iocap, 3);
	memcpy(m + 51, a1, 7);
	memcpy(m + 58, a2, 7);

	return smp_aes_cmac(mackey, m, 65, result);
}


/*
 * BT SMP g2: numeric comparison value generation.
 * g2(U, V, X, Y) = AES-CMAC_X(U || V || Y) mod 10^6
 */
status_t
smp_g2(const uint8 u[32], const uint8 v[32], const uint8 x[16],
	const uint8 y[16], uint32* _passkey)
{
	uint8 m[80]; /* 32 + 32 + 16 */
	memcpy(m, u, 32);
	memcpy(m + 32, v, 32);
	memcpy(m + 64, y, 16);

	uint8 result[16];
	status_t status = smp_aes_cmac(x, m, 80, result);
	if (status != B_OK)
		return status;

	/* Extract 32-bit value from last 4 bytes (big-endian) and mod 10^6 */
	uint32 val = ((uint32)result[12] << 24) | ((uint32)result[13] << 16)
		| ((uint32)result[14] << 8) | result[15];
	*_passkey = val % 1000000;

	return B_OK;
}


/*
 * ECDH P-256 implementation using standalone constant-time p256.cpp.
 * RNG is handled here so p256.cpp remains pure computation (testable
 * in userspace without kernel dependencies).
 */

status_t
smp_ecdh_generate_keypair(uint8 privateKey[32], uint8 publicKeyX[32],
	uint8 publicKeyY[32])
{
	uint8 random[32];

#ifdef _KERNEL_MODE
	for (int i = 0; i < 8; i++) {
		uint32 val = secure_random_value();
		memcpy(random + i * 4, &val, 4);
	}
#else
	/* Userspace test fallback: use system_time() as entropy.
	   NOT cryptographically secure — tests only. */
	bigtime_t time = system_time();
	for (int i = 0; i < 32; i++) {
		random[i] = (uint8)((time >> ((i % 8) * 8)) ^ (i * 37));
		time = time * 6364136223846793005ULL + 1442695040888963407ULL;
	}
#endif

	status_t status = p256_generate_keypair_from_random(random, privateKey,
		publicKeyX, publicKeyY);

	/* Wipe random data from stack */
	memset(random, 0, 32);

	TRACE("%s: ECDH keypair generated (real P-256)\n", __func__);
	return status;
}


status_t
smp_ecdh_compute_dhkey(const uint8 privateKey[32], const uint8 remoteX[32],
	const uint8 remoteY[32], uint8 dhkey[32])
{
	status_t status = p256_compute_dhkey(privateKey, remoteX, remoteY, dhkey);

	TRACE("%s: DH key computed (real P-256), status=%d\n", __func__,
		(int)status);
	return status;
}
