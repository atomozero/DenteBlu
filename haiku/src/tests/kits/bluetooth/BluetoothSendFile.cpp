/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BluetoothSendFile — Standalone app to send files via Bluetooth OPP.
 *
 * Usage:
 *   BluetoothSendFile                     (pick device from paired list)
 *   BluetoothSendFile <BD_ADDR> [name]    (send to specific device)
 */

#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Button.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <StatusBar.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/OppClient.h>


// Messages
static const uint32 kMsgDeviceSelected = 'dSel';
static const uint32 kMsgChooseFile     = 'cFil';
static const uint32 kMsgFileChosen     = 'fChn';
static const uint32 kMsgSend           = 'send';
static const uint32 kMsgProgress       = 'prog';
static const uint32 kMsgDone           = 'done';
static const uint32 kMsgFailed         = 'fail';


class SendFileWindow : public BWindow {
public:
	SendFileWindow(const bdaddr_t* address, const char* deviceName)
		:
		BWindow(BRect(200, 200, 650, 430),
			"Bluetooth Send File",
			B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
				| B_QUIT_ON_WINDOW_CLOSE),
		fFilePanel(NULL),
		fTransferThread(-1),
		fCancelRequested(false),
		fHasDevice(false)
	{
		memset(&fAddress, 0, sizeof(fAddress));

		// Device selector
		fDeviceMenu = new BPopUpMenu("(select device)");
		_PopulateDevices();

		fDeviceField = new BMenuField("device", "Device:", fDeviceMenu);

		// If address was passed on command line, select it
		if (address != NULL) {
			bdaddrUtils::Copy(fAddress, *address);
			fDeviceName = deviceName ? deviceName : "Device";
			fHasDevice = true;

			BString label;
			label.SetToFormat("%s (%s)", fDeviceName.String(),
				bdaddrUtils::ToString(fAddress).String());
			fDeviceMenu->Superitem()->SetLabel(label.String());
		}

		// File selection
		fFileLabel = new BStringView("file", "File: (none selected)");

		// Progress
		fProgressBar = new BStatusBar("progress");
		fProgressBar->SetMaxValue(100);
		fStatusText = new BStringView("status", "Ready");

		// Buttons
		fChooseButton = new BButton("choose", "Choose file" B_UTF8_ELLIPSIS,
			new BMessage(kMsgChooseFile));
		fSendButton = new BButton("send", "Send",
			new BMessage(kMsgSend));
		fSendButton->SetEnabled(false);

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fDeviceField)
			.Add(fFileLabel)
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fProgressBar)
			.Add(fStatusText)
			.AddGroup(B_HORIZONTAL)
				.Add(fChooseButton)
				.Add(fSendButton)
				.AddGlue()
			.End()
		.End();
	}


	virtual ~SendFileWindow()
	{
		fCancelRequested = true;
		if (fTransferThread >= 0) {
			status_t exitVal;
			wait_for_thread(fTransferThread, &exitVal);
		}
		delete fFilePanel;
	}


	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgDeviceSelected:
			{
				const char* addr = NULL;
				const char* name = NULL;
				if (message->FindString("bdaddr", &addr) == B_OK) {
					fAddress = bdaddrUtils::FromString(addr);
					fHasDevice = true;
					if (message->FindString("name", &name) == B_OK)
						fDeviceName = name;
					else
						fDeviceName = addr;
					_UpdateSendEnabled();
				}
				break;
			}

			case kMsgChooseFile:
			{
				if (fFilePanel == NULL) {
					fFilePanel = new BFilePanel(B_OPEN_PANEL, NULL, NULL,
						B_FILE_NODE, false, NULL, NULL, true, true);
					fFilePanel->SetTarget(BMessenger(this));
					fFilePanel->SetMessage(new BMessage(kMsgFileChosen));
				}
				fFilePanel->Show();
				break;
			}

			case kMsgFileChosen:
			{
				entry_ref ref;
				if (message->FindRef("refs", &ref) == B_OK) {
					BPath path(&ref);
					fFilePath = path.Path();
					BString label;
					label.SetToFormat("File: %s", path.Leaf());
					fFileLabel->SetText(label.String());
					_UpdateSendEnabled();
				}
				break;
			}

			case kMsgSend:
			{
				if (fFilePath.Length() == 0 || !fHasDevice
					|| fTransferThread >= 0)
					break;
				fCancelRequested = false;
				fSendButton->SetEnabled(false);
				fChooseButton->SetEnabled(false);
				fDeviceField->SetEnabled(false);
				fTransferThread = spawn_thread(_TransferEntry,
					"opp_send", B_NORMAL_PRIORITY, this);
				if (fTransferThread >= 0) {
					resume_thread(fTransferThread);
				} else {
					fTransferThread = -1;
					fSendButton->SetEnabled(true);
					fChooseButton->SetEnabled(true);
					fDeviceField->SetEnabled(true);
					fStatusText->SetText("Failed to start transfer thread");
				}
				break;
			}

			case kMsgProgress:
			{
				float percent = 0;
				const char* text = NULL;
				if (message->FindFloat("percent", &percent) == B_OK)
					fProgressBar->SetTo(percent);
				if (message->FindString("text", &text) == B_OK)
					fStatusText->SetText(text);
				break;
			}

			case kMsgDone:
			{
				fTransferThread = -1;
				fProgressBar->SetTo(100);
				fStatusText->SetText("Transfer complete");
				fChooseButton->SetEnabled(true);
				fSendButton->SetEnabled(true);
				fDeviceField->SetEnabled(true);
				break;
			}

			case kMsgFailed:
			{
				fTransferThread = -1;
				fProgressBar->Reset();
				const char* text = NULL;
				if (message->FindString("text", &text) == B_OK)
					fStatusText->SetText(text);
				else
					fStatusText->SetText("Transfer failed");
				fChooseButton->SetEnabled(true);
				fSendButton->SetEnabled(true);
				fDeviceField->SetEnabled(true);
				break;
			}

			default:
				BWindow::MessageReceived(message);
				break;
		}
	}


private:
	void _PopulateDevices()
	{
		BMessage pairedList;
		if (LocalDevice::GetPairedDevices(&pairedList) != B_OK)
			return;

		int32 count = 0;
		pairedList.FindInt32("count", &count);

		for (int32 i = 0; i < count; i++) {
			BString addrStr, name;
			if (pairedList.FindString("bdaddr", i, &addrStr) != B_OK)
				continue;
			pairedList.FindString("name", i, &name);
			if (name.Length() == 0)
				name = addrStr;

			BString label;
			label.SetToFormat("%s (%s)", name.String(), addrStr.String());

			BMessage* msg = new BMessage(kMsgDeviceSelected);
			msg->AddString("bdaddr", addrStr);
			msg->AddString("name", name);

			fDeviceMenu->AddItem(new BMenuItem(label.String(), msg));
		}

		if (count == 0) {
			BMenuItem* empty = new BMenuItem("(no paired devices)", NULL);
			empty->SetEnabled(false);
			fDeviceMenu->AddItem(empty);
		}
	}


	void _UpdateSendEnabled()
	{
		fSendButton->SetEnabled(fHasDevice && fFilePath.Length() > 0
			&& fTransferThread < 0);
	}


	static int32 _TransferEntry(void* arg)
	{
		static_cast<SendFileWindow*>(arg)->_TransferThread();
		return 0;
	}


	void _TransferThread()
	{
		BMessage progress(kMsgProgress);
		progress.AddString("text", "Connecting" B_UTF8_ELLIPSIS);
		progress.AddFloat("percent", 0);
		PostMessage(&progress);

		Bluetooth::OppClient opp;
		status_t err = opp.Connect(fAddress);
		if (err != B_OK || fCancelRequested) {
			BMessage fail(kMsgFailed);
			BString errText;
			errText.SetToFormat("Connection failed: %s", strerror(err));
			fail.AddString("text", errText.String());
			PostMessage(&fail);
			return;
		}

		progress.MakeEmpty();
		progress.what = kMsgProgress;
		progress.AddString("text", "Sending file" B_UTF8_ELLIPSIS);
		progress.AddFloat("percent", 25);
		PostMessage(&progress);

		err = opp.PushFile(fFilePath.String());
		opp.Disconnect();

		if (err != B_OK) {
			BMessage fail(kMsgFailed);
			BString errText;
			errText.SetToFormat("Send failed: %s", strerror(err));
			fail.AddString("text", errText.String());
			PostMessage(&fail);
			return;
		}

		BMessage done(kMsgDone);
		PostMessage(&done);
	}


	BPopUpMenu*			fDeviceMenu;
	BMenuField*			fDeviceField;
	BStringView*		fFileLabel;
	BStatusBar*			fProgressBar;
	BStringView*		fStatusText;
	BButton*			fChooseButton;
	BButton*			fSendButton;
	BFilePanel*			fFilePanel;

	bdaddr_t			fAddress;
	BString				fDeviceName;
	BString				fFilePath;
	thread_id			fTransferThread;
	volatile bool		fCancelRequested;
	bool				fHasDevice;
};


// #pragma mark - Application


class SendFileApp : public BApplication {
public:
	SendFileApp()
		:
		BApplication("application/x-vnd.Haiku-BluetoothSendFile"),
		fAddress(NULL),
		fDeviceName(NULL),
		fWindow(NULL)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		if (argc >= 2) {
			fParsedAddr = bdaddrUtils::FromString(argv[1]);
			fAddress = &fParsedAddr;
			if (argc >= 3)
				fDeviceName = argv[2];
			else
				fDeviceName = argv[1];
		}
	}

	virtual void ReadyToRun()
	{
		fWindow = new SendFileWindow(fAddress, fDeviceName);
		fWindow->Show();
	}

	virtual void RefsReceived(BMessage* message)
	{
		// Handle drag & drop or "Open with" from Tracker
		if (fWindow != NULL) {
			message->what = kMsgFileChosen;
			fWindow->PostMessage(message);
		}
	}

private:
	bdaddr_t	fParsedAddr;
	bdaddr_t*	fAddress;
	const char*	fDeviceName;
	SendFileWindow*	fWindow;
};


int
main(int, char**)
{
	SendFileApp app;
	app.Run();
	return 0;
}
