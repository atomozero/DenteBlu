/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BluetoothTerminal — Standalone GUI serial terminal over SPP.
 *
 * Usage: BluetoothTerminal <BD_ADDR> [device_name]
 *   e.g. BluetoothTerminal 0C:7D:B0:B2:81:6A "moto g15"
 */

#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/SppClient.h>


// Messages
static const uint32 kMsgSppConnect    = 'spCo';
static const uint32 kMsgSppConnected  = 'spCd';
static const uint32 kMsgSppConnFailed = 'spCf';
static const uint32 kMsgSppDisconnect = 'spDc';
static const uint32 kMsgSppSend       = 'spSd';
static const uint32 kMsgSppDataRecv   = 'spRv';
static const uint32 kMsgSppLineEnd    = 'spLe';
static const uint32 kMsgSppClear      = 'spCl';


class TerminalWindow : public BWindow {
public:
	TerminalWindow(const bdaddr_t& address, const char* deviceName)
		:
		BWindow(BRect(200, 200, 700, 600),
			"Bluetooth Terminal",
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
		title.SetToFormat("Bluetooth Terminal \xE2\x80\x94 %s",
			deviceName);
		SetTitle(title.String());

		fStatusView = new BStringView("status",
			"Status: Connecting" B_UTF8_ELLIPSIS);

		fTerminalView = new BTextView("terminal");
		fTerminalView->SetFont(be_fixed_font);
		fTerminalView->MakeEditable(false);
		fTerminalView->MakeSelectable(true);
		fTerminalView->SetWordWrap(true);
		fTermScrollView = new BScrollView("scroll", fTerminalView,
			B_WILL_DRAW, false, true);

		fInputField = new BTextControl("input", NULL, "",
			new BMessage(kMsgSppSend));

		fSendButton = new BButton("send", "Send",
			new BMessage(kMsgSppSend));
		fSendButton->MakeDefault(true);

		fClearButton = new BButton("clear", "Clear",
			new BMessage(kMsgSppClear));

		fDisconnectButton = new BButton("disconnect", "Disconnect",
			new BMessage(kMsgSppDisconnect));

		BMenu* lineEndMenu = new BMenu("Line ending");
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
		lineEndMenu->AddItem(new BMenuItem("None", noneMsg));

		fLineEndMenu = new BMenuField("lineend",
			"Line ending:", lineEndMenu);

		BLayoutBuilder::Group<>(this, B_VERTICAL,
				B_USE_HALF_ITEM_SPACING)
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

		_UpdateControls();

		fConnectThread = spawn_thread(_ConnectThreadEntry,
			"spp_connect", B_NORMAL_PRIORITY, this);
		if (fConnectThread >= 0)
			resume_thread(fConnectThread);
	}

	virtual ~TerminalWindow()
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

	virtual bool QuitRequested()
	{
		fStopRequested = true;
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSppConnected:
			{
				fConnectThread = -1;
				fStatusView->SetText("Status: Connected");

				fRecvThread = spawn_thread(_RecvThreadEntry,
					"spp_recv", B_NORMAL_PRIORITY, this);
				if (fRecvThread >= 0)
					resume_thread(fRecvThread);

				_UpdateControls();
				fInputField->MakeFocus(true);
				break;
			}

			case kMsgSppConnFailed:
			{
				fConnectThread = -1;
				const char* text = NULL;
				if (message->FindString("text", &text) == B_OK)
					fStatusView->SetText(text);
				else
					fStatusView->SetText(
						"Status: Connection failed");
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

				BString data(text);
				data << fLineEnding;

				ssize_t sent = fSpp.Send(data.String(),
					data.Length());
				if (sent > 0) {
					_AppendText("> ", text);
					fInputField->SetText("");
				} else {
					BString errStr;
					errStr.SetToFormat(
						"Status: Send failed: %s",
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
				fStatusView->SetText("Status: Disconnected");
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

private:
	static int32 _ConnectThreadEntry(void* arg)
	{
		static_cast<TerminalWindow*>(arg)->_ConnectThread();
		return 0;
	}

	void _ConnectThread()
	{
		status_t err = fSpp.Connect(fAddress);
		if (err != B_OK) {
			BMessage fail(kMsgSppConnFailed);
			BString errText;
			errText.SetToFormat(
				"Status: Connection failed: %s",
				strerror(err));
			fail.AddString("text", errText.String());
			PostMessage(&fail);
			return;
		}

		PostMessage(new BMessage(kMsgSppConnected));
	}

	static int32 _RecvThreadEntry(void* arg)
	{
		static_cast<TerminalWindow*>(arg)->_RecvThread();
		return 0;
	}

	void _RecvThread()
	{
		uint8 buf[512];

		while (!fStopRequested && fSpp.IsConnected()) {
			ssize_t received = fSpp.Receive(buf,
				sizeof(buf) - 1, 500000LL);
			if (received > 0) {
				BMessage msg(kMsgSppDataRecv);
				msg.AddData("data", B_RAW_TYPE, buf, received);
				PostMessage(&msg);
			}
		}
	}

	void _AppendText(const char* prefix, const char* text)
	{
		BString line;
		line << prefix << text << "\n";
		int32 len = fTerminalView->TextLength();
		fTerminalView->Insert(len, line.String(), line.Length());
		fTerminalView->ScrollToOffset(fTerminalView->TextLength());
	}

	void _UpdateControls()
	{
		bool connected = fSpp.IsConnected();
		bool connecting = (fConnectThread >= 0);

		fInputField->SetEnabled(connected);
		fSendButton->SetEnabled(connected);
		fDisconnectButton->SetEnabled(connected || connecting);
		fClearButton->SetEnabled(true);
	}

	bdaddr_t				fAddress;
	BString					fDeviceName;
	Bluetooth::SppClient	fSpp;
	volatile bool			fStopRequested;
	thread_id				fConnectThread;
	thread_id				fRecvThread;

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


class TerminalApp : public BApplication {
public:
	TerminalApp()
		:
		BApplication("application/x-vnd.Haiku-BluetoothTerminal"),
		fWindow(NULL)
	{
	}

	void ArgvReceived(int32 argc, char** argv)
	{
		if (argc < 2) {
			fprintf(stderr,
				"Usage: %s <BD_ADDR> [device_name]\n"
				"  e.g. %s 0C:7D:B0:B2:81:6A \"moto g15\"\n",
				argv[0], argv[0]);
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		bdaddr_t addr = bdaddrUtils::FromString(argv[1]);
		if (bdaddrUtils::Compare(addr, bdaddrUtils::NullAddress())) {
			fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		const char* name = (argc >= 3) ? argv[2] : argv[1];
		fWindow = new TerminalWindow(addr, name);
		fWindow->Show();
	}

	void ReadyToRun()
	{
		if (fWindow == NULL) {
			fprintf(stderr,
				"Usage: BluetoothTerminal <BD_ADDR> [name]\n");
			PostMessage(B_QUIT_REQUESTED);
		}
	}

private:
	TerminalWindow*	fWindow;
};


int
main()
{
	TerminalApp app;
	app.Run();
	return 0;
}
