/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpSource — Advanced Audio Distribution Profile (A2DP) Source.
 *
 * Sends SBC-encoded audio to a remote A2DP Sink (e.g., headphones)
 * using AVDTP signaling over L2CAP and SBC codec encoding.
 */
#ifndef _A2DP_SOURCE_H_
#define _A2DP_SOURCE_H_

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class AvdtpSession;
class SbcEncoder;
struct AvdtpCapability;


namespace Bluetooth {


class A2dpSource {
public:
								A2dpSource();
	virtual						~A2dpSource();

	/* Connect to a remote A2DP Sink (headphones). Performs:
	 * 1. AVDTP signaling connection
	 * 2. Endpoint discovery (finds SBC sink endpoint)
	 * 3. Codec negotiation (SBC configuration)
	 * 4. Stream open + media channel */
	status_t					Connect(const bdaddr_t& address);
	void						Disconnect();
	bool						IsConnected() const;

	/* Start/stop audio streaming */
	status_t					StartStream();
	status_t					StopStream();
	bool						IsStreaming() const
								    { return fStreaming; }

	/* Send PCM audio data to the headphones.
	 * pcm: interleaved int16 PCM samples (host byte order).
	 * sampleCount: number of samples PER CHANNEL.
	 * Blocks until pacing delay is reached (maintains real-time). */
	status_t					SendAudio(const int16* pcm,
								    size_t sampleCount);

	/* Stream info (valid after Connect) */
	uint32						SampleRate() const;
	uint8						Channels() const;
	uint16						FrameLength() const;
	uint16						SamplesPerFrame() const;

private:
	static bool					_EnsureAclConnection(
								    const bdaddr_t& remote);
	status_t					_NegotiateCodec(
								    const AvdtpCapability* remoteCaps,
								    uint8 remoteCapCount);
	status_t					_BuildRtpPacket(const uint8* sbcFrames,
								    uint8 frameCount,
								    uint16 totalSbcLen);

	AvdtpSession*				fAvdtp;
	SbcEncoder*					fEncoder;
	bdaddr_t					fRemoteAddr;
	uint8						fRemoteSeid;
	uint8						fLocalSeid;
	bool						fConnected;
	bool						fStreaming;

	/* RTP state */
	uint16						fRtpSeqNumber;
	uint32						fRtpTimestamp;
	uint32						fSsrc;

	/* Pacing state */
	bigtime_t					fStreamStartTime;
	uint64						fTotalSamplesSent;

	/* Negotiated SBC parameters (set by _NegotiateCodec) */
	uint32						fNegSampleRate;
	uint8						fNegChannels;
	uint8						fNegBlocks;
	uint8						fNegSubbands;
	uint8						fNegChannelMode;
	uint8						fNegAllocMethod;
	uint8						fNegBitpool;

	/* RTP send buffer */
	uint8						fRtpBuf[4096];
};


} /* namespace Bluetooth */


#endif /* _A2DP_SOURCE_H_ */
