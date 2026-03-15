/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_contact_sync — Bluetooth Contact Sync GUI tool.
 *
 * Downloads contacts from a phone via PBAP and creates Haiku People files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Alert.h>
#include <Application.h>
#include <Box.h>
#include <Button.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <ObjectList.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <StatusBar.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/obex.h>
#include <bluetooth/PbapClient.h>
#include <bluetooth/PeopleWriter.h>
#include <bluetooth/VCardParser.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>


static const uint32 MSG_SYNC			= 'sync';
static const uint32 MSG_SYNC_DONE		= 'sdne';
static const uint32 MSG_STATUS			= 'stat';
static const uint32 MSG_PROGRESS		= 'prog';
static const uint32 MSG_DEVICE_SELECTED	= 'dsel';


static status_t
SetDeviceClass(hci_id hid, uint8 devClass[3])
{
	size_t size;
	void* command = buildWriteClassOfDevice(devClass, &size);
	if (command == NULL)
		return B_NO_MEMORY;

	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		free(command);
		return B_ERROR;
	}

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;
	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE, command, size);
	request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CLASS_OF_DEV));

	status_t status = messenger.SendMessage(&request, &reply);
	free(command);
	if (status == B_OK) {
		int8 bt_status;
		if (reply.FindInt8("status", &bt_status) == B_OK)
			return bt_status == BT_OK ? B_OK : B_ERROR;
	}
	return status;
}


class ContactSyncWindow : public BWindow {
public:
						ContactSyncWindow();
	virtual void		MessageReceived(BMessage* message);
	virtual bool		QuitRequested();

private:
	static int32		_SyncThread(void* data);
	void				_SetStatus(const char* text);
	void				_SetProgress(float value, const char* text);

	BPopUpMenu*			fDevicePopUp;
	BMenuField*			fDeviceMenu;
	BStringView*		fAddrLabel;
	BButton*			fSyncButton;
	BStringView*		fStatusView;
	BStatusBar*			fProgressBar;
	thread_id			fSyncThread;
};


ContactSyncWindow::ContactSyncWindow()
	:
	BWindow(BRect(100, 100, 560, 340), "Bluetooth Contact Sync",
		B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	fSyncThread(-1)
{
	/* Header */
	BStringView* titleView = new BStringView("title",
		"Bluetooth Contact Sync");
	titleView->SetFont(be_bold_font);
	titleView->SetFontSize(14.0);

	BStringView* subtitleView = new BStringView("subtitle",
		"Download phone contacts as People files");
	subtitleView->SetHighUIColor(B_PANEL_TEXT_COLOR, 0.7);

	/* Device selection */
	fDevicePopUp = new BPopUpMenu("Select a device" B_UTF8_ELLIPSIS);

	BMessage pairedList;
	if (Bluetooth::LocalDevice::GetPairedDevices(&pairedList) == B_OK) {
		int32 count = 0;
		pairedList.FindInt32("count", &count);
		for (int32 i = 0; i < count; i++) {
			BString addr, name;
			if (pairedList.FindString("bdaddr", i, &addr) != B_OK)
				continue;
			pairedList.FindString("name", i, &name);

			BString label;
			if (name.Length() > 0)
				label = name;
			else
				label = addr;

			BMessage* itemMsg = new BMessage(MSG_DEVICE_SELECTED);
			itemMsg->AddString("bdaddr", addr);
			fDevicePopUp->AddItem(new BMenuItem(label.String(),
				itemMsg));
		}
	}

	fDeviceMenu = new BMenuField("device", "Device:", fDevicePopUp);

	fAddrLabel = new BStringView("addr", "");
	fAddrLabel->SetHighUIColor(B_PANEL_TEXT_COLOR, 0.7);

	/* Auto-select first device */
	BMenuItem* first = fDevicePopUp->ItemAt(0);
	if (first != NULL) {
		first->SetMarked(true);
		const char* addr;
		if (first->Message()->FindString("bdaddr", &addr) == B_OK)
			fAddrLabel->SetText(addr);
	}

	BBox* deviceBox = new BBox("deviceBox");
	deviceBox->SetLabel("Phone");
	BLayoutBuilder::Group<>(deviceBox, B_VERTICAL,
			B_USE_HALF_ITEM_SPACING)
		.SetInsets(B_USE_ITEM_SPACING, B_USE_ITEM_SPACING * 2,
			B_USE_ITEM_SPACING, B_USE_ITEM_SPACING)
		.Add(fDeviceMenu)
		.Add(fAddrLabel)
	.End();

	/* Sync button */
	fSyncButton = new BButton("sync", "Sync Contacts",
		new BMessage(MSG_SYNC));

	/* Progress area */
	fProgressBar = new BStatusBar("progress");
	fProgressBar->SetMaxValue(100.0);

	fStatusView = new BStringView("status", "Ready");

	/* Layout */
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(titleView)
		.Add(subtitleView)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(deviceBox)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fSyncButton)
			.AddGlue()
		.End()
		.Add(fProgressBar)
		.Add(fStatusView)
	.End();

	CenterOnScreen();
}


void
ContactSyncWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_DEVICE_SELECTED:
		{
			const char* addr;
			if (message->FindString("bdaddr", &addr) == B_OK)
				fAddrLabel->SetText(addr);
			break;
		}

		case MSG_SYNC:
		{
			if (fSyncThread >= 0) {
				_SetStatus("Sync already in progress...");
				break;
			}
			fSyncButton->SetEnabled(false);
			fProgressBar->Reset();
			fSyncThread = spawn_thread(_SyncThread, "sync_thread",
				B_NORMAL_PRIORITY, this);
			if (fSyncThread >= 0)
				resume_thread(fSyncThread);
			else {
				_SetStatus("Failed to create thread");
				fSyncButton->SetEnabled(true);
			}
			break;
		}

		case MSG_STATUS:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK)
				fStatusView->SetText(text);
			break;
		}

		case MSG_PROGRESS:
		{
			float value;
			const char* text = NULL;
			if (message->FindFloat("value", &value) == B_OK) {
				message->FindString("text", &text);
				fProgressBar->Reset();
				fProgressBar->Update(value, text, NULL);
			}
			break;
		}

		case MSG_SYNC_DONE:
		{
			int32 count = 0;
			message->FindInt32("count", &count);

			const char* error;
			if (message->FindString("error", &error) == B_OK) {
				fStatusView->SetText(error);
				fProgressBar->Reset();
			} else {
				BString status;
				status.SetToFormat("Done! %ld contacts synced.",
					(long)count);
				fStatusView->SetText(status.String());
				fProgressBar->Reset();
				fProgressBar->Update(100.0, "Complete", NULL);
			}
			fSyncButton->SetEnabled(true);
			fSyncThread = -1;
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
ContactSyncWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
ContactSyncWindow::_SetStatus(const char* text)
{
	BMessage msg(MSG_STATUS);
	msg.AddString("text", text);
	PostMessage(&msg);
}


void
ContactSyncWindow::_SetProgress(float value, const char* text)
{
	BMessage msg(MSG_PROGRESS);
	msg.AddFloat("value", value);
	if (text != NULL)
		msg.AddString("text", text);
	PostMessage(&msg);
}


/* static */
int32
ContactSyncWindow::_SyncThread(void* data)
{
	ContactSyncWindow* window = (ContactSyncWindow*)data;

	/* Get address from selected device */
	BString addrStr;
	if (window->LockLooper()) {
		BMenuItem* selected = window->fDevicePopUp->FindMarked();
		if (selected != NULL && selected->Message() != NULL) {
			const char* addr;
			if (selected->Message()->FindString("bdaddr",
					&addr) == B_OK) {
				addrStr = addr;
			}
		}
		window->UnlockLooper();
	}

	if (addrStr.IsEmpty()) {
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "No device selected");
		window->PostMessage(&done);
		return 1;
	}

	bdaddr_t remote = bdaddrUtils::FromString(addrStr.String());
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "Invalid BD_ADDR");
		window->PostMessage(&done);
		return 1;
	}

	/* Initialize LocalDevice (enables SSP, Event Mask, EIR) */
	window->_SetStatus("Initializing Bluetooth...");
	window->_SetProgress(5.0, "Initializing...");

	Bluetooth::LocalDevice* localDev =
		Bluetooth::LocalDevice::GetLocalDevice();
	if (localDev == NULL) {
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "No local Bluetooth device");
		window->PostMessage(&done);
		return 1;
	}

	/* Set friendly name and Class of Device */
	BString name("Haiku");
	localDev->SetFriendlyName(name);
	localDev->SetDiscoverable(0x00);

	/* CoD: Computer/Laptop + Object Transfer service */
	uint8 devClassBytes[3] = {0x0C, 0x01, 0x08};
	SetDeviceClass(localDev->ID(), devClassBytes);

	/* Step 1: Connect */
	window->_SetStatus("Connecting...");
	window->_SetProgress(10.0, "Connecting...");

	Bluetooth::PbapClient pbap;
	status_t result = pbap.Connect(remote);
	if (result != B_OK) {
		BString err;
		err.SetToFormat("Connection failed: %s", strerror(result));
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", err.String());
		window->PostMessage(&done);
		return 1;
	}

	/* Step 2: Wait for phone to approve contact sharing.
	 * Android shows a "Contact sharing" notification/dialog
	 * the first time a PBAP client connects. The user must
	 * approve it before the phone will respond to GET requests.
	 * Show a countdown to give the user time. */
	window->_SetProgress(20.0, "Waiting for phone...");
	for (int i = 15; i > 0; i--) {
		BString waitMsg;
		waitMsg.SetToFormat(
			"Check phone! Approve 'Contact sharing' for Haiku (%ds)",
			i);
		window->_SetStatus(waitMsg.String());
		sleep(1);
	}

	/* Step 3: Download contacts */
	window->_SetStatus("Downloading contacts...");
	window->_SetProgress(40.0, "Downloading...");

	uint8* vcardData = NULL;
	size_t vcardLen = 0;
	result = pbap.PullPhoneBook("telecom/pb.vcf",
		PBAP_FORMAT_VCARD_21, &vcardData, &vcardLen);
	if (result != B_OK) {
		BString err;
		err.SetToFormat("Download failed: %s "
			"(enable Contact sharing on phone?)", strerror(result));
		window->_SetStatus(err.String());
		pbap.Disconnect();
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", err.String());
		window->PostMessage(&done);
		return 1;
	}

	/* Step 4: Parse vCard data */
	window->_SetStatus("Parsing vCard data...");
	window->_SetProgress(70.0, "Parsing...");

	BObjectList<VCardContact, true> contacts(20);
	result = VCardParser::Parse(vcardData, vcardLen, &contacts);
	free(vcardData);

	if (result != B_OK) {
		pbap.Disconnect();
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "Failed to parse vCard data");
		window->PostMessage(&done);
		return 1;
	}

	/* Step 5: Create People files */
	BString creating;
	creating.SetToFormat("Creating %ld People files...",
		(long)contacts.CountItems());
	window->_SetStatus(creating.String());
	window->_SetProgress(85.0, "Writing...");

	int32 written = PeopleWriter::WriteContacts(contacts);

	/* Step 6: Done */
	pbap.Disconnect();

	BMessage done(MSG_SYNC_DONE);
	done.AddInt32("count", written);
	window->PostMessage(&done);

	return 0;
}


class ContactSyncApp : public BApplication {
public:
						ContactSyncApp();
	virtual void		ReadyToRun();
};


ContactSyncApp::ContactSyncApp()
	:
	BApplication("application/x-vnd.Haiku-BtContactSync")
{
}


void
ContactSyncApp::ReadyToRun()
{
	ContactSyncWindow* window = new ContactSyncWindow();
	window->Show();
}


int
main()
{
	ContactSyncApp app;
	app.Run();
	return 0;
}
