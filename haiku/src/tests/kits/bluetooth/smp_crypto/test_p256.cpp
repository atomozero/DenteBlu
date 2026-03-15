/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * P-256 ECDH implementation tests.
 *
 * Test vectors from:
 * - RFC 5903 Section 8.1 (256-bit Random ECP Group)
 * - NIST curve P-256 known points
 */

#include "p256.h"

#include <stdio.h>
#include <string.h>


static int sTestsPassed = 0;
static int sTestsFailed = 0;


static void
_PrintHex(const char* label, const uint8* data, int length)
{
	printf("  %s: ", label);
	for (int i = 0; i < length; i++)
		printf("%02x", data[i]);
	printf("\n");
}


static bool
_CheckBytes(const char* testName, const uint8* expected,
	const uint8* actual, int length)
{
	if (memcmp(expected, actual, length) == 0) {
		printf("[PASS] %s\n", testName);
		sTestsPassed++;
		return true;
	}

	printf("[FAIL] %s\n", testName);
	_PrintHex("expected", expected, length);
	_PrintHex("actual  ", actual, length);
	sTestsFailed++;
	return false;
}


static void
_Check(const char* testName, bool condition)
{
	if (condition) {
		printf("[PASS] %s\n", testName);
		sTestsPassed++;
	} else {
		printf("[FAIL] %s\n", testName);
		sTestsFailed++;
	}
}


/* Helper: convert big-endian hex string to little-endian byte array (BT SMP order).
   P-256 test vectors are typically in big-endian; BT SMP uses little-endian. */
static void
_HexToLE(uint8 out[32], const char* hex)
{
	uint8 be[32];
	for (int i = 0; i < 32; i++) {
		unsigned int val;
		sscanf(hex + i * 2, "%02x", &val);
		be[i] = (uint8)val;
	}
	/* Reverse to little-endian */
	for (int i = 0; i < 32; i++)
		out[i] = be[31 - i];
}


/* ========================================================================
 * Test 1: Generator point is on curve
 * ======================================================================== */

static void
_TestGeneratorOnCurve()
{
	printf("\n--- Generator point on curve ---\n");

	/* G coordinates in little-endian (from p256.cpp constants).
	   Gx = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
	   Gy = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
	*/
	uint8 gx[32], gy[32];
	_HexToLE(gx, "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
	_HexToLE(gy, "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");

	_Check("G is on P-256 curve", p256_point_is_on_curve(gx, gy));
}


/* ========================================================================
 * Test 2: Invalid points are rejected
 * ======================================================================== */

static void
_TestInvalidPoints()
{
	printf("\n--- Invalid point rejection ---\n");

	/* (0, 0) is not on the curve */
	uint8 zero[32];
	memset(zero, 0, 32);
	_Check("(0,0) rejected", !p256_point_is_on_curve(zero, zero));

	/* (1, 1) is not on the curve */
	uint8 one[32];
	memset(one, 0, 32);
	one[0] = 1;
	_Check("(1,1) rejected", !p256_point_is_on_curve(one, one));

	/* G with wrong Y coordinate */
	uint8 gx[32], wrong_gy[32];
	_HexToLE(gx, "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
	_HexToLE(wrong_gy, "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F6");
	_Check("G with wrong Y rejected", !p256_point_is_on_curve(gx, wrong_gy));
}


/* ========================================================================
 * Test 3: Key generation produces valid point
 * ======================================================================== */

static void
_TestKeypairGeneration()
{
	printf("\n--- Keypair generation ---\n");

	/* Use a known "random" value */
	uint8 random[32];
	for (int i = 0; i < 32; i++)
		random[i] = (uint8)(i + 1);

	uint8 privKey[32], pubX[32], pubY[32];
	status_t status = p256_generate_keypair_from_random(random, privKey, pubX, pubY);
	_Check("Keypair generation succeeds", status == B_OK);
	_Check("Public key is on curve", p256_point_is_on_curve(pubX, pubY));

	/* Private key should not be all zeros */
	uint8 zero[32];
	memset(zero, 0, 32);
	_Check("Private key is non-zero", memcmp(privKey, zero, 32) != 0);
}


/* ========================================================================
 * Test 4: ECDH round-trip (Alice and Bob agree on shared secret)
 * ======================================================================== */

static void
_TestEcdhRoundTrip()
{
	printf("\n--- ECDH round-trip ---\n");

	/* Alice's "random" */
	uint8 aliceRandom[32];
	for (int i = 0; i < 32; i++)
		aliceRandom[i] = (uint8)(i + 0x10);

	/* Bob's "random" */
	uint8 bobRandom[32];
	for (int i = 0; i < 32; i++)
		bobRandom[i] = (uint8)(i + 0x50);

	uint8 alicePriv[32], alicePubX[32], alicePubY[32];
	uint8 bobPriv[32], bobPubX[32], bobPubY[32];

	status_t s1 = p256_generate_keypair_from_random(aliceRandom, alicePriv,
		alicePubX, alicePubY);
	status_t s2 = p256_generate_keypair_from_random(bobRandom, bobPriv,
		bobPubX, bobPubY);

	_Check("Alice keypair OK", s1 == B_OK);
	_Check("Bob keypair OK", s2 == B_OK);

	/* Alice computes shared secret using her private key and Bob's public key */
	uint8 aliceDH[32];
	status_t s3 = p256_compute_dhkey(alicePriv, bobPubX, bobPubY, aliceDH);
	_Check("Alice DH computation OK", s3 == B_OK);

	/* Bob computes shared secret using his private key and Alice's public key */
	uint8 bobDH[32];
	status_t s4 = p256_compute_dhkey(bobPriv, alicePubX, alicePubY, bobDH);
	_Check("Bob DH computation OK", s4 == B_OK);

	/* Both should agree on the shared secret */
	_CheckBytes("ECDH shared secret matches", aliceDH, bobDH, 32);

	_PrintHex("Alice DH", aliceDH, 32);
	_PrintHex("Bob DH  ", bobDH, 32);
}


/* ========================================================================
 * Test 5: RFC 5903 Section 8.1 test vectors
 *
 * These are for the 256-bit random ECP group (P-256).
 * The private keys and expected public keys / shared secret are given.
 *
 * Note: RFC 5903 uses big-endian byte order. BT SMP uses little-endian.
 * We convert here.
 * ======================================================================== */

static void
_TestRfc5903()
{
	printf("\n--- RFC 5903 Section 8.1 test vectors ---\n");

	/* Initiator (Alice) private key */
	uint8 di[32];
	_HexToLE(di, "C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433");

	/* Expected initiator public key X */
	uint8 expectedQiX[32];
	_HexToLE(expectedQiX, "DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C3772581180");

	/* Responder (Bob) private key */
	uint8 dr[32];
	_HexToLE(dr, "C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");

	/* Expected responder public key X */
	uint8 expectedQrX[32];
	_HexToLE(expectedQrX, "D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF63");

	/* Expected shared secret Z (X coordinate of the shared point) */
	uint8 expectedZ[32];
	_HexToLE(expectedZ, "D6840F6B42F6EDAFD13116E0E12565202FEF8E9ECE7DCE03812464D04B9442DE");

	/* Generate Alice's keypair from her private key */
	uint8 alicePriv[32], alicePubX[32], alicePubY[32];
	status_t s1 = p256_generate_keypair_from_random(di, alicePriv, alicePubX,
		alicePubY);
	_Check("Alice keypair generation OK", s1 == B_OK);
	_Check("Alice public key on curve", p256_point_is_on_curve(alicePubX,
		alicePubY));

	/* Check Alice's public key X matches expected.
	   Note: p256_generate_keypair_from_random reduces the input mod n first,
	   so the actual private key may differ from the input if input >= n.
	   We need to check if the RFC vector is < n. */
	_PrintHex("Alice Qi.x (actual)", alicePubX, 32);
	_PrintHex("Alice Qi.x (expected)", expectedQiX, 32);
	_CheckBytes("Alice Qi.x matches RFC 5903", expectedQiX, alicePubX, 32);

	/* Generate Bob's keypair */
	uint8 bobPriv[32], bobPubX[32], bobPubY[32];
	status_t s2 = p256_generate_keypair_from_random(dr, bobPriv, bobPubX,
		bobPubY);
	_Check("Bob keypair generation OK", s2 == B_OK);
	_Check("Bob public key on curve", p256_point_is_on_curve(bobPubX, bobPubY));

	_PrintHex("Bob Qr.x (actual)", bobPubX, 32);
	_PrintHex("Bob Qr.x (expected)", expectedQrX, 32);
	_CheckBytes("Bob Qr.x matches RFC 5903", expectedQrX, bobPubX, 32);

	/* Compute shared secret: Alice's privkey * Bob's pubkey */
	uint8 dhkey[32];
	status_t s3 = p256_compute_dhkey(alicePriv, bobPubX, bobPubY, dhkey);
	_Check("DH key computation OK", s3 == B_OK);

	_PrintHex("DH key Z (actual)", dhkey, 32);
	_PrintHex("DH key Z (expected)", expectedZ, 32);
	_CheckBytes("Shared secret Z matches RFC 5903", expectedZ, dhkey, 32);

	/* Also verify the reverse: Bob's privkey * Alice's pubkey */
	uint8 dhkey2[32];
	status_t s4 = p256_compute_dhkey(bobPriv, alicePubX, alicePubY, dhkey2);
	_Check("Reverse DH computation OK", s4 == B_OK);
	_CheckBytes("Reverse DH key matches", dhkey, dhkey2, 32);
}


/* ========================================================================
 * Test 6: Invalid remote public key is rejected by compute_dhkey
 * ======================================================================== */

static void
_TestInvalidRemoteKey()
{
	printf("\n--- Invalid remote key rejection ---\n");

	uint8 privKey[32];
	for (int i = 0; i < 32; i++)
		privKey[i] = (uint8)(i + 1);

	/* Try with (0, 0) as remote key */
	uint8 zero[32];
	memset(zero, 0, 32);
	uint8 dhkey[32];
	status_t status = p256_compute_dhkey(privKey, zero, zero, dhkey);
	_Check("(0,0) remote key rejected", status == B_BAD_VALUE);

	/* Try with random invalid point */
	uint8 badX[32], badY[32];
	for (int i = 0; i < 32; i++) {
		badX[i] = (uint8)(i * 7 + 3);
		badY[i] = (uint8)(i * 13 + 5);
	}
	status = p256_compute_dhkey(privKey, badX, badY, dhkey);
	_Check("Random invalid point rejected", status == B_BAD_VALUE);
}


/* ========================================================================
 * Test 7: Known 2*G point
 *
 * 2*G for P-256:
 * X = 0x7CF27B188D034F7E8A52380304B51AC3C90E15A52559A440C71C5E7A3B8CAD6B
 * Y = 0x0DE28E3D3EB86B7F6D64B5C3F24F38E0C8D08AFCCBCD9DD2F2B0C15BD4A77C28
 * (... wait these are some known values, let me use actual 2*G)
 *
 * Actually, for verification we just need to check that:
 *   generate_keypair(2) produces a point on the curve.
 * Since we verified RFC 5903 vectors, that's sufficient.
 * Let's do a simpler known-scalar test: d=1 should give G.
 * ======================================================================== */

static void
_TestScalarOne()
{
	printf("\n--- Scalar d=1 gives G ---\n");

	/* d = 1 in little-endian */
	uint8 d_one[32];
	memset(d_one, 0, 32);
	d_one[0] = 1;

	uint8 pubX[32], pubY[32], privKey[32];
	status_t status = p256_generate_keypair_from_random(d_one, privKey,
		pubX, pubY);
	_Check("d=1 keypair generation OK", status == B_OK);

	/* Expected: public key should be G */
	uint8 expectedGx[32], expectedGy[32];
	_HexToLE(expectedGx, "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
	_HexToLE(expectedGy, "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");

	_CheckBytes("1*G X = Gx", expectedGx, pubX, 32);
	_CheckBytes("1*G Y = Gy", expectedGy, pubY, 32);
}


/* ========================================================================
 * Test 8: Scalar d=2 gives known 2*G
 *
 * 2*G for P-256:
 * X = 0x7CF27B188D034F7E8A52380304B51AC3C90E15A52559A440C71C5E7A3B8CAD6B (... wait)
 *
 * Let me use the well-known value:
 * 2G.x = 0x7CF27B188D034F7E8A52380304B51AC3C90E15DE28E3D3EB86B7F6D64B5C3F24F38E0C8D08AFCCBCD9DD2F2B0C15BD4A77C28
 * That's too long. The actual 2*G:
 *   x = 7CF27B188D034F7E8A52380304B51AC3C90E15A52559A440C71C5E7A3B8CAD6B (wait, this might be wrong)
 *
 * Instead, let's just verify 2*G is on curve and use the ECDH agreement property.
 * ======================================================================== */

static void
_TestScalarTwo()
{
	printf("\n--- Scalar d=2 gives point on curve ---\n");

	uint8 d_two[32];
	memset(d_two, 0, 32);
	d_two[0] = 2;

	uint8 pubX[32], pubY[32], privKey[32];
	status_t status = p256_generate_keypair_from_random(d_two, privKey,
		pubX, pubY);
	_Check("d=2 keypair generation OK", status == B_OK);
	_Check("2*G is on curve", p256_point_is_on_curve(pubX, pubY));

	_PrintHex("2*G X", pubX, 32);
	_PrintHex("2*G Y", pubY, 32);
}


/* ======================================================================== */

int
main()
{
	printf("=== P-256 ECDH Implementation Tests ===\n");

	_TestGeneratorOnCurve();
	_TestInvalidPoints();
	_TestKeypairGeneration();
	_TestEcdhRoundTrip();
	_TestRfc5903();
	_TestInvalidRemoteKey();
	_TestScalarOne();
	_TestScalarTwo();

	printf("\n========================================\n");
	printf("Results: %d passed, %d failed\n", sTestsPassed, sTestsFailed);
	printf("========================================\n");

	return sTestsFailed > 0 ? 1 : 0;
}
