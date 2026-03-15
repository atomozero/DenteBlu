/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpCallWindow — GUI for Hands-Free Profile call control.
 */

#include "HfpCallWindow.h"

#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <SeparatorView.h>

#include <bluetooth/HfpClient.h>
#include <bluetooth/bdaddrUtils.h>

#include <stdio.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "HFP Call"

static const uint32 kMsgHfpConnect    = 'hCon';
static const uint32 kMsgHfpDisconnect = 'hDis';
static const uint32 kMsgHfpDial       = 'hDia';
static const uint32 kMsgHfpAnswer     = 'hAns';
static const uint32 kMsgHfpHangUp     = 'hHup';
static const uint32 kMsgHfpSpeakerVol = 'hSpk';
static const uint32 kMsgHfpMicVol     = 'hMic';
static const uint32 kMsgHfpConnected  = 'hCnd';
static const uint32 kMsgHfpFailed     = 'hFal';
static const uint32 kMsgHfpClose      = 'hCls';


HfpCallWindow::HfpCallWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 550, 550),
		B_TRANSLATE("Hands-Free"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fHfp(NULL),
	fConnectThread(-1)
{
	bdaddrUtils::Copy(fAddress, address);

	BString title;
	title.SetToFormat(B_TRANSLATE("Hands-Free \xE2\x80\x94 %s"),
		deviceName);
	SetTitle(title.String());

	BString addrStr;
	addrStr.SetToFormat(B_TRANSLATE("Device: %s"),
		bdaddrUtils::ToString(fAddress).String());
	BStringView* addrLabel = new BStringView("addr", addrStr.String());

	fStatusLabel = new BStringView("status",
		B_TRANSLATE("Status: Disconnected"));

	fNumberField = new BTextControl("number",
		B_TRANSLATE("Number:"), "", NULL);

	fConnectButton = new BButton("connect", B_TRANSLATE("Connect"),
		new BMessage(kMsgHfpConnect));
	fDisconnectButton = new BButton("disconnect",
		B_TRANSLATE("Disconnect"),
		new BMessage(kMsgHfpDisconnect));
	fDialButton = new BButton("dial", B_TRANSLATE("Dial"),
		new BMessage(kMsgHfpDial));
	fAnswerButton = new BButton("answer", B_TRANSLATE("Answer"),
		new BMessage(kMsgHfpAnswer));
	fHangUpButton = new BButton("hangup", B_TRANSLATE("Hang Up"),
		new BMessage(kMsgHfpHangUp));
	fCloseButton = new BButton("close", B_TRANSLATE("Close"),
		new BMessage(kMsgHfpClose));

	fSpeakerSlider = new BSlider("speaker", B_TRANSLATE("Speaker:"),
		new BMessage(kMsgHfpSpeakerVol), 0, 15, B_HORIZONTAL);
	fSpeakerSlider->SetValue(10);
	fSpeakerSlider->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fSpeakerSlider->SetHashMarkCount(16);
	fSpeakerSlider->SetLimitLabels("0", "15");
	fSpeakerSlider->SetModificationMessage(
		new BMessage(kMsgHfpSpeakerVol));

	fMicSlider = new BSlider("mic", B_TRANSLATE("Microphone:"),
		new BMessage(kMsgHfpMicVol), 0, 15, B_HORIZONTAL);
	fMicSlider->SetValue(8);
	fMicSlider->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fMicSlider->SetHashMarkCount(16);
	fMicSlider->SetLimitLabels("0", "15");
	fMicSlider->SetModificationMessage(
		new BMessage(kMsgHfpMicVol));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(addrLabel)
		.Add(fStatusLabel)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fNumberField)
		.AddGroup(B_HORIZONTAL)
			.Add(fConnectButton)
			.Add(fDisconnectButton)
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(fDialButton)
			.Add(fAnswerButton)
			.Add(fHangUpButton)
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fSpeakerSlider)
		.Add(fMicSlider)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCloseButton)
		.End()
	.End();

	_UpdateButtons();
}


HfpCallWindow::~HfpCallWindow()
{
	if (fHfp != NULL) {
		if (fHfp->IsConnected())
			fHfp->Disconnect();
		delete fHfp;
	}
	if (fConnectThread >= 0) {
		status_t exitVal;
		wait_for_thread(fConnectThread, &exitVal);
	}
}


void
HfpCallWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgHfpConnect:
		{
			if (fConnectThread >= 0)
				break;
			fConnectButton->SetEnabled(false);
			fStatusLabel->SetText(
				B_TRANSLATE("Status: Connecting" B_UTF8_ELLIPSIS));
			fConnectThread = spawn_thread(_ConnectThreadEntry,
				"hfp_connect", B_NORMAL_PRIORITY, this);
			if (fConnectThread >= 0)
				resume_thread(fConnectThread);
			break;
		}

		case kMsgHfpConnected:
		{
			fConnectThread = -1;
			fStatusLabel->SetText(B_TRANSLATE("Status: Connected"));
			_UpdateButtons();
			break;
		}

		case kMsgHfpFailed:
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

		case kMsgHfpDisconnect:
		{
			if (fHfp != NULL) {
				fHfp->Disconnect();
				delete fHfp;
				fHfp = NULL;
			}
			fStatusLabel->SetText(B_TRANSLATE("Status: Disconnected"));
			_UpdateButtons();
			break;
		}

		case kMsgHfpDial:
		{
			if (fHfp == NULL || !fHfp->IsConnected())
				break;
			const char* number = fNumberField->Text();
			if (number == NULL || number[0] == '\0')
				break;
			status_t err = fHfp->Dial(number);
			if (err == B_OK)
				fStatusLabel->SetText(
					B_TRANSLATE("Status: Dialing" B_UTF8_ELLIPSIS));
			else {
				BString errStr;
				errStr.SetToFormat(
					B_TRANSLATE("Status: Dial failed: %s"),
					strerror(err));
				fStatusLabel->SetText(errStr.String());
			}
			break;
		}

		case kMsgHfpAnswer:
		{
			if (fHfp == NULL || !fHfp->IsConnected())
				break;
			status_t err = fHfp->Answer();
			if (err == B_OK)
				fStatusLabel->SetText(
					B_TRANSLATE("Status: Call active"));
			else {
				BString errStr;
				errStr.SetToFormat(
					B_TRANSLATE("Status: Answer failed: %s"),
					strerror(err));
				fStatusLabel->SetText(errStr.String());
			}
			break;
		}

		case kMsgHfpHangUp:
		{
			if (fHfp == NULL || !fHfp->IsConnected())
				break;
			status_t err = fHfp->HangUp();
			if (err == B_OK)
				fStatusLabel->SetText(
					B_TRANSLATE("Status: Call ended"));
			else {
				BString errStr;
				errStr.SetToFormat(
					B_TRANSLATE("Status: Hang up failed: %s"),
					strerror(err));
				fStatusLabel->SetText(errStr.String());
			}
			break;
		}

		case kMsgHfpSpeakerVol:
		{
			if (fHfp != NULL && fHfp->IsConnected())
				fHfp->SetSpeakerVolume(
					(uint8)fSpeakerSlider->Value());
			break;
		}

		case kMsgHfpMicVol:
		{
			if (fHfp != NULL && fHfp->IsConnected())
				fHfp->SetMicVolume(
					(uint8)fMicSlider->Value());
			break;
		}

		case kMsgHfpClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


/*static*/ int32
HfpCallWindow::_ConnectThreadEntry(void* arg)
{
	static_cast<HfpCallWindow*>(arg)->_ConnectThread();
	return 0;
}


void
HfpCallWindow::_ConnectThread()
{
	Bluetooth::HfpClient* hfp = new Bluetooth::HfpClient;
	status_t err = hfp->Connect(fAddress);
	if (err != B_OK) {
		delete hfp;
		BMessage fail(kMsgHfpFailed);
		BString errText;
		errText.SetToFormat(
			B_TRANSLATE("Status: Connection failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	err = hfp->EstablishServiceLevel();
	if (err != B_OK) {
		hfp->Disconnect();
		delete hfp;
		BMessage fail(kMsgHfpFailed);
		BString errText;
		errText.SetToFormat(
			B_TRANSLATE("Status: SLC failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	fHfp = hfp;
	PostMessage(new BMessage(kMsgHfpConnected));
}


void
HfpCallWindow::_UpdateButtons()
{
	bool connected = (fHfp != NULL && fHfp->IsConnected());

	fConnectButton->SetEnabled(!connected && fConnectThread < 0);
	fDisconnectButton->SetEnabled(connected);
	fDialButton->SetEnabled(connected);
	fAnswerButton->SetEnabled(connected);
	fHangUpButton->SetEnabled(connected);
	fSpeakerSlider->SetEnabled(connected);
	fMicSlider->SetEnabled(connected);
}
