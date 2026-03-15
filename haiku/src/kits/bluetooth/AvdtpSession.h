/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AvdtpSession — Audio/Video Distribution Transport Protocol session
 * over L2CAP PSM 0x0019.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _AVDTP_SESSION_H_
#define _AVDTP_SESSION_H_

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/avdtp.h>


#define AVDTP_MAX_SEPS			16
#define AVDTP_MAX_CAPABILITIES	8
#define AVDTP_SIGNAL_BUF_SIZE	512
#define AVDTP_MEDIA_BUF_SIZE	4096


struct AvdtpCapability {
	uint8	category;
	uint8	dataLen;
	uint8	data[32];
};


class AvdtpSession {
public:
								AvdtpSession();
								~AvdtpSession();

	/* Signaling connection (client — initiator) */
	status_t					Connect(const bdaddr_t& remote);
	void						Disconnect();
	bool						IsConnected() const
									{ return fSignalingSocket >= 0; }

	/* AVDTP discovery and configuration */
	status_t					DiscoverEndpoints(
									avdtp_sep_info* seps,
									uint8* count, uint8 maxSeps);
	status_t					GetCapabilities(uint8 remoteSeid,
									AvdtpCapability* caps,
									uint8* count, uint8 maxCaps);
	status_t					SetConfiguration(uint8 remoteSeid,
									uint8 localSeid,
									const AvdtpCapability* caps,
									uint8 capCount);
	status_t					Open(uint8 remoteSeid);
	status_t					Start(uint8 remoteSeid);
	status_t					Suspend(uint8 remoteSeid);
	status_t					Close(uint8 remoteSeid);
	status_t					Abort(uint8 remoteSeid);

	/* Media transport socket (valid after Open+Start) */
	int							MediaSocket() const
									{ return fMediaSocket; }
	status_t					OpenMediaChannel(
									const bdaddr_t& remote);

private:
	/* Signaling helpers */
	status_t					_SendSignal(uint8 signalId,
									const uint8* params,
									size_t paramLen);
	ssize_t						_RecvSignal(uint8* buf, size_t maxLen,
									bigtime_t timeout);
	uint8						_NextTxLabel();

	int							fSignalingSocket;
	int							fMediaSocket;
	uint8						fTxLabel;
	bdaddr_t					fRemoteAddr;
};


#endif /* _AVDTP_SESSION_H_ */
