/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_contact_sync_daemon -- Background Deskbar contact sync daemon.
 *
 * Sits in the Deskbar tray and silently syncs phone contacts via PBAP
 * every 2 hours.  First successful sync shows a BNotification; subsequent
 * syncs are silent unless there is an error.
 *
 * The Deskbar replicant lives in a separate add-on
 * (ContactSyncReplicant) so that Deskbar can load it without pulling
 * in libbluetooth.so.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Deskbar.h>
#include <Entry.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Notification.h>
#include <ObjectList.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

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


static const char* kAppSignature
	= "application/x-vnd.Haiku-BtContactSyncDaemon";
static const char* kDeskbarItemName = "ContactSyncReplicant";

static const uint32 MSG_SYNC		= 'csyn';
static const uint32 MSG_SYNC_DONE	= 'cdne';
static const uint32 MSG_SYNC_NOW	= 'cnow';
static const uint32 MSG_QUIT_DAEMON	= 'cqit';

static const bigtime_t kSyncInterval = 2LL * 60 * 60 * 1000000;
	// 2 hours in microseconds

// Path to the replicant add-on (installed next to the daemon)
static const char* kReplicantAddon = "ContactSyncReplicant";


// #pragma mark - ContactSyncDaemon


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


class ContactSyncDaemon : public BApplication {
public:
						ContactSyncDaemon();
	virtual				~ContactSyncDaemon();

	virtual	void		ReadyToRun();
	virtual	void		MessageReceived(BMessage* message);
	virtual	bool		QuitRequested();

private:
			bool		_FindPairedPhone();
	static	int32		_SyncThread(void* data);
			void		_Notify(const char* title, const char* content,
							notification_type type);
			void		_InstallReplicant();
			void		_RemoveReplicant();

			BMessageRunner*	fRunner;
			thread_id	fSyncThread;
			bool		fFirstSync;
			BString		fDeviceAddr;
			BString		fDeviceName;
			uint32		fDeviceCod;
};


ContactSyncDaemon::ContactSyncDaemon()
	:
	BApplication(kAppSignature),
	fRunner(NULL),
	fSyncThread(-1),
	fFirstSync(true),
	fDeviceCod(0)
{
}


ContactSyncDaemon::~ContactSyncDaemon()
{
	delete fRunner;
}


void
ContactSyncDaemon::ReadyToRun()
{
	if (!_FindPairedPhone()) {
		_Notify("Contact Sync", "No paired phone found. Pair a phone "
			"first, then relaunch.", B_ERROR_NOTIFICATION);
		PostMessage(B_QUIT_REQUESTED);
		return;
	}

	_InstallReplicant();

	// Start periodic timer (2-hour interval)
	BMessage timerMsg(MSG_SYNC);
	fRunner = new BMessageRunner(BMessenger(this), &timerMsg,
		kSyncInterval);

	// Trigger first sync immediately
	PostMessage(MSG_SYNC);
}


void
ContactSyncDaemon::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_SYNC:
		case MSG_SYNC_NOW:
		{
			if (fSyncThread >= 0)
				break;

			fSyncThread = spawn_thread(_SyncThread, "contact_sync",
				B_NORMAL_PRIORITY, this);
			if (fSyncThread >= 0)
				resume_thread(fSyncThread);
			break;
		}

		case MSG_SYNC_DONE:
		{
			const char* error;
			if (message->FindString("error", &error) == B_OK) {
				_Notify("Contact Sync Error", error,
					B_ERROR_NOTIFICATION);
			} else if (fFirstSync) {
				int32 count = 0;
				message->FindInt32("count", &count);

				BString content;
				content.SetToFormat("Synced %ld contacts from %s",
					(long)count, fDeviceName.String());
				_Notify("Contact Sync", content.String(),
					B_INFORMATION_NOTIFICATION);
			}
			fFirstSync = false;
			fSyncThread = -1;
			break;
		}

		case MSG_QUIT_DAEMON:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


bool
ContactSyncDaemon::QuitRequested()
{
	if (fSyncThread >= 0) {
		// Wait for sync thread to finish
		status_t unused;
		wait_for_thread(fSyncThread, &unused);
		fSyncThread = -1;
	}

	_RemoveReplicant();
	return true;
}


bool
ContactSyncDaemon::_FindPairedPhone()
{
	BMessage pairedList;
	if (Bluetooth::LocalDevice::GetPairedDevices(&pairedList) != B_OK)
		return false;

	int32 count = 0;
	pairedList.FindInt32("count", &count);
	if (count == 0)
		return false;

	// First pass: look for a device with CoD major class == 0x02 (Phone)
	for (int32 i = 0; i < count; i++) {
		int32 cod = 0;
		if (pairedList.FindInt32("cod", i, &cod) != B_OK)
			continue;

		uint8 majorClass = (cod >> 8) & 0x1F;
		if (majorClass == 0x02) {
			BString addr, name;
			if (pairedList.FindString("bdaddr", i, &addr) != B_OK)
				continue;
			pairedList.FindString("name", i, &name);

			fDeviceAddr = addr;
			fDeviceName = name.Length() > 0 ? name : addr;
			fDeviceCod = (uint32)cod;
			return true;
		}
	}

	// Fallback: pick first paired device
	BString addr, name;
	int32 cod = 0;
	if (pairedList.FindString("bdaddr", (int32)0, &addr) != B_OK)
		return false;
	pairedList.FindString("name", (int32)0, &name);
	pairedList.FindInt32("cod", (int32)0, &cod);

	fDeviceAddr = addr;
	fDeviceName = name.Length() > 0 ? name : addr;
	fDeviceCod = (uint32)cod;
	return true;
}


/* static */
int32
ContactSyncDaemon::_SyncThread(void* data)
{
	ContactSyncDaemon* app = (ContactSyncDaemon*)data;
	BString addrStr = app->fDeviceAddr;

	bdaddr_t remote = bdaddrUtils::FromString(addrStr.String());
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "Invalid BD_ADDR");
		app->PostMessage(&done);
		return 1;
	}

	// Initialize LocalDevice
	Bluetooth::LocalDevice* localDev =
		Bluetooth::LocalDevice::GetLocalDevice();
	if (localDev == NULL) {
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "No local Bluetooth device");
		app->PostMessage(&done);
		return 1;
	}

	BString localName("Haiku");
	localDev->SetFriendlyName(localName);
	localDev->SetDiscoverable(0x00);

	uint8 devClassBytes[3] = {0x0C, 0x01, 0x08};
	SetDeviceClass(localDev->ID(), devClassBytes);

	// Connect via PBAP
	Bluetooth::PbapClient pbap;
	status_t result = pbap.Connect(remote);
	if (result != B_OK) {
		BString err;
		err.SetToFormat("Connection failed: %s", strerror(result));
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", err.String());
		app->PostMessage(&done);
		return 1;
	}

	// Persist device name and CoD in keystore (fixes display
	// in Bluetooth Preferences for devices paired before name
	// resolution was added)
	uint32 cod = app->fDeviceCod;
	if (cod == 0)
		cod = 0x5A020C;  // Phone/Smartphone, Networking+Capturing+Object Transfer
	Bluetooth::LocalDevice::SaveDeviceName(remote,
		app->fDeviceName.String(), cod);

	// Wait for phone approval (Android "Contact sharing" dialog)
	sleep(15);

	// Download contacts
	uint8* vcardData = NULL;
	size_t vcardLen = 0;
	result = pbap.PullPhoneBook("telecom/pb.vcf",
		PBAP_FORMAT_VCARD_21, &vcardData, &vcardLen);
	if (result != B_OK) {
		BString err;
		err.SetToFormat("Download failed: %s", strerror(result));
		pbap.Disconnect();
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", err.String());
		app->PostMessage(&done);
		return 1;
	}

	// Parse vCard data
	BObjectList<VCardContact, true> contacts(20);
	result = VCardParser::Parse(vcardData, vcardLen, &contacts);
	free(vcardData);

	if (result != B_OK) {
		pbap.Disconnect();
		BMessage done(MSG_SYNC_DONE);
		done.AddString("error", "Failed to parse vCard data");
		app->PostMessage(&done);
		return 1;
	}

	// Write People files
	int32 written = PeopleWriter::WriteContacts(contacts);

	pbap.Disconnect();

	BMessage done(MSG_SYNC_DONE);
	done.AddInt32("count", written);
	app->PostMessage(&done);

	return 0;
}


void
ContactSyncDaemon::_Notify(const char* title, const char* content,
	notification_type type)
{
	BNotification notification(type);
	notification.SetGroup("Bluetooth");
	notification.SetTitle(title);
	notification.SetContent(content);
	notification.Send();
}


void
ContactSyncDaemon::_InstallReplicant()
{
	BDeskbar deskbar;
	if (deskbar.HasItem(kDeskbarItemName)) {
		fprintf(stderr, "Replicant already in Deskbar\n");
		return;
	}

	// Find the replicant add-on next to our own binary.
	// The add-on is a separate shared library that only links
	// libbe.so, so Deskbar can load it without libbluetooth.so.
	image_info info;
	int32 cookie = 0;
	bool found = false;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE) {
			found = true;
			break;
		}
	}

	if (!found) {
		fprintf(stderr, "Could not find app image\n");
		return;
	}

	BPath addonPath(info.name);
	addonPath.GetParent(&addonPath);
	addonPath.Append(kReplicantAddon);

	BEntry entry(addonPath.Path());
	if (!entry.Exists()) {
		fprintf(stderr, "Replicant add-on not found: %s\n",
			addonPath.Path());
		return;
	}

	entry_ref ref;
	entry.GetRef(&ref);

	fprintf(stderr, "Installing replicant from: %s\n", addonPath.Path());

	status_t status = deskbar.AddItem(&ref);
	if (status != B_OK)
		fprintf(stderr, "Deskbar AddItem failed: %s (%" B_PRId32 ")\n",
			strerror(status), status);
	else
		fprintf(stderr, "Replicant installed\n");
}


void
ContactSyncDaemon::_RemoveReplicant()
{
	BDeskbar deskbar;
	deskbar.RemoveItem(kDeskbarItemName);
}


// #pragma mark - main


int
main()
{
	ContactSyncDaemon app;
	app.Run();
	return 0;
}
