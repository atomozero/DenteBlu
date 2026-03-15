/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AvrcpTarget — AVRCP Target role implementation.
 * Connects to a remote device over L2CAP PSM 0x0017 (AVCTP) and
 * handles pass-through button commands and absolute volume.
 */

#include <bluetooth/AvrcpTarget.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <Application.h>
#include <Messenger.h>
#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>


#define TRACE_AVRCP(fmt, ...) \
	fprintf(stderr, "AVRCP: " fmt, ##__VA_ARGS__)


/* AVCTP header constants */
#define AVCTP_HEADER_SIZE		3
#define AVCTP_PID_AVRCP_HI		0x11
#define AVCTP_PID_AVRCP_LO		0x0E

/* AVCTP packet type field */
#define AVCTP_PKT_SINGLE		0x00

/* AV/C constants */
#define AVC_CTYPE_CONTROL		0x00
#define AVC_CTYPE_STATUS		0x01
#define AVC_CTYPE_NOTIFY		0x03
#define AVC_RESPONSE_ACCEPTED	0x09
#define AVC_RESPONSE_REJECTED	0x0A
#define AVC_RESPONSE_STABLE		0x0C
#define AVC_RESPONSE_CHANGED	0x0D
#define AVC_RESPONSE_INTERIM	0x0F

#define AVC_SUBUNIT_PANEL		0x48	/* subunit_type=9, subunit_id=0 */

#define AVC_OP_PASS_THROUGH		0x7C
#define AVC_OP_VENDOR_DEPENDENT	0x00

/* Vendor dependent: BT SIG Company ID */
#define BT_SIG_COMPANY_ID_0	0x00
#define BT_SIG_COMPANY_ID_1	0x19
#define BT_SIG_COMPANY_ID_2	0x58

/* AVRCP PDU IDs for vendor dependent */
#define AVRCP_PDU_SET_ABSOLUTE_VOLUME		0x50
#define AVRCP_PDU_REGISTER_NOTIFICATION		0x31
#define AVRCP_EVENT_VOLUME_CHANGED			0x0D


using Bluetooth::AvrcpTarget;


AvrcpTarget::AvrcpTarget()
	:
	fSocket(-1),
	fConnected(false),
	fRecvThread(-1),
	fButtonCallback(NULL),
	fButtonCookie(NULL),
	fVolumeCallback(NULL),
	fVolumeCookie(NULL)
{
	memset(&fRemoteAddr, 0, sizeof(fRemoteAddr));
}


AvrcpTarget::~AvrcpTarget()
{
	Disconnect();
}


status_t
AvrcpTarget::Connect(const bdaddr_t& address)
{
	if (fConnected)
		return B_BUSY;

	memcpy(&fRemoteAddr, &address, sizeof(bdaddr_t));

	/* Ensure ACL link */
	TRACE_AVRCP("Ensuring ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_AVRCP("ACL connection failed\n");
		return B_ERROR;
	}

	/* Create L2CAP socket */
	fSocket = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (fSocket < 0) {
		TRACE_AVRCP("socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	/* Bind to local any address */
	struct sockaddr_l2cap local;
	memset(&local, 0, sizeof(local));
	local.l2cap_len = sizeof(local);
	local.l2cap_family = AF_BLUETOOTH;
	local.l2cap_psm = 0;  /* any */
	memset(&local.l2cap_bdaddr, 0, sizeof(local.l2cap_bdaddr));

	if (bind(fSocket, (struct sockaddr*)&local, sizeof(local)) < 0) {
		TRACE_AVRCP("bind() failed: %s\n", strerror(errno));
		close(fSocket);
		fSocket = -1;
		return B_ERROR;
	}

	/* Connect to remote AVCTP PSM */
	struct sockaddr_l2cap remote;
	memset(&remote, 0, sizeof(remote));
	remote.l2cap_len = sizeof(remote);
	remote.l2cap_family = AF_BLUETOOTH;
	remote.l2cap_psm = L2CAP_PSM_AVCTP;
	memcpy(&remote.l2cap_bdaddr, &address, sizeof(bdaddr_t));

	TRACE_AVRCP("Connecting L2CAP PSM 0x%04x...\n", L2CAP_PSM_AVCTP);

	if (connect(fSocket, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
		TRACE_AVRCP("connect() failed: %s\n", strerror(errno));
		close(fSocket);
		fSocket = -1;
		return B_ERROR;
	}

	fConnected = true;
	TRACE_AVRCP("AVCTP connected\n");

	/* Start receive thread */
	fRecvThread = spawn_thread(_RecvThreadEntry, "avrcp_recv",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fRecvThread < 0) {
		TRACE_AVRCP("spawn_thread() failed\n");
		Disconnect();
		return B_ERROR;
	}
	resume_thread(fRecvThread);

	return B_OK;
}


void
AvrcpTarget::Disconnect()
{
	fConnected = false;

	if (fSocket >= 0) {
		shutdown(fSocket, SHUT_RDWR);
		close(fSocket);
		fSocket = -1;
	}

	if (fRecvThread >= 0) {
		status_t exitVal;
		wait_for_thread(fRecvThread, &exitVal);
		fRecvThread = -1;
	}
}


bool
AvrcpTarget::IsConnected() const
{
	return fConnected;
}


void
AvrcpTarget::SetButtonCallback(avrcp_button_callback callback, void* cookie)
{
	fButtonCallback = callback;
	fButtonCookie = cookie;
}


void
AvrcpTarget::SetVolumeCallback(avrcp_volume_callback callback, void* cookie)
{
	fVolumeCallback = callback;
	fVolumeCookie = cookie;
}


status_t
AvrcpTarget::NotifyVolumeChange(uint8 volume)
{
	if (!fConnected || fSocket < 0)
		return B_NO_INIT;

	/* Build AVCTP + AV/C vendor dependent notification (CHANGED) */
	uint8 pkt[AVCTP_HEADER_SIZE + 3 + 3 + 7 + 1];
	/*     AVCTP(3) + AVC header(3) + company(3) + PDU header(4) +
	 *     param length(2) + volume(1) */
	size_t len = 0;

	/* AVCTP header: txLabel=1, pktType=single, C/R=1 (response), IPID=0 */
	pkt[0] = (1 << 4) | (AVCTP_PKT_SINGLE << 2) | (1 << 1) | 0;
	pkt[1] = AVCTP_PID_AVRCP_HI;
	pkt[2] = AVCTP_PID_AVRCP_LO;
	len = 3;

	/* AV/C frame: CHANGED response */
	pkt[len++] = AVC_RESPONSE_CHANGED;
	pkt[len++] = AVC_SUBUNIT_PANEL;
	pkt[len++] = AVC_OP_VENDOR_DEPENDENT;

	/* Company ID (BT SIG) */
	pkt[len++] = BT_SIG_COMPANY_ID_0;
	pkt[len++] = BT_SIG_COMPANY_ID_1;
	pkt[len++] = BT_SIG_COMPANY_ID_2;

	/* PDU ID + packet type + param length + event + volume */
	pkt[len++] = AVRCP_PDU_REGISTER_NOTIFICATION;
	pkt[len++] = 0x00;  /* packet type: single */
	pkt[len++] = 0x00;  /* param length high */
	pkt[len++] = 0x02;  /* param length low */
	pkt[len++] = AVRCP_EVENT_VOLUME_CHANGED;
	pkt[len++] = volume & 0x7F;

	ssize_t sent = send(fSocket, pkt, len, 0);
	if (sent < 0) {
		TRACE_AVRCP("NotifyVolumeChange send failed: %s\n",
			strerror(errno));
		return B_ERROR;
	}

	return B_OK;
}


/* =========================================================================
 * Private: ACL connection (same pattern as A2dpSink)
 * ========================================================================= */

bool
AvrcpTarget::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_AVRCP("Cannot reach BluetoothServer\n");
		return false;
	}

	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply,
			B_INFINITE_TIMEOUT, 5000000LL) != B_OK) {
		TRACE_AVRCP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_AVRCP("No local Bluetooth device found\n");
		return false;
	}

	BluetoothCommand<typed_command(hci_cp_create_conn)>
		createConn(OGF_LINK_CONTROL, OCF_CREATE_CONN);

	bdaddrUtils::Copy(createConn->bdaddr, remote);
	createConn->pkt_type = 0xCC18;
	createConn->pscan_rep_mode = 0x01;
	createConn->pscan_mode = 0x00;
	createConn->clock_offset = 0x0000;
	createConn->role_switch = 0x01;

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		createConn.Data(), createConn.Size());

	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN));
	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_REQUEST);
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_RESPONSE);
	request.AddInt16("eventExpected",
		HCI_EVENT_USER_CONFIRMATION_REQUEST);
	request.AddInt16("eventExpected",
		HCI_EVENT_SIMPLE_PAIRING_COMPLETE);
	request.AddInt16("eventExpected", HCI_EVENT_CONN_COMPLETE);

	TRACE_AVRCP("Sending HCI Create Connection (timeout 30s)...\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_AVRCP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_AVRCP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus == 0x0B) {
		TRACE_AVRCP("ACL already exists\n");
		return true;
	} else if (btStatus != BT_OK) {
		TRACE_AVRCP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_AVRCP("ACL connected (handle=0x%04X)\n",
		(unsigned)(uint16)handle);

	if (handle < 0)
		return true;

	/* Authenticate */
	{
		BluetoothCommand<typed_command(hci_cp_auth_requested)>
			authCmd(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED);
		authCmd->handle = (uint16)handle;

		BMessage authReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
		BMessage authReply;
		authReq.AddInt32("hci_id", hid);
		authReq.AddData("raw command", B_ANY_TYPE,
			authCmd.Data(), authCmd.Size());
		authReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		authReq.AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED));
		authReq.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
		authReq.AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

		result = messenger.SendMessage(&authReq, &authReply,
			B_INFINITE_TIMEOUT, 10000000LL);
		int8 authStatus = BT_ERROR;
		if (result == B_OK)
			authReply.FindInt8("status", &authStatus);
		if (authStatus != BT_OK) {
			TRACE_AVRCP("Authentication failed (0x%02X)\n",
				(unsigned)(uint8)authStatus);
			return false;
		}
	}

	/* Encrypt */
	{
		BluetoothCommand<typed_command(hci_cp_set_conn_encrypt)>
			encCmd(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT);
		encCmd->handle = (uint16)handle;
		encCmd->encrypt = 1;

		BMessage encReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
		BMessage encReply;
		encReq.AddInt32("hci_id", hid);
		encReq.AddData("raw command", B_ANY_TYPE,
			encCmd.Data(), encCmd.Size());
		encReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		encReq.AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT));
		encReq.AddInt16("eventExpected", HCI_EVENT_ENCRYPT_CHANGE);

		result = messenger.SendMessage(&encReq, &encReply,
			B_INFINITE_TIMEOUT, 10000000LL);
		int8 encStatus = BT_ERROR;
		if (result == B_OK)
			encReply.FindInt8("status", &encStatus);
		if (encStatus != BT_OK) {
			TRACE_AVRCP("Encryption failed (0x%02X)\n",
				(unsigned)(uint8)encStatus);
			return false;
		}
	}

	return true;
}


/* =========================================================================
 * Private: Receive thread
 * ========================================================================= */

status_t
AvrcpTarget::_RecvThreadEntry(void* arg)
{
	((AvrcpTarget*)arg)->_RecvLoop();
	return B_OK;
}


void
AvrcpTarget::_RecvLoop()
{
	uint8 buf[512];

	TRACE_AVRCP("Receive loop started\n");

	while (fConnected) {
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		setsockopt(fSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		ssize_t received = recv(fSocket, buf, sizeof(buf), 0);
		if (received < 0) {
			if (errno == ETIMEDOUT || errno == EWOULDBLOCK)
				continue;
			if (fConnected) {
				TRACE_AVRCP("recv() error: %s\n", strerror(errno));
				fConnected = false;
			}
			break;
		}
		if (received == 0) {
			TRACE_AVRCP("Connection closed by remote\n");
			fConnected = false;
			break;
		}

		/* Parse AVCTP header (3 bytes minimum) */
		if (received < AVCTP_HEADER_SIZE)
			continue;

		uint8 txLabel = (buf[0] >> 4) & 0x0F;
		uint8 crBit = (buf[0] >> 1) & 0x01;  /* 0=command, 1=response */
		uint16 pid = ((uint16)buf[1] << 8) | buf[2];

		if (pid != 0x110E) {
			TRACE_AVRCP("Unknown PID 0x%04X, ignoring\n", pid);
			continue;
		}

		/* We only handle commands (C/R=0) */
		if (crBit != 0) {
			TRACE_AVRCP("Received response (txLabel=%u), ignoring\n",
				txLabel);
			continue;
		}

		/* Parse AV/C frame (at least 3 bytes: ctype, subunit, opcode) */
		if (received < AVCTP_HEADER_SIZE + 3)
			continue;

		uint8 ctype = buf[3];
		uint8 opcode = buf[5];
		const uint8* operand = buf + 6;
		size_t operandLen = received - 6;

		switch (opcode) {
			case AVC_OP_PASS_THROUGH:
				_HandlePassThrough(txLabel, operand, operandLen);
				break;

			case AVC_OP_VENDOR_DEPENDENT:
				_HandleVendorDependent(txLabel, ctype, operand,
					operandLen);
				break;

			default:
				TRACE_AVRCP("Unknown AV/C opcode 0x%02X\n", opcode);
				/* Send REJECTED response */
				_SendResponse(txLabel, AVC_RESPONSE_REJECTED,
					AVC_SUBUNIT_PANEL, opcode, NULL, 0);
				break;
		}
	}

	TRACE_AVRCP("Receive loop exited\n");
}


void
AvrcpTarget::_HandlePassThrough(uint8 txLabel, const uint8* operand,
	size_t len)
{
	if (len < 2)
		return;

	bool pressed = !(operand[0] & 0x80);
	uint8 opId = operand[0] & 0x7F;

	TRACE_AVRCP("Pass-through: op=0x%02X %s\n", opId,
		pressed ? "pressed" : "released");

	/* Send ACCEPTED response */
	uint8 respOperand[2];
	respOperand[0] = operand[0];  /* same state_flag + op_id */
	respOperand[1] = operand[1];  /* operation_data_length (usually 0) */

	_SendResponse(txLabel, AVC_RESPONSE_ACCEPTED, AVC_SUBUNIT_PANEL,
		AVC_OP_PASS_THROUGH, respOperand, 2);

	/* Notify callback */
	if (fButtonCallback != NULL)
		fButtonCallback((avrcp_op_id)opId, pressed, fButtonCookie);
}


void
AvrcpTarget::_HandleVendorDependent(uint8 txLabel, uint8 ctype,
	const uint8* operand, size_t len)
{
	/* operand starts with 3-byte Company ID + PDU ID + pkt type +
	 * 2-byte param length + params */
	if (len < 7)
		return;

	uint8 pduId = operand[3];
	uint16 paramLen = ((uint16)operand[5] << 8) | operand[6];

	TRACE_AVRCP("Vendor dep: ctype=0x%02X pdu=0x%02X paramLen=%u\n",
		ctype, pduId, paramLen);

	switch (pduId) {
		case AVRCP_PDU_SET_ABSOLUTE_VOLUME:
		{
			if (paramLen < 1 || len < 8)
				break;

			uint8 volume = operand[7] & 0x7F;
			TRACE_AVRCP("SetAbsoluteVolume: %u (%.0f%%)\n",
				volume, volume * 100.0 / 127.0);

			/* Send ACCEPTED with the volume back */
			uint8 resp[8];
			resp[0] = BT_SIG_COMPANY_ID_0;
			resp[1] = BT_SIG_COMPANY_ID_1;
			resp[2] = BT_SIG_COMPANY_ID_2;
			resp[3] = AVRCP_PDU_SET_ABSOLUTE_VOLUME;
			resp[4] = 0x00;  /* single */
			resp[5] = 0x00;  /* param len high */
			resp[6] = 0x01;  /* param len low */
			resp[7] = volume & 0x7F;

			_SendResponse(txLabel, AVC_RESPONSE_ACCEPTED,
				AVC_SUBUNIT_PANEL, AVC_OP_VENDOR_DEPENDENT,
				resp, 8);

			if (fVolumeCallback != NULL)
				fVolumeCallback(volume, fVolumeCookie);
			break;
		}

		case AVRCP_PDU_REGISTER_NOTIFICATION:
		{
			if (paramLen < 1 || len < 8)
				break;

			uint8 eventId = operand[7];
			TRACE_AVRCP("RegisterNotification: event=0x%02X\n", eventId);

			if (eventId == AVRCP_EVENT_VOLUME_CHANGED) {
				/* Send INTERIM response with current volume (0) */
				uint8 resp[9];
				resp[0] = BT_SIG_COMPANY_ID_0;
				resp[1] = BT_SIG_COMPANY_ID_1;
				resp[2] = BT_SIG_COMPANY_ID_2;
				resp[3] = AVRCP_PDU_REGISTER_NOTIFICATION;
				resp[4] = 0x00;  /* single */
				resp[5] = 0x00;  /* param len high */
				resp[6] = 0x02;  /* param len low */
				resp[7] = AVRCP_EVENT_VOLUME_CHANGED;
				resp[8] = 0x40;  /* ~50% volume */

				_SendResponse(txLabel, AVC_RESPONSE_INTERIM,
					AVC_SUBUNIT_PANEL, AVC_OP_VENDOR_DEPENDENT,
					resp, 9);
			} else {
				/* Reject unknown events */
				uint8 resp[8];
				resp[0] = BT_SIG_COMPANY_ID_0;
				resp[1] = BT_SIG_COMPANY_ID_1;
				resp[2] = BT_SIG_COMPANY_ID_2;
				resp[3] = AVRCP_PDU_REGISTER_NOTIFICATION;
				resp[4] = 0x00;
				resp[5] = 0x00;
				resp[6] = 0x01;
				resp[7] = 0x00;  /* error: invalid parameter */

				_SendResponse(txLabel, AVC_RESPONSE_REJECTED,
					AVC_SUBUNIT_PANEL, AVC_OP_VENDOR_DEPENDENT,
					resp, 8);
			}
			break;
		}

		default:
			TRACE_AVRCP("Unknown vendor PDU 0x%02X\n", pduId);
			{
				uint8 resp[7];
				resp[0] = BT_SIG_COMPANY_ID_0;
				resp[1] = BT_SIG_COMPANY_ID_1;
				resp[2] = BT_SIG_COMPANY_ID_2;
				resp[3] = pduId;
				resp[4] = 0x00;
				resp[5] = 0x00;
				resp[6] = 0x00;

				_SendResponse(txLabel, AVC_RESPONSE_REJECTED,
					AVC_SUBUNIT_PANEL, AVC_OP_VENDOR_DEPENDENT,
					resp, 7);
			}
			break;
	}
}


ssize_t
AvrcpTarget::_SendResponse(uint8 txLabel, uint8 responseCode,
	uint8 subunit, uint8 opcode, const uint8* operand, size_t operandLen)
{
	if (fSocket < 0)
		return B_NO_INIT;

	size_t pktLen = AVCTP_HEADER_SIZE + 3 + operandLen;
	uint8 pkt[512];
	if (pktLen > sizeof(pkt))
		return B_NO_MEMORY;

	/* AVCTP header: txLabel, single packet, C/R=1 (response), IPID=0 */
	pkt[0] = (txLabel << 4) | (AVCTP_PKT_SINGLE << 2) | (1 << 1) | 0;
	pkt[1] = AVCTP_PID_AVRCP_HI;
	pkt[2] = AVCTP_PID_AVRCP_LO;

	/* AV/C frame */
	pkt[3] = responseCode;
	pkt[4] = subunit;
	pkt[5] = opcode;

	if (operand != NULL && operandLen > 0)
		memcpy(pkt + 6, operand, operandLen);

	ssize_t sent = send(fSocket, pkt, pktLen, 0);
	if (sent < 0)
		TRACE_AVRCP("send() failed: %s\n", strerror(errno));

	return sent;
}
