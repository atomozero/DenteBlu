/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpSink — Advanced Audio Distribution Profile (A2DP) Sink.
 *
 * Receives audio from a remote A2DP Source (e.g., phone, computer)
 * using AVDTP signaling over L2CAP and SBC codec decoding.
 */
#ifndef _A2DP_SINK_H_
#define _A2DP_SINK_H_

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class AvdtpSession;
class SbcDecoder;


namespace Bluetooth {


class A2dpSink {
public:
								A2dpSink();
	virtual						~A2dpSink();

	/* Connect to a remote A2DP Source. Performs:
	 * 1. AVDTP signaling connection
	 * 2. Endpoint discovery (finds SBC source endpoint)
	 * 3. Codec negotiation (SBC configuration)
	 * 4. Stream open */
	status_t					Connect(const bdaddr_t& address);
	void						Disconnect();
	bool						IsConnected() const;

	/* Start/stop audio streaming */
	status_t					StartStream();
	status_t					StopStream();
	bool						IsStreaming() const
									{ return fStreaming; }

	/* Audio data callback — called from receive thread with decoded
	 * PCM samples (interleaved int16, host byte order).
	 * sampleCount = number of samples per channel in this callback. */
	typedef void				(*audio_data_callback)(
									const int16* pcm,
									size_t sampleCount,
									uint32 sampleRate,
									uint8 channels,
									void* cookie);
	void						SetAudioCallback(
									audio_data_callback callback,
									void* cookie);

	/* Stream info (valid after Connect) */
	uint32						SampleRate() const;
	uint8						Channels() const;

private:
	static bool					_EnsureAclConnection(
									const bdaddr_t& remote);
	status_t					_NegotiateCodec();
	static status_t				_ReceiveThreadEntry(void* arg);
	void						_ReceiveLoop();

	AvdtpSession*				fAvdtp;
	SbcDecoder*					fDecoder;
	bdaddr_t					fRemoteAddr;
	uint8						fRemoteSeid;
	uint8						fLocalSeid;
	bool						fConnected;
	bool						fStreaming;
	thread_id					fReceiveThread;
	audio_data_callback			fAudioCallback;
	void*						fAudioCookie;
};


} /* namespace Bluetooth */


#endif /* _A2DP_SINK_H_ */
