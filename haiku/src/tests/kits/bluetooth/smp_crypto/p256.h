/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Standalone ECDH P-256 implementation for kernel space.
 * No external crypto library dependencies.
 */
#ifndef _P256_H_
#define _P256_H_

#include <SupportDefs.h>


/* 256-bit integer, stored as 8 little-endian 32-bit limbs.
   limb[0] is the least significant word. */
typedef struct {
	uint32 limb[8];
} p256_int;


/* EC point in Jacobian coordinates: affine (X/Z^2, Y/Z^3).
   The point at infinity is represented by Z = 0. */
typedef struct {
	p256_int x;
	p256_int y;
	p256_int z;
} p256_point;


/* Generate an ECDH P-256 key pair from 32 bytes of random data.
   The random input is reduced mod n to form the private key,
   then the public key Q = d * G is computed.
   Returns B_OK on success, B_BAD_VALUE if the random input
   produces a zero or out-of-range private key (astronomically unlikely). */
status_t p256_generate_keypair_from_random(const uint8 random[32],
	uint8 privateKey[32], uint8 publicKeyX[32], uint8 publicKeyY[32]);

/* Compute the ECDH shared secret (DH key).
   Validates that the remote public key is on the curve,
   then computes S = privateKey * (remoteX, remoteY) and returns
   the X coordinate of S as the DH key.
   Returns B_OK on success, B_BAD_VALUE if the remote point is invalid. */
status_t p256_compute_dhkey(const uint8 privateKey[32],
	const uint8 remoteX[32], const uint8 remoteY[32], uint8 dhkey[32]);

/* Verify that the point (x, y) is on the P-256 curve.
   Returns true if y^2 = x^3 - 3x + b (mod p). */
bool p256_point_is_on_curve(const uint8 x[32], const uint8 y[32]);


#endif /* _P256_H_ */
