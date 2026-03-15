/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcEncoder — Subband Codec encoder for A2DP audio.
 * Implements the mandatory SBC codec per BT A2DP specification.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _SBC_ENCODER_H_
#define _SBC_ENCODER_H_

#include <SupportDefs.h>


class SbcEncoder {
public:
								SbcEncoder();
								~SbcEncoder();

	/* Configure encoder parameters.
	 * Must be called before EncodeFrame().
	 * channelMode: 0=Mono, 1=DualChannel, 2=Stereo, 3=JointStereo
	 * allocMethod: 0=Loudness, 1=SNR */
	status_t					Configure(uint32 sampleRate,
									uint8 channels,
									uint8 blocks,
									uint8 subbands,
									uint8 channelMode,
									uint8 allocMethod,
									uint8 bitpool);

	/* Encode one frame of PCM samples to SBC.
	 * input: interleaved int16 PCM, must contain blocks*subbands*channels samples.
	 * output: buffer for SBC frame data.
	 * maxOutputLen: size of output buffer.
	 * Returns number of bytes written to output, or negative on error. */
	ssize_t						EncodeFrame(const int16* input,
									uint8* output,
									size_t maxOutputLen);

	/* Frame length in bytes for current configuration */
	uint16						FrameLength() const;

	/* Number of PCM samples needed per frame (per channel) */
	uint16						SamplesPerFrame() const
									{ return fBlocks * fSubbands; }

	/* Accessors */
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
	/* Encode steps */
	void						_BuildHeader(uint8* output);
	void						_Analyze(int ch, int blk,
									const int16* input);
	void						_ComputeScaleFactors();
	void						_ComputeBitAllocation(
									int32 bits[2][8]);
	void						_JointStereoProcess();
	void						_QuantizeAndPack(uint8* output,
									const int32 bits[2][8]);

	/* Frame parameters */
	uint32						fSampleRate;
	uint8						fChannels;
	uint8						fBlocks;
	uint8						fSubbands;
	uint8						fChannelMode;
	uint8						fAllocMethod;
	uint8						fBitpool;

	/* Working buffers */
	int32						fScaleFactors[2][8];
	int32						fSbSamples[2][16][8];
	int32						fX[2][160];		/* analysis window buffer */
	uint8						fJoin;			/* joint stereo flags */
};


#endif /* _SBC_ENCODER_H_ */
