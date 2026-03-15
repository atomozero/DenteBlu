/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpCallWindow — GUI for Hands-Free Profile call control.
 */
#ifndef HFP_CALL_WINDOW_H
#define HFP_CALL_WINDOW_H

#include <Button.h>
#include <Slider.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>

namespace Bluetooth {
class HfpClient;
}


class HfpCallWindow : public BWindow {
public:
						HfpCallWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~HfpCallWindow();

	virtual	void		MessageReceived(BMessage* message);

private:
	static	int32		_ConnectThreadEntry(void* arg);
			void		_ConnectThread();
			void		_UpdateButtons();

	bdaddr_t				fAddress;
	BString					fDeviceName;
	Bluetooth::HfpClient*	fHfp;
	thread_id				fConnectThread;

	// UI
	BStringView*			fStatusLabel;
	BTextControl*			fNumberField;
	BButton*				fConnectButton;
	BButton*				fDisconnectButton;
	BButton*				fDialButton;
	BButton*				fAnswerButton;
	BButton*				fHangUpButton;
	BSlider*				fSpeakerSlider;
	BSlider*				fMicSlider;
	BButton*				fCloseButton;
};


#endif // HFP_CALL_WINDOW_H
