/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * OppTransferWindow — GUI for sending files via Object Push Profile.
 */
#ifndef OPP_TRANSFER_WINDOW_H
#define OPP_TRANSFER_WINDOW_H

#include <Button.h>
#include <FilePanel.h>
#include <StatusBar.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>


class OppTransferWindow : public BWindow {
public:
						OppTransferWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~OppTransferWindow();

	virtual	void		MessageReceived(BMessage* message);

private:
	static	int32		_TransferThreadEntry(void* arg);
			void		_TransferThread();

	bdaddr_t			fAddress;
	BString				fDeviceName;
	BString				fFilePath;
	BStringView*		fFileLabel;
	BStatusBar*			fProgressBar;
	BStringView*		fStatusText;
	BButton*			fChooseButton;
	BButton*			fSendButton;
	BButton*			fCloseButton;
	BFilePanel*			fFilePanel;
	thread_id			fTransferThread;
	volatile bool		fCancelRequested;
};


#endif // OPP_TRANSFER_WINDOW_H
