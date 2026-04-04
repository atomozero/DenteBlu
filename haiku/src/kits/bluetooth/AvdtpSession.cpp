/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AvdtpSession — AVDTP signaling and media transport over L2CAP.
 */

#include "AvdtpSession.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <bluetooth/L2CAP/btL2CAP.h>
#include <l2cap.h>


#define TRACE_AVDTP(fmt, ...) \
	fprintf(stderr, "AVDTP: " fmt, ##__VA_ARGS__)


AvdtpSession::AvdtpSession()
	:
	fSignalingSocket(-1),
	fMediaSocket(-1),
	fTxLabel(0)
{
	memset(&fRemoteAddr, 0, sizeof(fRemoteAddr));
}


AvdtpSession::~AvdtpSession()
{
	Disconnect();
}


status_t
AvdtpSession::Connect(const bdaddr_t& remote)
{
	if (fSignalingSocket >= 0)
		return B_BUSY;

	memcpy(&fRemoteAddr, &remote, sizeof(bdaddr_t));

	/* Create L2CAP socket for signaling */
	fSignalingSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (fSignalingSocket < 0) {
		TRACE_AVDTP("socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	/* Set 10-second receive timeout */
	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	setsockopt(fSignalingSocket, SOL_SOCKET, SO_RCVTIMEO, &tv,
		sizeof(tv));

	/* Connect to remote AVDTP PSM (0x0019) */
	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = PF_BLUETOOTH;
	addr.l2cap_psm = L2CAP_PSM_AVDTP;
	memcpy(&addr.l2cap_bdaddr, &remote, sizeof(bdaddr_t));

	TRACE_AVDTP("Connecting L2CAP to PSM 0x0019 (AVDTP)...\n");
	if (connect(fSignalingSocket, (struct sockaddr*)&addr,
			sizeof(addr)) < 0) {
		TRACE_AVDTP("connect() failed: %s\n", strerror(errno));
		close(fSignalingSocket);
		fSignalingSocket = -1;
		return B_ERROR;
	}
	TRACE_AVDTP("Signaling channel connected.\n");

	return B_OK;
}


void
AvdtpSession::Disconnect()
{
	if (fMediaSocket >= 0) {
		close(fMediaSocket);
		fMediaSocket = -1;
	}

	if (fSignalingSocket >= 0) {
		close(fSignalingSocket);
		fSignalingSocket = -1;
	}
}


status_t
AvdtpSession::DiscoverEndpoints(avdtp_sep_info* seps, uint8* count,
	uint8 maxSeps)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	/* Send AVDTP_DISCOVER (no parameters) */
	status_t err = _SendSignal(AVDTP_DISCOVER, NULL, 0);
	if (err != B_OK)
		return err;

	/* Receive response */
	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType == AVDTP_MSG_TYPE_RESPONSE_REJECT) {
		TRACE_AVDTP("Discover rejected: error 0x%02x\n",
			received > AVDTP_HEADER_SIZE ? buf[AVDTP_HEADER_SIZE] : 0);
		return B_ERROR;
	}

	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT) {
		TRACE_AVDTP("Discover: unexpected message type %u\n", msgType);
		return B_ERROR;
	}

	/* Parse SEP info entries (2 bytes each) */
	size_t payloadLen = received - AVDTP_HEADER_SIZE;
	uint8 sepCount = payloadLen / sizeof(avdtp_sep_info);
	if (sepCount > maxSeps)
		sepCount = maxSeps;

	memcpy(seps, buf + AVDTP_HEADER_SIZE,
		sepCount * sizeof(avdtp_sep_info));
	*count = sepCount;

	TRACE_AVDTP("Discovered %u endpoints\n", sepCount);
	for (uint8 i = 0; i < sepCount; i++) {
		TRACE_AVDTP("  SEP[%u]: SEID=%u type=%s media=%u inUse=%u\n",
			i, AVDTP_SEP_SEID(&seps[i]),
			AVDTP_SEP_TSEP(&seps[i]) == AVDTP_SEP_SINK
				? "Sink" : "Source",
			AVDTP_SEP_MEDIA_TYPE(&seps[i]),
			AVDTP_SEP_IN_USE(&seps[i]));
	}

	return B_OK;
}


status_t
AvdtpSession::GetCapabilities(uint8 remoteSeid, AvdtpCapability* caps,
	uint8* count, uint8 maxCaps)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	/* Send GET_CAPABILITIES with ACP SEID */
	uint8 param = (remoteSeid << 2);
	status_t err = _SendSignal(AVDTP_GET_CAPABILITIES, &param, 1);
	if (err != B_OK)
		return err;

	/* Receive response */
	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType == AVDTP_MSG_TYPE_RESPONSE_REJECT) {
		TRACE_AVDTP("GetCapabilities rejected: error 0x%02x\n",
			received > AVDTP_HEADER_SIZE + 1
				? buf[AVDTP_HEADER_SIZE + 1] : 0);
		return B_ERROR;
	}

	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT)
		return B_ERROR;

	/* Parse capability entries: [category, length, data...] */
	const uint8* p = buf + AVDTP_HEADER_SIZE;
	size_t remaining = received - AVDTP_HEADER_SIZE;
	uint8 capCount = 0;

	while (remaining >= 2 && capCount < maxCaps) {
		uint8 category = p[0];
		uint8 dataLen = p[1];
		p += 2;
		remaining -= 2;

		if (dataLen > remaining)
			break;

		caps[capCount].category = category;
		caps[capCount].dataLen = (dataLen > 32) ? 32 : dataLen;
		if (dataLen > 0)
			memcpy(caps[capCount].data, p, caps[capCount].dataLen);

		TRACE_AVDTP("  Capability: category=0x%02x len=%u\n",
			category, dataLen);

		p += dataLen;
		remaining -= dataLen;
		capCount++;
	}

	*count = capCount;

	return B_OK;
}


status_t
AvdtpSession::SetConfiguration(uint8 remoteSeid, uint8 localSeid,
	const AvdtpCapability* caps, uint8 capCount)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	/* Build parameter: ACP SEID (1) + INT SEID (1) + capabilities */
	uint8 params[256];
	size_t offset = 0;

	params[offset++] = (remoteSeid << 2);
	params[offset++] = (localSeid << 2);

	for (uint8 i = 0; i < capCount; i++) {
		params[offset++] = caps[i].category;
		params[offset++] = caps[i].dataLen;
		if (caps[i].dataLen > 0) {
			memcpy(params + offset, caps[i].data, caps[i].dataLen);
			offset += caps[i].dataLen;
		}
	}

	status_t err = _SendSignal(AVDTP_SET_CONFIGURATION, params, offset);
	if (err != B_OK)
		return err;

	/* Receive response */
	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType == AVDTP_MSG_TYPE_RESPONSE_REJECT) {
		TRACE_AVDTP("SetConfiguration rejected: cat=0x%02x err=0x%02x\n",
			received > AVDTP_HEADER_SIZE
				? buf[AVDTP_HEADER_SIZE] : 0,
			received > AVDTP_HEADER_SIZE + 1
				? buf[AVDTP_HEADER_SIZE + 1] : 0);
		return B_ERROR;
	}

	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT)
		return B_ERROR;

	TRACE_AVDTP("Configuration set for remote SEID %u\n", remoteSeid);
	return B_OK;
}


status_t
AvdtpSession::Open(uint8 remoteSeid)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	uint8 param = (remoteSeid << 2);
	status_t err = _SendSignal(AVDTP_OPEN, &param, 1);
	if (err != B_OK)
		return err;

	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT) {
		TRACE_AVDTP("Open rejected\n");
		return B_ERROR;
	}

	TRACE_AVDTP("Stream opened for SEID %u\n", remoteSeid);
	return B_OK;
}


status_t
AvdtpSession::Start(uint8 remoteSeid)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	uint8 param = (remoteSeid << 2);
	status_t err = _SendSignal(AVDTP_START, &param, 1);
	if (err != B_OK)
		return err;

	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT) {
		TRACE_AVDTP("Start rejected\n");
		return B_ERROR;
	}

	TRACE_AVDTP("Stream started for SEID %u\n", remoteSeid);
	return B_OK;
}


status_t
AvdtpSession::Suspend(uint8 remoteSeid)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	uint8 param = (remoteSeid << 2);
	status_t err = _SendSignal(AVDTP_SUSPEND, &param, 1);
	if (err != B_OK)
		return err;

	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT) {
		TRACE_AVDTP("Suspend rejected\n");
		return B_ERROR;
	}

	return B_OK;
}


status_t
AvdtpSession::Close(uint8 remoteSeid)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	uint8 param = (remoteSeid << 2);
	status_t err = _SendSignal(AVDTP_CLOSE, &param, 1);
	if (err != B_OK)
		return err;

	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	ssize_t received = _RecvSignal(buf, sizeof(buf), 5000000);
	if (received < AVDTP_HEADER_SIZE)
		return B_TIMED_OUT;

	uint8 msgType = AVDTP_GET_MSG_TYPE(buf[0]);
	if (msgType != AVDTP_MSG_TYPE_RESPONSE_ACCEPT) {
		TRACE_AVDTP("Close rejected\n");
		return B_ERROR;
	}

	/* Close media channel */
	if (fMediaSocket >= 0) {
		close(fMediaSocket);
		fMediaSocket = -1;
	}

	return B_OK;
}


status_t
AvdtpSession::Abort(uint8 remoteSeid)
{
	if (fSignalingSocket < 0)
		return B_NO_INIT;

	uint8 param = (remoteSeid << 2);
	_SendSignal(AVDTP_ABORT, &param, 1);

	/* Read and discard response */
	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];
	_RecvSignal(buf, sizeof(buf), 2000000);

	if (fMediaSocket >= 0) {
		close(fMediaSocket);
		fMediaSocket = -1;
	}

	return B_OK;
}


status_t
AvdtpSession::OpenMediaChannel(const bdaddr_t& remote)
{
	if (fMediaSocket >= 0)
		return B_BUSY;

	/* Create L2CAP socket for media transport.
	 * Ideally SOCK_SEQPACKET to preserve RTP packet boundaries,
	 * but Haiku's L2CAP only supports SOCK_STREAM. Each send()
	 * on SOCK_STREAM should still produce a separate L2CAP PDU
	 * as long as we don't send faster than the link can transmit. */
	fMediaSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (fMediaSocket < 0) {
		TRACE_AVDTP("media socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	/* Longer timeout for media — 30 seconds */
	struct timeval tv;
	tv.tv_sec = 30;
	tv.tv_usec = 0;
	setsockopt(fMediaSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = PF_BLUETOOTH;
	addr.l2cap_psm = L2CAP_PSM_AVDTP;
	memcpy(&addr.l2cap_bdaddr, &remote, sizeof(bdaddr_t));

	TRACE_AVDTP("Opening media transport channel...\n");
	if (connect(fMediaSocket, (struct sockaddr*)&addr,
			sizeof(addr)) < 0) {
		TRACE_AVDTP("media connect() failed: %s\n", strerror(errno));
		close(fMediaSocket);
		fMediaSocket = -1;
		return B_ERROR;
	}

	TRACE_AVDTP("Media transport channel connected.\n");
	return B_OK;
}


/* =========================================================================
 * Private helpers
 * ========================================================================= */

status_t
AvdtpSession::_SendSignal(uint8 signalId, const uint8* params,
	size_t paramLen)
{
	uint8 buf[AVDTP_SIGNAL_BUF_SIZE];

	if (AVDTP_HEADER_SIZE + paramLen > sizeof(buf))
		return B_BUFFER_OVERFLOW;

	uint8 label = _NextTxLabel();
	buf[0] = AVDTP_SET_HEADER(label, AVDTP_PKT_TYPE_SINGLE,
		AVDTP_MSG_TYPE_COMMAND);
	buf[1] = signalId;

	if (paramLen > 0)
		memcpy(buf + AVDTP_HEADER_SIZE, params, paramLen);

	size_t totalLen = AVDTP_HEADER_SIZE + paramLen;

	TRACE_AVDTP("Send signal 0x%02x label=%u len=%zu\n",
		signalId, label, totalLen);

	ssize_t sent = send(fSignalingSocket, buf, totalLen, 0);
	if (sent < 0) {
		TRACE_AVDTP("send() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	return B_OK;
}


ssize_t
AvdtpSession::_RecvSignal(uint8* buf, size_t maxLen, bigtime_t timeout)
{
	/* Set receive timeout */
	struct timeval tv;
	tv.tv_sec = timeout / 1000000;
	tv.tv_usec = timeout % 1000000;
	setsockopt(fSignalingSocket, SOL_SOCKET, SO_RCVTIMEO, &tv,
		sizeof(tv));

	ssize_t received = recv(fSignalingSocket, buf, maxLen, 0);
	if (received < 0) {
		TRACE_AVDTP("recv() failed: %s\n", strerror(errno));
		return -1;
	}

	if (received >= AVDTP_HEADER_SIZE) {
		TRACE_AVDTP("Recv signal: label=%u pkt=%u msg=%u sig=0x%02x "
			"len=%zd\n",
			AVDTP_GET_TX_LABEL(buf[0]),
			AVDTP_GET_PKT_TYPE(buf[0]),
			AVDTP_GET_MSG_TYPE(buf[0]),
			buf[1], received);
	}

	return received;
}


uint8
AvdtpSession::_NextTxLabel()
{
	uint8 label = fTxLabel;
	fTxLabel = (fTxLabel + 1) & 0x0F;
	return label;
}
