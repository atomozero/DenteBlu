/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SppTerminalWindow — GUI serial terminal over SPP (Serial Port Profile).
 */

#include "SppTerminalWindow.h"

#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <SeparatorView.h>

#include <bluetooth/bdaddrUtils.h>

#include <stdio.h>
#include <string.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SPP Terminal"

static const uint32 kMsgSppConnect    = 'spCo';
static const uint32 kMsgSppConnected  = 'spCd';
static const uint32 kMsgSppConnFailed = 'spCf';
static const uint32 kMsgSppListening  = 'spLi';
static const uint32 kMsgSppDisconnect = 'spDc';
static const uint32 kMsgSppSend       = 'spSd';
static const uint32 kMsgSppDataRecv   = 'spRv';
static const uint32 kMsgSppLineEnd    = 'spLe';
static const uint32 kMsgSppClear      = 'spCl';


SppTerminalWindow::SppTerminalWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 700, 600),
		B_TRANSLATE("Bluetooth Terminal"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fStopRequested(false),
	fConnectThread(-1),
	fRecvThread(-1),
	fLineEnding("\r\n")
{
	bdaddrUtils::Copy(fAddress, address);

	BString title;
	title.SetToFormat(B_TRANSLATE("Bluetooth Terminal \xE2\x80\x94 %s"),
		deviceName);
	SetTitle(title.String());

	// Status bar
	fStatusView = new BStringView("status",
		B_TRANSLATE("Status: Connecting" B_UTF8_ELLIPSIS));

	// Terminal text view
	fTerminalView = new BTextView("terminal");
	fTerminalView->SetFont(be_fixed_font);
	fTerminalView->MakeEditable(false);
	fTerminalView->MakeSelectable(true);
	fTerminalView->SetWordWrap(true);
	fTermScrollView = new BScrollView("scroll", fTerminalView,
		B_WILL_DRAW, false, true);

	// Input field — Enter sends
	fInputField = new BTextControl("input", NULL, "",
		new BMessage(kMsgSppSend));

	// Buttons
	fSendButton = new BButton("send", B_TRANSLATE("Send"),
		new BMessage(kMsgSppSend));
	fSendButton->MakeDefault(true);

	fClearButton = new BButton("clear", B_TRANSLATE("Clear"),
		new BMessage(kMsgSppClear));

	fDisconnectButton = new BButton("disconnect",
		B_TRANSLATE("Disconnect"),
		new BMessage(kMsgSppDisconnect));

	// Line ending menu
	BMenu* lineEndMenu = new BMenu(B_TRANSLATE("Line ending"));
	lineEndMenu->SetRadioMode(true);
	lineEndMenu->SetLabelFromMarked(true);

	BMessage* crlfMsg = new BMessage(kMsgSppLineEnd);
	crlfMsg->AddString("ending", "\r\n");
	BMenuItem* crlfItem = new BMenuItem("CR+LF", crlfMsg);
	crlfItem->SetMarked(true);
	lineEndMenu->AddItem(crlfItem);

	BMessage* lfMsg = new BMessage(kMsgSppLineEnd);
	lfMsg->AddString("ending", "\n");
	lineEndMenu->AddItem(new BMenuItem("LF", lfMsg));

	BMessage* crMsg = new BMessage(kMsgSppLineEnd);
	crMsg->AddString("ending", "\r");
	lineEndMenu->AddItem(new BMenuItem("CR", crMsg));

	BMessage* noneMsg = new BMessage(kMsgSppLineEnd);
	noneMsg->AddString("ending", "");
	lineEndMenu->AddItem(new BMenuItem(B_TRANSLATE("None"), noneMsg));

	fLineEndMenu = new BMenuField("lineend",
		B_TRANSLATE("Line ending:"), lineEndMenu);

	// Layout
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fStatusView)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fTermScrollView, 10.0f)
		.AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
			.Add(fInputField, 10.0f)
			.Add(fSendButton)
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fLineEndMenu)
			.AddGlue()
			.Add(fClearButton)
			.Add(fDisconnectButton)
		.End()
	.End();

	// Disable input until connected
	_UpdateControls();

	// Auto-start connection
	fConnectThread = spawn_thread(_ConnectThreadEntry,
		"spp_connect", B_NORMAL_PRIORITY, this);
	if (fConnectThread >= 0)
		resume_thread(fConnectThread);
}


SppTerminalWindow::~SppTerminalWindow()
{
	fStopRequested = true;

	if (fRecvThread >= 0) {
		status_t exitVal;
		wait_for_thread(fRecvThread, &exitVal);
	}
	if (fConnectThread >= 0) {
		status_t exitVal;
		wait_for_thread(fConnectThread, &exitVal);
	}

	if (fSpp.IsConnected())
		fSpp.Disconnect();
}


bool
SppTerminalWindow::QuitRequested()
{
	fStopRequested = true;
	return true;
}


void
SppTerminalWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSppConnected:
		{
			fConnectThread = -1;
			fStatusView->SetText(B_TRANSLATE("Status: Connected"));

			// Start receive thread
			fRecvThread = spawn_thread(_RecvThreadEntry,
				"spp_recv", B_NORMAL_PRIORITY, this);
			if (fRecvThread >= 0)
				resume_thread(fRecvThread);

			_UpdateControls();
			fInputField->MakeFocus(true);
			break;
		}

		case kMsgSppListening:
			fStatusView->SetText(B_TRANSLATE(
				"Status: Waiting for phone to connect"
				B_UTF8_ELLIPSIS));
			break;

		case kMsgSppConnFailed:
		{
			fConnectThread = -1;
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusView->SetText(text);
			else
				fStatusView->SetText(
					B_TRANSLATE("Status: Connection failed"));
			_UpdateControls();
			break;
		}

		case kMsgSppSend:
		{
			if (!fSpp.IsConnected())
				break;
			const char* text = fInputField->Text();
			if (text == NULL || text[0] == '\0')
				break;

			// Build data to send: text + line ending
			BString data(text);
			data << fLineEnding;

			ssize_t sent = fSpp.Send(data.String(), data.Length());
			if (sent > 0) {
				_AppendText("> ", text);
				fInputField->SetText("");
			} else {
				BString errStr;
				errStr.SetToFormat(
					B_TRANSLATE("Status: Send failed: %s"),
					strerror((status_t)sent));
				fStatusView->SetText(errStr.String());
			}
			break;
		}

		case kMsgSppDataRecv:
		{
			const char* data = NULL;
			ssize_t length = 0;
			if (message->FindData("data", B_RAW_TYPE,
					(const void**)&data, &length) == B_OK
				&& length > 0) {
				BString text(data, length);
				_AppendText("< ", text.String());
			}
			break;
		}

		case kMsgSppDisconnect:
		{
			fStopRequested = true;

			if (fRecvThread >= 0) {
				status_t exitVal;
				wait_for_thread(fRecvThread, &exitVal);
				fRecvThread = -1;
			}

			if (fSpp.IsConnected())
				fSpp.Disconnect();

			fStopRequested = false;
			fStatusView->SetText(B_TRANSLATE("Status: Disconnected"));
			_UpdateControls();
			break;
		}

		case kMsgSppLineEnd:
		{
			const char* ending = NULL;
			if (message->FindString("ending", &ending) == B_OK)
				fLineEnding = ending;
			break;
		}

		case kMsgSppClear:
			fTerminalView->SetText("");
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


/*static*/ int32
SppTerminalWindow::_ConnectThreadEntry(void* arg)
{
	static_cast<SppTerminalWindow*>(arg)->_ConnectThread();
	return 0;
}


void
SppTerminalWindow::_ConnectThread()
{
	status_t err = fSpp.Connect(fAddress);
	if (err == B_OK) {
		PostMessage(new BMessage(kMsgSppConnected));
		return;
	}

	if (err != B_NAME_NOT_FOUND) {
		BMessage fail(kMsgSppConnFailed);
		BString errText;
		errText.SetToFormat(
			B_TRANSLATE("Status: Connection failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	/* Remote device has no SPP service — switch to listen mode
	 * so the phone's app can connect to us instead */
	PostMessage(new BMessage(kMsgSppListening));

	err = fSpp.Listen(60000000LL);
	if (err != B_OK) {
		BMessage fail(kMsgSppConnFailed);
		BString errText;
		errText.SetToFormat(
			B_TRANSLATE("Status: Listen failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	PostMessage(new BMessage(kMsgSppConnected));
}


/*static*/ int32
SppTerminalWindow::_RecvThreadEntry(void* arg)
{
	static_cast<SppTerminalWindow*>(arg)->_RecvThread();
	return 0;
}


void
SppTerminalWindow::_RecvThread()
{
	uint8 buf[512];

	while (!fStopRequested && fSpp.IsConnected()) {
		ssize_t received = fSpp.Receive(buf, sizeof(buf) - 1,
			500000LL);
		if (received > 0) {
			BMessage msg(kMsgSppDataRecv);
			msg.AddData("data", B_RAW_TYPE, buf, received);
			PostMessage(&msg);
		}
	}
}


void
SppTerminalWindow::_AppendText(const char* prefix, const char* text)
{
	BString line;
	line << prefix << text << "\n";
	int32 len = fTerminalView->TextLength();
	fTerminalView->Insert(len, line.String(), line.Length());
	fTerminalView->ScrollToOffset(fTerminalView->TextLength());
}


void
SppTerminalWindow::_UpdateControls()
{
	bool connected = fSpp.IsConnected();
	bool connecting = (fConnectThread >= 0);

	fInputField->SetEnabled(connected);
	fSendButton->SetEnabled(connected);
	fDisconnectButton->SetEnabled(connected || connecting);
	fClearButton->SetEnabled(true);
}
