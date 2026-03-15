/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SppTerminalWindow — GUI serial terminal over SPP (Serial Port Profile).
 */
#ifndef SPP_TERMINAL_WINDOW_H
#define SPP_TERMINAL_WINDOW_H

#include <Button.h>
#include <MenuField.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/SppClient.h>


class SppTerminalWindow : public BWindow {
public:
						SppTerminalWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~SppTerminalWindow();

	virtual	void		MessageReceived(BMessage* message);
	virtual	bool		QuitRequested();

private:
	static	int32		_ConnectThreadEntry(void* arg);
			void		_ConnectThread();
	static	int32		_RecvThreadEntry(void* arg);
			void		_RecvThread();
			void		_AppendText(const char* prefix,
							const char* text);
			void		_UpdateControls();

	bdaddr_t				fAddress;
	BString					fDeviceName;
	Bluetooth::SppClient	fSpp;
	volatile bool			fStopRequested;
	thread_id				fConnectThread;
	thread_id				fRecvThread;

	// UI
	BStringView*			fStatusView;
	BTextView*				fTerminalView;
	BScrollView*			fTermScrollView;
	BTextControl*			fInputField;
	BButton*				fSendButton;
	BButton*				fDisconnectButton;
	BButton*				fClearButton;
	BMenuField*				fLineEndMenu;
	BString					fLineEnding;
};


#endif // SPP_TERMINAL_WINDOW_H
