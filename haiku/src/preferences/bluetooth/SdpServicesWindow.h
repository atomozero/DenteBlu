/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef SDP_SERVICES_WINDOW_H
#define SDP_SERVICES_WINDOW_H

#include <OutlineListView.h>
#include <ScrollView.h>
#include <StatusBar.h>
#include <String.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>


class SdpServicesWindow : public BWindow
{
public:
						SdpServicesWindow(const bdaddr_t& address,
							const char* deviceName);
	virtual				~SdpServicesWindow();

	virtual	void		MessageReceived(BMessage* message);
	virtual	bool		QuitRequested();

private:
	static	int32		_QueryThreadEntry(void* data);
			void		_QueryThread();

			status_t	_EnsureAclConnection();
			bool		_DoSdpQuery(int sock, uint8** outBuf, uint32* outLen);
			void		_ParseAndPost(const uint8* data, uint32 len);
			void		_PopulateResults(BMessage* message);

	bdaddr_t			fAddress;
	BString				fDeviceName;
	BOutlineListView*	fServiceList;
	BScrollView*		fScrollView;
	BStatusBar*			fStatusBar;
	thread_id			fQueryThread;
	uint8				fHciError;
};


#endif // SDP_SERVICES_WINDOW_H
