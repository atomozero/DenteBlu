/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpAudioGateway — HFP Audio Gateway role.
 * Accepts incoming RFCOMM connections from Bluetooth headphones (HF role),
 * handles AT commands for SLC establishment and call control.
 */
#ifndef _HFP_AUDIO_GATEWAY_H_
#define _HFP_AUDIO_GATEWAY_H_

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class RfcommSession;


namespace Bluetooth {


struct HfpIndicators {
	uint8	service;	/* 0-1: network service available */
	uint8	call;		/* 0-1: active call */
	uint8	callsetup;	/* 0-3: call setup state */
	uint8	signal;		/* 0-5: signal strength */
	uint8	roam;		/* 0-1: roaming */
	uint8	battchg;	/* 0-5: battery level */
	uint8	callheld;	/* 0-2: held call state */
};


typedef void (*hfp_ag_dial_callback)(const char* number, void* cookie);
typedef void (*hfp_ag_action_callback)(void* cookie);
typedef void (*hfp_ag_volume_callback)(uint8 level, void* cookie);


class HfpAudioGateway {
public:
								HfpAudioGateway();
	virtual						~HfpAudioGateway();

	/* Listen for incoming RFCOMM connection on channel 4.
	 * Blocks until a headphone connects or timeout. */
	status_t					Listen(bigtime_t timeout = 30000000);
	void						Disconnect();
	bool						IsSlcEstablished() const;

	/* Indicator updates: sends +CIEV to headphone */
	status_t					SetIndicators(
									const HfpIndicators& indicators);

	/* Simulated incoming call: RING + +CLIP */
	status_t					SendRing(const char* callerNumber = NULL);

	/* Callbacks */
	void						SetDialCallback(
									hfp_ag_dial_callback cb,
									void* cookie);
	void						SetAnswerCallback(
									hfp_ag_action_callback cb,
									void* cookie);
	void						SetHangupCallback(
									hfp_ag_action_callback cb,
									void* cookie);
	void						SetSpeakerVolumeCallback(
									hfp_ag_volume_callback cb,
									void* cookie);
	void						SetMicVolumeCallback(
									hfp_ag_volume_callback cb,
									void* cookie);

	/* Remote features from AT+BRSF */
	uint32						RemoteFeatures() const
									{ return fRemoteFeatures; }

private:
	static status_t				_AtLoopEntry(void* arg);
	void						_AtLoop();
	status_t					_HandleAtCommand(const char* line);
	status_t					_SendAtLine(const char* line);
	status_t					_ReadAtLine(char* buf, size_t maxLen,
									bigtime_t timeout);

	RfcommSession*				fRfcomm;
	uint8						fDlci;
	bool						fSlcEstablished;
	thread_id					fAtThread;
	volatile bool				fRunning;

	uint32						fLocalFeatures;
	uint32						fRemoteFeatures;
	HfpIndicators				fIndicators;
	bool						fIndicatorReporting;
	bool						fClipEnabled;

	hfp_ag_dial_callback		fDialCallback;
	void*						fDialCookie;
	hfp_ag_action_callback		fAnswerCallback;
	void*						fAnswerCookie;
	hfp_ag_action_callback		fHangupCallback;
	void*						fHangupCookie;
	hfp_ag_volume_callback		fSpeakerVolCallback;
	void*						fSpeakerVolCookie;
	hfp_ag_volume_callback		fMicVolCallback;
	void*						fMicVolCookie;
};


} /* namespace Bluetooth */


#endif /* _HFP_AUDIO_GATEWAY_H_ */
