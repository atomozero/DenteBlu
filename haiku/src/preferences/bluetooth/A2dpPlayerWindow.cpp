/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpPlayerWindow — GUI for A2DP audio streaming.
 */

#include "A2dpPlayerWindow.h"

#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <SeparatorView.h>

#include <bluetooth/A2dpSink.h>
#include <bluetooth/bdaddrUtils.h>

#include <string.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "A2DP Player"

static const uint32 kMsgA2dpConnect    = 'aCon';
static const uint32 kMsgA2dpDisconnect = 'aDis';
static const uint32 kMsgA2dpStart      = 'aStr';
static const uint32 kMsgA2dpStop       = 'aStp';
static const uint32 kMsgA2dpConnected  = 'aCnd';
static const uint32 kMsgA2dpFailed     = 'aFal';
static const uint32 kMsgA2dpClose      = 'aCls';


A2dpPlayerWindow::A2dpPlayerWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 600, 500),
		B_TRANSLATE("Audio Streaming"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fSink(NULL),
	fPlayer(NULL),
	fConnectThread(-1),
	fRingWrite(0),
	fRingRead(0),
	fSampleRate(44100),
	fChannels(2)
{
	bdaddrUtils::Copy(fAddress, address);
	memset(fRingBuf, 0, sizeof(fRingBuf));

	BString title;
	title.SetToFormat(B_TRANSLATE("Audio Streaming \xE2\x80\x94 %s"),
		deviceName);
	SetTitle(title.String());

	BString addrStr;
	addrStr.SetToFormat(B_TRANSLATE("Device: %s"),
		bdaddrUtils::ToString(fAddress).String());
	BStringView* addrLabel = new BStringView("addr", addrStr.String());

	fStatusLabel = new BStringView("status",
		B_TRANSLATE("Status: Disconnected"));
	fSampleRateLabel = new BStringView("srate",
		B_TRANSLATE("Sample Rate: --"));
	fChannelsLabel = new BStringView("channels",
		B_TRANSLATE("Channels: --"));

	fConnectButton = new BButton("connect", B_TRANSLATE("Connect"),
		new BMessage(kMsgA2dpConnect));
	fDisconnectButton = new BButton("disconnect",
		B_TRANSLATE("Disconnect"),
		new BMessage(kMsgA2dpDisconnect));
	fStartButton = new BButton("start", B_TRANSLATE("Start"),
		new BMessage(kMsgA2dpStart));
	fStopButton = new BButton("stop", B_TRANSLATE("Stop"),
		new BMessage(kMsgA2dpStop));
	fCloseButton = new BButton("close", B_TRANSLATE("Close"),
		new BMessage(kMsgA2dpClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(addrLabel)
		.Add(fStatusLabel)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fSampleRateLabel)
		.Add(fChannelsLabel)
		.AddStrut(B_USE_DEFAULT_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(fConnectButton)
			.Add(fDisconnectButton)
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(fStartButton)
			.Add(fStopButton)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCloseButton)
		.End()
	.End();

	_UpdateButtons();
}


A2dpPlayerWindow::~A2dpPlayerWindow()
{
	if (fPlayer != NULL) {
		fPlayer->Stop();
		delete fPlayer;
	}
	if (fSink != NULL) {
		if (fSink->IsStreaming())
			fSink->StopStream();
		if (fSink->IsConnected())
			fSink->Disconnect();
		delete fSink;
	}
	if (fConnectThread >= 0) {
		status_t exitVal;
		wait_for_thread(fConnectThread, &exitVal);
	}
}


bool
A2dpPlayerWindow::QuitRequested()
{
	return true;
}


void
A2dpPlayerWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgA2dpConnect:
		{
			if (fConnectThread >= 0)
				break;
			fConnectButton->SetEnabled(false);
			fStatusLabel->SetText(
				B_TRANSLATE("Status: Connecting" B_UTF8_ELLIPSIS));
			fConnectThread = spawn_thread(_ConnectThreadEntry,
				"a2dp_connect", B_NORMAL_PRIORITY, this);
			if (fConnectThread >= 0)
				resume_thread(fConnectThread);
			break;
		}

		case kMsgA2dpConnected:
		{
			fConnectThread = -1;
			fStatusLabel->SetText(B_TRANSLATE("Status: Connected"));

			BString srStr;
			srStr.SetToFormat(B_TRANSLATE("Sample Rate: %u Hz"),
				(unsigned)fSampleRate);
			fSampleRateLabel->SetText(srStr.String());

			BString chStr;
			chStr.SetToFormat(B_TRANSLATE("Channels: %u"),
				(unsigned)fChannels);
			fChannelsLabel->SetText(chStr.String());

			_UpdateButtons();
			break;
		}

		case kMsgA2dpFailed:
		{
			fConnectThread = -1;
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusLabel->SetText(text);
			else
				fStatusLabel->SetText(
					B_TRANSLATE("Status: Connection failed"));
			_UpdateButtons();
			break;
		}

		case kMsgA2dpDisconnect:
		{
			if (fPlayer != NULL) {
				fPlayer->Stop();
				delete fPlayer;
				fPlayer = NULL;
			}
			if (fSink != NULL) {
				if (fSink->IsStreaming())
					fSink->StopStream();
				fSink->Disconnect();
				delete fSink;
				fSink = NULL;
			}
			fRingWrite = 0;
			fRingRead = 0;
			fStatusLabel->SetText(B_TRANSLATE("Status: Disconnected"));
			fSampleRateLabel->SetText(B_TRANSLATE("Sample Rate: --"));
			fChannelsLabel->SetText(B_TRANSLATE("Channels: --"));
			_UpdateButtons();
			break;
		}

		case kMsgA2dpStart:
		{
			if (fSink == NULL || !fSink->IsConnected())
				break;

			status_t err = fSink->StartStream();
			if (err != B_OK) {
				BString errStr;
				errStr.SetToFormat(
					B_TRANSLATE("Status: Start failed: %s"),
					strerror(err));
				fStatusLabel->SetText(errStr.String());
				break;
			}

			// Wait briefly for first audio data
			snooze(200000);

			media_raw_audio_format format;
			format.frame_rate = fSampleRate;
			format.channel_count = fChannels;
			format.format = media_raw_audio_format::B_AUDIO_SHORT;
			format.byte_order = B_MEDIA_HOST_ENDIAN;
			format.buffer_size = 4096;

			fPlayer = new BSoundPlayer(&format, "A2DP Audio",
				_SoundPlayerCallback, NULL, this);
			fPlayer->Start();
			fPlayer->SetHasData(true);

			fStatusLabel->SetText(B_TRANSLATE("Status: Streaming"));
			_UpdateButtons();
			break;
		}

		case kMsgA2dpStop:
		{
			if (fPlayer != NULL) {
				fPlayer->Stop();
				delete fPlayer;
				fPlayer = NULL;
			}
			if (fSink != NULL && fSink->IsStreaming())
				fSink->StopStream();
			fRingWrite = 0;
			fRingRead = 0;
			fStatusLabel->SetText(B_TRANSLATE("Status: Connected"));
			_UpdateButtons();
			break;
		}

		case kMsgA2dpClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


/*static*/ int32
A2dpPlayerWindow::_ConnectThreadEntry(void* arg)
{
	static_cast<A2dpPlayerWindow*>(arg)->_ConnectThread();
	return 0;
}


void
A2dpPlayerWindow::_ConnectThread()
{
	Bluetooth::A2dpSink* sink = new Bluetooth::A2dpSink;
	sink->SetAudioCallback(_AudioDataCallback, this);

	status_t err = sink->Connect(fAddress);
	if (err != B_OK) {
		delete sink;
		BMessage fail(kMsgA2dpFailed);
		BString errText;
		errText.SetToFormat(
			B_TRANSLATE("Status: Connection failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	fSink = sink;
	fSampleRate = sink->SampleRate();
	fChannels = sink->Channels();

	PostMessage(new BMessage(kMsgA2dpConnected));
}


/*static*/ void
A2dpPlayerWindow::_AudioDataCallback(const int16* pcm,
	size_t sampleCount, uint32 sampleRate, uint8 channels, void* cookie)
{
	A2dpPlayerWindow* window = static_cast<A2dpPlayerWindow*>(cookie);
	window->fSampleRate = sampleRate;
	window->fChannels = channels;

	size_t totalSamples = sampleCount * channels;
	for (size_t i = 0; i < totalSamples; i++) {
		uint32 next = (window->fRingWrite + 1) % kRingBufSamples;
		if (next == window->fRingRead)
			break;
		window->fRingBuf[window->fRingWrite] = pcm[i];
		window->fRingWrite = next;
	}
}


/*static*/ void
A2dpPlayerWindow::_SoundPlayerCallback(void* cookie, void* buffer,
	size_t size, const media_raw_audio_format& format)
{
	A2dpPlayerWindow* window = static_cast<A2dpPlayerWindow*>(cookie);
	int16* out = (int16*)buffer;
	uint8 channels = window->fChannels;
	if (channels == 0)
		channels = 2;
	size_t frames = size / (sizeof(int16) * channels);

	for (size_t f = 0; f < frames; f++) {
		for (uint8 ch = 0; ch < channels; ch++) {
			if (window->fRingRead != window->fRingWrite) {
				out[f * channels + ch]
					= window->fRingBuf[window->fRingRead];
				window->fRingRead
					= (window->fRingRead + 1) % kRingBufSamples;
			} else {
				out[f * channels + ch] = 0;
			}
		}
	}
}


void
A2dpPlayerWindow::_UpdateButtons()
{
	bool connected = (fSink != NULL && fSink->IsConnected());
	bool streaming = (fSink != NULL && fSink->IsStreaming());

	fConnectButton->SetEnabled(!connected && fConnectThread < 0);
	fDisconnectButton->SetEnabled(connected);
	fStartButton->SetEnabled(connected && !streaming);
	fStopButton->SetEnabled(streaming);
}
