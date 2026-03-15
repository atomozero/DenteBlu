/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpAudioGateway — HFP Audio Gateway role implementation.
 * Listens for incoming RFCOMM connections from headphones (HF) and
 * handles AT command exchange for SLC establishment and call control.
 */

#include <bluetooth/HfpAudioGateway.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <new>
#include <sys/socket.h>
#include <sys/time.h>

#include <String.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HfpClient.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <bluetooth/rfcomm.h>
#include <l2cap.h>

#include "AtParser.h"
#include "RfcommSession.h"


#define TRACE_HFP_AG(fmt, ...) \
	fprintf(stderr, "HFP-AG: " fmt, ##__VA_ARGS__)


/* AG features we advertise */
#define AG_FEATURES \
	(HFP_AG_FEATURE_3WAY | HFP_AG_FEATURE_ECNR | \
	 HFP_AG_FEATURE_REJECT | HFP_AG_FEATURE_ECS | \
	 HFP_AG_FEATURE_INBAND_RING)

/* RFCOMM channel for HFP AG */
#define HFP_AG_RFCOMM_CHANNEL	4


using Bluetooth::HfpAudioGateway;
using Bluetooth::HfpIndicators;


HfpAudioGateway::HfpAudioGateway()
	:
	fRfcomm(NULL),
	fDlci(0),
	fSlcEstablished(false),
	fAtThread(-1),
	fRunning(false),
	fLocalFeatures(AG_FEATURES),
	fRemoteFeatures(0),
	fIndicatorReporting(false),
	fClipEnabled(false),
	fDialCallback(NULL),
	fDialCookie(NULL),
	fAnswerCallback(NULL),
	fAnswerCookie(NULL),
	fHangupCallback(NULL),
	fHangupCookie(NULL),
	fSpeakerVolCallback(NULL),
	fSpeakerVolCookie(NULL),
	fMicVolCallback(NULL),
	fMicVolCookie(NULL)
{
	memset(&fIndicators, 0, sizeof(fIndicators));
	fIndicators.service = 1;  /* network service available */
	fIndicators.signal = 5;   /* full signal */
	fIndicators.battchg = 5;  /* full battery */
}


HfpAudioGateway::~HfpAudioGateway()
{
	Disconnect();
}


status_t
HfpAudioGateway::Listen(bigtime_t timeout)
{
	if (fRfcomm != NULL)
		return B_BUSY;

	/* Create L2CAP socket for RFCOMM */
	int listenSock = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (listenSock < 0) {
		TRACE_HFP_AG("socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	/* Bind to RFCOMM PSM */
	struct sockaddr_l2cap local;
	memset(&local, 0, sizeof(local));
	local.l2cap_len = sizeof(local);
	local.l2cap_family = AF_BLUETOOTH;
	local.l2cap_psm = L2CAP_PSM_RFCOMM;
	memset(&local.l2cap_bdaddr, 0, sizeof(local.l2cap_bdaddr));

	if (bind(listenSock, (struct sockaddr*)&local, sizeof(local)) < 0) {
		TRACE_HFP_AG("bind(PSM 3) failed: %s\n", strerror(errno));
		close(listenSock);
		return B_ERROR;
	}

	if (listen(listenSock, 1) < 0) {
		TRACE_HFP_AG("listen() failed: %s\n", strerror(errno));
		close(listenSock);
		return B_ERROR;
	}

	TRACE_HFP_AG("Listening on PSM 3 (RFCOMM) for HFP HF...\n");

	/* Set accept timeout */
	struct timeval tv;
	tv.tv_sec = timeout / 1000000;
	tv.tv_usec = timeout % 1000000;
	setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_l2cap remote;
	socklen_t remoteLen = sizeof(remote);
	int connSock = accept(listenSock, (struct sockaddr*)&remote,
		&remoteLen);
	close(listenSock);

	if (connSock < 0) {
		TRACE_HFP_AG("accept() failed: %s\n", strerror(errno));
		return B_TIMED_OUT;
	}

	BString addrStr = bdaddrUtils::ToString(remote.l2cap_bdaddr);
	TRACE_HFP_AG("Accepted connection from %s\n", addrStr.String());

	/* Set up RFCOMM session in responder mode */
	fRfcomm = new(std::nothrow) RfcommSession();
	if (fRfcomm == NULL) {
		close(connSock);
		return B_NO_MEMORY;
	}

	status_t err = fRfcomm->AcceptFrom(connSock);
	if (err != B_OK) {
		TRACE_HFP_AG("RFCOMM AcceptFrom failed: %s\n", strerror(err));
		delete fRfcomm;
		fRfcomm = NULL;
		close(connSock);
		return err;
	}

	/* Wait for HF to open channel */
	TRACE_HFP_AG("Waiting for HF to open RFCOMM channel...\n");
	uint8 channel = fRfcomm->WaitForChannel(timeout);
	if (channel == 0) {
		TRACE_HFP_AG("No RFCOMM channel opened by HF\n");
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return B_TIMED_OUT;
	}

	fDlci = RFCOMM_DLCI(0, channel);
	TRACE_HFP_AG("HF opened RFCOMM channel %u (DLCI %u)\n",
		channel, fDlci);

	/* Start AT command loop thread */
	fRunning = true;
	fAtThread = spawn_thread(_AtLoopEntry, "hfp_ag_at",
		B_NORMAL_PRIORITY, this);
	if (fAtThread < 0) {
		TRACE_HFP_AG("spawn_thread() failed\n");
		Disconnect();
		return B_ERROR;
	}
	resume_thread(fAtThread);

	return B_OK;
}


void
HfpAudioGateway::Disconnect()
{
	fRunning = false;
	fSlcEstablished = false;
	fRemoteFeatures = 0;
	fIndicatorReporting = false;

	if (fRfcomm != NULL) {
		if (fDlci != 0)
			fRfcomm->CloseChannel(fDlci);
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
	}

	if (fAtThread >= 0) {
		status_t exitVal;
		wait_for_thread(fAtThread, &exitVal);
		fAtThread = -1;
	}

	fDlci = 0;
}


bool
HfpAudioGateway::IsSlcEstablished() const
{
	return fSlcEstablished;
}


status_t
HfpAudioGateway::SetIndicators(const HfpIndicators& indicators)
{
	if (!fSlcEstablished || !fIndicatorReporting)
		return B_NOT_ALLOWED;

	/* Send +CIEV for each changed indicator */
	char buf[64];

#define SEND_CIEV(idx, field) \
	if (indicators.field != fIndicators.field) { \
		snprintf(buf, sizeof(buf), "+CIEV: %d,%d", idx, indicators.field); \
		_SendAtLine(buf); \
	}

	SEND_CIEV(1, service)
	SEND_CIEV(2, call)
	SEND_CIEV(3, callsetup)
	SEND_CIEV(4, signal)
	SEND_CIEV(5, roam)
	SEND_CIEV(6, battchg)
	SEND_CIEV(7, callheld)

#undef SEND_CIEV

	fIndicators = indicators;
	return B_OK;
}


status_t
HfpAudioGateway::SendRing(const char* callerNumber)
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	status_t err = _SendAtLine("RING");
	if (err != B_OK)
		return err;

	if (callerNumber != NULL && fClipEnabled) {
		char buf[128];
		snprintf(buf, sizeof(buf), "+CLIP: \"%s\",129", callerNumber);
		_SendAtLine(buf);
	}

	return B_OK;
}


void
HfpAudioGateway::SetDialCallback(hfp_ag_dial_callback cb, void* cookie)
{
	fDialCallback = cb;
	fDialCookie = cookie;
}


void
HfpAudioGateway::SetAnswerCallback(hfp_ag_action_callback cb, void* cookie)
{
	fAnswerCallback = cb;
	fAnswerCookie = cookie;
}


void
HfpAudioGateway::SetHangupCallback(hfp_ag_action_callback cb, void* cookie)
{
	fHangupCallback = cb;
	fHangupCookie = cookie;
}


void
HfpAudioGateway::SetSpeakerVolumeCallback(hfp_ag_volume_callback cb,
	void* cookie)
{
	fSpeakerVolCallback = cb;
	fSpeakerVolCookie = cookie;
}


void
HfpAudioGateway::SetMicVolumeCallback(hfp_ag_volume_callback cb,
	void* cookie)
{
	fMicVolCallback = cb;
	fMicVolCookie = cookie;
}


/* =========================================================================
 * Private: AT command loop
 * ========================================================================= */

status_t
HfpAudioGateway::_AtLoopEntry(void* arg)
{
	((HfpAudioGateway*)arg)->_AtLoop();
	return B_OK;
}


void
HfpAudioGateway::_AtLoop()
{
	TRACE_HFP_AG("AT loop started\n");

	while (fRunning && fRfcomm != NULL) {
		char line[256];
		status_t err = _ReadAtLine(line, sizeof(line), 2000000);
		if (err == B_TIMED_OUT)
			continue;
		if (err != B_OK) {
			TRACE_HFP_AG("AT read error: %s\n", strerror(err));
			break;
		}

		TRACE_HFP_AG("HF -> AG: %s\n", line);
		_HandleAtCommand(line);
	}

	TRACE_HFP_AG("AT loop exited\n");
}


status_t
HfpAudioGateway::_HandleAtCommand(const char* line)
{
	AtParser::Command cmd;
	if (!AtParser::Parse(line, cmd)) {
		TRACE_HFP_AG("Failed to parse: %s\n", line);
		BString err = AtParser::FormatError();
		return _SendAtLine(err.String());
	}

	switch (cmd.type) {
		case AtParser::AT_CMD_BRSF:
		{
			/* HF sends its features; we respond with ours */
			fRemoteFeatures = (uint32)atoi(cmd.argument.String());
			TRACE_HFP_AG("HF features: 0x%08x\n",
				(unsigned)fRemoteFeatures);

			BString resp = AtParser::FormatBrsf(fLocalFeatures);
			_SendAtLine(resp.String());

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CIND_TEST:
		{
			BString resp = AtParser::FormatCindTest();
			_SendAtLine(resp.String());

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CIND_READ:
		{
			BString resp = AtParser::FormatCindRead(
				fIndicators.service, fIndicators.call,
				fIndicators.callsetup, fIndicators.signal,
				fIndicators.roam, fIndicators.battchg,
				fIndicators.callheld);
			_SendAtLine(resp.String());

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CMER:
		{
			/* Enable indicator status reporting */
			fIndicatorReporting = true;
			TRACE_HFP_AG("Indicator reporting enabled\n");

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CHLD_TEST:
		{
			BString resp = AtParser::FormatChldTest();
			_SendAtLine(resp.String());

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());

			/* SLC is now established */
			fSlcEstablished = true;
			TRACE_HFP_AG("SLC established\n");
			break;
		}

		case AtParser::AT_CMD_CLIP:
		{
			fClipEnabled = (cmd.argument == "1");
			TRACE_HFP_AG("CLIP %s\n",
				fClipEnabled ? "enabled" : "disabled");

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_VGS:
		{
			uint8 level = (uint8)atoi(cmd.argument.String());
			TRACE_HFP_AG("Speaker volume: %u\n", level);

			if (fSpeakerVolCallback != NULL)
				fSpeakerVolCallback(level, fSpeakerVolCookie);

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_VGM:
		{
			uint8 level = (uint8)atoi(cmd.argument.String());
			TRACE_HFP_AG("Mic volume: %u\n", level);

			if (fMicVolCallback != NULL)
				fMicVolCallback(level, fMicVolCookie);

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_ATD:
		{
			TRACE_HFP_AG("Dial: %s\n", cmd.argument.String());

			if (fDialCallback != NULL)
				fDialCallback(cmd.argument.String(), fDialCookie);

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_ATA:
		{
			TRACE_HFP_AG("Answer\n");

			if (fAnswerCallback != NULL)
				fAnswerCallback(fAnswerCookie);

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CHUP:
		{
			TRACE_HFP_AG("Hang up\n");

			if (fHangupCallback != NULL)
				fHangupCallback(fHangupCookie);

			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CHLD:
		{
			TRACE_HFP_AG("Call hold: %s\n", cmd.argument.String());
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_COPS_READ:
		{
			/* Return operator name */
			_SendAtLine("+COPS: 0,0,\"Haiku\"");
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_COPS_SET:
		{
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_CLCC:
		{
			/* No active calls */
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_NREC:
		{
			/* Noise reduction: acknowledge but don't implement */
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_BIA:
		{
			/* Indicator activation: acknowledge */
			BString ok = AtParser::FormatOK();
			_SendAtLine(ok.String());
			break;
		}

		case AtParser::AT_CMD_BVRA:
		{
			/* Voice recognition: not supported */
			BString err = AtParser::FormatError();
			_SendAtLine(err.String());
			break;
		}

		default:
			TRACE_HFP_AG("Unknown AT command: %s\n", line);
			BString err = AtParser::FormatError();
			_SendAtLine(err.String());
			break;
	}

	return B_OK;
}


status_t
HfpAudioGateway::_SendAtLine(const char* line)
{
	if (fRfcomm == NULL || fDlci == 0)
		return B_NO_INIT;

	/* Send \r\n<response>\r\n */
	size_t len = strlen(line);
	uint8 buf[512];
	if (len + 4 > sizeof(buf))
		return B_BUFFER_OVERFLOW;

	buf[0] = '\r';
	buf[1] = '\n';
	memcpy(buf + 2, line, len);
	buf[2 + len] = '\r';
	buf[3 + len] = '\n';

	ssize_t sent = fRfcomm->Send(fDlci, buf, len + 4);
	if (sent < 0)
		return (status_t)sent;

	TRACE_HFP_AG("AG -> HF: %s\n", line);
	return B_OK;
}


status_t
HfpAudioGateway::_ReadAtLine(char* buf, size_t maxLen, bigtime_t timeout)
{
	if (fRfcomm == NULL)
		return B_NO_INIT;

	size_t pos = 0;
	bigtime_t deadline = system_time() + timeout;

	while (pos < maxLen - 1) {
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0) {
			if (pos > 0)
				break;
			return B_TIMED_OUT;
		}

		uint8 byte;
		ssize_t received = fRfcomm->Receive(fDlci, &byte, 1, remaining);

		if (received < 0) {
			if (received == B_TIMED_OUT && pos > 0)
				break;
			return (status_t)received;
		}
		if (received == 0)
			return B_IO_ERROR;

		if (byte == '\r' || byte == '\n') {
			if (pos > 0)
				break;
			continue;  /* skip leading \r\n */
		}

		buf[pos++] = (char)byte;
	}

	buf[pos] = '\0';

	if (pos == 0)
		return B_TIMED_OUT;

	return B_OK;
}
