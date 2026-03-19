/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcEncoder — Subband Codec encoder for A2DP audio.
 * Uses libsbc (BlueZ reference implementation) for encoding.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _SBC_ENCODER_H_
#define _SBC_ENCODER_H_

#include <SupportDefs.h>


class SbcEncoder {
public:
								SbcEncoder();
								~SbcEncoder();

	status_t					Configure(uint32 sampleRate,
									uint8 channels,
									uint8 blocks,
									uint8 subbands,
									uint8 channelMode,
									uint8 allocMethod,
									uint8 bitpool);

	ssize_t						EncodeFrame(const int16* input,
									uint8* output,
									size_t maxOutputLen);

	uint16						FrameLength() const;

	uint16						SamplesPerFrame() const
									{ return fBlocks * fSubbands; }

	uint32						SampleRate() const
									{ return fSampleRate; }
	uint8						Channels() const
									{ return fChannels; }
	uint8						Subbands() const
									{ return fSubbands; }
	uint8						Blocks() const { return fBlocks; }
	uint8						Bitpool() const
									{ return fBitpool; }

private:
	void*						fSbcHandle;

	uint32						fSampleRate;
	uint8						fChannels;
	uint8						fBlocks;
	uint8						fSubbands;
	uint8						fChannelMode;
	uint8						fAllocMethod;
	uint8						fBitpool;
};


#endif /* _SBC_ENCODER_H_ */
