/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SbcEncoder — Subband Codec encoder per BT A2DP specification.
 * Reference: A2DP Spec Appendix B (SBC).
 */

#include "SbcEncoder.h"

#include <math.h>
#include <string.h>


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


/* Analysis filter bank prototype coefficients (same as decoder).
 * Scaled by 2^15. From A2DP Spec Table 12.8. */

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


/* Analysis cosine matrix for 8 subbands (M×2M = 8×16, scaled by 2^13).
 * M[sb][k] = cos((sb + 0.5) * (k - 8) * PI / 8) * 2^13
 * Precomputed from the SBC analysis filter bank specification. */
static int32 sAnalysis8x16[8][16];
static int32 sAnalysis4x8[4][8];
static bool sAnalysisTablesInit = false;


static void
_InitAnalysisTables()
{
	if (sAnalysisTablesInit)
		return;

	for (int sb = 0; sb < 8; sb++)
		for (int k = 0; k < 16; k++)
			sAnalysis8x16[sb][k] = (int32)round(
				cos((sb + 0.5) * (k - 8) * M_PI / 8.0) * 8192.0);

	for (int sb = 0; sb < 4; sb++)
		for (int k = 0; k < 8; k++)
			sAnalysis4x8[sb][k] = (int32)round(
				cos((sb + 0.5) * (k - 4) * M_PI / 4.0) * 8192.0);

	sAnalysisTablesInit = true;
}


/* CRC-8 for SBC (polynomial x^8 + x^4 + x^3 + x^2 + 1 = 0x1D) */
static uint8
_Crc8(const uint8* data, size_t len)
{
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


/* Bit writer for packing quantized samples */
struct BitWriter {
	uint8*	data;
	size_t	offset;		/* byte offset */
	uint8	bitPos;		/* bits written in current byte (0..7) */

	void Init(uint8* d, size_t byteOffset) {
		data = d;
		offset = byteOffset;
		bitPos = 0;
		data[offset] = 0;
	}

	void Write(uint32 val, uint8 bits) {
		while (bits > 0) {
			uint8 space = 8 - bitPos;
			uint8 put = (bits < space) ? bits : space;
			uint8 shift = bits - put;
			data[offset] |=
				((val >> shift) & ((1 << put) - 1)) << (space - put);
			bitPos += put;
			bits -= put;
			if (bitPos == 8) {
				offset++;
				data[offset] = 0;
				bitPos = 0;
			}
		}
	}
};


SbcEncoder::SbcEncoder()
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
	memset(fX, 0, sizeof(fX));
}


SbcEncoder::~SbcEncoder()
{
}


status_t
SbcEncoder::Configure(uint32 sampleRate, uint8 channels, uint8 blocks,
	uint8 subbands, uint8 channelMode, uint8 allocMethod, uint8 bitpool)
{
	if (subbands != 4 && subbands != 8)
		return B_BAD_VALUE;
	if (blocks != 4 && blocks != 8 && blocks != 12 && blocks != 16)
		return B_BAD_VALUE;
	if (channelMode > 3)
		return B_BAD_VALUE;
	if (sampleRate != 16000 && sampleRate != 32000
			&& sampleRate != 44100 && sampleRate != 48000)
		return B_BAD_VALUE;

	uint8 expectedCh = (channelMode == SBC_CM_MONO) ? 1 : 2;
	if (channels != expectedCh)
		return B_BAD_VALUE;

	fSampleRate = sampleRate;
	fChannels = channels;
	fBlocks = blocks;
	fSubbands = subbands;
	fChannelMode = channelMode;
	fAllocMethod = allocMethod;
	fBitpool = bitpool;

	memset(fX, 0, sizeof(fX));

	return B_OK;
}


uint16
SbcEncoder::FrameLength() const
{
	uint16 len = 4; /* header: syncword + byte1 + byte2 + CRC */

	if (fChannelMode == SBC_CM_MONO)
		len += (fBlocks * fChannels * fBitpool
			+ 4 * fSubbands * fChannels + 7) / 8;
	else if (fChannelMode == SBC_CM_DUAL_CHANNEL)
		len += (fBlocks * 2 * fBitpool
			+ 4 * fSubbands * 2 + 7) / 8;
	else if (fChannelMode == SBC_CM_STEREO)
		len += (fBlocks * fBitpool
			+ 4 * fSubbands * 2 + 7) / 8;
	else /* joint stereo */
		len += (fBlocks * fBitpool + fSubbands
			+ 4 * fSubbands * 2 + 7) / 8;

	return len;
}


ssize_t
SbcEncoder::EncodeFrame(const int16* input, uint8* output,
	size_t maxOutputLen)
{
	uint16 frameLen = FrameLength();
	if (maxOutputLen < frameLen)
		return -1;

	/* Step 1: Analysis filter bank — convert PCM to subband samples */
	for (int blk = 0; blk < fBlocks; blk++) {
		for (int ch = 0; ch < fChannels; ch++) {
			_Analyze(ch, blk,
				input + blk * fSubbands * fChannels);
		}
	}

	/* Step 2: Joint stereo processing (if applicable) */
	fJoin = 0;
	if (fChannelMode == SBC_CM_JOINT_STEREO)
		_JointStereoProcess();

	/* Step 3: Compute scale factors from subband samples */
	_ComputeScaleFactors();

	/* Step 4: Compute bit allocation */
	int32 bits[2][8];
	memset(bits, 0, sizeof(bits));
	_ComputeBitAllocation(bits);

	/* Step 5: Build output frame */
	memset(output, 0, frameLen);
	_BuildHeader(output);
	_QuantizeAndPack(output, bits);

	/* Step 6: CRC (covers bytes 1-2 of header) */
	output[3] = _Crc8(output + 1, 2);

	return frameLen;
}


void
SbcEncoder::_BuildHeader(uint8* output)
{
	output[0] = 0x9C; /* syncword */

	/* Byte 1: sampling_frequency(2) | blocks(2) | channel_mode(2) |
	 *         allocation_method(1) | subbands(1) */
	uint8 freqBits;
	switch (fSampleRate) {
		case 16000: freqBits = 0; break;
		case 32000: freqBits = 1; break;
		case 44100: freqBits = 2; break;
		default:    freqBits = 3; break; /* 48000 */
	}

	uint8 blockBits;
	switch (fBlocks) {
		case 4:  blockBits = 0; break;
		case 8:  blockBits = 1; break;
		case 12: blockBits = 2; break;
		default: blockBits = 3; break; /* 16 */
	}

	output[1] = (freqBits << 6) | (blockBits << 4)
		| (fChannelMode << 2)
		| (fAllocMethod << 1)
		| ((fSubbands == 8) ? 1 : 0);

	/* Byte 2: bitpool */
	output[2] = fBitpool;

	/* Byte 3: CRC — filled in later */
}


void
SbcEncoder::_Analyze(int ch, int blk, const int16* input)
{
	_InitAnalysisTables();

	int M = fSubbands;

	/* Step 1: Shift X buffer by M positions */
	memmove(fX[ch] + M, fX[ch],
		(10 * M - M) * sizeof(int32));

	/* Step 2: Input new PCM samples into X[0..M-1]
	 * Note: samples are in reverse order per SBC spec */
	for (int i = 0; i < M; i++) {
		fX[ch][M - 1 - i] =
			(int32)input[i * fChannels + ch];
	}

	/* Step 3: Window by prototype coefficients → Z */
	int32 Z[80];
	const int32* proto = (M == 8) ? sProto8 : sProto4;
	int protoLen = M * 10;

	for (int i = 0; i < protoLen; i++)
		Z[i] = (int32)(((int64)fX[ch][i] * proto[i]) >> 15);

	/* Step 4: Partial calculation — fold into 2M groups.
	 * Y[j] = sum_{k=0}^{4} Z[j + k*2M]  for j = 0..2M-1 */
	int32 Y[16];
	int twoM = 2 * M;
	for (int j = 0; j < twoM; j++) {
		int64 sum = 0;
		for (int k = 0; k < 5; k++) {
			int idx = j + k * twoM;
			if (idx < protoLen)
				sum += Z[idx];
		}
		Y[j] = (int32)sum;
	}

	/* Step 5: Apply M×2M analysis cosine matrix.
	 * S[sb] = sum_{k=0}^{2M-1} M[sb][k] * Y[k]
	 * Matrix scaled by 2^13. Sum over 2M terms (vs M in synthesis >>14),
	 * so shift by 15 to match decoder's subband sample scale. */
	if (M == 8) {
		for (int sb = 0; sb < 8; sb++) {
			int64 sum = 0;
			for (int k = 0; k < 16; k++)
				sum += (int64)sAnalysis8x16[sb][k] * Y[k];
			fSbSamples[ch][blk][sb] = (int32)(sum >> 15);
		}
	} else {
		for (int sb = 0; sb < 4; sb++) {
			int64 sum = 0;
			for (int k = 0; k < 8; k++)
				sum += (int64)sAnalysis4x8[sb][k] * Y[k];
			fSbSamples[ch][blk][sb] = (int32)(sum >> 15);
		}
	}
}


void
SbcEncoder::_JointStereoProcess()
{
	/* For each subband (except last), decide M/S vs L/R.
	 * Use the subband that gives smaller scale factors. */
	fJoin = 0;

	for (int sb = 0; sb < fSubbands - 1; sb++) {
		int32 maxLR = 0;
		int32 maxMS = 0;

		for (int blk = 0; blk < fBlocks; blk++) {
			int32 left = fSbSamples[0][blk][sb];
			int32 right = fSbSamples[1][blk][sb];
			int32 mid = (left + right) / 2;
			int32 side = (left - right) / 2;

			int32 absL = (left < 0) ? -left : left;
			int32 absR = (right < 0) ? -right : right;
			int32 absM = (mid < 0) ? -mid : mid;
			int32 absS = (side < 0) ? -side : side;

			if (absL > maxLR) maxLR = absL;
			if (absR > maxLR) maxLR = absR;
			if (absM > maxMS) maxMS = absM;
			if (absS > maxMS) maxMS = absS;
		}

		if (maxMS < maxLR) {
			/* M/S is better — use joint stereo for this subband */
			fJoin |= (1 << sb);
			for (int blk = 0; blk < fBlocks; blk++) {
				int32 left = fSbSamples[0][blk][sb];
				int32 right = fSbSamples[1][blk][sb];
				fSbSamples[0][blk][sb] = (left + right) / 2;
				fSbSamples[1][blk][sb] = (left - right) / 2;
			}
		}
	}
}


void
SbcEncoder::_ComputeScaleFactors()
{
	for (int ch = 0; ch < fChannels; ch++) {
		for (int sb = 0; sb < fSubbands; sb++) {
			/* Find maximum absolute value in this subband */
			int32 maxVal = 0;
			for (int blk = 0; blk < fBlocks; blk++) {
				int32 abs = fSbSamples[ch][blk][sb];
				if (abs < 0) abs = -abs;
				if (abs > maxVal) maxVal = abs;
			}

			/* Scale factor = floor(log2(maxVal)) or 0 if silent */
			if (maxVal == 0) {
				fScaleFactors[ch][sb] = 0;
			} else {
				int32 sf = 0;
				int32 test = maxVal;
				while (test > 1) {
					test >>= 1;
					sf++;
				}
				fScaleFactors[ch][sb] = sf;
			}
		}
	}
}


void
SbcEncoder::_ComputeBitAllocation(int32 bits[2][8])
{
	memset(bits, 0, sizeof(int32) * 2 * 8);

	if (fAllocMethod == SBC_AM_SNR) {
		/* SNR allocation */
		int32 bitneed[2][8];
		for (int ch = 0; ch < fChannels; ch++)
			for (int sb = 0; sb < fSubbands; sb++)
				bitneed[ch][sb] = fScaleFactors[ch][sb];

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
		int freqIdx = (fSampleRate >= 44100) ? 3
			: ((fSampleRate >= 32000) ? 2
				: ((fSampleRate >= 16000) ? 1 : 0));

		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (fScaleFactors[ch][sb] == 0)
					bitneed[ch][sb] = -5;
				else {
					int32 loudness;
					if (fSubbands == 4)
						loudness = fScaleFactors[ch][sb]
							- offset4[freqIdx][sb];
					else
						loudness = fScaleFactors[ch][sb]
							- offset8[freqIdx][sb];
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
}


void
SbcEncoder::_QuantizeAndPack(uint8* output, const int32 bits[2][8])
{
	BitWriter bw;

	/* Start after header (4 bytes) */
	size_t byteOff = 4;

	/* For joint stereo, write join flags first */
	if (fChannelMode == SBC_CM_JOINT_STEREO) {
		bw.Init(output, byteOff);
		for (int sb = 0; sb < fSubbands - 1; sb++)
			bw.Write((fJoin >> sb) & 1, 1);
		/* Padding bit for 4 subbands */
		if (fSubbands == 4)
			bw.Write(0, 1);
	} else {
		bw.Init(output, byteOff);
	}

	/* Write scale factors: 4 bits each */
	for (int ch = 0; ch < fChannels; ch++)
		for (int sb = 0; sb < fSubbands; sb++)
			bw.Write(fScaleFactors[ch][sb] & 0x0F, 4);

	/* Quantize and write subband samples */
	for (int blk = 0; blk < fBlocks; blk++) {
		for (int ch = 0; ch < fChannels; ch++) {
			for (int sb = 0; sb < fSubbands; sb++) {
				if (bits[ch][sb] > 0) {
					int32 nLevels = (1 << bits[ch][sb]) - 1;
					int32 sf = fScaleFactors[ch][sb];
					int32 sbSample = fSbSamples[ch][blk][sb];

					/* Quantize per spec:
					 * quantized = floor((sample / scalefactor + 1)
					 *             * levels / 2) */
					int64 scalefactor = (int64)1 << (sf + 1);
					int64 quantized;
					if (scalefactor == 0) {
						quantized = 0;
					} else {
						quantized = (((int64)sbSample << 15)
							/ scalefactor + (1 << 15))
							* nLevels / (1 << 16);
					}

					/* Clamp */
					if (quantized < 0)
						quantized = 0;
					if (quantized > nLevels)
						quantized = nLevels;

				bw.Write((uint32)quantized, bits[ch][sb]);
				}
			}
		}
	}
}
