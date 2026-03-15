/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * A2dpPlayerWindow — GUI for A2DP audio streaming.
 */
#ifndef A2DP_PLAYER_WINDOW_H
#define A2DP_PLAYER_WINDOW_H

#include <Button.h>
#include <SoundPlayer.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>

namespace Bluetooth {
class A2dpSink;
}


class A2dpPlayerWindow : public BWindow {
public:
						A2dpPlayerWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~A2dpPlayerWindow();

	virtual	void		MessageReceived(BMessage* message);
	virtual	bool		QuitRequested();

private:
	static	int32		_ConnectThreadEntry(void* arg);
			void		_ConnectThread();
	static	void		_AudioDataCallback(const int16* pcm,
							size_t sampleCount, uint32 sampleRate,
							uint8 channels, void* cookie);
	static	void		_SoundPlayerCallback(void* cookie,
							void* buffer, size_t size,
							const media_raw_audio_format& format);
			void		_UpdateButtons();

	bdaddr_t				fAddress;
	BString					fDeviceName;
	Bluetooth::A2dpSink*	fSink;
	BSoundPlayer*			fPlayer;
	thread_id				fConnectThread;

	// Ring buffer
	enum { kRingBufSamples = 48000 * 2 };
	int16					fRingBuf[kRingBufSamples];
	volatile uint32			fRingWrite;
	volatile uint32			fRingRead;
	volatile uint32			fSampleRate;
	volatile uint8			fChannels;

	// UI
	BStringView*			fStatusLabel;
	BStringView*			fSampleRateLabel;
	BStringView*			fChannelsLabel;
	BButton*				fConnectButton;
	BButton*				fDisconnectButton;
	BButton*				fStartButton;
	BButton*				fStopButton;
	BButton*				fCloseButton;
};


#endif // A2DP_PLAYER_WINDOW_H
