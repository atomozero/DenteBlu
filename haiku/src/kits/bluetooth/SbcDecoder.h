/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcDecoder — Subband Codec decoder for A2DP audio.
 * Implements the mandatory SBC codec per BT A2DP specification.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _SBC_DECODER_H_
#define _SBC_DECODER_H_

#include <SupportDefs.h>


class SbcDecoder {
public:
								SbcDecoder();
								~SbcDecoder();

	/* Decode one SBC frame.
	 * Returns the number of PCM samples written to output
	 * (per channel — total samples = return * channels).
	 * Returns negative on error. */
	ssize_t						DecodeFrame(const uint8* input,
									size_t inputLen,
									int16* output,
									size_t maxOutputSamples);

	/* Frame info from last decoded frame */
	uint32						SampleRate() const
									{ return fSampleRate; }
	uint8						Channels() const
									{ return fChannels; }
	uint8						Subbands() const
									{ return fSubbands; }
	uint8						Blocks() const { return fBlocks; }
	uint16						FrameLength() const;

private:
	/* Decode steps */
	bool						_ParseHeader(const uint8* data,
									size_t len);
	void						_UnpackSamples(const uint8* data);
	void						_Reconstruct(int ch);
	void						_Synthesize(int ch, int blk,
									int16* output);

	/* Frame parameters */
	uint32						fSampleRate;
	uint8						fChannels;
	uint8						fBlocks;
	uint8						fSubbands;
	uint8						fChannelMode;
	uint8						fAllocMethod;
	uint8						fBitpool;
	uint8						fJoin;

	/* Working buffers */
	int32						fScaleFactors[2][8];
	int32						fSbSamples[2][16][8];
	int32						fV[2][160];
};


#endif /* _SBC_DECODER_H_ */
