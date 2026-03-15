/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PbapBrowserWindow — GUI for browsing phone contacts via PBAP.
 */
#ifndef PBAP_BROWSER_WINDOW_H
#define PBAP_BROWSER_WINDOW_H

#include <Button.h>
#include <FilePanel.h>
#include <ListView.h>
#include <MenuField.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>


class PbapBrowserWindow : public BWindow {
public:
						PbapBrowserWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~PbapBrowserWindow();

	virtual	void		MessageReceived(BMessage* message);

private:
	static	int32		_DownloadThreadEntry(void* arg);
			void		_DownloadThread();
			void		_ParseVCards(const uint8* data, size_t len);
			void		_ClearContacts();

	bdaddr_t				fAddress;
	BString					fDeviceName;
	volatile bool			fCancelRequested;
	thread_id				fDownloadThread;

	// UI
	BStringView*			fStatusView;
	BListView*				fContactList;
	BScrollView*			fContactScroll;
	BButton*				fDownloadButton;
	BButton*				fSaveButton;
	BButton*				fCloseButton;
	BMenuField*				fFolderMenu;
	BFilePanel*				fSavePanel;

	// Data
	uint8*					fVcardData;
	size_t					fVcardLen;
	BString					fSelectedPath;
	BString					fSelectedFolder;
};


#endif // PBAP_BROWSER_WINDOW_H
