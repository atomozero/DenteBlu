/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpClient — Hands-Free Profile client for Classic Bluetooth.
 *
 * Implements the HF (Hands-Free) role, connecting to a phone's AG
 * (Audio Gateway). Uses RFCOMM for AT commands and SCO for audio.
 */
#ifndef _HFP_CLIENT_H_
#define _HFP_CLIENT_H_

#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class RfcommSession;


namespace Bluetooth {


/* HFP HF-side supported features (AT+BRSF from HF to AG) */
#define HFP_HF_FEATURE_ECNR			0x0001
#define HFP_HF_FEATURE_3WAY			0x0002
#define HFP_HF_FEATURE_CLIP			0x0004
#define HFP_HF_FEATURE_VOICE_RECOG		0x0008
#define HFP_HF_FEATURE_VOLUME			0x0010
#define HFP_HF_FEATURE_ECS				0x0020
#define HFP_HF_FEATURE_ECC				0x0040
#define HFP_HF_FEATURE_CODEC_NEGO		0x0080
#define HFP_HF_FEATURE_HF_INDICATORS	0x0100
#define HFP_HF_FEATURE_ESCO_S4			0x0200

/* HFP AG-side supported features (response from AG) */
#define HFP_AG_FEATURE_3WAY			0x0001
#define HFP_AG_FEATURE_ECNR			0x0002
#define HFP_AG_FEATURE_VOICE_RECOG		0x0004
#define HFP_AG_FEATURE_INBAND_RING		0x0008
#define HFP_AG_FEATURE_VOICE_TAG		0x0010
#define HFP_AG_FEATURE_REJECT			0x0020
#define HFP_AG_FEATURE_ECS				0x0040
#define HFP_AG_FEATURE_ECC				0x0080
#define HFP_AG_FEATURE_EERR			0x0100
#define HFP_AG_FEATURE_CODEC_NEGO		0x0200


class HfpClient {
public:
								HfpClient();
	virtual						~HfpClient();

	/* Connect to a remote AG (phone) on the given RFCOMM channel.
	 * If rfcommChannel == 0, queries SDP for UUID 0x111E. */
	status_t					Connect(const bdaddr_t& address,
									uint8 rfcommChannel = 0);
	void						Disconnect();
	bool						IsConnected() const;

	/* Establish HFP Service Level Connection (SLC).
	 * Must be called after Connect() succeeds.
	 * Performs: AT+BRSF, AT+CIND=?, AT+CIND?, AT+CMER, AT+CHLD=? */
	status_t					EstablishServiceLevel();
	bool						IsServiceLevelEstablished() const;

	/* Call control */
	status_t					Dial(const char* number);
	status_t					Answer();
	status_t					HangUp();

	/* Volume control (0-15) */
	status_t					SetSpeakerVolume(uint8 level);
	status_t					SetMicVolume(uint8 level);

	/* AG feature query */
	uint32						RemoteFeatures() const
									{ return fRemoteFeatures; }

	/* Send raw AT command and receive response */
	status_t					SendAt(const char* command,
									char* response, size_t maxLen,
									bigtime_t timeout = 5000000);

private:
	static bool					_EnsureAclConnection(
									const bdaddr_t& remote);
	static bool					_QuerySdpForHfp(
									const bdaddr_t& remote,
									uint8& outChannel);
	status_t					_SendAtLine(const char* line);
	status_t					_ReadAtLine(char* buf, size_t maxLen,
									bigtime_t timeout);

	RfcommSession*				fRfcomm;
	uint8						fDlci;
	uint32						fLocalFeatures;
	uint32						fRemoteFeatures;
	bool						fSlcEstablished;
};


} /* namespace Bluetooth */


#endif /* _HFP_CLIENT_H_ */
