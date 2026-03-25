/*
 * Copyright 2007-2009 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <Entry.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <TypeConstants.h>
#include <syslog.h>

#include <bluetoothserver_p.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_command_le.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth.h>

#include "BluetoothServer.h"
#include "DeskbarReplicant.h"
#include "LocalDeviceImpl.h"
#include "SdpServer.h"
#include "Debug.h"


status_t
DispatchEvent(struct hci_event_header* header, int32 code, size_t size)
{
	// we only handle events
	if (GET_PORTCODE_TYPE(code)!= BT_EVENT) {
		TRACE_BT("BluetoothServer: Wrong type frame code\n");
		return B_OK;
	}

	// fetch the LocalDevice who belongs this event
	LocalDeviceImpl* lDeviceImplementation = ((BluetoothServer*)be_app)->
		LocateLocalDeviceImpl(GET_PORTCODE_HID(code));

	if (lDeviceImplementation == NULL) {
		TRACE_BT("BluetoothServer: LocalDevice could not be fetched\n");
		return B_OK;
	}

	lDeviceImplementation->HandleEvent(header);

	return B_OK;
}


BluetoothServer::BluetoothServer()
	:
	BApplication(BLUETOOTH_SIGNATURE),
	fLeScanResults(20),
	fLeScanActive(false),
	fSDPThreadID(-1),
	fIsShuttingDown(false)
{
	fDeviceManager = new DeviceManager();
	fLocalDevicesList.MakeEmpty();

	fEventListener2 = new BluetoothPortListener(BT_USERLAND_PORT_NAME,
		(BluetoothPortListener::port_listener_func)&DispatchEvent);

	// Load persisted Bluetooth keys (link keys and LTKs)
	status_t status = fKeyStore.Load();
	if (status == B_OK)
		TRACE_BT("BluetoothServer: Key store loaded\n");
	else
		TRACE_BT("BluetoothServer: No key store found (status=%" B_PRId32
			"), starting fresh\n", status);
}


bool BluetoothServer::QuitRequested(void)
{
	LocalDeviceImpl* lDeviceImpl = NULL;
	while ((lDeviceImpl = (LocalDeviceImpl*)
		fLocalDevicesList.RemoveItemAt(0)) != NULL)
		delete lDeviceImpl;

	_RemoveDeskbarIcon();

	// stop the SDP server thread
	fIsShuttingDown = true;

	status_t threadReturnStatus;
	wait_for_thread(fSDPThreadID, &threadReturnStatus);
	TRACE_BT("BluetoothServer server thread exited with: %s\n",
		strerror(threadReturnStatus));

	delete fEventListener2;
	TRACE_BT("Shutting down bluetooth_server.\n");

	return BApplication::QuitRequested();
}


void BluetoothServer::ArgvReceived(int32 argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "--finish") == 0)
			PostMessage(B_QUIT_REQUESTED);
	}
}


void BluetoothServer::ReadyToRun(void)
{
	fDeviceManager->StartMonitoringDevice("bluetooth/h2");
	fDeviceManager->StartMonitoringDevice("bluetooth/h3");
	fDeviceManager->StartMonitoringDevice("bluetooth/h4");
	fDeviceManager->StartMonitoringDevice("bluetooth/h5");

	if (fEventListener2->Launch() != B_OK)
		TRACE_BT("General: Bluetooth event listener failed\n");
	else
		TRACE_BT("General: Bluetooth event listener Ready\n");

	_InstallDeskbarIcon();

	// Spawn the SDP server thread
	fSDPThreadID = spawn_thread(SDPServerThread, "SDP server thread",
		B_NORMAL_PRIORITY, this);

	if (fSDPThreadID <= 0 || resume_thread(fSDPThreadID) != B_OK) {
		TRACE_BT("BluetoothServer: Failed launching the SDP server thread\n");
	}
}


void BluetoothServer::AppActivated(bool act)
{
	printf("Activated %d\n",act);
}


void BluetoothServer::MessageReceived(BMessage* message)
{
	BMessage reply;
	status_t status = B_WOULD_BLOCK; // mark somehow to do not reply anything

	switch (message->what)
	{
		case BT_MSG_ADD_DEVICE:
		{
			BString str;
			message->FindString("name", &str);

			TRACE_BT("BluetoothServer: Requested LocalDevice %s\n", str.String());

			BPath path(str.String());

			LocalDeviceImpl* lDeviceImpl
				= LocalDeviceImpl::CreateTransportAccessor(&path);

			if (lDeviceImpl->GetID() >= 0) {
				lDeviceImpl->SetKeyStore(&fKeyStore);
				fLocalDevicesList.AddItem(lDeviceImpl);

				TRACE_BT("LocalDevice %s id=%" B_PRId32 " added\n", str.String(),
					lDeviceImpl->GetID());
			} else {
				TRACE_BT("BluetoothServer: Adding LocalDevice hci id invalid\n");
			}

			status = B_WOULD_BLOCK;
			/* TODO: This should be by user request only! */
			// Reset controller first to clear stale ACL connections
			// that survive across server restarts.
			lDeviceImpl->ResetController();
			lDeviceImpl->Launch();
			// Delete all keys from controller so authentication MUST
			// go through the host (Link_Key_Request). This ensures
			// both hosts are aware of the auth state.
			lDeviceImpl->DeleteControllerLinkKeys();
			break;
		}

		case BT_MSG_REMOVE_DEVICE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl != NULL) {
				fLocalDevicesList.RemoveItem(lDeviceImpl);
				delete lDeviceImpl;
			}
			break;
		}

		case BT_MSG_COUNT_LOCAL_DEVICES:
			status = HandleLocalDevicesCount(message, &reply);
			break;

		case BT_MSG_ACQUIRE_LOCAL_DEVICE:
			status = HandleAcquireLocalDevice(message, &reply);
			break;

		case BT_MSG_HANDLE_SIMPLE_REQUEST:
			status = HandleSimpleRequest(message, &reply);
			break;

		case BT_MSG_GET_PROPERTY:
			status = HandleGetProperty(message, &reply);
			break;

		case BT_MSG_LE_SCAN_START:
			status = HandleLeScanStart(message, &reply);
			break;

		case BT_MSG_LE_SCAN_STOP:
			status = HandleLeScanStop(message, &reply);
			break;

		case BT_MSG_LE_CONNECT:
			status = HandleLeConnect(message, &reply);
			break;

		case BT_MSG_LE_CONN_COMPLETE:
			TRACE_BT("BluetoothServer: LE connection complete event\n");
			status = B_WOULD_BLOCK;
			break;

		case BT_MSG_LE_SCAN_RESULT:
		{
			TRACE_BT("BluetoothServer: LE scan result received\n");
			if (fLeScanActive) {
				BMessage* copy = new BMessage(*message);
				fLeScanResults.AddItem(copy);
			}
			status = B_WOULD_BLOCK;
			break;
		}

		case BT_MSG_LE_SCAN_RESULTS_GET:
			status = HandleLeScanResultsGet(message, &reply);
			break;

		case BT_MSG_LE_GATT_DISCOVER:
			status = HandleLeGattDiscover(message, &reply);
			break;

		case BT_MSG_LE_GATT_READ:
			status = HandleLeGattRead(message, &reply);
			break;

		case BT_MSG_LE_GATT_WRITE:
			status = HandleLeGattWrite(message, &reply);
			break;

		case BT_MSG_LE_GATT_SUBSCRIBE:
			status = HandleLeGattSubscribe(message, &reply);
			break;

		case BT_MSG_LE_PAIR:
			status = HandleLePair(message, &reply);
			break;

		case BT_MSG_LE_NC_CONFIRM:
			status = HandleLeNcConfirm(message, &reply);
			break;

		case BT_MSG_GET_PAIRED_DEVICES:
			status = HandleGetPairedDevices(message, &reply);
			break;

		case BT_MSG_SAVE_DEVICE_NAME:
			status = HandleSaveDeviceName(message, &reply);
			break;

		// Handle if the bluetooth preferences is running?
		case B_SOME_APP_LAUNCHED:
		{
			const char* signature;

			if (message->FindString("be:signature", &signature) == B_OK) {
				printf("input_server : %s\n", signature);
				if (strcmp(signature, "application/x-vnd.Be-TSKB") == 0) {

				}
			}
			return;
		}

		default:
			BApplication::MessageReceived(message);
			break;
	}

	// Can we reply right now?
	// TOD: review this condition
	if (status != B_WOULD_BLOCK) {
		reply.AddInt32("status", status);
		message->SendReply(&reply);
//		printf("Sending reply message for->\n");
//		message->PrintToStream();
	}
}


#if 0
#pragma mark -
#endif

LocalDeviceImpl*
BluetoothServer::LocateDelegateFromMessage(BMessage* message)
{
	LocalDeviceImpl* lDeviceImpl = NULL;
	hci_id hid;

	if (message->FindInt32("hci_id", &hid) == B_OK) {
		// Try to find out when a ID was specified
		int index;
		for (index = 0; index < fLocalDevicesList.CountItems(); index ++) {
			lDeviceImpl = fLocalDevicesList.ItemAt(index);
			if (lDeviceImpl->GetID() == hid)
				break;
		}
	}

	return lDeviceImpl;

}


LocalDeviceImpl*
BluetoothServer::LocateLocalDeviceImpl(hci_id hid)
{
	// Try to find out when a ID was specified
	int index;

	for (index = 0; index < fLocalDevicesList.CountItems(); index++) {
		LocalDeviceImpl* lDeviceImpl = fLocalDevicesList.ItemAt(index);
		if (lDeviceImpl->GetID() == hid)
			return lDeviceImpl;
	}

	return NULL;
}


#if 0
#pragma - Messages reply
#endif

status_t
BluetoothServer::HandleLocalDevicesCount(BMessage* message, BMessage* reply)
{
	TRACE_BT("BluetoothServer: count requested\n");

	return reply->AddInt32("count", fLocalDevicesList.CountItems());
}


status_t
BluetoothServer::HandleAcquireLocalDevice(BMessage* message, BMessage* reply)
{
	hci_id hid;
	ssize_t size;
	bdaddr_t bdaddr;
	LocalDeviceImpl* lDeviceImpl = NULL;
	static int32 lastIndex = 0;

	if (message->FindInt32("hci_id", &hid) == B_OK)	{
		TRACE_BT("BluetoothServer: GetLocalDevice requested with id\n");
		lDeviceImpl = LocateDelegateFromMessage(message);

	} else if (message->FindData("bdaddr", B_ANY_TYPE,
		(const void**)&bdaddr, &size) == B_OK) {

		// Try to find out when the user specified the address
		TRACE_BT("BluetoothServer: GetLocalDevice requested with bdaddr\n");
		for (lastIndex = 0; lastIndex < fLocalDevicesList.CountItems();
			lastIndex ++) {
			// TODO: Only possible if the property is available
			// bdaddr_t local;
			// lDeviceImpl = fLocalDevicesList.ItemAt(lastIndex);
			// if ((lDeviceImpl->GetAddress(&local, message) == B_OK)
			// 	&& bacmp(&local, &bdaddr)) {
			// 	break;
			// }
		}

	} else {
		// Careless, any device not performing operations will be fine
		TRACE_BT("BluetoothServer: GetLocalDevice plain request\n");
		// from last assigned till end
		for (int index = lastIndex + 1;
			index < fLocalDevicesList.CountItems();	index++) {
			lDeviceImpl= fLocalDevicesList.ItemAt(index);
			if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
				printf("Requested local device %" B_PRId32 "\n",
					lDeviceImpl->GetID());
				TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
				lastIndex = index;
				break;
			}
		}

		// from starting till last assigned if not yet found
		if (lDeviceImpl == NULL) {
			for (int index = 0; index <= lastIndex ; index ++) {
				lDeviceImpl = fLocalDevicesList.ItemAt(index);
				if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
					printf("Requested local device %" B_PRId32 "\n",
						lDeviceImpl->GetID());
					TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
					lastIndex = index;
					break;
				}
			}
		}
	}

	if (lastIndex <= fLocalDevicesList.CountItems() && lDeviceImpl != NULL
		&& lDeviceImpl->Available()) {

		hid = lDeviceImpl->GetID();
		lDeviceImpl->Acquire();

		TRACE_BT("BluetoothServer: Device acquired %" B_PRId32 "\n", hid);
		return reply->AddInt32("hci_id", hid);
	}

	return B_ERROR;

}


status_t
BluetoothServer::HandleSimpleRequest(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {
		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {
			// Dump everything
			reply->AddMessage("properties", lDeviceImpl->GetPropertiesMessage());
			return B_OK;
		}
	}

	// we are gonna need issue the command ...
	if (lDeviceImpl->ProcessSimpleRequest(DetachCurrentMessage()) == B_OK)
		return B_WOULD_BLOCK;
	else {
		lDeviceImpl->Unregister();
		return B_ERROR;
	}

}


status_t
BluetoothServer::HandleGetProperty(BMessage* message, BMessage* reply)
{
	// User side will look for the reply in a result field and will
	// not care about status fields, therefore we return OK in all cases

	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {

		TRACE_BT("BluetoothServer: Searching %s property...\n", propertyRequested);

		// Connection handle lookup by bdaddr (dynamic, not stored)
		if (strcmp(propertyRequested, "handle") == 0) {
			const void* addrData;
			ssize_t addrSize;
			if (message->FindData("bdaddr", B_ANY_TYPE,
					&addrData, &addrSize) == B_OK
				&& addrSize == sizeof(bdaddr_t)) {
				bdaddr_t bdaddr;
				memcpy(&bdaddr, addrData, sizeof(bdaddr_t));
				uint16 handle =
					lDeviceImpl->FindHandleByAddress(bdaddr);
				if (handle > 0) {
					reply->AddInt16("handle", handle);
					TRACE_BT("BluetoothServer: handle=%#x "
						"for %s\n", handle,
						bdaddrUtils::ToString(bdaddr).String());
				} else {
					TRACE_BT("BluetoothServer: no active "
						"connection for %s\n",
						bdaddrUtils::ToString(bdaddr).String());
				}
			}

		// Check if the property has been already retrieved
		} else if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {

			// 1 bytes requests
			if (strcmp(propertyRequested, "hci_version") == 0
				|| strcmp(propertyRequested, "lmp_version") == 0
				|| strcmp(propertyRequested, "sco_mtu") == 0) {

				uint8 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt8(propertyRequested);
				reply->AddInt32("result", result);

			// 2 bytes requests
			} else if (strcmp(propertyRequested, "hci_revision") == 0
					|| strcmp(propertyRequested, "lmp_subversion") == 0
					|| strcmp(propertyRequested, "manufacturer") == 0
					|| strcmp(propertyRequested, "acl_mtu") == 0
					|| strcmp(propertyRequested, "acl_max_pkt") == 0
					|| strcmp(propertyRequested, "sco_max_pkt") == 0
					|| strcmp(propertyRequested, "packet_type") == 0 ) {

				uint16 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt16(propertyRequested);
				reply->AddInt32("result", result);

			// 1 bit requests
			} else if (strcmp(propertyRequested, "role_switch_capable") == 0
					|| strcmp(propertyRequested, "encrypt_capable") == 0) {

				bool result = lDeviceImpl->GetPropertiesMessage()->
					FindBool(propertyRequested);

				reply->AddInt32("result", result);

			} else {
				TRACE_BT("BluetoothServer: Property %s could not be satisfied\n", propertyRequested);
			}
		}
	}

	return B_OK;
}


#if 0
#pragma mark -
#endif

int32
BluetoothServer::SDPServerThread(void* data)
{
	const BluetoothServer* server = (BluetoothServer*)data;

	struct sockaddr_l2cap loc_addr = { 0 };
	int socketServer;
	int client;

	TRACE_BT("SDP: SDP server thread up...\n");

	// Retry socket+bind with backoff — L2CAP module may not be loaded yet
	bool bound = false;
	for (int retry = 0; retry < 10; retry++) {
		if (server->fIsShuttingDown)
			return B_OK;

		socketServer = socket(PF_BLUETOOTH, SOCK_STREAM,
			BLUETOOTH_PROTO_L2CAP);
		if (socketServer < 0) {
			TRACE_BT("SDP: Could not create server socket (%s), "
				"retry %d\n", strerror(errno), retry);
			snooze(500000 * (1 << retry));	// 500ms * 2^retry
			continue;
		}

		int reuse = 1;
		setsockopt(socketServer, SOL_SOCKET, SO_REUSEADDR,
			&reuse, sizeof(reuse));

		loc_addr.l2cap_family = AF_BLUETOOTH;
		loc_addr.l2cap_bdaddr = BDADDR_ANY;
		loc_addr.l2cap_psm = B_HOST_TO_LENDIAN_INT16(1);
		loc_addr.l2cap_len = sizeof(struct sockaddr_l2cap);

		if (bind(socketServer, (struct sockaddr*)&loc_addr,
				sizeof(struct sockaddr_l2cap)) < 0) {
			TRACE_BT("SDP: Could not bind server socket (%s), "
				"retry %d\n", strerror(errno), retry);
			close(socketServer);
			snooze(500000 * (1 << retry));
			continue;
		}

		bound = true;
		break;
	}

	if (!bound) {
		TRACE_BT("SDP: Failed to bind after 10 retries, giving up\n");
		return B_ERROR;
	}

	if (listen(socketServer, 10) < 0) {
		TRACE_BT("SDP: Could not listen on server socket (%s)\n",
			strerror(errno));
		close(socketServer);
		return B_ERROR;
	}

	TRACE_BT("SDP: Listening on PSM 1\n");

	SdpServer sdpServer;

	uint8 recvBuf[672];
	uint8 sendBuf[672];

	while (!server->fIsShuttingDown) {

		TRACE_BT("SDP: Waiting for connection...\n");

		uint len = sizeof(struct sockaddr_l2cap);
		client = accept(socketServer, (struct sockaddr*)&loc_addr, &len);

		if (client < 0) {
			if (server->fIsShuttingDown)
				break;
			TRACE_BT("SDP: accept() failed (%s)\n", strerror(errno));
			snooze(1000000);
			continue;
		}

		TRACE_BT("SDP: Client connected (fd=%d)\n", client);

		ssize_t receivedSize;
		while ((receivedSize = recv(client, recvBuf, sizeof(recvBuf), 0))
				> 0) {
			TRACE_BT("SDP: Received %zd bytes\n", receivedSize);

			ssize_t respLen = sdpServer.HandleRequest(recvBuf,
				receivedSize, sendBuf, sizeof(sendBuf));

			if (respLen > 0) {
				send(client, sendBuf, respLen, 0);
				TRACE_BT("SDP: Sent %zd bytes response\n", respLen);
			}
		}

		close(client);
		TRACE_BT("SDP: Client disconnected\n");
	}

	close(socketServer);

	return B_NO_ERROR;
}


void
BluetoothServer::ShowWindow(BWindow* pWindow)
{
	pWindow->Lock();
	if (pWindow->IsHidden())
		pWindow->Show();
	else
		pWindow->Activate();
	pWindow->Unlock();
}


void
BluetoothServer::_InstallDeskbarIcon()
{
	BDeskbar deskbar;

	if (deskbar.HasItem(kDeskbarItemName)) {
		_RemoveDeskbarIcon();
	}

	// Use the lightweight add-on that only links libbe.so,
	// so Deskbar can load it without needing libbluetooth.so.
	entry_ref ref;
	status_t res = get_ref_for_path(
		"/boot/system/non-packaged/add-ons/BluetoothDeskbarReplicant", &ref);
	if (res == B_OK)
		res = deskbar.AddItem(&ref);

	if (res != B_OK) {
		// Fallback: try the server binary itself
		app_info appInfo;
		be_app->GetAppInfo(&appInfo);
		res = deskbar.AddItem(&appInfo.ref);
	}

	if (res != B_OK)
		TRACE_BT("Failed adding deskbar icon: %" B_PRId32 "\n", res);
}


void
BluetoothServer::_RemoveDeskbarIcon()
{
	BDeskbar deskbar;
	status_t res = deskbar.RemoveItem(kDeskbarItemName);
	if (res != B_OK)
		TRACE_BT("Failed removing Deskbar icon: %" B_PRId32 ": \n", res);
}


#if 0
#pragma mark - LE Handlers
#endif


status_t
BluetoothServer::HandleLeScanStart(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		// Try first available device
		if (fLocalDevicesList.CountItems() > 0)
			lDeviceImpl = fLocalDevicesList.ItemAt(0);
	}

	if (lDeviceImpl == NULL)
		return B_ERROR;

	int8 scanType = HCI_LE_SCAN_ACTIVE;
	int16 interval = 0x0060; /* 60ms */
	int16 window = 0x0030;   /* 30ms */
	int8 filterDup = 0x01;

	message->FindInt8("scan_type", &scanType);
	message->FindInt16("interval", &interval);
	message->FindInt16("window", &window);
	message->FindInt8("filter_dup", &filterDup);

	TRACE_BT("BluetoothServer: Starting LE scan type=%d interval=%d window=%d\n",
		scanType, interval, window);

	fLeScanResults.MakeEmpty();
	fLeScanActive = true;

	lDeviceImpl->StartLeScan(scanType, interval, window, filterDup);

	return B_OK;
}


status_t
BluetoothServer::HandleLeScanStop(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		if (fLocalDevicesList.CountItems() > 0)
			lDeviceImpl = fLocalDevicesList.ItemAt(0);
	}

	if (lDeviceImpl == NULL)
		return B_ERROR;

	TRACE_BT("BluetoothServer: Stopping LE scan\n");
	fLeScanActive = false;
	lDeviceImpl->StopLeScan();

	return B_OK;
}


status_t
BluetoothServer::HandleLeConnect(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		if (fLocalDevicesList.CountItems() > 0)
			lDeviceImpl = fLocalDevicesList.ItemAt(0);
	}

	if (lDeviceImpl == NULL)
		return B_ERROR;

	const void* bdaddrData;
	ssize_t size;
	if (message->FindData("bdaddr", B_ANY_TYPE, &bdaddrData, &size) != B_OK
		|| size != sizeof(bdaddr_t)) {
		return B_BAD_VALUE;
	}

	bdaddr_t bdaddr;
	memcpy(&bdaddr, bdaddrData, sizeof(bdaddr_t));

	int8 addressType = HCI_LE_ADDR_PUBLIC;
	message->FindInt8("address_type", &addressType);

	TRACE_BT("BluetoothServer: Creating LE connection to %s type=%d\n",
		bdaddrUtils::ToString(bdaddr).String(), addressType);

	lDeviceImpl->CreateLeConnection(bdaddr, addressType);

	return B_WOULD_BLOCK;
}


status_t
BluetoothServer::HandleLeScanResultsGet(BMessage* message, BMessage* reply)
{
	int32 count = fLeScanResults.CountItems();
	TRACE_BT("BluetoothServer: Returning %" B_PRId32 " LE scan results\n",
		count);

	for (int32 i = 0; i < count; i++) {
		BMessage* result = fLeScanResults.ItemAt(i);
		if (result != NULL)
			reply->AddMessage("device", result);
	}

	reply->AddInt32("count", count);
	fLeScanResults.MakeEmpty();

	return B_OK;
}


#if 0
#pragma mark - LE GATT Handlers
#endif


status_t
BluetoothServer::HandleLeGattDiscover(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle;
	if (message->FindInt16("conn_handle", &connHandle) != B_OK)
		return B_BAD_VALUE;

	int8 discoverType = 0; // 0=services, 1=characteristics, 2=descriptors
	message->FindInt8("discover_type", &discoverType);

	switch (discoverType) {
		case 0:
		{
			bt_le_gatt_discover_services_params params = {};
			params.conn_handle = connHandle;

			status_t result = dev->LeGattDiscoverServices(&params);
			reply->AddInt32("status", params.status);

			if (result == B_OK && params.status == B_OK) {
				reply->AddInt16("count", params.count);
				reply->AddData("services", B_ANY_TYPE, params.services,
					params.count * sizeof(gatt_service_t));
			}
			return B_OK;
		}

		case 1:
		{
			int16 startHandle, endHandle;
			if (message->FindInt16("start_handle", &startHandle) != B_OK
				|| message->FindInt16("end_handle", &endHandle) != B_OK)
				return B_BAD_VALUE;

			bt_le_gatt_discover_chars_params params = {};
			params.conn_handle = connHandle;
			params.start_handle = startHandle;
			params.end_handle = endHandle;

			status_t result = dev->LeGattDiscoverCharacteristics(&params);
			reply->AddInt32("status", params.status);

			if (result == B_OK && params.status == B_OK) {
				reply->AddInt16("count", params.count);
				reply->AddData("characteristics", B_ANY_TYPE, params.chars,
					params.count * sizeof(gatt_characteristic_t));
			}
			return B_OK;
		}

		case 2:
		{
			int16 startHandle, endHandle;
			if (message->FindInt16("start_handle", &startHandle) != B_OK
				|| message->FindInt16("end_handle", &endHandle) != B_OK)
				return B_BAD_VALUE;

			bt_le_gatt_discover_descriptors_params params = {};
			params.conn_handle = connHandle;
			params.start_handle = startHandle;
			params.end_handle = endHandle;

			const void* charData;
			ssize_t charSize;
			if (message->FindData("characteristic", B_ANY_TYPE, &charData,
					&charSize) == B_OK
				&& charSize == sizeof(gatt_characteristic_t)) {
				memcpy(&params.characteristic, charData,
					sizeof(gatt_characteristic_t));
			}

			status_t result = dev->LeGattDiscoverDescriptors(&params);
			reply->AddInt32("status", params.status);

			if (result == B_OK && params.status == B_OK) {
				reply->AddData("characteristic", B_ANY_TYPE,
					&params.characteristic,
					sizeof(gatt_characteristic_t));
			}
			return B_OK;
		}

		default:
			return B_BAD_VALUE;
	}
}


status_t
BluetoothServer::HandleLeGattRead(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle, attrHandle;
	if (message->FindInt16("conn_handle", &connHandle) != B_OK
		|| message->FindInt16("attr_handle", &attrHandle) != B_OK)
		return B_BAD_VALUE;

	bt_le_gatt_read_params params = {};
	params.conn_handle = connHandle;
	params.attr_handle = attrHandle;

	status_t result = dev->LeGattRead(&params);
	reply->AddInt32("status", params.status);

	if (result == B_OK && params.status == B_OK) {
		reply->AddData("value", B_ANY_TYPE, params.value,
			params.value_length);
	}

	return B_OK;
}


status_t
BluetoothServer::HandleLeGattWrite(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle, attrHandle;
	if (message->FindInt16("conn_handle", &connHandle) != B_OK
		|| message->FindInt16("attr_handle", &attrHandle) != B_OK)
		return B_BAD_VALUE;

	const void* valueData;
	ssize_t valueSize;
	if (message->FindData("value", B_ANY_TYPE, &valueData, &valueSize) != B_OK)
		return B_BAD_VALUE;

	bool noResponse = false;
	message->FindBool("no_response", &noResponse);

	bt_le_gatt_write_params params = {};
	params.conn_handle = connHandle;
	params.attr_handle = attrHandle;
	params.value_length = valueSize;
	if (valueSize > BT_GATT_MAX_VALUE_SIZE)
		return B_BAD_VALUE;
	memcpy(params.value, valueData, valueSize);

	if (noResponse)
		dev->LeGattWriteNoResponse(&params);
	else
		dev->LeGattWrite(&params);

	reply->AddInt32("status", params.status);

	return B_OK;
}


status_t
BluetoothServer::HandleLeGattSubscribe(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle, cccHandle;
	if (message->FindInt16("conn_handle", &connHandle) != B_OK
		|| message->FindInt16("ccc_handle", &cccHandle) != B_OK)
		return B_BAD_VALUE;

	bool enable = true;
	message->FindBool("enable", &enable);

	bt_le_gatt_subscribe_params params = {};
	params.conn_handle = connHandle;
	params.ccc_handle = cccHandle;
	params.enable = enable;

	dev->LeGattSubscribe(&params);
	reply->AddInt32("status", params.status);

	return B_OK;
}


#if 0
#pragma mark - LE Pairing Handlers
#endif


status_t
BluetoothServer::HandleLePair(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle;
	if (message->FindInt16("handle", &connHandle) != B_OK)
		return B_BAD_VALUE;

	int32 passkey = 0;
	message->FindInt32("passkey", &passkey);

	bt_le_smp_pair_params params;
	params.conn_handle = connHandle;
	params.passkey = (uint32)passkey;

	return dev->LeSmPairInitiate(&params);
}


status_t
BluetoothServer::HandleLeNcConfirm(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* dev = LocateDelegateFromMessage(message);
	if (dev == NULL && fLocalDevicesList.CountItems() > 0)
		dev = fLocalDevicesList.ItemAt(0);
	if (dev == NULL)
		return B_ERROR;

	int16 connHandle;
	if (message->FindInt16("handle", &connHandle) != B_OK)
		return B_BAD_VALUE;

	bool confirmed = false;
	message->FindBool("confirmed", &confirmed);

	return dev->LeNcConfirm(connHandle, confirmed);
}


#if 0
#pragma mark - Paired Device Handlers
#endif


status_t
BluetoothServer::HandleGetPairedDevices(BMessage* message, BMessage* reply)
{
	const BMessage& keys = fKeyStore.Keys();
	int32 count = 0;

	// Collect unique bdaddr strings from "lk:" and "ltk:" prefixed keys
	char* name;
	type_code type;
	int32 countFound;
	for (int32 i = 0;
		keys.GetInfo(B_RAW_TYPE, i, &name, &type, &countFound) == B_OK;
		i++) {
		BString keyName(name);
		BString addrStr;

		if (keyName.FindFirst("lk:") == 0)
			addrStr = keyName.String() + 3;
		else if (keyName.FindFirst("ltk:") == 0)
			addrStr = keyName.String() + 4;
		else
			continue;

		// Check for duplicates already added
		BString existing;
		bool duplicate = false;
		for (int32 j = 0;
			reply->FindString("bdaddr", j, &existing) == B_OK; j++) {
			if (existing == addrStr) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		// Look up friendly name
		bdaddr_t addr;
		if (sscanf(addrStr.String(), "%hhX:%hhX:%hhX:%hhX:%hhX:%hhX",
				&addr.b[5], &addr.b[4], &addr.b[3],
				&addr.b[2], &addr.b[1], &addr.b[0]) != 6)
			continue;

		BString friendlyName;
		fKeyStore.FindDeviceName(addr, &friendlyName);

		uint32 cod = 0;
		fKeyStore.FindDeviceClass(addr, &cod);

		reply->AddString("bdaddr", addrStr);
		reply->AddString("name", friendlyName);
		reply->AddInt32("cod", (int32)cod);
		count++;
	}

	reply->AddInt32("count", count);
	TRACE_BT("BluetoothServer: GetPairedDevices returning %" B_PRId32
		" devices\n", count);

	return B_OK;
}


status_t
BluetoothServer::HandleSaveDeviceName(BMessage* message, BMessage* reply)
{
	const char* addrStr;
	const char* name;

	if (message->FindString("bdaddr", &addrStr) != B_OK
		|| message->FindString("name", &name) != B_OK)
		return B_BAD_VALUE;

	// Reject empty or error-marker names
	if (name[0] == '\0' || name[0] == '#')
		return B_BAD_VALUE;

	bdaddr_t addr;
	if (sscanf(addrStr, "%hhX:%hhX:%hhX:%hhX:%hhX:%hhX",
			&addr.b[5], &addr.b[4], &addr.b[3],
			&addr.b[2], &addr.b[1], &addr.b[0]) != 6)
		return B_BAD_VALUE;

	fKeyStore.AddDeviceName(addr, name);

	int32 cod;
	if (message->FindInt32("cod", &cod) == B_OK)
		fKeyStore.AddDeviceClass(addr, (uint32)cod);

	fKeyStore.Save();

	TRACE_BT("BluetoothServer: Saved device name '%s' for %s\n",
		name, addrStr);

	return B_OK;
}


#if 0
#pragma mark -
#endif

int
main(int /*argc*/, char** /*argv*/)
{
	BluetoothServer* bluetoothServer = new BluetoothServer;

	bluetoothServer->Run();
	delete bluetoothServer;

	return 0;
}

