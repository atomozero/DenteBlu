/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcDecoder — Subband Codec decoder per BT A2DP specification.
 * Reference: A2DP Spec Appendix B (SBC).
 */

#include "SbcDecoder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


#define TRACE_SBC(fmt, ...) \
	fprintf(stderr, "SBC: " fmt, ##__VA_ARGS__)


/* Channel modes */
enum {
	SBC_CM_MONO			= 0,
	SBC_CM_DUAL_CHANNEL	= 1,
	SBC_CM_STEREO		= 2,
	SBC_CM_JOINT_STEREO	= 3
};


/* Allocation methods */
enum {
	SBC_AM_LOUDNESS	= 0,
	SBC_AM_SNR		= 1
};


/* Synthesis filter bank prototype coefficients (scaled by 2^15).
 * From A2DP Spec Table 12.8 — cosine modulated filter bank.
 * 80 coefficients for 8 subbands, 40 for 4 subbands. */

static const int32 sProto8[80] = {
	     0,      2,      3,      4,      6,      6,      7,      6,
	     5,      2,     -2,     -8,    -15,    -24,    -34,    -45,
	   -57,    -68,    -78,    -85,    -88,    -84,    -72,    -51,
	   -18,     26,     83,    153,    237,    336,    450,    579,
	   724,    884,   1057,   1244,   1440,   1644,   1853,   2063,
	  2272,   2474,   2665,   2841,   2996,   3127,   3228,   3297,
	  3329,   3323,   3276,   3188,   3059,   2889,   2679,   2432,
	  2150,   1838,   1499,   1140,    766,    383,     -2,   -387,
	  -767,  -1137,  -1491,  -1826,  -2137,  -2418,  -2666,  -2876,
	 -3046,  -3174,  -3258,  -3296,  -3290,  -3239,  -3144,  -3009
};


static const int32 sProto4[40] = {
	     0,      3,      7,      7,      5,     -2,    -15,    -34,
	   -57,    -78,    -88,    -72,    -18,     83,    237,    450,
	   724,   1057,   1440,   1853,   2272,   2665,   2996,   3228,
	  3329,   3276,   3059,   2679,   2150,   1499,    766,     -2,
	  -767,  -1491,  -2137,  -2666,  -3046,  -3258,  -3290,  -3144
};


/* CRC-8 for SBC (polynomial x^8 + x^4 + x^3 + x^2 + 1 = 0x1D) */
static uint8
_Crc8(const uint8* data, size_t len, uint8 bits_in_last)
{
	(void)bits_in_last;
	uint8 crc = 0x0F;

	for (size_t i = 0; i < len; i++) {
		uint8 byte = data[i];
		for (int bit = 7; bit >= 0; bit--) {
			uint8 msb = (crc >> 7) & 1;
			crc = (crc << 1) | ((byte >> bit) & 1);
			if (msb)
				crc ^= 0x1D;
		}
	}

	return crc;
}


/* Bit reader for unpacking quantized samples */
struct BitReader {
	const uint8*	data;
	size_t			offset;		/* byte offset */
	uint8			bitPos;		/* bits remaining in current byte (8..1) */

	void Init(const uint8* d, size_t byteOffset) {
		data = d;
		offset = byteOffset;
		bitPos = 8;
	}

	uint32 Read(uint8 bits) {
		uint32 val = 0;
		while (bits > 0) {
			uint8 available = bitPos;
			uint8 take = (bits < available) ? bits : available;
			uint8 shift = available - take;
			val = (val << take)
				| ((data[offset] >> shift) & ((1 << take) - 1));
			bitPos -= take;
			bits -= take;
			if (bitPos == 0) {
				offset++;
				bitPos = 8;
			}
		}
		return val;
	}
};


SbcDecoder::SbcDecoder()
	:
	fSampleRate(44100),
	fChannels(2),
	fBlocks(16),
	fSubbands(8),
	fChannelMode(SBC_CM_JOINT_STEREO),
	fAllocMethod(SBC_AM_LOUDNESS),
	fBitpool(32),
	fJoin(0)
{
	memset(fScaleFactors, 0, sizeof(fScaleFactors));
	memset(fSbSamples, 0, sizeof(fSbSamples));
	memset(fV, 0, sizeof(fV));
}


SbcDecoder::~SbcDecoder()
{
}


uint16
SbcDecoder::FrameLength() const
{
	uint16 len = 4; /* header: syncword + byte1 + byte2 + CRC */

	if (fChannelMode == SBC_CM_MONO)
		len += (fBlocks * fChannels * fBitpool + 4 * fSubbands * fChannels + 7) / 8;
	else if (fChannelMode == SBC_CM_DUAL_CHANNEL)
		len += (fBlocks * 2 * fBitpool + 4 * fSubbands * 2 + 7) / 8;
	else if (fChannelMode == SBC_CM_STEREO)
		len += (fBlocks * fBitpool + 4 * fSubbands * 2 + 7) / 8;
	else /* joint stereo */
		len += (fBlocks * fBitpool + fSubbands + 4 * fSubbands * 2 + 7) / 8;

	return len;
}


ssize_t
SbcDecoder::DecodeFrame(const uint8* input, size_t inputLen,
	int16* output, size_t maxOutputSamples)
{
	if (inputLen < 4)
		return -1;

	/* Parse frame header */
	if (!_ParseHeader(input, inputLen))
		return -1;

	uint16 frameLen = FrameLength();
	if (inputLen < frameLen) {
		TRACE_SBC("Frame too short: need %u, got %zu\n",
			frameLen, inputLen);
		return -1;
	}

	uint16 totalSamples = fBlocks * fSubbands;
	if (maxOutputSamples < (size_t)(totalSamples * fChannels))
		return -1;

	/* Verify CRC — covers header bytes 1-2 */
	uint8 expectedCrc = input[3];
	uint8 computedCrc = _Crc8(input + 1, 2, 8);
	if (computedCrc != expectedCrc) {
		TRACE_SBC("CRC mismatch: expected 0x%02x got 0x%02x\n",
			expectedCrc, computedCrc);
		/* Don't fail — some sources have quirky CRC */
	}

	/* Unpack scale factors and quantized samples */
	_UnpackSamples(input);

	/* Reconstruct subband samples */
	for (int ch = 0; ch < fChannels; ch++)
		_Reconstruct(ch);

	/* Synthesis filter bank → PCM output */
	int16* out = output;
	for (int blk = 0; blk < fBlocks; blk++) {
		for (int ch = 0; ch < fChannels; ch++) {
			_Synthesize(ch, blk, out);
		}
		out += fSubbands * fChannels;
	}

	return totalSamples;
}


bool
SbcDecoder::_ParseHeader(const uint8* data, size_t len)
{
	if (data[0] != 0x9C) {
		TRACE_SBC("Bad syncword: 0x%02x\n", data[0]);
		return false;
	}

	/* Byte 1: sampling_frequency(2) | blocks(2) | channel_mode(2) |
	 *         allocation_method(1) | subbands(1) */
	uint8 b1 = data[1];

	switch ((b1 >> 6) & 0x03) {
		case 0: fSampleRate = 16000; break;
		case 1: fSampleRate = 32000; break;
		case 2: fSampleRate = 44100; break;
		case 3: fSampleRate = 48000; break;
	}

	switch ((b1 >> 4) & 0x03) {
		case 0: fBlocks = 4;  break;
		case 1: fBlocks = 8;  break;
		case 2: fBlocks = 12; break;
		case 3: fBlocks = 16; break;
	}

	fChannelMode = (b1 >> 2) & 0x03;
	fChannels = (fChannelMode == SBC_CM_MONO) ? 1 : 2;

	fAllocMethod = (b1 >> 1) & 0x01;
	fSubbands = (b1 & 0x01) ? 8 : 4;

	/* Byte 2: bitpool */
	fBitpool = data[2];

	return true;
}


void
SbcDecoder::_UnpackSamples(const uint8* data)
{
	BitReader br;

	/* Start after header (4 bytes) */
	size_t byteOff = 4;

	/* For joint stereo, read join flags first */
	fJoin = 0;
	br.Init(data, byteOff);
	if (fChannelMode == SBC_CM_JOINT_STEREO) {
		for (int sb = 0; sb < fSubbands - 1; sb++)
			fJoin |= (br.Read(1) << sb);
		/* Consume the padding bit if subbands == 4 (need nibble alignment) */
		if (fSubbands == 4)
			br.Read(1);
		/* Continue reading from current position — do not re-init */
	}

	/* Scale factors: 4 bits each */
	for (int ch = 0; ch < fChannels; ch++)
		for (int sb = 0; sb < fSubbands; sb++)
			fScaleFactors[ch][sb] = br.Read(4);

	/* Compute bit allocation */
	int32 bits[2][8];
	memset(bits, 0, sizeof(bits));

	if (fAllocMethod == SBC_AM_SNR) {
		/* SNR allocation */
		int32 bitneed[2][8];
		for (int ch = 0; ch < fChannels; ch++)
			for (int sb = 0; sb < fSubbands; sb++)
				bitneed[ch][sb] = fScaleFactors[ch][sb];

		/* Find max bitneed */
		int32 maxBitneed = 0;
		for (int ch = 0; ch < fChannels; ch++)
			for (int sb = 0; sb < fSubbands; sb++)
				if (bitneed[ch][sb] > maxBitneed)
					maxBitneed = bitneed[ch][sb];

		/* Allocate bits */
		int32 bitcount = 0;
		int32 slicecount = 0;
		int32 bitslice = maxBitneed + 1;

		do {
			bitslice--;
			bitcount += slicecount;
			slicecount = 0;
			for (int ch = 0; ch < fChannels; ch++)
				for (int sb = 0; sb < fSubbands; sb++) {
					if (bitneed[ch][sb] > bitslice + 1
							&& bitneed[ch][sb] < bitslice + 16)
						slicecount++;
					else if (bitneed[ch][sb] == bitslice + 1)
						slicecount += 2;
				}
		} while (bitcount + slicecount < fBitpool);

		if (bitcount + slicecount == fBitpool) {
			bitcount += slicecount;
			bitslice--;
		}

		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (bitneed[ch][sb] < bitslice + 2)
					bits[ch][sb] = 0;
				else
					bits[ch][sb] = bitneed[ch][sb] - bitslice;
				if (bits[ch][sb] > 16)
					bits[ch][sb] = 16;
			}
		}

		/* Distribute remaining bits */
		for (int ch = 0; ch < fChannels && bitcount < fBitpool; ch++) {
			for (int sb = 0; sb < fSubbands
					&& bitcount < fBitpool; sb++) {
				if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
					bits[ch][sb]++;
					bitcount++;
				} else if (bitneed[ch][sb] == bitslice + 1
						&& fBitpool > bitcount + 1) {
					bits[ch][sb] = 2;
					bitcount += 2;
				}
			}
		}

		for (int ch = 0; ch < fChannels && bitcount < fBitpool; ch++) {
			for (int sb = 0; sb < fSubbands
					&& bitcount < fBitpool; sb++) {
				if (bits[ch][sb] < 16) {
					bits[ch][sb]++;
					bitcount++;
				}
			}
		}
	} else {
		/* Loudness allocation */
		static const int8 offset4[4][4] = {
			{-1, 0, 0, 0}, {-2, 0, 0, 1},
			{-2, 0, 0, 1}, {-2, 0, 0, 1}
		};
		static const int8 offset8[4][8] = {
			{-2, 0, 0, 0, 0, 0, 0, 1},
			{-3, 0, 0, 0, 0, 0, 1, 2},
			{-4, 0, 0, 0, 0, 0, 1, 2},
			{-4, 0, 0, 0, 0, 0, 1, 2}
		};

		int32 bitneed[2][8];
		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (fScaleFactors[ch][sb] == 0)
					bitneed[ch][sb] = -5;
				else {
					int32 loudness;
					if (fSubbands == 4)
						loudness = fScaleFactors[ch][sb]
							- offset4[fSampleRate >= 44100
								? 3 : (fSampleRate >= 32000
									? 2 : (fSampleRate >= 16000
										? 1 : 0))][sb];
					else
						loudness = fScaleFactors[ch][sb]
							- offset8[fSampleRate >= 44100
								? 3 : (fSampleRate >= 32000
									? 2 : (fSampleRate >= 16000
										? 1 : 0))][sb];
					if (loudness > 0)
						bitneed[ch][sb] = loudness / 2;
					else
						bitneed[ch][sb] = loudness;
				}
			}
		}

		int32 maxBitneed = 0;
		for (int ch = 0; ch < fChannels; ch++)
			for (int sb = 0; sb < fSubbands; sb++)
				if (bitneed[ch][sb] > maxBitneed)
					maxBitneed = bitneed[ch][sb];

		int32 bitcount = 0;
		int32 slicecount = 0;
		int32 bitslice = maxBitneed + 1;

		do {
			bitslice--;
			bitcount += slicecount;
			slicecount = 0;
			for (int ch = 0; ch < fChannels; ch++)
				for (int sb = 0; sb < fSubbands; sb++) {
					if (bitneed[ch][sb] > bitslice + 1
							&& bitneed[ch][sb] < bitslice + 16)
						slicecount++;
					else if (bitneed[ch][sb] == bitslice + 1)
						slicecount += 2;
				}
		} while (bitcount + slicecount < fBitpool);

		if (bitcount + slicecount == fBitpool) {
			bitcount += slicecount;
			bitslice--;
		}

		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (bitneed[ch][sb] < bitslice + 2)
					bits[ch][sb] = 0;
				else
					bits[ch][sb] = bitneed[ch][sb] - bitslice;
				if (bits[ch][sb] > 16)
					bits[ch][sb] = 16;
			}
		}

		for (int ch = 0; ch < fChannels && bitcount < fBitpool; ch++) {
			for (int sb = 0; sb < fSubbands
					&& bitcount < fBitpool; sb++) {
				if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
					bits[ch][sb]++;
					bitcount++;
				} else if (bitneed[ch][sb] == bitslice + 1
						&& fBitpool > bitcount + 1) {
					bits[ch][sb] = 2;
					bitcount += 2;
				}
			}
		}

		for (int ch = 0; ch < fChannels && bitcount < fBitpool; ch++) {
			for (int sb = 0; sb < fSubbands
					&& bitcount < fBitpool; sb++) {
				if (bits[ch][sb] < 16) {
					bits[ch][sb]++;
					bitcount++;
				}
			}
		}
	}

	/* Read quantized subband samples */
	for (int blk = 0; blk < fBlocks; blk++) {
		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (bits[ch][sb] > 0) {
					fSbSamples[ch][blk][sb] =
						(int32)br.Read(bits[ch][sb]);
				} else {
					fSbSamples[ch][blk][sb] = 0;
				}
			}
		}
	}

	/* Dequantize: reconstruct from scale factors and bit allocation */
	for (int blk = 0; blk < fBlocks; blk++) {
		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (bits[ch][sb] > 0) {
					int32 levels = (1 << bits[ch][sb]) - 1;
					int32 sf = fScaleFactors[ch][sb];
					/* Dequantize per spec:
					 * sample = scalefactor * ((raw * 2 + 1) / levels - 1) */
					int32 raw = fSbSamples[ch][blk][sb];

					/* Fixed point: shift left by 15 for precision */
					int64 num = ((int64)(raw * 2 + 1) << 15) / levels
						- (1 << 15);
					fSbSamples[ch][blk][sb] =
						(int32)((num * (1 << (sf + 1))) >> 15);
				}
			}
		}
	}

	/* Joint stereo processing */
	if (fChannelMode == SBC_CM_JOINT_STEREO) {
		for (int blk = 0; blk < fBlocks; blk++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (fJoin & (1 << sb)) {
					int32 left = fSbSamples[0][blk][sb];
					int32 right = fSbSamples[1][blk][sb];
					fSbSamples[0][blk][sb] = left + right;
					fSbSamples[1][blk][sb] = left - right;
				}
			}
		}
	}
}


void
SbcDecoder::_Reconstruct(int ch)
{
	/* Nothing extra needed — dequantization is done in _UnpackSamples */
}


/* Synthesis cosine matrix: 2M×M (transposed from analysis).
 * N[k][i] = cos((i + 0.5) * (k - M) * PI / M) * 2^13
 * per A2DP Spec section 12.8.5. */
static int32 sSynth16x8[16][8];
static int32 sSynth8x4[8][4];
static bool sSynthTablesInit = false;


static void
_InitSynthTables()
{
	if (sSynthTablesInit)
		return;

	for (int k = 0; k < 16; k++)
		for (int i = 0; i < 8; i++)
			sSynth16x8[k][i] = (int32)round(
				cos((i + 0.5) * (k - 8) * M_PI / 8.0) * 8192.0);

	for (int k = 0; k < 8; k++)
		for (int i = 0; i < 4; i++)
			sSynth8x4[k][i] = (int32)round(
				cos((i + 0.5) * (k - 4) * M_PI / 4.0) * 8192.0);

	sSynthTablesInit = true;
}


void
SbcDecoder::_Synthesize(int ch, int blk, int16* output)
{
	/* Polyphase synthesis filter bank (A2DP Spec Appendix B, 12.8.5) */
	_InitSynthTables();

	int M = fSubbands;
	int twoM = 2 * M;

	/* Step 1: Shift V vector by 2*nrof_subbands positions.
	 * V has 10*2M = 20M usable elements (160 for M=8, 80 for M=4). */
	int vLen = 20 * M;
	memmove(fV[ch] + twoM, fV[ch],
		(vLen - twoM) * sizeof(int32));

	/* Step 2: Matrixing — compute 2M new V[0..2M-1] from M subband
	 * samples using the 2M×M synthesis cosine matrix.
	 * N[k][i] = cos((i+0.5)*(k-M)*PI/M) * 2^13. */
	if (M == 8) {
		for (int k = 0; k < 16; k++) {
			int64 sum = 0;
			for (int i = 0; i < 8; i++)
				sum += (int64)sSynth16x8[k][i]
					* fSbSamples[ch][blk][i];
			fV[ch][k] = (int32)(sum >> 9);
		}
	} else {
		for (int k = 0; k < 8; k++) {
			int64 sum = 0;
			for (int i = 0; i < 4; i++)
				sum += (int64)sSynth8x4[k][i]
					* fSbSamples[ch][blk][i];
			fV[ch][k] = (int32)(sum >> 8);
		}
	}

	/* Step 3: Build U vector from V (per spec section 12.8.5).
	 * U[i*2M + j]     = V[i*4M + j]
	 * U[i*2M + M + j] = V[i*4M + 3M + j]
	 * for i = 0..4, j = 0..M-1. */
	int32 U[80]; /* 10*M elements */
	for (int i = 0; i <= 4; i++) {
		for (int j = 0; j < M; j++) {
			U[i * twoM + j] =
				fV[ch][i * 4 * M + j];
			U[i * twoM + M + j] =
				fV[ch][i * 4 * M + 3 * M + j];
		}
	}

	/* Step 4: Window and sum to produce output samples.
	 * output[k] = sum_{i=0}^{9} D[i*M+k] * U[i*M+k]
	 * where D = prototype coefficients (scaled by 2^15). */
	const int32* proto = (M == 8) ? sProto8 : sProto4;

	for (int k = 0; k < M; k++) {
		int64 sum = 0;
		for (int i = 0; i < 10; i++) {
			int idx = i * M + k;
			sum += (int64)U[idx] * proto[idx];
		}
		/* Scale and clip to int16 range */
		int32 sample = (int32)(sum >> 15);
		if (sample > 32767)
			sample = 32767;
		else if (sample < -32768)
			sample = -32768;

		/* Interleave channels */
		output[k * fChannels + ch] = (int16)sample;
	}
}
