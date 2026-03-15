/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * OppTransferWindow — GUI for sending files via Object Push Profile.
 */

#include "OppTransferWindow.h"

#include <Catalog.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <Path.h>
#include <SeparatorView.h>

#include <bluetooth/OppClient.h>
#include <bluetooth/bdaddrUtils.h>

#include <stdio.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "OPP Transfer"

static const uint32 kMsgOppChooseFile = 'ocFl';
static const uint32 kMsgOppFileChosen = 'ofCh';
static const uint32 kMsgOppSend       = 'oSnd';
static const uint32 kMsgOppProgress   = 'oPrg';
static const uint32 kMsgOppDone       = 'oDne';
static const uint32 kMsgOppFailed     = 'oFal';
static const uint32 kMsgOppClose      = 'oCls';


OppTransferWindow::OppTransferWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 650, 450),
		B_TRANSLATE("Send File"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fFilePanel(NULL),
	fTransferThread(-1),
	fCancelRequested(false)
{
	bdaddrUtils::Copy(fAddress, address);

	BString title;
	title.SetToFormat(B_TRANSLATE("Send File to %s"), deviceName);
	SetTitle(title.String());

	BString addrStr;
	addrStr.SetToFormat(B_TRANSLATE("Device: %s"),
		bdaddrUtils::ToString(fAddress).String());
	BStringView* addrLabel = new BStringView("addr", addrStr.String());

	fFileLabel = new BStringView("file",
		B_TRANSLATE("File: (none selected)"));

	fProgressBar = new BStatusBar("progress");
	fProgressBar->SetMaxValue(100);

	fStatusText = new BStringView("status", B_TRANSLATE("Ready"));

	fChooseButton = new BButton("choose",
		B_TRANSLATE("Choose file" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOppChooseFile));
	fSendButton = new BButton("send", B_TRANSLATE("Send"),
		new BMessage(kMsgOppSend));
	fSendButton->SetEnabled(false);
	fCloseButton = new BButton("close", B_TRANSLATE("Close"),
		new BMessage(kMsgOppClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(addrLabel)
		.Add(fFileLabel)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fProgressBar)
		.Add(fStatusText)
		.AddGroup(B_HORIZONTAL)
			.Add(fChooseButton)
			.Add(fSendButton)
			.AddGlue()
			.Add(fCloseButton)
		.End()
	.End();
}


OppTransferWindow::~OppTransferWindow()
{
	fCancelRequested = true;
	if (fTransferThread >= 0) {
		status_t exitVal;
		wait_for_thread(fTransferThread, &exitVal);
	}
	delete fFilePanel;
}


void
OppTransferWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOppChooseFile:
		{
			if (fFilePanel == NULL) {
				fFilePanel = new BFilePanel(B_OPEN_PANEL, NULL, NULL,
					B_FILE_NODE, false, NULL, NULL, true, true);
				fFilePanel->SetTarget(BMessenger(this));
				fFilePanel->SetMessage(new BMessage(kMsgOppFileChosen));
			}
			fFilePanel->Show();
			break;
		}

		case kMsgOppFileChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) == B_OK) {
				BPath path(&ref);
				fFilePath = path.Path();
				BString label;
				label.SetToFormat(B_TRANSLATE("File: %s"), path.Leaf());
				fFileLabel->SetText(label.String());
				fSendButton->SetEnabled(true);
			}
			break;
		}

		case kMsgOppSend:
		{
			if (fFilePath.Length() == 0 || fTransferThread >= 0)
				break;
			fCancelRequested = false;
			fSendButton->SetEnabled(false);
			fChooseButton->SetEnabled(false);
			fTransferThread = spawn_thread(_TransferThreadEntry,
				"opp_transfer", B_NORMAL_PRIORITY, this);
			if (fTransferThread >= 0)
				resume_thread(fTransferThread);
			break;
		}

		case kMsgOppProgress:
		{
			const char* text = NULL;
			float percent = 0;
			if (message->FindFloat("percent", &percent) == B_OK)
				fProgressBar->SetTo(percent);
			if (message->FindString("text", &text) == B_OK)
				fStatusText->SetText(text);
			break;
		}

		case kMsgOppDone:
		{
			fTransferThread = -1;
			fProgressBar->SetTo(100);
			fStatusText->SetText(B_TRANSLATE("Transfer complete"));
			fChooseButton->SetEnabled(true);
			fSendButton->SetEnabled(true);
			break;
		}

		case kMsgOppFailed:
		{
			fTransferThread = -1;
			fProgressBar->Reset();
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusText->SetText(text);
			else
				fStatusText->SetText(B_TRANSLATE("Transfer failed"));
			fChooseButton->SetEnabled(true);
			fSendButton->SetEnabled(true);
			break;
		}

		case kMsgOppClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


/*static*/ int32
OppTransferWindow::_TransferThreadEntry(void* arg)
{
	static_cast<OppTransferWindow*>(arg)->_TransferThread();
	return 0;
}


void
OppTransferWindow::_TransferThread()
{
	BMessage progress(kMsgOppProgress);
	progress.AddString("text",
		B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS));
	progress.AddFloat("percent", 0);
	PostMessage(&progress);

	Bluetooth::OppClient opp;
	status_t err = opp.Connect(fAddress);
	if (err != B_OK || fCancelRequested) {
		BMessage fail(kMsgOppFailed);
		BString errText;
		errText.SetToFormat(B_TRANSLATE("Connection failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	progress.MakeEmpty();
	progress.what = kMsgOppProgress;
	progress.AddString("text",
		B_TRANSLATE("Sending file" B_UTF8_ELLIPSIS));
	progress.AddFloat("percent", 25);
	PostMessage(&progress);

	err = opp.PushFile(fFilePath.String());
	opp.Disconnect();

	if (err != B_OK) {
		BMessage fail(kMsgOppFailed);
		BString errText;
		errText.SetToFormat(B_TRANSLATE("Send failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	PostMessage(new BMessage(kMsgOppDone));
}
