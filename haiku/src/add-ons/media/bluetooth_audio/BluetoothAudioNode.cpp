/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "BluetoothAudioNode.h"

#include "BluetoothAudioAddOn.h"

#include <Buffer.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include <bluetooth/A2dpSource.h>
#include <bluetooth/bdaddrUtils.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


#define TRACE(fmt, ...) fprintf(stderr, "BluetoothAudioNode: " fmt, \
	##__VA_ARGS__)


static const bigtime_t kBluetoothLatency = 50000; // 50 ms


BluetoothAudioNode::BluetoothAudioNode(BluetoothAudioAddOn* addOn)
	:
	BMediaNode("Bluetooth Audio Output"),
	BMediaEventLooper(),
	BBufferConsumer(B_MEDIA_RAW_AUDIO),
	BControllable(),
	fAddOn(addOn),
	fInputConnected(false),
	fVolume(1.0f),
	fVolumeLastChange(0),
	fA2dp(NULL)
{
	memset(&fInput, 0, sizeof(fInput));
}


BluetoothAudioNode::~BluetoothAudioNode()
{
	Quit();
	delete fA2dp;
}


BMediaAddOn*
BluetoothAudioNode::AddOn(int32* _internalId) const
{
	if (_internalId != NULL)
		*_internalId = 0;
	return fAddOn;
}


void
BluetoothAudioNode::NodeRegistered()
{
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.node = Node();
	fInput.format.type = B_MEDIA_RAW_AUDIO;
	fInput.format.u.raw_audio = media_raw_audio_format::wildcard;
	strcpy(fInput.name, "Bluetooth Audio Input");

	SetPriority(B_URGENT_PRIORITY);
	SetEventLatency(kBluetoothLatency);

	_InitParameterWeb();

	Run();
}


status_t
BluetoothAudioNode::HandleMessage(int32 message, const void* data,
	size_t size)
{
	if (BBufferConsumer::HandleMessage(message, data, size) == B_OK
		|| BControllable::HandleMessage(message, data, size) == B_OK
		|| BMediaEventLooper::HandleMessage(message, data, size) == B_OK) {
		return B_OK;
	}
	return BMediaNode::HandleMessage(message, data, size);
}


void
BluetoothAudioNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	switch (event->type) {
		case BTimedEventQueue::B_START:
		{
			TRACE("B_START\n");
			if (fA2dp == NULL) {
				bdaddr_t address;
				if (_ReadDeviceAddress(&address) != B_OK) {
					TRACE("no device address configured\n");
					break;
				}
				fA2dp = new(std::nothrow) Bluetooth::A2dpSource;
				if (fA2dp == NULL)
					break;

				status_t err = fA2dp->Connect(address);
				if (err != B_OK) {
					TRACE("A2DP Connect failed: %s\n", strerror(err));
					delete fA2dp;
					fA2dp = NULL;
					break;
				}
			}
			if (fA2dp != NULL && !fA2dp->IsStreaming()) {
				status_t err = fA2dp->StartStream();
				if (err != B_OK)
					TRACE("StartStream failed: %s\n", strerror(err));
			}
			break;
		}

		case BTimedEventQueue::B_STOP:
		{
			TRACE("B_STOP\n");
			if (fA2dp != NULL) {
				if (fA2dp->IsStreaming())
					fA2dp->StopStream();
				fA2dp->Disconnect();
				delete fA2dp;
				fA2dp = NULL;
			}
			EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
				BTimedEventQueue::B_HANDLE_BUFFER);
			break;
		}

		case BTimedEventQueue::B_HANDLE_BUFFER:
			// handled directly in BufferReceived
			break;

		default:
			break;
	}
}


/*
 * AcceptFormat — accetta solo 44100 Hz, stereo, host endian.
 * Formati ammessi: B_AUDIO_SHORT e B_AUDIO_FLOAT.
 * Se il formato è wildcard, lo specializza a 44100/stereo/SHORT.
 */
status_t
BluetoothAudioNode::AcceptFormat(const media_destination& destination,
	media_format* format)
{
	if (format->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;

	media_raw_audio_format& raw = format->u.raw_audio;

	/* frame rate: deve essere 44100 o wildcard */
	if (raw.frame_rate != 0
		&& raw.frame_rate != media_raw_audio_format::wildcard.frame_rate
		&& raw.frame_rate != 44100.0f) {
		return B_MEDIA_BAD_FORMAT;
	}

	/* canali: deve essere 2 o wildcard */
	if (raw.channel_count != 0
		&& raw.channel_count
			!= media_raw_audio_format::wildcard.channel_count
		&& raw.channel_count != 2) {
		return B_MEDIA_BAD_FORMAT;
	}

	/* formato campione: SHORT, FLOAT o wildcard */
	if (raw.format != 0
		&& raw.format != media_raw_audio_format::wildcard.format
		&& raw.format != media_raw_audio_format::B_AUDIO_SHORT
		&& raw.format != media_raw_audio_format::B_AUDIO_FLOAT) {
		return B_MEDIA_BAD_FORMAT;
	}

	/* byte order: host endian o wildcard */
	if (raw.byte_order != 0
		&& raw.byte_order != media_raw_audio_format::wildcard.byte_order
		&& raw.byte_order != B_MEDIA_HOST_ENDIAN) {
		return B_MEDIA_BAD_FORMAT;
	}

	/* Specializza i campi wildcard */
	if (raw.frame_rate == 0
		|| raw.frame_rate == media_raw_audio_format::wildcard.frame_rate) {
		raw.frame_rate = 44100.0f;
	}
	if (raw.channel_count == 0
		|| raw.channel_count
			== media_raw_audio_format::wildcard.channel_count) {
		raw.channel_count = 2;
	}
	if (raw.format == 0
		|| raw.format == media_raw_audio_format::wildcard.format) {
		raw.format = media_raw_audio_format::B_AUDIO_SHORT;
	}
	if (raw.byte_order == 0
		|| raw.byte_order == media_raw_audio_format::wildcard.byte_order) {
		raw.byte_order = B_MEDIA_HOST_ENDIAN;
	}
	if (raw.buffer_size == 0
		|| raw.buffer_size == media_raw_audio_format::wildcard.buffer_size) {
		raw.buffer_size = 4096;
	}

	return B_OK;
}


status_t
BluetoothAudioNode::GetNextInput(int32* cookie, media_input* _input)
{
	if (*cookie != 0)
		return B_BAD_INDEX;

	*_input = fInput;
	(*cookie)++;
	return B_OK;
}


void
BluetoothAudioNode::DisposeInputCookie(int32 cookie)
{
}


void
BluetoothAudioNode::BufferReceived(BBuffer* buffer)
{
	if (RunState() != BMediaEventLooper::B_STARTED
		|| fA2dp == NULL || !fA2dp->IsStreaming()) {
		buffer->Recycle();
		return;
	}

	size_t size = buffer->SizeUsed();
	const media_raw_audio_format& raw = fInput.format.u.raw_audio;

	if (raw.format == media_raw_audio_format::B_AUDIO_FLOAT) {
		/* Converti float → int16 con volume e clamping */
		const float* src = (const float*)buffer->Data();
		size_t sampleCount = size / sizeof(float);
		int16* pcm = (int16*)malloc(sampleCount * sizeof(int16));
		if (pcm != NULL) {
			ConvertFloatToInt16(src, pcm, sampleCount, fVolume);
			size_t framesPerChannel = sampleCount / raw.channel_count;
			fA2dp->SendAudio(pcm, framesPerChannel);
			free(pcm);
		}
	} else {
		/* Già int16 — applica solo volume se != 1.0 */
		int16* pcm = (int16*)buffer->Data();
		size_t sampleCount = size / sizeof(int16);

		if (fVolume < 0.999f || fVolume > 1.001f) {
			/* Copia per non modificare il buffer originale */
			int16* tmp = (int16*)malloc(sampleCount * sizeof(int16));
			if (tmp != NULL) {
				for (size_t i = 0; i < sampleCount; i++) {
					float s = pcm[i] * fVolume;
					if (s > 32767.0f)
						s = 32767.0f;
					else if (s < -32767.0f)
						s = -32767.0f;
					tmp[i] = (int16)s;
				}
				size_t framesPerChannel = sampleCount / raw.channel_count;
				fA2dp->SendAudio(tmp, framesPerChannel);
				free(tmp);
			}
		} else {
			size_t framesPerChannel = sampleCount / raw.channel_count;
			fA2dp->SendAudio(pcm, framesPerChannel);
		}
	}

	buffer->Recycle();
}


void
BluetoothAudioNode::ProducerDataStatus(const media_destination& forWhom,
	int32 status, bigtime_t atPerformanceTime)
{
	TRACE("ProducerDataStatus: %d\n", (int)status);
}


status_t
BluetoothAudioNode::GetLatencyFor(const media_destination& forWhom,
	bigtime_t* _latency, media_node_id* _timesource)
{
	*_latency = kBluetoothLatency;
	*_timesource = TimeSource()->ID();
	return B_OK;
}


status_t
BluetoothAudioNode::Connected(const media_source& producer,
	const media_destination& where, const media_format& withFormat,
	media_input* _input)
{
	fInput.source = producer;
	fInput.format = withFormat;
	fInputConnected = true;

	*_input = fInput;
	return B_OK;
}


void
BluetoothAudioNode::Disconnected(const media_source& producer,
	const media_destination& where)
{
	fInput.source = media_source::null;
	fInputConnected = false;

	fInput.format.type = B_MEDIA_RAW_AUDIO;
	fInput.format.u.raw_audio = media_raw_audio_format::wildcard;
}


status_t
BluetoothAudioNode::FormatChanged(const media_source& producer,
	const media_destination& consumer, int32 changeTag,
	const media_format& format)
{
	return B_ERROR;
}


/* BControllable */

status_t
BluetoothAudioNode::GetParameterValue(int32 id, bigtime_t* lastChange,
	void* value, size_t* ioSize)
{
	if (id != kParamVolume)
		return B_BAD_VALUE;

	if (*ioSize < sizeof(float))
		return B_NO_MEMORY;

	*(float*)value = fVolume;
	*lastChange = fVolumeLastChange;
	*ioSize = sizeof(float);
	return B_OK;
}


void
BluetoothAudioNode::SetParameterValue(int32 id, bigtime_t when,
	const void* value, size_t size)
{
	if (id != kParamVolume || size < sizeof(float))
		return;

	float v = *(const float*)value;
	if (v < 0.0f)
		v = 0.0f;
	else if (v > 1.0f)
		v = 1.0f;

	fVolume = v;
	fVolumeLastChange = when;

	BroadcastNewParameterValue(when, kParamVolume, &fVolume, sizeof(float));
}


/* static */
void
BluetoothAudioNode::ConvertFloatToInt16(const float* src, int16* dst,
	size_t sampleCount, float volume)
{
	for (size_t i = 0; i < sampleCount; i++) {
		float s = src[i] * volume * 32767.0f;
		if (s > 32767.0f)
			s = 32767.0f;
		else if (s < -32767.0f)
			s = -32767.0f;
		dst[i] = (int16)s;
	}
}


void
BluetoothAudioNode::_InitParameterWeb()
{
	BParameterWeb* web = new BParameterWeb;
	BParameterGroup* group = web->MakeGroup("Bluetooth Audio");

	group->MakeContinuousParameter(kParamVolume, B_MEDIA_RAW_AUDIO,
		"Volume", B_GAIN, "", 0.0f, 1.0f, 0.05f);

	SetParameterWeb(web);
}


status_t
BluetoothAudioNode::_ReadDeviceAddress(bdaddr_t* address)
{
	BPath path;
	status_t err = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (err != B_OK)
		return err;

	path.Append("bluetooth_audio_device");

	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return file.InitCheck();

	char buf[32];
	ssize_t bytesRead = file.Read(buf, sizeof(buf) - 1);
	if (bytesRead <= 0)
		return B_ERROR;

	buf[bytesRead] = '\0';

	/* Rimuovi newline/spazi finali */
	char* end = buf + bytesRead - 1;
	while (end >= buf && (*end == '\n' || *end == '\r' || *end == ' '))
		*end-- = '\0';

	*address = bdaddrUtils::FromString(buf);
	if (bdaddrUtils::Compare(*address, bdaddrUtils::NullAddress()))
		return B_BAD_VALUE;

	return B_OK;
}
