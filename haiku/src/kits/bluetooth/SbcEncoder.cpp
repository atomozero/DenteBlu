/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcEncoder — wrapper around libsbc (BlueZ reference SBC implementation).
 * sbc/sbc.h is included ONLY here (not in the header) to avoid conflicts
 * between libsbc's SBC_FREQ_* constants and our avdtp.h constants.
 */

#include "SbcEncoder.h"

#include <stdlib.h>
#include <string.h>

#include <sbc/sbc.h>


SbcEncoder::SbcEncoder()
	:
	fSbcHandle(NULL),
	fSampleRate(44100),
	fChannels(2),
	fBlocks(16),
	fSubbands(8),
	fChannelMode(3),
	fAllocMethod(0),
	fBitpool(53)
{
}


SbcEncoder::~SbcEncoder()
{
	if (fSbcHandle != NULL) {
		sbc_finish((sbc_t*)fSbcHandle);
		free(fSbcHandle);
	}
}


status_t
SbcEncoder::Configure(uint32 sampleRate, uint8 channels, uint8 blocks,
	uint8 subbands, uint8 channelMode, uint8 allocMethod, uint8 bitpool)
{
	if (subbands != 4 && subbands != 8)
		return B_BAD_VALUE;
	if (blocks != 4 && blocks != 8 && blocks != 12 && blocks != 16)
		return B_BAD_VALUE;

	if (fSbcHandle != NULL) {
		sbc_finish((sbc_t*)fSbcHandle);
		free(fSbcHandle);
	}

	fSbcHandle = malloc(sizeof(sbc_t));
	if (fSbcHandle == NULL)
		return B_NO_MEMORY;

	sbc_t* sbc = (sbc_t*)fSbcHandle;
	sbc_init(sbc, 0);

	/* Map sample rate to libsbc constants */
	switch (sampleRate) {
		case 16000: sbc->frequency = SBC_FREQ_16000; break;
		case 32000: sbc->frequency = SBC_FREQ_32000; break;
		case 44100: sbc->frequency = SBC_FREQ_44100; break;
		case 48000: sbc->frequency = SBC_FREQ_48000; break;
		default: return B_BAD_VALUE;
	}

	switch (blocks) {
		case 4:  sbc->blocks = SBC_BLK_4; break;
		case 8:  sbc->blocks = SBC_BLK_8; break;
		case 12: sbc->blocks = SBC_BLK_12; break;
		case 16: sbc->blocks = SBC_BLK_16; break;
	}

	switch (channelMode) {
		case 0: sbc->mode = SBC_MODE_MONO; break;
		case 1: sbc->mode = SBC_MODE_DUAL_CHANNEL; break;
		case 2: sbc->mode = SBC_MODE_STEREO; break;
		case 3: sbc->mode = SBC_MODE_JOINT_STEREO; break;
		default: return B_BAD_VALUE;
	}

	sbc->subbands = (subbands == 8) ? SBC_SB_8 : SBC_SB_4;
	sbc->allocation = (allocMethod == 0) ? SBC_AM_LOUDNESS : SBC_AM_SNR;
	sbc->bitpool = bitpool;

	fSampleRate = sampleRate;
	fChannels = channels;
	fBlocks = blocks;
	fSubbands = subbands;
	fChannelMode = channelMode;
	fAllocMethod = allocMethod;
	fBitpool = bitpool;

	return B_OK;
}


ssize_t
SbcEncoder::EncodeFrame(const int16* input, uint8* output,
	size_t maxOutputLen)
{
	if (fSbcHandle == NULL)
		return -1;

	sbc_t* sbc = (sbc_t*)fSbcHandle;
	size_t codesize = sbc_get_codesize(sbc);
	ssize_t written = 0;

	ssize_t consumed = sbc_encode(sbc, input, codesize,
		output, maxOutputLen, &written);

	if (consumed <= 0 || written <= 0)
		return -1;

	return written;
}


uint16
SbcEncoder::FrameLength() const
{
	if (fSbcHandle == NULL)
		return 0;
	return sbc_get_frame_length((sbc_t*)fSbcHandle);
}
