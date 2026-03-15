/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _SMP_MANAGER_H_
#define _SMP_MANAGER_H_

#include <OS.h>

#include <bluetooth/bluetooth.h>
#include <btCoreData.h>
#include <bt_smp.h>


/* SMP State Machine */
typedef enum {
	SMP_STATE_IDLE = 0,
	SMP_STATE_WAIT_RSP,
	SMP_STATE_WAIT_PUBLIC_KEY,
	SMP_STATE_WAIT_CONFIRM,
	SMP_STATE_WAIT_RANDOM,
	SMP_STATE_WAIT_DHKEY_CHECK,
	SMP_STATE_PAIRED,
	SMP_STATE_FAILED
} smp_state_t;


/* Forward declarations */
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


class SmpManager {
public:
								SmpManager(HciConnection* connection);
	virtual						~SmpManager();

	status_t					ReceiveData(net_buffer* buffer);

	status_t					InitiatePairing();
	void						SetPasskey(uint32 passkey);
	void						SetNumericComparisonResult(bool confirmed);

	smp_state_t					State() const { return fState; }
	const uint8*				Ltk() const { return fLtk; }

private:
	status_t					_SendPdu(const uint8* pdu, uint16 length);
	void						_HandlePairingRsp(const uint8* data,
									uint16 length);
	void						_HandlePublicKey(const uint8* data,
									uint16 length);
	void						_HandlePairingConfirm(const uint8* data,
									uint16 length);
	void						_HandlePairingRandom(const uint8* data,
									uint16 length);
	void						_HandleDhkeyCheck(const uint8* data,
									uint16 length);
	void						_HandlePairingFailed(const uint8* data,
									uint16 length);
	void						_SendPairingFailed(uint8 reason);

	void						_StartScPasskeyEntry();
	void						_ProcessPasskeyBit();

	HciConnection*				fConnection;
	smp_state_t					fState;

	/* Pairing parameters */
	uint8						fIoCapability;
	uint8						fAuthReq;
	uint8						fMaxKeySize;

	/* LE Secure Connections keys */
	uint8						fLocalPrivateKey[32];
	uint8						fLocalPublicKeyX[32];
	uint8						fLocalPublicKeyY[32];
	uint8						fRemotePublicKeyX[32];
	uint8						fRemotePublicKeyY[32];
	uint8						fDhKey[32];

	/* Pairing state */
	uint8						fLocalRandom[16];
	uint8						fRemoteRandom[16];
	uint8						fLocalConfirm[16];
	uint8						fRemoteConfirm[16];

	/* Passkey entry / Numeric Comparison */
	uint32						fPasskey;
	uint8						fPasskeyBit;
	bool						fNcConfirmed;
	sem_id						fPasskeySem;

	/* Result */
	uint8						fMacKey[16];
	uint8						fLtk[16];
};


#endif /* _SMP_MANAGER_H_ */
