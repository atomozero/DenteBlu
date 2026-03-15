/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpSink — Advanced Audio Distribution Profile Sink implementation.
 */

#include <bluetooth/A2dpSink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/avdtp.h>
#include <bluetooth/sdp.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>

#include "AvdtpSession.h"
#include "SbcDecoder.h"


#define TRACE_A2DP(fmt, ...) \
	fprintf(stderr, "A2DP: " fmt, ##__VA_ARGS__)


using Bluetooth::A2dpSink;


/* =========================================================================
 * Constructor / Destructor
 * ========================================================================= */

A2dpSink::A2dpSink()
	:
	fAvdtp(NULL),
	fDecoder(NULL),
	fRemoteSeid(0),
	fLocalSeid(1),
	fConnected(false),
	fStreaming(false),
	fReceiveThread(-1),
	fAudioCallback(NULL),
	fAudioCookie(NULL)
{
	memset(&fRemoteAddr, 0, sizeof(fRemoteAddr));
}


A2dpSink::~A2dpSink()
{
	Disconnect();
}


/* =========================================================================
 * Connection
 * ========================================================================= */

status_t
A2dpSink::Connect(const bdaddr_t& address)
{
	if (fConnected)
		return B_BUSY;

	memcpy(&fRemoteAddr, &address, sizeof(bdaddr_t));

	/* Step 1: Ensure ACL link to remote */
	TRACE_A2DP("Ensuring ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_A2DP("ACL connection failed\n");
		return B_ERROR;
	}

	/* Step 2: AVDTP signaling connection */
	fAvdtp = new AvdtpSession();
	status_t err = fAvdtp->Connect(address);
	if (err != B_OK) {
		TRACE_A2DP("AVDTP signaling connect failed: %s\n",
			strerror(err));
		delete fAvdtp;
		fAvdtp = NULL;
		return err;
	}

	/* Step 3: Discover remote endpoints */
	avdtp_sep_info seps[AVDTP_MAX_SEPS];
	uint8 sepCount = 0;
	err = fAvdtp->DiscoverEndpoints(seps, &sepCount, AVDTP_MAX_SEPS);
	if (err != B_OK) {
		TRACE_A2DP("Discover failed\n");
		Disconnect();
		return err;
	}

	/* Find an Audio Source endpoint that's not in use */
	fRemoteSeid = 0;
	for (uint8 i = 0; i < sepCount; i++) {
		if (AVDTP_SEP_MEDIA_TYPE(&seps[i]) == AVDTP_MEDIA_TYPE_AUDIO
				&& AVDTP_SEP_TSEP(&seps[i]) == AVDTP_SEP_SOURCE
				&& !AVDTP_SEP_IN_USE(&seps[i])) {
			fRemoteSeid = AVDTP_SEP_SEID(&seps[i]);
			TRACE_A2DP("Found audio source: SEID=%u\n", fRemoteSeid);
			break;
		}
	}

	if (fRemoteSeid == 0) {
		TRACE_A2DP("No available audio source endpoint found\n");
		Disconnect();
		return B_ENTRY_NOT_FOUND;
	}

	/* Step 4: Get capabilities of the source endpoint */
	AvdtpCapability caps[AVDTP_MAX_CAPABILITIES];
	uint8 capCount = 0;
	err = fAvdtp->GetCapabilities(fRemoteSeid, caps, &capCount,
		AVDTP_MAX_CAPABILITIES);
	if (err != B_OK) {
		TRACE_A2DP("GetCapabilities failed\n");
		Disconnect();
		return err;
	}

	/* Step 5: Negotiate codec configuration */
	err = _NegotiateCodec();
	if (err != B_OK) {
		TRACE_A2DP("Codec negotiation failed\n");
		Disconnect();
		return err;
	}

	/* Step 6: Open stream */
	err = fAvdtp->Open(fRemoteSeid);
	if (err != B_OK) {
		TRACE_A2DP("Stream open failed\n");
		Disconnect();
		return err;
	}

	/* Step 7: Open media transport channel */
	err = fAvdtp->OpenMediaChannel(address);
	if (err != B_OK) {
		TRACE_A2DP("Media channel open failed\n");
		Disconnect();
		return err;
	}

	/* Create decoder */
	fDecoder = new SbcDecoder();

	fConnected = true;
	TRACE_A2DP("A2DP Sink connected and stream opened\n");
	return B_OK;
}


void
A2dpSink::Disconnect()
{
	StopStream();

	if (fAvdtp != NULL) {
		if (fRemoteSeid != 0)
			fAvdtp->Close(fRemoteSeid);
		fAvdtp->Disconnect();
		delete fAvdtp;
		fAvdtp = NULL;
	}

	delete fDecoder;
	fDecoder = NULL;

	fConnected = false;
	fRemoteSeid = 0;
}


bool
A2dpSink::IsConnected() const
{
	return fConnected;
}


/* =========================================================================
 * Streaming
 * ========================================================================= */

status_t
A2dpSink::StartStream()
{
	if (!fConnected || fAvdtp == NULL)
		return B_NO_INIT;

	if (fStreaming)
		return B_OK;

	/* Send AVDTP Start */
	status_t err = fAvdtp->Start(fRemoteSeid);
	if (err != B_OK)
		return err;

	/* Start receive thread */
	fStreaming = true;
	fReceiveThread = spawn_thread(_ReceiveThreadEntry,
		"a2dp_recv", B_URGENT_DISPLAY_PRIORITY, this);
	if (fReceiveThread < 0) {
		fStreaming = false;
		return B_ERROR;
	}
	resume_thread(fReceiveThread);

	TRACE_A2DP("Streaming started\n");
	return B_OK;
}


status_t
A2dpSink::StopStream()
{
	if (!fStreaming)
		return B_OK;

	fStreaming = false;

	/* Wait for receive thread to exit */
	if (fReceiveThread >= 0) {
		status_t exitVal;
		wait_for_thread(fReceiveThread, &exitVal);
		fReceiveThread = -1;
	}

	/* Send AVDTP Suspend */
	if (fAvdtp != NULL && fRemoteSeid != 0)
		fAvdtp->Suspend(fRemoteSeid);

	TRACE_A2DP("Streaming stopped\n");
	return B_OK;
}


void
A2dpSink::SetAudioCallback(audio_data_callback callback, void* cookie)
{
	fAudioCallback = callback;
	fAudioCookie = cookie;
}


uint32
A2dpSink::SampleRate() const
{
	return (fDecoder != NULL) ? fDecoder->SampleRate() : 0;
}


uint8
A2dpSink::Channels() const
{
	return (fDecoder != NULL) ? fDecoder->Channels() : 0;
}


/* =========================================================================
 * Private: ACL connection
 * ========================================================================= */

bool
A2dpSink::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_A2DP("Cannot reach BluetoothServer\n");
		return false;
	}

	/* Acquire local HCI device */
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply,
			B_INFINITE_TIMEOUT, 5000000LL) != B_OK) {
		TRACE_A2DP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_A2DP("No local Bluetooth device found\n");
		return false;
	}

	/* Build HCI Create Connection */
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

	TRACE_A2DP("Sending HCI Create Connection (timeout 30s)...\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_A2DP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_A2DP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus == 0x0B) {
		TRACE_A2DP("ACL already exists\n");
		return true;
	} else if (btStatus != BT_OK) {
		TRACE_A2DP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_A2DP("ACL connected (handle=0x%04X)\n",
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
			TRACE_A2DP("Authentication failed (0x%02X)\n",
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
			TRACE_A2DP("Encryption failed (0x%02X)\n",
				(unsigned)(uint8)encStatus);
			return false;
		}
	}

	return true;
}


/* =========================================================================
 * Private: Codec negotiation
 * ========================================================================= */

status_t
A2dpSink::_NegotiateCodec()
{
	/* Configure SBC with standard parameters:
	 * 44.1kHz, Joint Stereo, 16 blocks, 8 subbands,
	 * Loudness allocation, bitpool 2-53 */

	AvdtpCapability caps[2];

	/* Media Transport */
	caps[0].category = AVDTP_MEDIA_TRANSPORT;
	caps[0].dataLen = 0;

	/* Media Codec: SBC */
	caps[1].category = AVDTP_MEDIA_CODEC;
	caps[1].dataLen = 6;
	caps[1].data[0] = AVDTP_MEDIA_TYPE_AUDIO << 4;
	caps[1].data[1] = AVDTP_CODEC_SBC;
	/* SBC codec info element */
	caps[1].data[2] = SBC_FREQ_44100 | SBC_CHANNEL_JOINT_STEREO;
	caps[1].data[3] = SBC_BLOCK_16 | SBC_SUBBANDS_8 | SBC_ALLOC_LOUDNESS;
	caps[1].data[4] = 2;   /* min bitpool */
	caps[1].data[5] = 53;  /* max bitpool */

	return fAvdtp->SetConfiguration(fRemoteSeid, fLocalSeid, caps, 2);
}


/* =========================================================================
 * Private: Receive thread
 * ========================================================================= */

status_t
A2dpSink::_ReceiveThreadEntry(void* arg)
{
	((A2dpSink*)arg)->_ReceiveLoop();
	return B_OK;
}


void
A2dpSink::_ReceiveLoop()
{
	int mediaSocket = fAvdtp->MediaSocket();
	if (mediaSocket < 0) {
		TRACE_A2DP("No media socket\n");
		return;
	}

	uint8 buf[AVDTP_MEDIA_BUF_SIZE];
	int16 pcmBuf[2048];

	TRACE_A2DP("Receive loop started on media socket %d\n",
		mediaSocket);

	while (fStreaming) {
		/* Set short timeout so we can check fStreaming flag */
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		setsockopt(mediaSocket, SOL_SOCKET, SO_RCVTIMEO, &tv,
			sizeof(tv));

		ssize_t received = recv(mediaSocket, buf, sizeof(buf), 0);
		if (received < 0) {
			if (errno == ETIMEDOUT || errno == EWOULDBLOCK)
				continue;
			TRACE_A2DP("recv error: %s\n", strerror(errno));
			break;
		}

		if (received == 0) {
			TRACE_A2DP("Media channel closed\n");
			break;
		}

		/* Skip RTP header (12 bytes) + media payload header (1 byte) */
		if (received < RTP_HEADER_SIZE + 1)
			continue;

		const uint8* payload = buf + RTP_HEADER_SIZE;
		size_t payloadLen = received - RTP_HEADER_SIZE;

		uint8 numFrames = A2DP_SBC_NUM_FRAMES(payload[0]);
		payload++;
		payloadLen--;

		/* Decode each SBC frame in this packet */
		for (uint8 f = 0; f < numFrames && payloadLen > 0; f++) {
			ssize_t samples = fDecoder->DecodeFrame(payload,
				payloadLen, pcmBuf, sizeof(pcmBuf) / sizeof(int16));

			if (samples < 0) {
				TRACE_A2DP("Decode error on frame %u\n", f);
				break;
			}

			/* Advance past this SBC frame */
			uint16 frameLen = fDecoder->FrameLength();
			if (frameLen > payloadLen)
				break;
			payload += frameLen;
			payloadLen -= frameLen;

			/* Deliver decoded audio */
			if (fAudioCallback != NULL) {
				fAudioCallback(pcmBuf, samples,
					fDecoder->SampleRate(),
					fDecoder->Channels(),
					fAudioCookie);
			}
		}
	}

	TRACE_A2DP("Receive loop exited\n");
}
