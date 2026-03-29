/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Numeric Comparison dialog for Bluetooth SSP and BLE pairing.
 */


#include <stdio.h>
#include <malloc.h>

#include <String.h>
#include <Message.h>
#include <Application.h>

#include <Button.h>
#include <GroupLayoutBuilder.h>
#include <InterfaceDefs.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth_error.h>

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <NumericComparisonWindow.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>


static const uint32 kMessageAccept = 'ncAc';
static const uint32 kMessageCancel = 'ncCn';


namespace Bluetooth {


NumericComparisonWindow::NumericComparisonWindow(bdaddr_t address,
	hci_id hid, uint32 numericValue)
	: BWindow(BRect(700, 200, 1050, 420), "Numeric Comparison",
		B_FLOATING_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE),
	fBdaddr(address),
	fHid(hid),
	fNumericValue(numericValue),
	fIsLE(false),
	fLeConnHandle(0)
{
	_InitUI();
}


NumericComparisonWindow::NumericComparisonWindow(bdaddr_t address,
	hci_id hid, uint32 numericValue, uint16 leConnHandle)
	: BWindow(BRect(700, 200, 1050, 420), "Numeric Comparison (LE)",
		B_FLOATING_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE),
	fBdaddr(address),
	fHid(hid),
	fNumericValue(numericValue),
	fIsLE(true),
	fLeConnHandle(leConnHandle)
{
	_InitUI();
}


void
NumericComparisonWindow::_InitUI()
{
	SetLayout(new BGroupLayout(B_HORIZONTAL));

	fIcon = new BluetoothIconView();

	fMessage = new BStringView("fMessage",
		"Confirm the number matches on both devices:");
	fMessage2 = new BStringView("fMessage2", "");

	fAddressLabel = new BStringView("fAddressLabel", "Device: ");
	fAddressLabel->SetFont(be_bold_font);

	fAddressText = new BStringView("fAddressText",
		bdaddrUtils::ToString(fBdaddr));

	fNumericLabel = new BStringView("fNumericLabel", "Passkey: ");
	fNumericLabel->SetFont(be_bold_font);

	BString numStr;
	numStr.SetToFormat("%06" B_PRIu32, fNumericValue);
	fNumericText = new BStringView("fNumericText", numStr);
	fNumericText->SetFont(be_bold_font);

	fAcceptButton = new BButton("fAcceptButton", "Confirm",
		new BMessage(kMessageAccept));
	fCancelButton = new BButton("fCancelButton", "Cancel",
		new BMessage(kMessageCancel));

	AddChild(BGroupLayoutBuilder(B_VERTICAL, 0)
		.Add(BGroupLayoutBuilder(B_HORIZONTAL, 0)
			.Add(BGroupLayoutBuilder(B_HORIZONTAL, 8)
				.Add(fIcon)
			)
			.Add(BGroupLayoutBuilder(B_VERTICAL, 0)
				.Add(fMessage)
				.Add(fMessage2)
				.AddGlue()
			)
		)
		.Add(BGroupLayoutBuilder(B_HORIZONTAL, 0)
			.Add(fAddressLabel)
			.AddGlue()
			.Add(fAddressText)
		)
		.Add(BGroupLayoutBuilder(B_HORIZONTAL, 0)
			.Add(fNumericLabel)
			.AddGlue()
			.Add(fNumericText)
		)
		.AddGlue()
		.Add(BGroupLayoutBuilder(B_HORIZONTAL, 10)
			.AddGlue()
			.Add(fCancelButton)
			.Add(fAcceptButton)
		)
		.SetInsets(8, 8, 8, 8)
	);
}


void
NumericComparisonWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {
		case kMessageAccept:
		{
			BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
			BMessage reply;
			size_t size;

			void* command = buildUserConfirmationRequestReply(
				fBdaddr, &size);
			if (command == NULL)
				break;

			request.AddInt32("hci_id", fHid);
			request.AddData("raw command", B_ANY_TYPE, command, size);
			request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
			request.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL,
					OCF_USER_CONFIRMATION_REQUEST_REPLY));

			if (be_app_messenger.SendMessage(&request, &reply) == B_OK)
				PostMessage(B_QUIT_REQUESTED);

			free(command);
			break;
		}

		case kMessageCancel:
		{
			BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
			BMessage reply;
			size_t size;

			void* command =
				buildUserConfirmationRequestNegReply(
					fBdaddr, &size);
			if (command == NULL)
				break;

			request.AddInt32("hci_id", fHid);
			request.AddData("raw command", B_ANY_TYPE, command, size);
			request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
			request.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL,
					OCF_USER_CONFIRMATION_NEG_REPLY));

			if (be_app_messenger.SendMessage(&request, &reply) == B_OK)
				PostMessage(B_QUIT_REQUESTED);

			free(command);
			break;
		}

		default:
			BWindow::MessageReceived(msg);
			break;
	}
}


bool
NumericComparisonWindow::QuitRequested()
{
	return BWindow::QuitRequested();
}


} /* end namespace Bluetooth */
