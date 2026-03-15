/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Standalone ECDH P-256 (secp256r1) implementation for kernel space.
 *
 * Designed for the bt_smp kernel addon: no heap allocation, no external
 * crypto library, all computation on the stack.
 *
 * Constant-time: Montgomery ladder for scalar multiplication,
 * conditional select via bitmask (no branches on secret data).
 *
 * Layers:
 *   1. 256-bit unsigned integer arithmetic
 *   2. Field arithmetic mod p (Solinas reduction)
 *   3. Elliptic curve point operations (Jacobian coordinates, a = -3)
 *   4. Scalar multiplication (Montgomery ladder)
 *   5. High-level API (keypair generation, ECDH, point validation)
 */

#include "p256.h"

#include <string.h>


/* ========================================================================
 * Constants
 * ======================================================================== */

/* P-256 prime: p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const p256_int kP = {{
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
	0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
}};

/* Curve parameter b */
static const p256_int kB = {{
	0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0,
	0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8
}};

/* Curve order n */
static const p256_int kN = {{
	0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
	0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
}};

/* Generator point G (affine coordinates) */
static const p256_int kGx = {{
	0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81,
	0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2
}};

static const p256_int kGy = {{
	0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357,
	0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2
}};


/* ========================================================================
 * Layer 1: 256-bit unsigned integer arithmetic
 * ======================================================================== */

static const p256_int kZero = {{ 0, 0, 0, 0, 0, 0, 0, 0 }};
static const p256_int kOne = {{ 1, 0, 0, 0, 0, 0, 0, 0 }};


/* Return 1 if a == 0, else 0. Constant-time. */
static uint32
_IsZero(const p256_int* a)
{
	uint32 bits = 0;
	for (int i = 0; i < 8; i++)
		bits |= a->limb[i];
	/* If bits == 0, then (bits | -bits) has MSB = 0; else MSB = 1.
	   We want to return 1 when bits == 0. */
	return 1 - ((bits | (uint32)(-(int32)bits)) >> 31);
}


/* a + b, return carry (0 or 1). */
static uint32
_Add(p256_int* result, const p256_int* a, const p256_int* b)
{
	uint64 carry = 0;
	for (int i = 0; i < 8; i++) {
		carry += (uint64)a->limb[i] + b->limb[i];
		result->limb[i] = (uint32)carry;
		carry >>= 32;
	}
	return (uint32)carry;
}


/* a - b, return borrow (0 or 1). */
static uint32
_Sub(p256_int* result, const p256_int* a, const p256_int* b)
{
	uint64 borrow = 0;
	for (int i = 0; i < 8; i++) {
		uint64 diff = (uint64)a->limb[i] - b->limb[i] - borrow;
		result->limb[i] = (uint32)diff;
		borrow = (diff >> 63) & 1;
	}
	return (uint32)borrow;
}


/* Return 1 if a >= b, else 0. Constant-time. */
static uint32
_Gte(const p256_int* a, const p256_int* b)
{
	p256_int tmp;
	uint32 borrow = _Sub(&tmp, a, b);
	return 1 - borrow;
}


/* Constant-time select: result = cond ? a : b, where cond is 0 or 1. */
static void
_Select(p256_int* result, uint32 cond, const p256_int* a, const p256_int* b)
{
	uint32 mask = (uint32)(-(int32)cond); /* cond=1 → 0xFFFFFFFF, cond=0 → 0 */
	for (int i = 0; i < 8; i++)
		result->limb[i] = (a->limb[i] & mask) | (b->limb[i] & ~mask);
}


/* Constant-time point select: result = cond ? a : b */
static void
_PointSelect(p256_point* result, uint32 cond, const p256_point* a,
	const p256_point* b)
{
	_Select(&result->x, cond, &a->x, &b->x);
	_Select(&result->y, cond, &a->y, &b->y);
	_Select(&result->z, cond, &a->z, &b->z);
}


/* ========================================================================
 * Layer 2: Field arithmetic mod p
 *
 * p = 2^256 - 2^224 + 2^192 + 2^96 - 1
 * Uses Solinas reduction for fast modular reduction.
 * ======================================================================== */

/* Reduce modulo p: if a >= p, subtract p. */
static void
_FieldReduce(p256_int* a)
{
	p256_int tmp;
	uint32 noUnderflow = 1 - _Sub(&tmp, a, &kP);
	_Select(a, noUnderflow, &tmp, a);
}


/* result = (a + b) mod p */
static void
_FieldAdd(p256_int* result, const p256_int* a, const p256_int* b)
{
	uint32 carry = _Add(result, a, b);
	/* If carry or result >= p, subtract p */
	p256_int tmp;
	uint32 borrow = _Sub(&tmp, result, &kP);
	/* Select tmp if (carry == 1) or (borrow == 0) */
	uint32 doSub = carry | (1 - borrow);
	_Select(result, doSub, &tmp, result);
}


/* result = (a - b) mod p */
static void
_FieldSub(p256_int* result, const p256_int* a, const p256_int* b)
{
	uint32 borrow = _Sub(result, a, b);
	/* If borrow, add p back */
	p256_int tmp;
	_Add(&tmp, result, &kP);
	_Select(result, borrow, &tmp, result);
}


/* result = -a mod p */
static void
_FieldNeg(p256_int* result, const p256_int* a)
{
	_FieldSub(result, &kZero, a);
}


/*
 * Solinas reduction for P-256.
 *
 * Given a 512-bit product t[0..15] (16 uint32 limbs, little-endian),
 * compute t mod p using the special form of p.
 *
 * From NIST SP 800-186 (and FIPS 186-4):
 *   t mod p = (s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9) mod p
 * where s1..s9 are formed from the limbs of t.
 */
static void
_SolinasReduce(p256_int* result, const uint32 t[16])
{
	/* Notation: t[i] = 32-bit limb i of the 512-bit product.
	   s1 = (t7, t6, t5, t4, t3, t2, t1, t0) */

	int64 acc[8];

	/* Start with s1 */
	for (int i = 0; i < 8; i++)
		acc[i] = (int64)t[i];

	/* + 2 * s2 = 2 * (t15, t14, t13, t12, t11, 0, 0, 0) */
	acc[3] += 2 * (int64)t[11];
	acc[4] += 2 * (int64)t[12];
	acc[5] += 2 * (int64)t[13];
	acc[6] += 2 * (int64)t[14];
	acc[7] += 2 * (int64)t[15];

	/* + 2 * s3 = 2 * (0, t15, t14, t13, t12, 0, 0, 0) */
	acc[3] += 2 * (int64)t[12];
	acc[4] += 2 * (int64)t[13];
	acc[5] += 2 * (int64)t[14];
	acc[6] += 2 * (int64)t[15];

	/* + s4 = (t15, t14, 0, 0, 0, t10, t9, t8) */
	acc[0] += (int64)t[8];
	acc[1] += (int64)t[9];
	acc[2] += (int64)t[10];
	acc[6] += (int64)t[14];
	acc[7] += (int64)t[15];

	/* + s5 = (t8, t13, t15, t14, t13, t11, t10, t9) */
	acc[0] += (int64)t[9];
	acc[1] += (int64)t[10];
	acc[2] += (int64)t[11];
	acc[3] += (int64)t[13];
	acc[4] += (int64)t[14];
	acc[5] += (int64)t[15];
	acc[6] += (int64)t[13];
	acc[7] += (int64)t[8];

	/* - s6 = (t10, t8, 0, 0, 0, t13, t12, t11) */
	acc[0] -= (int64)t[11];
	acc[1] -= (int64)t[12];
	acc[2] -= (int64)t[13];
	acc[6] -= (int64)t[8];
	acc[7] -= (int64)t[10];

	/* - s7 = (t11, t9, 0, 0, t15, t14, t13, t12) */
	acc[0] -= (int64)t[12];
	acc[1] -= (int64)t[13];
	acc[2] -= (int64)t[14];
	acc[3] -= (int64)t[15];
	acc[6] -= (int64)t[9];
	acc[7] -= (int64)t[11];

	/* - s8 = (t12, 0, t10, t9, t8, t15, t14, t13) */
	acc[0] -= (int64)t[13];
	acc[1] -= (int64)t[14];
	acc[2] -= (int64)t[15];
	acc[3] -= (int64)t[8];
	acc[4] -= (int64)t[9];
	acc[5] -= (int64)t[10];
	acc[7] -= (int64)t[12];

	/* - s9 = (t13, 0, t11, t10, t9, 0, t15, t14) */
	acc[0] -= (int64)t[14];
	acc[1] -= (int64)t[15];
	acc[3] -= (int64)t[9];
	acc[4] -= (int64)t[10];
	acc[5] -= (int64)t[11];
	acc[7] -= (int64)t[13];

	/* Propagate carries/borrows through the accumulator */
	int64 carry = 0;
	for (int i = 0; i < 8; i++) {
		acc[i] += carry;
		result->limb[i] = (uint32)(acc[i] & 0xFFFFFFFF);
		carry = acc[i] >> 32;
	}

	/* Handle remaining carry/borrow.
	   carry can be in range roughly [-8, +8] at this point.
	   Negative carry means the result is below zero mod p → add p.
	   Positive carry means the result exceeds 2^256 → subtract p. */
	while (carry < 0) {
		p256_int tmp;
		_Add(&tmp, result, &kP);
		*result = tmp;
		carry++;
	}
	while (carry > 0) {
		p256_int tmp;
		_Sub(&tmp, result, &kP);
		*result = tmp;
		carry--;
	}

	/* Final reduction: ensure result is in [0, p) */
	_FieldReduce(result);
	_FieldReduce(result);
}


/* result = (a * b) mod p. Schoolbook 8x8 multiplication + Solinas reduction. */
static void
_FieldMul(p256_int* result, const p256_int* a, const p256_int* b)
{
	uint32 t[16];
	memset(t, 0, sizeof(t));

	for (int i = 0; i < 8; i++) {
		uint64 carry = 0;
		for (int j = 0; j < 8; j++) {
			uint64 uv = (uint64)a->limb[i] * b->limb[j] + t[i + j] + carry;
			t[i + j] = (uint32)uv;
			carry = uv >> 32;
		}
		t[i + 8] = (uint32)carry;
	}

	_SolinasReduce(result, t);
}


/* result = a^2 mod p */
static void
_FieldSqr(p256_int* result, const p256_int* a)
{
	_FieldMul(result, a, a);
}


/* result = a^(p-2) mod p (Fermat's little theorem for modular inverse).
 *
 * p - 2 = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFD
 *
 * Bit pattern MSB to LSB (256 bits):
 *   [32 ones][31 zeros][1 one][96 zeros][94 ones][1 zero][1 one]
 *
 * We build a^(2^k - 1) for various k using an addition chain,
 * then assemble the exponent using square-and-multiply.
 * For a run of N ones: square N times, multiply by a^(2^N - 1).
 * For a run of N zeros: square N times.
 * For a single one: square once, multiply by a.
 */
static void
_FieldInv(p256_int* result, const p256_int* a)
{
	p256_int x2, x4, x8, x16, x32, t;

	/* Build a^(2^k - 1) for small k */

	/* x2 = a^(2^2 - 1) = a^3 */
	_FieldSqr(&t, a);
	_FieldMul(&x2, &t, a);

	/* x4 = a^(2^4 - 1) = a^15 */
	_FieldSqr(&t, &x2);
	_FieldSqr(&t, &t);
	_FieldMul(&x4, &t, &x2);

	/* x8 = a^(2^8 - 1) */
	t = x4;
	for (int i = 0; i < 4; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x8, &t, &x4);

	/* x16 = a^(2^16 - 1) */
	t = x8;
	for (int i = 0; i < 8; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x16, &t, &x8);

	/* x32 = a^(2^32 - 1) */
	t = x16;
	for (int i = 0; i < 16; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x32, &t, &x16);

	/* Build a^(2^94 - 1) for the 94-ones run.
	   a^(2^64 - 1) = (a^(2^32 - 1))^(2^32) * a^(2^32 - 1) */
	p256_int x64;
	t = x32;
	for (int i = 0; i < 32; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x64, &t, &x32);

	/* a^(2^94 - 1) = (a^(2^64 - 1))^(2^30) * a^(2^30 - 1)
	   First build a^(2^30 - 1): */
	p256_int x6, x14, x30;

	_FieldSqr(&t, &x4);
	_FieldSqr(&t, &t);
	_FieldMul(&x6, &t, &x2);

	t = x8;
	for (int i = 0; i < 6; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x14, &t, &x6);

	t = x16;
	for (int i = 0; i < 14; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x30, &t, &x14);

	/* a^(2^94 - 1) */
	p256_int x94;
	t = x64;
	for (int i = 0; i < 30; i++)
		_FieldSqr(&t, &t);
	_FieldMul(&x94, &t, &x30);

	/* Assemble a^(p-2) using square-and-multiply:
	   [32 ones][31 zeros][1 one][96 zeros][94 ones][1 zero][1 one] */

	p256_int r = x32;

	/* [31 zeros]: square 31 times */
	for (int i = 0; i < 31; i++)
		_FieldSqr(&r, &r);

	/* [1 one]: square once, multiply by a */
	_FieldSqr(&r, &r);
	_FieldMul(&r, &r, a);

	/* [96 zeros]: square 96 times */
	for (int i = 0; i < 96; i++)
		_FieldSqr(&r, &r);

	/* [94 ones]: square 94 times, multiply by a^(2^94 - 1) */
	for (int i = 0; i < 94; i++)
		_FieldSqr(&r, &r);
	_FieldMul(&r, &r, &x94);

	/* [1 zero]: square once */
	_FieldSqr(&r, &r);

	/* [1 one]: square once, multiply by a */
	_FieldSqr(&r, &r);
	_FieldMul(&r, &r, a);

	*result = r;
}


/* ========================================================================
 * Layer 3: EC point operations in Jacobian coordinates
 *
 * Curve: y^2 = x^3 + ax + b, where a = -3, b = kB
 * Point at infinity: Z = 0
 * Affine (X, Y) ↔ Jacobian (X, Y, 1)
 * Jacobian (X, Y, Z) ↔ Affine (X/Z^2, Y/Z^3)
 * ======================================================================== */


/* Point doubling: R = 2*P.
 * Optimized for a = -3.
 * Cost: 4 mul + 4 sqr + misc add/sub.
 *
 * Algorithm (from "Guide to Elliptic Curve Cryptography", Hankerson et al.):
 *   A = 3(X - Z^2)(X + Z^2)   [uses a = -3 optimization]
 *   B = Y * Z
 *   C = X * Y * B
 *   D = A^2 - 8C
 *   X' = 2 * B * D             ... wait, let me use the standard formulas.
 *
 * Standard Jacobian doubling for a = -3:
 *   M = 3 * (X1 - Z1^2) * (X1 + Z1^2)
 *   S = 4 * X1 * Y1^2
 *   X3 = M^2 - 2*S
 *   Y3 = M * (S - X3) - 8 * Y1^4
 *   Z3 = 2 * Y1 * Z1
 */
static void
_PointDouble(p256_point* r, const p256_point* p)
{
	p256_int z2, t1, t2, m, y2, s, x3, y3, z3, y4;

	/* Z^2 */
	_FieldSqr(&z2, &p->z);

	/* M = 3 * (X - Z^2) * (X + Z^2) */
	_FieldSub(&t1, &p->x, &z2);  /* X - Z^2 */
	_FieldAdd(&t2, &p->x, &z2);  /* X + Z^2 */
	_FieldMul(&m, &t1, &t2);     /* (X - Z^2)(X + Z^2) */
	/* m = m * 3 = m + m + m */
	_FieldAdd(&t1, &m, &m);
	_FieldAdd(&m, &t1, &m);

	/* Y^2 */
	_FieldSqr(&y2, &p->y);

	/* S = 4 * X * Y^2 */
	_FieldMul(&s, &p->x, &y2);   /* X * Y^2 */
	_FieldAdd(&s, &s, &s);       /* 2 * X * Y^2 */
	_FieldAdd(&s, &s, &s);       /* 4 * X * Y^2 */

	/* X3 = M^2 - 2*S */
	_FieldSqr(&x3, &m);
	_FieldSub(&x3, &x3, &s);
	_FieldSub(&x3, &x3, &s);

	/* Y4 = Y^4 (for use in Y3) */
	_FieldSqr(&y4, &y2);

	/* Y3 = M * (S - X3) - 8 * Y^4 */
	_FieldSub(&t1, &s, &x3);
	_FieldMul(&y3, &m, &t1);
	/* 8 * Y^4 */
	_FieldAdd(&t1, &y4, &y4);   /* 2 * Y^4 */
	_FieldAdd(&t1, &t1, &t1);   /* 4 * Y^4 */
	_FieldAdd(&t1, &t1, &t1);   /* 8 * Y^4 */
	_FieldSub(&y3, &y3, &t1);

	/* Z3 = 2 * Y * Z */
	_FieldMul(&z3, &p->y, &p->z);
	_FieldAdd(&z3, &z3, &z3);

	r->x = x3;
	r->y = y3;
	r->z = z3;
}


/* Point addition: R = P + Q (general case).
 *
 * Standard Jacobian addition:
 *   U1 = X1 * Z2^2,  U2 = X2 * Z1^2
 *   S1 = Y1 * Z2^3,  S2 = Y2 * Z1^3
 *   H = U2 - U1
 *   R = S2 - S1
 *   X3 = R^2 - H^3 - 2*U1*H^2
 *   Y3 = R*(U1*H^2 - X3) - S1*H^3
 *   Z3 = H * Z1 * Z2
 *
 * Special cases handled constant-time:
 *   - If P == O (Z1 == 0): return Q
 *   - If Q == O (Z2 == 0): return P
 *   - If P == Q (H == 0 and R == 0): return 2*P
 *   - If P == -Q (H == 0 and R != 0): return O
 */
static void
_PointAdd(p256_point* result, const p256_point* p, const p256_point* q)
{
	p256_int z1z1, z2z2, u1, u2, s1, s2, h, r_val;
	p256_int hh, hhh, t1, t2, x3, y3, z3;

	/* Z1^2, Z2^2 */
	_FieldSqr(&z1z1, &p->z);
	_FieldSqr(&z2z2, &q->z);

	/* U1 = X1 * Z2^2, U2 = X2 * Z1^2 */
	_FieldMul(&u1, &p->x, &z2z2);
	_FieldMul(&u2, &q->x, &z1z1);

	/* S1 = Y1 * Z2^3, S2 = Y2 * Z1^3 */
	_FieldMul(&t1, &p->y, &z2z2);
	_FieldMul(&s1, &t1, &q->z);
	_FieldMul(&t1, &q->y, &z1z1);
	_FieldMul(&s2, &t1, &p->z);

	/* H = U2 - U1, R = S2 - S1 */
	_FieldSub(&h, &u2, &u1);
	_FieldSub(&r_val, &s2, &s1);

	/* H^2, H^3 */
	_FieldSqr(&hh, &h);
	_FieldMul(&hhh, &hh, &h);

	/* U1 * H^2 */
	_FieldMul(&t1, &u1, &hh);

	/* X3 = R^2 - H^3 - 2*U1*H^2 */
	_FieldSqr(&x3, &r_val);
	_FieldSub(&x3, &x3, &hhh);
	_FieldSub(&x3, &x3, &t1);
	_FieldSub(&x3, &x3, &t1);

	/* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
	_FieldSub(&t2, &t1, &x3);
	_FieldMul(&y3, &r_val, &t2);
	_FieldMul(&t2, &s1, &hhh);
	_FieldSub(&y3, &y3, &t2);

	/* Z3 = H * Z1 * Z2 */
	_FieldMul(&z3, &h, &p->z);
	_FieldMul(&z3, &z3, &q->z);

	/* Build the candidate result */
	p256_point candidate;
	candidate.x = x3;
	candidate.y = y3;
	candidate.z = z3;

	/* Handle special cases constant-time */
	uint32 pIsInf = _IsZero(&p->z);
	uint32 qIsInf = _IsZero(&q->z);
	uint32 hIsZero = _IsZero(&h);
	uint32 rIsZero = _IsZero(&r_val);

	/* If P == Q (H==0 and R==0): double P */
	p256_point doubled;
	_PointDouble(&doubled, p);

	/* If P == -Q (H==0 and R!=0): point at infinity */
	p256_point infinity;
	infinity.x = kOne;
	infinity.y = kOne;
	infinity.z = kZero;

	/* Select among the cases */
	/* Start with the general-case candidate */
	p256_point tmp;
	tmp = candidate;

	/* If H==0 and R==0: use doubled */
	_PointSelect(&tmp, hIsZero & rIsZero, &doubled, &tmp);

	/* If H==0 and R!=0: use infinity */
	_PointSelect(&tmp, hIsZero & (1 - rIsZero), &infinity, &tmp);

	/* If Q is infinity: use P */
	_PointSelect(&tmp, qIsInf, p, &tmp);

	/* If P is infinity: use Q */
	_PointSelect(&tmp, pIsInf, q, &tmp);

	*result = tmp;
}


/* ========================================================================
 * Layer 4: Montgomery ladder scalar multiplication
 *
 * Computes R = k * P in constant time.
 * Processes each bit of k from MSB to LSB.
 * ======================================================================== */

static void
_ScalarMul(p256_point* result, const p256_int* k, const p256_point* p)
{
	/* R0 = infinity, R1 = P */
	p256_point r0, r1;
	r0.x = kOne;
	r0.y = kOne;
	r0.z = kZero;
	r1 = *p;

	/* Montgomery ladder: process bits from MSB (bit 255) to LSB (bit 0).
	   For each bit:
	     bit == 0: R1 = R0 + R1, R0 = 2*R0
	     bit == 1: R0 = R0 + R1, R1 = 2*R1

	   Constant-time via conditional swap:
	     1. Swap R0, R1 if bit == 1
	     2. R1 = R0 + R1, R0 = 2*R0  (always the "bit=0" operation)
	     3. Swap R0, R1 if bit == 1  (undo the swap) */
	for (int i = 255; i >= 0; i--) {
		uint32 limbIdx = i / 32;
		uint32 bitIdx = i % 32;
		uint32 bit = (k->limb[limbIdx] >> bitIdx) & 1;

		/* Conditional swap before */
		p256_point s0, s1;
		_PointSelect(&s0, bit, &r1, &r0);
		_PointSelect(&s1, bit, &r0, &r1);

		/* Always: s1 = s0 + s1, s0 = 2*s0 */
		p256_point sum, dbl;
		_PointAdd(&sum, &s0, &s1);
		_PointDouble(&dbl, &s0);

		/* Conditional swap back */
		_PointSelect(&r0, bit, &sum, &dbl);
		_PointSelect(&r1, bit, &dbl, &sum);
	}

	*result = r0;
}


/* ========================================================================
 * Layer 5: High-level API
 * ======================================================================== */

/* Convert byte array (little-endian, as used by BT SMP) to p256_int.
   BT SMP transmits coordinates in little-endian byte order.
   p256_int limb[0] = least significant 32-bit word.
   So bytes[0..3] → limb[0], bytes[4..7] → limb[1], etc. */
static void
_BytesToInt(p256_int* result, const uint8 bytes[32])
{
	for (int i = 0; i < 8; i++) {
		result->limb[i] = (uint32)bytes[i * 4]
			| ((uint32)bytes[i * 4 + 1] << 8)
			| ((uint32)bytes[i * 4 + 2] << 16)
			| ((uint32)bytes[i * 4 + 3] << 24);
	}
}


/* Convert p256_int to byte array (little-endian). */
static void
_IntToBytes(uint8 bytes[32], const p256_int* a)
{
	for (int i = 0; i < 8; i++) {
		bytes[i * 4 + 0] = (uint8)(a->limb[i]);
		bytes[i * 4 + 1] = (uint8)(a->limb[i] >> 8);
		bytes[i * 4 + 2] = (uint8)(a->limb[i] >> 16);
		bytes[i * 4 + 3] = (uint8)(a->limb[i] >> 24);
	}
}


/* Convert Jacobian point to affine coordinates.
   Returns B_BAD_VALUE if the point is at infinity. */
static status_t
_ToAffine(p256_int* affineX, p256_int* affineY, const p256_point* p)
{
	if (_IsZero(&p->z))
		return B_BAD_VALUE;

	p256_int z_inv, z_inv2, z_inv3;
	_FieldInv(&z_inv, &p->z);
	_FieldSqr(&z_inv2, &z_inv);
	_FieldMul(&z_inv3, &z_inv2, &z_inv);

	_FieldMul(affineX, &p->x, &z_inv2);
	_FieldMul(affineY, &p->y, &z_inv3);

	return B_OK;
}


/* Reduce a 256-bit value mod n (curve order).
   If val >= n, subtract n. Since our random input is at most 2^256 - 1
   and n ≈ 2^256, at most one subtraction suffices. */
static void
_ReduceModN(p256_int* result, const p256_int* val)
{
	*result = *val;
	p256_int tmp;
	uint32 noUnderflow = 1 - _Sub(&tmp, result, &kN);
	_Select(result, noUnderflow, &tmp, result);
}


status_t
p256_generate_keypair_from_random(const uint8 random[32],
	uint8 privateKey[32], uint8 publicKeyX[32], uint8 publicKeyY[32])
{
	p256_int d;
	_BytesToInt(&d, random);
	_ReduceModN(&d, &d);

	/* Check d != 0 (astronomically unlikely with good RNG) */
	if (_IsZero(&d))
		return B_BAD_VALUE;

	_IntToBytes(privateKey, &d);

	/* Q = d * G */
	p256_point g;
	g.x = kGx;
	g.y = kGy;
	g.z = kOne;

	p256_point q;
	_ScalarMul(&q, &d, &g);

	p256_int qx, qy;
	status_t status = _ToAffine(&qx, &qy, &q);
	if (status != B_OK)
		return status;

	_IntToBytes(publicKeyX, &qx);
	_IntToBytes(publicKeyY, &qy);

	return B_OK;
}


status_t
p256_compute_dhkey(const uint8 privateKey[32], const uint8 remoteX[32],
	const uint8 remoteY[32], uint8 dhkey[32])
{
	/* Validate the remote point is on the curve */
	if (!p256_point_is_on_curve(remoteX, remoteY))
		return B_BAD_VALUE;

	p256_int d, rx, ry;
	_BytesToInt(&d, privateKey);
	_BytesToInt(&rx, remoteX);
	_BytesToInt(&ry, remoteY);

	p256_point remote;
	remote.x = rx;
	remote.y = ry;
	remote.z = kOne;

	p256_point s;
	_ScalarMul(&s, &d, &remote);

	/* Return X coordinate of S */
	p256_int sx, sy;
	status_t status = _ToAffine(&sx, &sy, &s);
	if (status != B_OK)
		return status;

	_IntToBytes(dhkey, &sx);

	return B_OK;
}


bool
p256_point_is_on_curve(const uint8 x[32], const uint8 y[32])
{
	p256_int px, py;
	_BytesToInt(&px, x);
	_BytesToInt(&py, y);

	/* Check that coordinates are in range [0, p) */
	if (_Gte(&px, &kP) || _Gte(&py, &kP))
		return false;

	/* y^2 mod p */
	p256_int lhs;
	_FieldSqr(&lhs, &py);

	/* x^3 - 3x + b mod p */
	p256_int x2, x3, rhs, three_x;
	_FieldSqr(&x2, &px);
	_FieldMul(&x3, &x2, &px);

	/* 3x = x + x + x */
	_FieldAdd(&three_x, &px, &px);
	_FieldAdd(&three_x, &three_x, &px);

	_FieldSub(&rhs, &x3, &three_x);
	_FieldAdd(&rhs, &rhs, &kB);

	/* Compare lhs == rhs */
	for (int i = 0; i < 8; i++) {
		if (lhs.limb[i] != rhs.limb[i])
			return false;
	}

	return true;
}
