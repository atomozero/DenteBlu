/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpSource — Advanced Audio Distribution Profile Source implementation.
 * Sends SBC-encoded audio to remote A2DP Sink (headphones).
 */

#include <bluetooth/A2dpSource.h>

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
#include "SbcEncoder.h"


#define TRACE_A2DP(fmt, ...) \
	fprintf(stderr, "A2DP-SRC: " fmt, ##__VA_ARGS__)


using Bluetooth::A2dpSource;


/* =========================================================================
 * Constructor / Destructor
 * ========================================================================= */

A2dpSource::A2dpSource()
	:
	fAvdtp(NULL),
	fEncoder(NULL),
	fRemoteSeid(0),
	fLocalSeid(2),
	fConnected(false),
	fStreaming(false),
	fRtpSeqNumber(0),
	fRtpTimestamp(0),
	fSsrc(0x12345678),
	fStreamStartTime(0),
	fTotalSamplesSent(0)
{
	memset(&fRemoteAddr, 0, sizeof(fRemoteAddr));
	memset(fRtpBuf, 0, sizeof(fRtpBuf));
}


A2dpSource::~A2dpSource()
{
	Disconnect();
}


/* =========================================================================
 * Connection
 * ========================================================================= */

status_t
A2dpSource::Connect(const bdaddr_t& address)
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

	/* Find an Audio Sink endpoint that's not in use */
	fRemoteSeid = 0;
	for (uint8 i = 0; i < sepCount; i++) {
		if (AVDTP_SEP_MEDIA_TYPE(&seps[i]) == AVDTP_MEDIA_TYPE_AUDIO
				&& AVDTP_SEP_TSEP(&seps[i]) == AVDTP_SEP_SINK
				&& !AVDTP_SEP_IN_USE(&seps[i])) {
			fRemoteSeid = AVDTP_SEP_SEID(&seps[i]);
			TRACE_A2DP("Found audio sink: SEID=%u\n", fRemoteSeid);
			break;
		}
	}

	if (fRemoteSeid == 0) {
		TRACE_A2DP("No available audio sink endpoint found\n");
		Disconnect();
		return B_ENTRY_NOT_FOUND;
	}

	/* Step 4: Get capabilities of the sink endpoint */
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

	/* Create encoder: 44.1kHz, Joint Stereo, 16 blocks, 8 subbands,
	 * Loudness, bitpool 53 */
	fEncoder = new SbcEncoder();
	err = fEncoder->Configure(44100, 2, 16, 8, 3, 0, 53);
	if (err != B_OK) {
		TRACE_A2DP("Encoder configure failed\n");
		Disconnect();
		return err;
	}

	fConnected = true;
	TRACE_A2DP("A2DP Source connected (frame=%u bytes, "
		"samples/frame=%u)\n", fEncoder->FrameLength(),
		fEncoder->SamplesPerFrame());
	return B_OK;
}


void
A2dpSource::Disconnect()
{
	StopStream();

	if (fAvdtp != NULL) {
		if (fRemoteSeid != 0)
			fAvdtp->Close(fRemoteSeid);
		fAvdtp->Disconnect();
		delete fAvdtp;
		fAvdtp = NULL;
	}

	delete fEncoder;
	fEncoder = NULL;

	fConnected = false;
	fRemoteSeid = 0;
}


bool
A2dpSource::IsConnected() const
{
	return fConnected;
}


/* =========================================================================
 * Streaming
 * ========================================================================= */

status_t
A2dpSource::StartStream()
{
	if (!fConnected || fAvdtp == NULL)
		return B_NO_INIT;

	if (fStreaming)
		return B_OK;

	/* Send AVDTP Start */
	status_t err = fAvdtp->Start(fRemoteSeid);
	if (err != B_OK)
		return err;

	fStreaming = true;
	fRtpSeqNumber = 0;
	fRtpTimestamp = 0;
	fStreamStartTime = system_time();
	fTotalSamplesSent = 0;

	TRACE_A2DP("Streaming started\n");
	return B_OK;
}


status_t
A2dpSource::StopStream()
{
	if (!fStreaming)
		return B_OK;

	fStreaming = false;

	/* Send AVDTP Suspend */
	if (fAvdtp != NULL && fRemoteSeid != 0)
		fAvdtp->Suspend(fRemoteSeid);

	TRACE_A2DP("Streaming stopped\n");
	return B_OK;
}


status_t
A2dpSource::SendAudio(const int16* pcm, size_t sampleCount)
{
	if (!fStreaming || fEncoder == NULL || fAvdtp == NULL)
		return B_NO_INIT;

	int mediaSocket = fAvdtp->MediaSocket();
	if (mediaSocket < 0)
		return B_NO_INIT;

	uint16 frameLen = fEncoder->FrameLength();
	uint16 samplesPerFrame = fEncoder->SamplesPerFrame();
	uint8 channels = fEncoder->Channels();

	/* Calculate max SBC frames per RTP packet.
	 * L2CAP default MTU is 672; media MTU may differ.
	 * RTP header (12) + media header (1) + N * frameLen <= MTU.
	 * Use 672 as conservative MTU estimate. */
	uint16 mtu = 672;
	uint8 maxFramesPerPacket = (mtu - 13) / frameLen;
	if (maxFramesPerPacket > 15)
		maxFramesPerPacket = 15;
	if (maxFramesPerPacket == 0)
		maxFramesPerPacket = 1;

	/* Encode and send PCM in frame-sized chunks */
	size_t samplesConsumed = 0;

	/* Temporary buffer for accumulating SBC frames */
	uint8 sbcBuf[4096];
	uint16 sbcBufLen = 0;
	uint8 frameCount = 0;

	while (samplesConsumed + samplesPerFrame <= sampleCount) {
		/* Encode one frame */
		const int16* frameInput = pcm
			+ samplesConsumed * channels;

		ssize_t encoded = fEncoder->EncodeFrame(frameInput,
			sbcBuf + sbcBufLen, sizeof(sbcBuf) - sbcBufLen);
		if (encoded < 0) {
			TRACE_A2DP("Encode error\n");
			return B_ERROR;
		}

		sbcBufLen += encoded;
		frameCount++;
		samplesConsumed += samplesPerFrame;

		/* If we've accumulated enough frames, send a packet */
		if (frameCount >= maxFramesPerPacket
				|| samplesConsumed + samplesPerFrame > sampleCount) {
			status_t err = _BuildRtpPacket(sbcBuf, frameCount,
				sbcBufLen);
			if (err != B_OK)
				return err;

			/* Send the RTP packet */
			size_t rtpLen = RTP_HEADER_SIZE + 1 + sbcBufLen;
			ssize_t sent = send(mediaSocket, fRtpBuf, rtpLen, 0);
			if (sent < 0) {
				TRACE_A2DP("send error: %s\n", strerror(errno));
				return B_ERROR;
			}

			fRtpTimestamp += frameCount * samplesPerFrame;
			fTotalSamplesSent += (uint64)frameCount * samplesPerFrame;

			sbcBufLen = 0;
			frameCount = 0;
		}
	}

	/* Pacing: sleep until we've reached the right wall-clock time */
	if (fTotalSamplesSent > 0) {
		bigtime_t targetTime = fStreamStartTime
			+ (fTotalSamplesSent * 1000000LL) / SampleRate();
		bigtime_t now = system_time();
		if (targetTime > now)
			snooze_until(targetTime, B_SYSTEM_TIMEBASE);
	}

	return B_OK;
}


uint32
A2dpSource::SampleRate() const
{
	return (fEncoder != NULL) ? fEncoder->SampleRate() : 44100;
}


uint8
A2dpSource::Channels() const
{
	return (fEncoder != NULL) ? fEncoder->Channels() : 2;
}


uint16
A2dpSource::FrameLength() const
{
	return (fEncoder != NULL) ? fEncoder->FrameLength() : 0;
}


uint16
A2dpSource::SamplesPerFrame() const
{
	return (fEncoder != NULL) ? fEncoder->SamplesPerFrame() : 0;
}


/* =========================================================================
 * Private: ACL connection
 * ========================================================================= */

bool
A2dpSource::_EnsureAclConnection(const bdaddr_t& remote)
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
A2dpSource::_NegotiateCodec()
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
 * Private: RTP packet construction
 * ========================================================================= */

status_t
A2dpSource::_BuildRtpPacket(const uint8* sbcFrames, uint8 frameCount,
	uint16 totalSbcLen)
{
	size_t totalLen = RTP_HEADER_SIZE + 1 + totalSbcLen;
	if (totalLen > sizeof(fRtpBuf))
		return B_BUFFER_OVERFLOW;

	/* RTP header (12 bytes) */
	fRtpBuf[0] = 0x80;		/* V=2, P=0, X=0, CC=0 */
	fRtpBuf[1] = 0x60;		/* M=0, PT=96 */
	fRtpBuf[2] = (uint8)(fRtpSeqNumber >> 8);
	fRtpBuf[3] = (uint8)(fRtpSeqNumber & 0xFF);
	fRtpBuf[4] = (uint8)(fRtpTimestamp >> 24);
	fRtpBuf[5] = (uint8)(fRtpTimestamp >> 16);
	fRtpBuf[6] = (uint8)(fRtpTimestamp >> 8);
	fRtpBuf[7] = (uint8)(fRtpTimestamp & 0xFF);
	fRtpBuf[8] = (uint8)(fSsrc >> 24);
	fRtpBuf[9] = (uint8)(fSsrc >> 16);
	fRtpBuf[10] = (uint8)(fSsrc >> 8);
	fRtpBuf[11] = (uint8)(fSsrc & 0xFF);

	/* A2DP SBC media payload header (1 byte) */
	fRtpBuf[12] = (frameCount & 0x0F) << 4;

	/* SBC frame data */
	memcpy(fRtpBuf + 13, sbcFrames, totalSbcLen);

	fRtpSeqNumber++;

	return B_OK;
}
