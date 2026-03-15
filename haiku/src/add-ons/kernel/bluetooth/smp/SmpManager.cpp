/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "SmpManager.h"

#include <string.h>

#include <ByteOrder.h>
#include <NetBufferUtilities.h>

#include <btDebug.h>
#include <btModules.h>
#include <bluetooth/HCI/btHCI_command_le.h>


extern net_buffer_module_info* gBufferModule;
extern bt_hci_module_info* btDevices;


SmpManager::SmpManager(HciConnection* connection)
	:
	fConnection(connection),
	fState(SMP_STATE_IDLE),
	fIoCapability(SMP_IO_KEYBOARD_DISPLAY),
	fAuthReq(SMP_AUTH_BONDING | SMP_AUTH_MITM | SMP_AUTH_SC),
	fMaxKeySize(16),
	fPasskey(0),
	fPasskeyBit(0),
	fNcConfirmed(false)
{
	fPasskeySem = create_sem(0, "smp passkey");
	memset(fLocalPrivateKey, 0, sizeof(fLocalPrivateKey));
	memset(fLtk, 0, sizeof(fLtk));
	memset(fMacKey, 0, sizeof(fMacKey));
}


SmpManager::~SmpManager()
{
	delete_sem(fPasskeySem);
}


status_t
SmpManager::ReceiveData(net_buffer* buffer)
{
	if (buffer->size < 1) {
		gBufferModule->free(buffer);
		return B_ERROR;
	}

	uint8 code;
	gBufferModule->read(buffer, 0, &code, 1);

	uint16 length = buffer->size;
	uint8 data[128];
	if (length > sizeof(data))
		length = sizeof(data);
	gBufferModule->read(buffer, 0, data, length);
	gBufferModule->free(buffer);

	TRACE("%s: SMP code=%#x state=%d length=%d\n", __func__, code,
		fState, length);

	switch (code) {
		case SMP_CMD_PAIRING_RSP:
			_HandlePairingRsp(data, length);
			break;

		case SMP_CMD_PUBLIC_KEY:
			_HandlePublicKey(data, length);
			break;

		case SMP_CMD_PAIRING_CONFIRM:
			_HandlePairingConfirm(data, length);
			break;

		case SMP_CMD_PAIRING_RANDOM:
			_HandlePairingRandom(data, length);
			break;

		case SMP_CMD_DHKEY_CHECK:
			_HandleDhkeyCheck(data, length);
			break;

		case SMP_CMD_PAIRING_FAILED:
			_HandlePairingFailed(data, length);
			break;

		default:
			TRACE("%s: unhandled SMP code %#x\n", __func__, code);
			_SendPairingFailed(SMP_ERR_COMMAND_NOT_SUPPORTED);
			break;
	}

	return B_OK;
}


status_t
SmpManager::InitiatePairing()
{
	TRACE("%s: initiating LE Secure Connections pairing\n", __func__);

	struct smp_pairing_req req;
	req.code = SMP_CMD_PAIRING_REQ;
	req.io_capability = fIoCapability;
	req.oob_data_flag = 0;
	req.auth_req = fAuthReq;
	req.max_key_size = fMaxKeySize;
	req.initiator_key_dist = SMP_DIST_ENC_KEY | SMP_DIST_ID_KEY;
	req.responder_key_dist = SMP_DIST_ENC_KEY | SMP_DIST_ID_KEY;

	fState = SMP_STATE_WAIT_RSP;
	return _SendPdu((uint8*)&req, sizeof(req));
}


void
SmpManager::SetPasskey(uint32 passkey)
{
	fPasskey = passkey;
	release_sem(fPasskeySem);
}


void
SmpManager::SetNumericComparisonResult(bool confirmed)
{
	fNcConfirmed = confirmed;
	release_sem(fPasskeySem);
}


void
SmpManager::_HandlePairingRsp(const uint8* data, uint16 length)
{
	if (fState != SMP_STATE_WAIT_RSP || length < sizeof(smp_pairing_rsp)) {
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	smp_pairing_rsp* rsp = (smp_pairing_rsp*)data;

	TRACE("%s: peer io=%d auth=%#x key_size=%d\n", __func__,
		rsp->io_capability, rsp->auth_req, rsp->max_key_size);

	/* Check for SC support */
	if (!(rsp->auth_req & SMP_AUTH_SC)) {
		ERROR("%s: peer does not support Secure Connections\n", __func__);
		_SendPairingFailed(SMP_ERR_AUTH_REQUIREMENTS);
		return;
	}

	/* Generate ECDH key pair */
	status_t status = smp_ecdh_generate_keypair(fLocalPrivateKey,
		fLocalPublicKeyX, fLocalPublicKeyY);
	if (status != B_OK) {
		ERROR("%s: failed to generate ECDH keypair\n", __func__);
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	/* Send our public key */
	struct smp_public_key pk;
	pk.code = SMP_CMD_PUBLIC_KEY;
	memcpy(pk.x, fLocalPublicKeyX, 32);
	memcpy(pk.y, fLocalPublicKeyY, 32);

	fState = SMP_STATE_WAIT_PUBLIC_KEY;
	_SendPdu((uint8*)&pk, sizeof(pk));
}


void
SmpManager::_HandlePublicKey(const uint8* data, uint16 length)
{
	if (fState != SMP_STATE_WAIT_PUBLIC_KEY
		|| length < sizeof(struct smp_public_key)) {
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	struct smp_public_key* pk = (struct smp_public_key*)data;
	memcpy(fRemotePublicKeyX, pk->x, 32);
	memcpy(fRemotePublicKeyY, pk->y, 32);

	/* Compute DH Key */
	status_t status = smp_ecdh_compute_dhkey(fLocalPrivateKey,
		fRemotePublicKeyX, fRemotePublicKeyY, fDhKey);
	if (status != B_OK) {
		ERROR("%s: failed to compute DH key\n", __func__);
		_SendPairingFailed(SMP_ERR_DHKEY_CHECK_FAILED);
		return;
	}

	TRACE("%s: DH key computed, starting passkey entry\n", __func__);
	_StartScPasskeyEntry();
}


void
SmpManager::_HandlePairingConfirm(const uint8* data, uint16 length)
{
	if (fState != SMP_STATE_WAIT_CONFIRM
		|| length < sizeof(struct smp_pairing_confirm)) {
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	struct smp_pairing_confirm* cfm = (struct smp_pairing_confirm*)data;
	memcpy(fRemoteConfirm, cfm->confirm_value, 16);

	TRACE("%s: received confirm, sending random\n", __func__);

	/* Send our random */
	struct smp_pairing_random rnd;
	rnd.code = SMP_CMD_PAIRING_RANDOM;
	memcpy(rnd.random_value, fLocalRandom, 16);

	fState = SMP_STATE_WAIT_RANDOM;
	_SendPdu((uint8*)&rnd, sizeof(rnd));
}


void
SmpManager::_HandlePairingRandom(const uint8* data, uint16 length)
{
	if (fState != SMP_STATE_WAIT_RANDOM
		|| length < sizeof(struct smp_pairing_random)) {
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	struct smp_pairing_random* rnd = (struct smp_pairing_random*)data;
	memcpy(fRemoteRandom, rnd->random_value, 16);

	/* Verify the peer's confirm value */
	uint8 ri = (fPasskey >> fPasskeyBit) & 0x01;
	uint8 expectedConfirm[16];
	smp_f4(fRemotePublicKeyX, fLocalPublicKeyX, fRemoteRandom, 0x80 + ri,
		expectedConfirm);

	if (memcmp(expectedConfirm, fRemoteConfirm, 16) != 0) {
		ERROR("%s: confirm value mismatch at bit %d\n", __func__,
			fPasskeyBit);
		_SendPairingFailed(SMP_ERR_CONFIRM_VALUE_FAILED);
		return;
	}

	fPasskeyBit++;

	if (fPasskeyBit < 20) {
		/* Continue with next passkey bit */
		_ProcessPasskeyBit();
	} else {
		/* All 20 passkey rounds complete, compute keys */
		TRACE("%s: passkey entry complete, computing LTK\n", __func__);

		/* Build address info for f5/f6 (type byte + 6 addr bytes) */
		uint8 a1[7], a2[7];
		a1[0] = HCI_LE_ADDR_PUBLIC;
		/* We don't have our own bdaddr easily here; use zeros as placeholder.
		   A real implementation would fetch the local address. */
		memset(a1 + 1, 0, 6);
		a2[0] = HCI_LE_ADDR_PUBLIC;
		memcpy(a2 + 1, &fConnection->destination, 6);

		smp_f5(fDhKey, fLocalRandom, fRemoteRandom, a1, a2,
			fMacKey, fLtk);

		/* Compute DHKey check Ea */
		uint8 iocap[3];
		iocap[0] = fAuthReq;
		iocap[1] = 0; /* OOB */
		iocap[2] = fIoCapability;

		uint8 ea[16];
		/* r = 0 for passkey entry in Ea computation */
		uint8 r[16];
		memset(r, 0, 16);
		/* Encode passkey in r for passkey entry */
		uint32 pk = fPasskey;
		r[0] = pk & 0xFF;
		r[1] = (pk >> 8) & 0xFF;
		r[2] = (pk >> 16) & 0xFF;
		r[3] = (pk >> 24) & 0xFF;

		smp_f6(fMacKey, fLocalRandom, fRemoteRandom, r, iocap, a1, a2, ea);

		struct smp_dhkey_check check;
		check.code = SMP_CMD_DHKEY_CHECK;
		memcpy(check.check, ea, 16);

		fState = SMP_STATE_WAIT_DHKEY_CHECK;
		_SendPdu((uint8*)&check, sizeof(check));
	}
}


void
SmpManager::_HandleDhkeyCheck(const uint8* data, uint16 length)
{
	if (fState != SMP_STATE_WAIT_DHKEY_CHECK
		|| length < sizeof(struct smp_dhkey_check)) {
		_SendPairingFailed(SMP_ERR_UNSPECIFIED_REASON);
		return;
	}

	struct smp_dhkey_check* check = (struct smp_dhkey_check*)data;

	/* Verify peer's DHKey check Eb */
	uint8 a1[7], a2[7];
	a1[0] = HCI_LE_ADDR_PUBLIC;
	memset(a1 + 1, 0, 6);
	a2[0] = HCI_LE_ADDR_PUBLIC;
	memcpy(a2 + 1, &fConnection->destination, 6);

	uint8 iocap[3];
	/* Peer's IO cap - use same as ours for symmetric case */
	iocap[0] = fAuthReq;
	iocap[1] = 0;
	iocap[2] = fIoCapability;

	uint8 r[16];
	memset(r, 0, 16);
	uint32 pk = fPasskey;
	r[0] = pk & 0xFF;
	r[1] = (pk >> 8) & 0xFF;
	r[2] = (pk >> 16) & 0xFF;
	r[3] = (pk >> 24) & 0xFF;

	uint8 expectedEb[16];
	smp_f6(fMacKey, fRemoteRandom, fLocalRandom, r, iocap, a2, a1,
		expectedEb);

	if (memcmp(expectedEb, check->check, 16) != 0) {
		ERROR("%s: DHKey check failed\n", __func__);
		_SendPairingFailed(SMP_ERR_DHKEY_CHECK_FAILED);
		return;
	}

	TRACE("%s: pairing complete, LTK established\n", __func__);
	fState = SMP_STATE_PAIRED;

	/* Now start encryption using the LTK via HCI LE Start Encryption.
	   For LE SC pairing, EDIV=0 and Rand=0. */
	/* Note: This would be issued via the HCI layer. The SMP module
	   signals completion and the upper layer triggers encryption. */
}


void
SmpManager::_HandlePairingFailed(const uint8* data, uint16 length)
{
	if (length >= sizeof(struct smp_pairing_failed)) {
		struct smp_pairing_failed* fail = (struct smp_pairing_failed*)data;
		ERROR("%s: pairing failed, reason=%#x\n", __func__, fail->reason);
	}
	fState = SMP_STATE_FAILED;
}


void
SmpManager::_SendPairingFailed(uint8 reason)
{
	struct smp_pairing_failed fail;
	fail.code = SMP_CMD_PAIRING_FAILED;
	fail.reason = reason;
	_SendPdu((uint8*)&fail, sizeof(fail));
	fState = SMP_STATE_FAILED;
}


void
SmpManager::_StartScPasskeyEntry()
{
	fPasskeyBit = 0;

	/* Wait for the passkey to be set by the user */
	TRACE("%s: waiting for passkey from user\n", __func__);
	status_t status = acquire_sem_etc(fPasskeySem, 1,
		B_RELATIVE_TIMEOUT, 60000000LL); /* 60 second timeout */
	if (status != B_OK) {
		ERROR("%s: passkey timeout\n", __func__);
		_SendPairingFailed(SMP_ERR_PASSKEY_ENTRY_FAILED);
		return;
	}

	TRACE("%s: passkey received, starting 20-round exchange\n", __func__);
	_ProcessPasskeyBit();
}


void
SmpManager::_ProcessPasskeyBit()
{
	uint8 ri = (fPasskey >> fPasskeyBit) & 0x01;

	/* Generate random Nai */
	/* In a real implementation this would use a proper CSRNG.
	   For now, use a basic approach. */
	for (int i = 0; i < 16; i++)
		fLocalRandom[i] = (uint8)(system_time() >> (i * 4));

	/* Compute confirm value: Cai = f4(PKax, PKbx, Nai, 0x80 + ri) */
	smp_f4(fLocalPublicKeyX, fRemotePublicKeyX, fLocalRandom,
		0x80 + ri, fLocalConfirm);

	/* Send confirm */
	struct smp_pairing_confirm cfm;
	cfm.code = SMP_CMD_PAIRING_CONFIRM;
	memcpy(cfm.confirm_value, fLocalConfirm, 16);

	fState = SMP_STATE_WAIT_CONFIRM;
	_SendPdu((uint8*)&cfm, sizeof(cfm));
}


status_t
SmpManager::_SendPdu(const uint8* pdu, uint16 length)
{
	net_buffer* buffer = gBufferModule->create(
		length + sizeof(l2cap_basic_header));
	if (buffer == NULL)
		return B_NO_MEMORY;

	l2cap_basic_header l2capHeader;
	l2capHeader.length = B_HOST_TO_LENDIAN_INT16(length);
	l2capHeader.dcid = B_HOST_TO_LENDIAN_INT16(L2CAP_SMP_CID);
	gBufferModule->append(buffer, &l2capHeader, sizeof(l2capHeader));

	gBufferModule->append(buffer, pdu, length);

	buffer->type = fConnection->handle;
	status_t status = btDevices->PostACL(fConnection->Hid, buffer);
	if (status != B_OK) {
		gBufferModule->free(buffer);
		return status;
	}

	return B_OK;
}
