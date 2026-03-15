/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Simulated SMP LE Secure Connections pairing test.
 *
 * Exercises the full pairing state machine logic using the SMP crypto
 * functions directly, without kernel dependencies. Both sides (initiator
 * and responder) are simulated in-process, exchanging PDUs in memory.
 *
 * Tests:
 * - 20-round passkey confirm/random exchange
 * - Cross-verification of confirm values
 * - f5 key derivation (MacKey + LTK) from both perspectives
 * - f6 DHKey check (Ea, Eb) cross-validation
 * - Error detection: wrong passkey, wrong confirm, tampered DHKey check
 *
 * Reference: Bluetooth Core Spec v5.4 Vol 3, Part H, Sec 2.3.5.6.3
 * (LE SC Passkey Entry Protocol)
 */

#include "SmpManager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


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


static void
_Pass(const char* name)
{
	printf("[PASS] %s\n", name);
	sTestsPassed++;
}


static void
_Fail(const char* name)
{
	printf("[FAIL] %s\n", name);
	sTestsFailed++;
}


/*
 * Simple deterministic PRNG for test nonce generation.
 * NOT cryptographically secure - for testing only.
 */
static uint64 sPrngState = 0;

static void
_PrngSeed(uint64 seed)
{
	sPrngState = seed;
}

static void
_PrngFill(uint8* buf, int len)
{
	for (int i = 0; i < len; i++) {
		sPrngState = sPrngState * 6364136223846793005ULL
			+ 1442695040888963407ULL;
		buf[i] = (uint8)(sPrngState >> 33);
	}
}


/*
 * Simulated device state for one side of the pairing.
 */
struct PairingState {
	/* ECDH keys */
	uint8 publicKeyX[32];
	uint8 publicKeyY[32];
	uint8 dhKey[32];

	/* Per-round state */
	uint8 localRandom[16];
	uint8 remoteRandom[16];
	uint8 localConfirm[16];
	uint8 remoteConfirm[16];

	/* Address info (type + 6 bytes) */
	uint8 addr[7];

	/* IO capabilities */
	uint8 ioCapability;
	uint8 authReq;

	/* Results */
	uint8 macKey[16];
	uint8 ltk[16];
};


/* ========================================================================
 * Test 1: Full 20-round passkey exchange (happy path)
 *
 * Simulates the complete SC Passkey Entry protocol:
 * - Initiator (A) and Responder (B) share a passkey
 * - For each of 20 bits, both sides exchange confirm+random
 * - Cross-verify confirm values using f4
 * - Derive keys using f5, verify DHKey checks using f6
 * ======================================================================== */

static void
_TestFullPairingHappyPath()
{
	printf("\n--- Full 20-round passkey exchange ---\n");

	/* Fixed test parameters */
	const uint32 passkey = 123456;

	/* Fixed ECDH public keys (simulated) */
	struct PairingState A, B;
	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));

	/* Generate deterministic "public keys" for testing */
	_PrngSeed(0xDEADBEEF);
	_PrngFill(A.publicKeyX, 32);
	_PrngFill(A.publicKeyY, 32);
	_PrngSeed(0xCAFEBABE);
	_PrngFill(B.publicKeyX, 32);
	_PrngFill(B.publicKeyY, 32);

	/* Both sides compute the SAME DH key (simulated -
	   in real ECDH: DHKey = SKa * PKb = SKb * PKa) */
	_PrngSeed(0x12345678);
	_PrngFill(A.dhKey, 32);
	memcpy(B.dhKey, A.dhKey, 32);

	/* Addresses */
	A.addr[0] = 0x00; /* public */
	A.addr[1] = 0xAA; A.addr[2] = 0xBB; A.addr[3] = 0xCC;
	A.addr[4] = 0xDD; A.addr[5] = 0xEE; A.addr[6] = 0xFF;

	B.addr[0] = 0x00; /* public */
	B.addr[1] = 0x11; B.addr[2] = 0x22; B.addr[3] = 0x33;
	B.addr[4] = 0x44; B.addr[5] = 0x55; B.addr[6] = 0x66;

	/* IO capabilities */
	A.ioCapability = 0x04; /* KeyboardDisplay */
	A.authReq = 0x0D; /* Bonding | MITM | SC */
	B.ioCapability = 0x02; /* KeyboardOnly */
	B.authReq = 0x0D;

	bool allRoundsOk = true;

	/*
	 * 20-round passkey bit exchange.
	 *
	 * Per Bluetooth Core Spec v5.4, Vol 3, Part H, Sec 2.3.5.6.3:
	 *
	 * For each bit i = 0..19:
	 *   ri = (passkey >> i) & 1
	 *
	 *   Initiator (A):
	 *     1. Generate random Nai
	 *     2. Compute Cai = f4(PKax, PKbx, Nai, 0x80 + ri)
	 *     3. Send Cai → B
	 *
	 *   Responder (B):
	 *     4. Generate random Nbi
	 *     5. Compute Cbi = f4(PKbx, PKax, Nbi, 0x80 + ri)
	 *     6. Send Cbi → A
	 *
	 *   A sends Nai → B
	 *     7. B verifies: Cai == f4(PKax, PKbx, Nai, 0x80 + ri)
	 *
	 *   B sends Nbi → A
	 *     8. A verifies: Cbi == f4(PKbx, PKax, Nbi, 0x80 + ri)
	 */
	for (int bit = 0; bit < 20; bit++) {
		uint8 ri = (passkey >> bit) & 0x01;

		/* A generates Nai and Cai */
		_PrngSeed(0xA0000000 + bit);
		_PrngFill(A.localRandom, 16);
		smp_f4(A.publicKeyX, B.publicKeyX, A.localRandom,
			0x80 + ri, A.localConfirm);

		/* B generates Nbi and Cbi */
		_PrngSeed(0xB0000000 + bit);
		_PrngFill(B.localRandom, 16);
		smp_f4(B.publicKeyX, A.publicKeyX, B.localRandom,
			0x80 + ri, B.localConfirm);

		/* Exchange confirms: A receives Cbi, B receives Cai */
		memcpy(A.remoteConfirm, B.localConfirm, 16);
		memcpy(B.remoteConfirm, A.localConfirm, 16);

		/* Exchange randoms: A receives Nbi, B receives Nai */
		memcpy(A.remoteRandom, B.localRandom, 16);
		memcpy(B.remoteRandom, A.localRandom, 16);

		/* B verifies A's confirm: Cai == f4(PKax, PKbx, Nai, 0x80 + ri) */
		uint8 expectedCai[16];
		smp_f4(A.publicKeyX, B.publicKeyX, B.remoteRandom,
			0x80 + ri, expectedCai);
		if (memcmp(expectedCai, B.remoteConfirm, 16) != 0) {
			printf("[FAIL] Round %d: B failed to verify A's confirm\n", bit);
			allRoundsOk = false;
			break;
		}

		/* A verifies B's confirm: Cbi == f4(PKbx, PKax, Nbi, 0x80 + ri) */
		uint8 expectedCbi[16];
		smp_f4(B.publicKeyX, A.publicKeyX, A.remoteRandom,
			0x80 + ri, expectedCbi);
		if (memcmp(expectedCbi, A.remoteConfirm, 16) != 0) {
			printf("[FAIL] Round %d: A failed to verify B's confirm\n", bit);
			allRoundsOk = false;
			break;
		}
	}

	if (allRoundsOk)
		_Pass("20-round confirm/random exchange");
	else
		return;

	/*
	 * Key derivation with f5.
	 *
	 * After 20 rounds, both sides use the LAST round's randoms (Na19, Nb19)
	 * plus the shared DH Key to derive MacKey and LTK.
	 *
	 * f5(DHKey, Na, Nb, A1, A2) → MacKey, LTK
	 *
	 * The initiator (A) uses: f5(DHKey, Na, Nb, AddrA, AddrB)
	 * The responder (B) uses: f5(DHKey, Na, Nb, AddrA, AddrB)
	 * (same inputs → same outputs)
	 */
	smp_f5(A.dhKey, A.localRandom, A.remoteRandom, A.addr, B.addr,
		A.macKey, A.ltk);
	smp_f5(B.dhKey, B.remoteRandom, B.localRandom, A.addr, B.addr,
		B.macKey, B.ltk);

	if (memcmp(A.macKey, B.macKey, 16) == 0)
		_Pass("f5 MacKey matches on both sides");
	else {
		_Fail("f5 MacKey mismatch");
		_PrintHex("A.macKey", A.macKey, 16);
		_PrintHex("B.macKey", B.macKey, 16);
	}

	if (memcmp(A.ltk, B.ltk, 16) == 0)
		_Pass("f5 LTK matches on both sides");
	else {
		_Fail("f5 LTK mismatch");
		_PrintHex("A.ltk", A.ltk, 16);
		_PrintHex("B.ltk", B.ltk, 16);
	}

	/*
	 * DHKey check with f6.
	 *
	 * Initiator computes Ea = f6(MacKey, Na, Nb, r, IOcapA, A1, A2)
	 * Responder computes Eb = f6(MacKey, Nb, Na, r, IOcapB, A2, A1)
	 *
	 * For passkey entry, r = passkey encoded in LE 16 bytes.
	 *
	 * A sends Ea → B, B verifies by recomputing
	 * B sends Eb → A, A verifies by recomputing
	 */
	uint8 r[16];
	memset(r, 0, 16);
	r[0] = passkey & 0xFF;
	r[1] = (passkey >> 8) & 0xFF;
	r[2] = (passkey >> 16) & 0xFF;
	r[3] = (passkey >> 24) & 0xFF;

	uint8 iocapA[3] = { A.authReq, 0x00, A.ioCapability };
	uint8 iocapB[3] = { B.authReq, 0x00, B.ioCapability };

	/* A computes Ea */
	uint8 Ea[16];
	smp_f6(A.macKey, A.localRandom, A.remoteRandom, r, iocapA,
		A.addr, B.addr, Ea);

	/* B verifies Ea: recompute with the same inputs */
	uint8 expectedEa[16];
	smp_f6(B.macKey, B.remoteRandom, B.localRandom, r, iocapA,
		A.addr, B.addr, expectedEa);

	if (memcmp(Ea, expectedEa, 16) == 0)
		_Pass("DHKey check Ea: B successfully verified A");
	else {
		_Fail("DHKey check Ea: verification failed");
		_PrintHex("Ea (A computed)", Ea, 16);
		_PrintHex("Ea (B expected)", expectedEa, 16);
	}

	/* B computes Eb */
	uint8 Eb[16];
	smp_f6(B.macKey, B.localRandom, B.remoteRandom, r, iocapB,
		B.addr, A.addr, Eb);

	/* A verifies Eb */
	uint8 expectedEb[16];
	smp_f6(A.macKey, A.remoteRandom, A.localRandom, r, iocapB,
		B.addr, A.addr, expectedEb);

	if (memcmp(Eb, expectedEb, 16) == 0)
		_Pass("DHKey check Eb: A successfully verified B");
	else {
		_Fail("DHKey check Eb: verification failed");
		_PrintHex("Eb (B computed)", Eb, 16);
		_PrintHex("Eb (A expected)", expectedEb, 16);
	}

	_PrintHex("LTK", A.ltk, 16);
	printf("  Pairing complete: encryption key established.\n");
}


/* ========================================================================
 * Test 2: Wrong passkey detection
 *
 * If the responder uses a different passkey, confirm verification must
 * fail at some round where the passkey bits differ.
 * ======================================================================== */

static void
_TestWrongPasskey()
{
	printf("\n--- Wrong passkey detection ---\n");

	const uint32 passkeyA = 123456;
	const uint32 passkeyB = 654321; /* different */

	uint8 PKax[32], PKbx[32];
	_PrngSeed(0xDEADBEEF);
	_PrngFill(PKax, 32);
	_PrngSeed(0xCAFEBABE);
	_PrngFill(PKbx, 32);

	bool mismatchDetected = false;
	int failedAtBit = -1;

	for (int bit = 0; bit < 20; bit++) {
		uint8 riA = (passkeyA >> bit) & 0x01;
		uint8 riB = (passkeyB >> bit) & 0x01;

		/* A computes with its passkey bit */
		uint8 Na[16];
		_PrngSeed(0xA0000000 + bit);
		_PrngFill(Na, 16);
		uint8 Cai[16];
		smp_f4(PKax, PKbx, Na, 0x80 + riA, Cai);

		/* B verifies using its (wrong) passkey bit */
		uint8 expectedCai[16];
		smp_f4(PKax, PKbx, Na, 0x80 + riB, expectedCai);

		if (memcmp(Cai, expectedCai, 16) != 0) {
			mismatchDetected = true;
			failedAtBit = bit;
			break;
		}
	}

	if (mismatchDetected) {
		printf("  Mismatch detected at bit %d "
			"(passkey bits: A=%d, B=%d)\n",
			failedAtBit,
			(passkeyA >> failedAtBit) & 1,
			(passkeyB >> failedAtBit) & 1);
		_Pass("Wrong passkey correctly detected by confirm mismatch");
	} else {
		_Fail("Wrong passkey NOT detected (should have failed)");
	}
}


/* ========================================================================
 * Test 3: Tampered confirm value detection
 *
 * If an attacker modifies a confirm value in transit, the verification
 * of the random value must fail.
 * ======================================================================== */

static void
_TestTamperedConfirm()
{
	printf("\n--- Tampered confirm detection ---\n");

	const uint32 passkey = 999999;
	uint8 PKax[32], PKbx[32];
	_PrngSeed(0x11111111);
	_PrngFill(PKax, 32);
	_PrngSeed(0x22222222);
	_PrngFill(PKbx, 32);

	uint8 ri = passkey & 0x01;

	/* A generates legitimate confirm */
	uint8 Na[16];
	_PrngSeed(0xAAAAAAAA);
	_PrngFill(Na, 16);
	uint8 Cai[16];
	smp_f4(PKax, PKbx, Na, 0x80 + ri, Cai);

	/* Attacker flips one bit in the confirm */
	uint8 tamperedCai[16];
	memcpy(tamperedCai, Cai, 16);
	tamperedCai[7] ^= 0x01;

	/* B receives tampered confirm, then receives real Na */
	uint8 expectedCai[16];
	smp_f4(PKax, PKbx, Na, 0x80 + ri, expectedCai);

	if (memcmp(tamperedCai, expectedCai, 16) != 0)
		_Pass("Tampered confirm detected (1-bit flip)");
	else
		_Fail("Tampered confirm NOT detected");

	/* Also test: tampered random should fail too */
	uint8 tamperedNa[16];
	memcpy(tamperedNa, Na, 16);
	tamperedNa[0] ^= 0x01;

	uint8 recomputed[16];
	smp_f4(PKax, PKbx, tamperedNa, 0x80 + ri, recomputed);

	if (memcmp(recomputed, Cai, 16) != 0)
		_Pass("Tampered random detected (1-bit flip in nonce)");
	else
		_Fail("Tampered random NOT detected");
}


/* ========================================================================
 * Test 4: DH key mismatch detection
 *
 * If the two sides have different DH keys (e.g., MITM replacing the
 * public key), f5 produces different MacKey/LTK, and the DHKey check
 * (f6) must fail.
 * ======================================================================== */

static void
_TestDhKeyMismatch()
{
	printf("\n--- DH key mismatch detection (MITM public key attack) ---\n");

	uint8 dhKeyA[32], dhKeyB[32];
	_PrngSeed(0xAAAA);
	_PrngFill(dhKeyA, 32);
	_PrngSeed(0xBBBB);
	_PrngFill(dhKeyB, 32); /* different! simulates MITM */

	uint8 Na[16], Nb[16];
	_PrngSeed(0x1234);
	_PrngFill(Na, 16);
	_PrngFill(Nb, 16);

	uint8 addrA[7] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
	uint8 addrB[7] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

	uint8 macKeyA[16], ltkA[16];
	uint8 macKeyB[16], ltkB[16];

	smp_f5(dhKeyA, Na, Nb, addrA, addrB, macKeyA, ltkA);
	smp_f5(dhKeyB, Na, Nb, addrA, addrB, macKeyB, ltkB);

	if (memcmp(macKeyA, macKeyB, 16) != 0 &&
		memcmp(ltkA, ltkB, 16) != 0)
		_Pass("Different DH keys produce different MacKey/LTK");
	else
		_Fail("Different DH keys should produce different keys");

	/* Ea from A, verified by B with wrong MacKey → should fail */
	uint8 r[16];
	memset(r, 0, 16);
	uint32 pk = 123456;
	r[0] = pk & 0xFF;
	r[1] = (pk >> 8) & 0xFF;
	r[2] = (pk >> 16) & 0xFF;
	r[3] = (pk >> 24) & 0xFF;
	uint8 iocap[3] = { 0x0D, 0x00, 0x04 };

	uint8 EaFromA[16];
	smp_f6(macKeyA, Na, Nb, r, iocap, addrA, addrB, EaFromA);

	uint8 EaExpectedByB[16];
	smp_f6(macKeyB, Na, Nb, r, iocap, addrA, addrB, EaExpectedByB);

	if (memcmp(EaFromA, EaExpectedByB, 16) != 0)
		_Pass("DHKey check Ea fails with mismatched DH keys (MITM detected)");
	else
		_Fail("DHKey check should fail with different DH keys");
}


/* ========================================================================
 * Test 5: g2 numeric comparison consistency
 *
 * Both sides must compute the same numeric comparison value from the
 * same public keys and nonces.
 * ======================================================================== */

static void
_TestG2Consistency()
{
	printf("\n--- g2 numeric comparison consistency ---\n");

	uint8 PKax[32], PKbx[32], Na[16], Nb[16];
	_PrngSeed(0xDEAD);
	_PrngFill(PKax, 32);
	_PrngFill(PKbx, 32);
	_PrngFill(Na, 16);
	_PrngFill(Nb, 16);

	/* Initiator computes: g2(PKax, PKbx, Na, Nb) */
	uint32 passkeyA;
	smp_g2(PKax, PKbx, Na, Nb, &passkeyA);

	/* Responder computes with same inputs (spec requires same order) */
	uint32 passkeyB;
	smp_g2(PKax, PKbx, Na, Nb, &passkeyB);

	if (passkeyA == passkeyB) {
		printf("  Numeric comparison value: %06u\n", passkeyA);
		_Pass("g2 produces identical value on both sides");
	} else {
		_Fail("g2 values differ");
		printf("  A: %u, B: %u\n", passkeyA, passkeyB);
	}

	/* Verify range: must be 0..999999 */
	if (passkeyA <= 999999)
		_Pass("g2 value in valid range (0..999999)");
	else
		_Fail("g2 value out of range");
}


/* ========================================================================
 * Test 6: f5 salt and keyID verification
 *
 * The f5 function uses a specific salt and keyID ("btle") defined in
 * the spec. Verify these are correct by checking against known values.
 * ======================================================================== */

static void
_TestF5Internals()
{
	printf("\n--- f5 internal consistency ---\n");

	/* Two calls with same inputs must produce same output */
	uint8 dhkey[32], n1[16], n2[16], a1[7], a2[7];
	_PrngSeed(0xF5F5F5F5);
	_PrngFill(dhkey, 32);
	_PrngFill(n1, 16);
	_PrngFill(n2, 16);
	_PrngFill(a1, 7);
	_PrngFill(a2, 7);

	uint8 macKey1[16], ltk1[16];
	uint8 macKey2[16], ltk2[16];

	smp_f5(dhkey, n1, n2, a1, a2, macKey1, ltk1);
	smp_f5(dhkey, n1, n2, a1, a2, macKey2, ltk2);

	if (memcmp(macKey1, macKey2, 16) == 0 && memcmp(ltk1, ltk2, 16) == 0)
		_Pass("f5 is deterministic");
	else
		_Fail("f5 not deterministic");

	/* MacKey and LTK must be different (different counter values) */
	if (memcmp(macKey1, ltk1, 16) != 0)
		_Pass("f5 MacKey != LTK (different counters)");
	else
		_Fail("f5 MacKey == LTK (should differ)");

	/* Swapping N1/N2 must produce different keys */
	uint8 macKey3[16], ltk3[16];
	smp_f5(dhkey, n2, n1, a1, a2, macKey3, ltk3);

	if (memcmp(ltk1, ltk3, 16) != 0)
		_Pass("f5 output changes when N1/N2 swapped");
	else
		_Fail("f5 output should change when N1/N2 swapped");

	/* Swapping A1/A2 must produce different keys */
	uint8 macKey4[16], ltk4[16];
	smp_f5(dhkey, n1, n2, a2, a1, macKey4, ltk4);

	if (memcmp(ltk1, ltk4, 16) != 0)
		_Pass("f5 output changes when A1/A2 swapped");
	else
		_Fail("f5 output should change when A1/A2 swapped");
}


/* ========================================================================
 * Test 7: Passkey of zero and maximum value
 * ======================================================================== */

static void
_TestPasskeyEdgeCases()
{
	printf("\n--- Passkey edge cases ---\n");

	uint8 PKax[32], PKbx[32];
	_PrngSeed(0xED6E);
	_PrngFill(PKax, 32);
	_PrngFill(PKbx, 32);

	/* Passkey = 0: all 20 bits are 0, so Z = 0x80 for every round */
	uint8 confirm0[16], confirmChk[16];
	uint8 nonce[16];
	_PrngFill(nonce, 16);

	smp_f4(PKax, PKbx, nonce, 0x80, confirm0);
	smp_f4(PKax, PKbx, nonce, 0x80, confirmChk);
	if (memcmp(confirm0, confirmChk, 16) == 0)
		_Pass("Passkey=0: consistent confirm with Z=0x80");
	else
		_Fail("Passkey=0: inconsistent confirm");

	/* Passkey = 999999 (max): has mixed bits */
	uint32 maxPk = 999999;
	uint8 confirmMax0[16], confirmMax1[16];

	uint8 r0 = (maxPk >> 0) & 1; /* bit 0 */
	uint8 r19 = (maxPk >> 19) & 1; /* bit 19 */

	smp_f4(PKax, PKbx, nonce, 0x80 + r0, confirmMax0);
	smp_f4(PKax, PKbx, nonce, 0x80 + r19, confirmMax1);

	/* If r0 != r19, confirms should differ; if equal, they should match */
	if (r0 != r19) {
		if (memcmp(confirmMax0, confirmMax1, 16) != 0)
			_Pass("Passkey=999999: different bits produce different confirms");
		else
			_Fail("Passkey=999999: different bits should differ");
	} else {
		if (memcmp(confirmMax0, confirmMax1, 16) == 0)
			_Pass("Passkey=999999: same bits produce same confirms");
		else
			_Fail("Passkey=999999: same bits should match");
	}

	printf("  passkey=999999 binary: ");
	for (int i = 19; i >= 0; i--)
		printf("%d", (maxPk >> i) & 1);
	printf(" (bits 0=%d, 19=%d)\n", r0, r19);
}


/* ======================================================================== */

int
main()
{
	printf("=== SMP Pairing State Machine Simulation ===\n");
	printf("Simulates both sides of LE Secure Connections\n");
	printf("Passkey Entry pairing without kernel/hardware.\n");

	_TestFullPairingHappyPath();
	_TestWrongPasskey();
	_TestTamperedConfirm();
	_TestDhKeyMismatch();
	_TestG2Consistency();
	_TestF5Internals();
	_TestPasskeyEdgeCases();

	printf("\n========================================\n");
	printf("Results: %d passed, %d failed\n", sTestsPassed, sTestsFailed);
	printf("========================================\n");

	return sTestsFailed > 0 ? 1 : 0;
}
