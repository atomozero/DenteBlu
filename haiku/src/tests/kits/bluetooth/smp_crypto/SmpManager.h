/*
 * Minimal SmpManager.h shim for userspace crypto tests.
 * Provides only the function declarations needed by smp_crypto.cpp.
 */
#ifndef _SMP_MANAGER_H_
#define _SMP_MANAGER_H_

#include <SupportDefs.h>
#include <OS.h>

status_t smp_aes128(const uint8 key[16], const uint8 plaintext[16],
	uint8 encrypted[16]);
status_t smp_aes_cmac(const uint8 key[16], const uint8* message,
	uint32 messageLength, uint8 mac[16]);
status_t smp_f4(const uint8 u[32], const uint8 v[32], const uint8 x[16],
	uint8 z, uint8 result[16]);
status_t smp_f5(const uint8 dhkey[32], const uint8 n1[16],
	const uint8 n2[16], const uint8 a1[7], const uint8 a2[7],
	uint8 mackey[16], uint8 ltk[16]);
status_t smp_f6(const uint8 mackey[16], const uint8 n1[16],
	const uint8 n2[16], const uint8 r[16], const uint8 iocap[3],
	const uint8 a1[7], const uint8 a2[7], uint8 result[16]);
status_t smp_g2(const uint8 u[32], const uint8 v[32], const uint8 x[16],
	const uint8 y[16], uint32* _passkey);
status_t smp_ecdh_generate_keypair(uint8 privateKey[32],
	uint8 publicKeyX[32], uint8 publicKeyY[32]);
status_t smp_ecdh_compute_dhkey(const uint8 privateKey[32],
	const uint8 remoteX[32], const uint8 remoteY[32], uint8 dhkey[32]);

#endif /* _SMP_MANAGER_H_ */
