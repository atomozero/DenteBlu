/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 */

#include "BluetoothServer.h"
#include "BluetoothKeyStore.h"

#include "LocalDeviceImpl.h"
#include "CommandManager.h"
#include "Debug.h"

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth_error.h>
#include <bluetooth/LinkKeyUtils.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_event_le.h>
#include <bluetooth/HCI/btHCI_command_le.h>

#include <bluetoothserver_p.h>
#include <ConnectionIncoming.h>
#include <NumericComparisonWindow.h>
#include <PincodeWindow.h>

#include <stdio.h>
#include <new>


#if 0
#pragma mark - Class methods -
#endif


// Factory methods
LocalDeviceImpl*
LocalDeviceImpl::CreateControllerAccessor(BPath* path)
{
	HCIDelegate* delegate = new(std::nothrow) HCIControllerAccessor(path);
	if (delegate == NULL)
		return NULL;

	LocalDeviceImpl* device = new(std::nothrow) LocalDeviceImpl(delegate);
	if (device == NULL) {
		delete delegate;
		return NULL;
	}

	return device;
}


LocalDeviceImpl*
LocalDeviceImpl::CreateTransportAccessor(BPath* path)
{
	HCIDelegate* delegate = new(std::nothrow) HCITransportAccessor(path);
	if (delegate == NULL)
		return NULL;

	LocalDeviceImpl* device = new(std::nothrow) LocalDeviceImpl(delegate);
	if (device == NULL) {
		delete delegate;
		return NULL;
	}

	return device;
}


LocalDeviceImpl::LocalDeviceImpl(HCIDelegate* hd)
	:
	LocalDeviceHandler(hd),
	fKeyStore(NULL),
	fLeConnectionHandle(0)
{
	memset(&fLeConnectionAddress, 0, sizeof(fLeConnectionAddress));
}


void
LocalDeviceImpl::SetKeyStore(BluetoothKeyStore* keyStore)
{
	fKeyStore = keyStore;
}


void
LocalDeviceImpl::PushStoredLinkKeys()
{
	if (fKeyStore == NULL)
		return;

	const BMessage& keys = fKeyStore->Keys();
	char* name;
	type_code type;
	int32 count;

	for (int32 i = 0;
		keys.GetInfo(B_RAW_TYPE, i, &name, &type, &count) == B_OK; i++) {

		if (strncmp(name, "lk:", 3) != 0)
			continue;

		// Parse bdaddr from "lk:XX:XX:XX:XX:XX:XX"
		unsigned int b[6];
		if (sscanf(name + 3, "%02X:%02X:%02X:%02X:%02X:%02X",
			&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
			continue;

		bdaddr_t addr;
		addr.b[5] = (uint8)b[0];
		addr.b[4] = (uint8)b[1];
		addr.b[3] = (uint8)b[2];
		addr.b[2] = (uint8)b[3];
		addr.b[1] = (uint8)b[4];
		addr.b[0] = (uint8)b[5];

		const void* data;
		ssize_t size;
		if (keys.FindData(name, B_RAW_TYPE, &data, &size) != B_OK)
			continue;

		if (size != 17) // 16 key bytes + 1 type byte
			continue;

		const uint8* keyData = (const uint8*)data;

		size_t cmdSize;
		void* cmd = buildWriteStoredLinkKey(addr, keyData, &cmdSize);
		if (cmd != NULL) {
			TRACE_BT("LocalDeviceImpl: Pushing link key for %s\n",
				name + 3);
			((HCITransportAccessor*)fHCIDelegate)
				->IssueCommand(cmd, cmdSize);
			free(cmd);
		}
	}
}


void
LocalDeviceImpl::ResetController()
{
	// Reset the HCI controller to clear stale ACL connections that
	// may persist from a previous server instance. Without this,
	// a phone that auto-connects would leave an ACL link that the
	// new server instance cannot use (btCoreData doesn't know about it).
	size_t cmdSize;
	void* cmd = buildReset(&cmdSize);
	if (cmd != NULL) {
		TRACE_BT("LocalDeviceImpl: Resetting HCI controller\n");

		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_RESET));
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(cmd, cmdSize) == B_ERROR)
			ClearWantedEvent(request);

		free(cmd);
		// Give controller time to reset
		snooze(500000);
	}
}


void
LocalDeviceImpl::DeleteControllerLinkKeys()
{
	// Delete all link keys from the controller so that any future
	// authentication MUST go through the host via Link_Key_Request.
	// This ensures the remote host's Bluetooth stack is aware of the
	// authentication state (required for Android's security checks).
	size_t cmdSize;
	void* cmd = buildDeleteStoredLinkKey(true, &cmdSize);
	if (cmd != NULL) {
		TRACE_BT("LocalDeviceImpl: Deleting all stored link keys "
			"from controller\n");

		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_DELETE_STORED_LINK_KEY));
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(cmd, cmdSize) == B_ERROR)
			ClearWantedEvent(request);

		free(cmd);
	}
}


LocalDeviceImpl::~LocalDeviceImpl()
{

}


void
LocalDeviceImpl::Unregister()
{
	BMessage* msg =	new	BMessage(BT_MSG_REMOVE_DEVICE);

	msg->AddInt32("hci_id", fHCIDelegate->Id());

	TRACE_BT("LocalDeviceImpl: Unregistering %" B_PRId32 "\n",
		fHCIDelegate->Id());

	be_app_messenger.SendMessage(msg);
}


#if 0
#pragma mark - Event handling methods -
#endif

template<typename T, typename Header>
inline T*
JumpEventHeader(Header* event)
{
	return (T*)(event + 1);
}


void
LocalDeviceImpl::HandleUnexpectedEvent(struct hci_event_header* event)
{
	// Events here might have not been initated by us
	// TODO: ML mark as handled pass a reply by parameter and reply in common
	switch (event->ecode) {
		case HCI_EVENT_HARDWARE_ERROR:
			HardwareError(
				JumpEventHeader<struct hci_ev_hardware_error>(event));
			break;

		case HCI_EVENT_CONN_REQUEST:
			ConnectionRequest(
				JumpEventHeader<struct hci_ev_conn_request>(event), NULL);
			break;

		case HCI_EVENT_NUM_COMP_PKTS:
			NumberOfCompletedPackets(
				JumpEventHeader<struct hci_ev_num_comp_pkts>(event));
			break;

		case HCI_EVENT_DISCONNECTION_COMPLETE:
			// should belong to a request?  can be sporadic or initiated by us?
			DisconnectionComplete(
				JumpEventHeader
					<struct hci_ev_disconnection_complete_reply>(event),
				NULL);
			break;
		case HCI_EVENT_PIN_CODE_REQ:
			PinCodeRequest(
				JumpEventHeader<struct hci_ev_pin_code_req>(event), NULL);
			break;
		case HCI_EVENT_IO_CAPABILITY_REQUEST:
			IoCapabilityRequest(
				JumpEventHeader<struct hci_ev_io_capability_request>(event),
				NULL);
			break;
		case HCI_EVENT_IO_CAPABILITY_RESPONSE:
			IoCapabilityResponse(
				JumpEventHeader<struct hci_ev_io_capability_response>(event));
			break;
		case HCI_EVENT_USER_CONFIRMATION_REQUEST:
			UserConfirmationRequest(
				JumpEventHeader<struct hci_ev_user_confirmation_request>(event),
				NULL);
			break;
		case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
			SimplePairingComplete(
				JumpEventHeader<struct hci_ev_simple_pairing_complete>(event),
				NULL);
			break;
		case HCI_EVENT_LINK_KEY_REQ:
			LinkKeyRequested(
				JumpEventHeader<struct hci_ev_link_key_req>(event), NULL);
			break;
		case HCI_EVENT_LINK_KEY_NOTIFY:
			LinkKeyNotify(
				JumpEventHeader<struct hci_ev_link_key_notify>(event), NULL);
			break;
		case HCI_EVENT_SMP_NC_REQUEST:
			SmpNumericComparisonRequest(
				JumpEventHeader<struct hci_ev_smp_nc_request>(event));
			break;
		case HCI_EVENT_AUTH_COMPLETE:
		{
			struct hci_ev_auth_complete* authEvent =
				JumpEventHeader<struct hci_ev_auth_complete>(event);
			TRACE_BT("LocalDeviceImpl: AuthComplete: "
				"handle=%#x status=%d\n",
				authEvent->handle, authEvent->status);

			if (authEvent->status == BT_OK) {
				// Authentication succeeded — now enable encryption
				TRACE_BT("LocalDeviceImpl: Requesting encryption for "
					"handle=%#x\n", authEvent->handle);
				size_t size;
				void* cmd = buildSetConnectionEncryption(
					authEvent->handle, 1, &size);
				if (cmd != NULL) {
					// Create a petition so CMD_STATUS and ENCRYPT_CHANGE
					// are routed to HandleExpectedRequest instead of lost
					BMessage* encRequest = new BMessage;
					encRequest->AddInt16("eventExpected",
						HCI_EVENT_CMD_STATUS);
					encRequest->AddInt16("opcodeExpected",
						PACK_OPCODE(OGF_LINK_CONTROL,
							OCF_SET_CONN_ENCRYPT));
					encRequest->AddInt16("eventExpected",
						HCI_EVENT_ENCRYPT_CHANGE);
					AddWantedEvent(encRequest);
					fHCIDelegate->IssueCommand(cmd, size);
					free(cmd);
				}
			}
			break;
		}
		case HCI_EVENT_ENCRYPT_CHANGE:
		{
			struct hci_ev_encrypt_change* encEvent =
				JumpEventHeader<struct hci_ev_encrypt_change>(event);
			TRACE_BT("LocalDeviceImpl: EncryptionChange: "
				"handle=%#x encrypt=%d status=%d\n",
				encEvent->handle, encEvent->encrypt, encEvent->status);
			break;
		}
		default:
			// TODO: feedback unexpected not handled
			break;
	}
}


void
LocalDeviceImpl::HandleExpectedRequest(struct hci_event_header* event,
	BMessage* request)
{
	// we are waiting for a reply
	switch (event->ecode) {
		case HCI_EVENT_INQUIRY_COMPLETE:
			InquiryComplete(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_INQUIRY_RESULT:
			InquiryResult(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_CONN_COMPLETE:
			ConnectionComplete(
				JumpEventHeader<struct hci_ev_conn_complete>(event), request);
			break;

		case HCI_EVENT_DISCONNECTION_COMPLETE:
			// should belong to a request?  can be sporadic or initiated by us?
			DisconnectionComplete(
				JumpEventHeader<struct hci_ev_disconnection_complete_reply>
				(event), request);
			break;

		case HCI_EVENT_AUTH_COMPLETE:
		{
			struct hci_ev_auth_complete* authEvent =
				JumpEventHeader<struct hci_ev_auth_complete>(event);
			TRACE_BT("LocalDeviceImpl: AuthenticationComplete: handle=%#x status=%d\n",
				authEvent->handle, authEvent->status);

			if (authEvent->status == BT_OK) {
				// Authentication succeeded — now enable encryption
				TRACE_BT("LocalDeviceImpl: Requesting encryption for "
					"handle=%#x\n", authEvent->handle);
				size_t size;
				void* cmd = buildSetConnectionEncryption(
					authEvent->handle, 1, &size);
				if (cmd != NULL) {
					// Create a petition so CMD_STATUS and ENCRYPT_CHANGE
					// are routed to HandleExpectedRequest instead of lost
					BMessage* encRequest = new BMessage;
					encRequest->AddInt16("eventExpected",
						HCI_EVENT_CMD_STATUS);
					encRequest->AddInt16("opcodeExpected",
						PACK_OPCODE(OGF_LINK_CONTROL,
							OCF_SET_CONN_ENCRYPT));
					encRequest->AddInt16("eventExpected",
						HCI_EVENT_ENCRYPT_CHANGE);
					AddWantedEvent(encRequest);
					fHCIDelegate->IssueCommand(cmd, size);
					free(cmd);
				}
			}

			if (request != NULL) {
				BMessage reply;
				reply.AddInt8("status", authEvent->status);
				request->SendReply(&reply);
				ClearWantedEvent(request);
			}
			break;
		}

		case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
			RemoteNameRequestComplete(
				JumpEventHeader
					<struct hci_ev_remote_name_request_complete_reply>
				(event), request);
			break;

		case HCI_EVENT_ENCRYPT_CHANGE:
		{
			struct hci_ev_encrypt_change* encEvent =
				JumpEventHeader<struct hci_ev_encrypt_change>(event);
			TRACE_BT("LocalDeviceImpl: EncryptionChange: handle=%#x encrypt=%d status=%d\n",
				encEvent->handle, encEvent->encrypt, encEvent->status);
			if (request != NULL) {
				BMessage reply;
				reply.AddInt8("status", encEvent->status);
				request->SendReply(&reply);
				ClearWantedEvent(request);
			}
			break;
		}

		case HCI_EVENT_CHANGE_CONN_LINK_KEY_COMPLETE:
			break;

		case HCI_EVENT_MASTER_LINK_KEY_COMPL:
			break;

		case HCI_EVENT_RMT_FEATURES:
			break;

		case HCI_EVENT_RMT_VERSION:
			break;

		case HCI_EVENT_QOS_SETUP_COMPLETE:
			break;

		case HCI_EVENT_FLUSH_OCCUR:
			break;

		case HCI_EVENT_ROLE_CHANGE:
			RoleChange(
				JumpEventHeader<struct hci_ev_role_change>(event), request);
			break;

		case HCI_EVENT_MODE_CHANGE:
			break;

		case HCI_EVENT_RETURN_LINK_KEYS:
			ReturnLinkKeys(
				JumpEventHeader<struct hci_ev_return_link_keys>(event));
			break;

		case HCI_EVENT_LINK_KEY_REQ:
			LinkKeyRequested(
				JumpEventHeader<struct hci_ev_link_key_req>(event), request);
			break;

		case HCI_EVENT_LINK_KEY_NOTIFY:
			LinkKeyNotify(
				JumpEventHeader<struct hci_ev_link_key_notify>(event), request);
			break;

		case HCI_EVENT_LOOPBACK_COMMAND:
			break;

		case HCI_EVENT_DATA_BUFFER_OVERFLOW:
			break;

		case HCI_EVENT_MAX_SLOT_CHANGE:
			MaxSlotChange(JumpEventHeader<struct hci_ev_max_slot_change>(event),
			request);
			break;

		case HCI_EVENT_READ_CLOCK_OFFSET_COMPL:
			break;

		case HCI_EVENT_CON_PKT_TYPE_CHANGED:
			break;

		case HCI_EVENT_QOS_VIOLATION:
			break;

		case HCI_EVENT_PAGE_SCAN_REP_MODE_CHANGE:
			PageScanRepetitionModeChange(
				JumpEventHeader<struct hci_ev_page_scan_rep_mode_change>(event),
				request);
			break;

		case HCI_EVENT_FLOW_SPECIFICATION:
			break;

		case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
			InquiryResultWithRSSI(JumpEventHeader<uint8>(event), request);
			break;
		case HCI_EVENT_EXTENDED_INQUIRY_RESULT:
			ExtendedInquiryResult(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_REMOTE_EXTENDED_FEATURES:
			break;

		case HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETED:
			SyncConnectionComplete(
				JumpEventHeader<struct hci_ev_sychronous_connection_completed>(event),
				request);
			break;

		case HCI_EVENT_SYNCHRONOUS_CONNECTION_CHANGED:
			break;

		case HCI_EVENT_IO_CAPABILITY_REQUEST:
			IoCapabilityRequest(
				JumpEventHeader<struct hci_ev_io_capability_request>(event),
				request);
			break;

		case HCI_EVENT_IO_CAPABILITY_RESPONSE:
			IoCapabilityResponse(
				JumpEventHeader<struct hci_ev_io_capability_response>(event));
			break;

		case HCI_EVENT_USER_CONFIRMATION_REQUEST:
			UserConfirmationRequest(
				JumpEventHeader<struct hci_ev_user_confirmation_request>(event),
				request);
			break;

		case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
			SimplePairingComplete(
				JumpEventHeader<struct hci_ev_simple_pairing_complete>(event),
				request);
			break;
	}
}


void
LocalDeviceImpl::HandleEvent(struct hci_event_header* event)
{
	TRACE_BT("### Incoming event: code=0x%02x len=%d\n",
		event->ecode, event->elen);
	BMessage* request = NULL;
	int32 eventIndexLocation;

	// Check if it is a requested one
	switch (event->ecode) {
		case HCI_EVENT_CMD_COMPLETE:
		{
			struct hci_ev_cmd_complete* commandComplete
				= JumpEventHeader<struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Incoming CommandComplete(%d) for %s\n", commandComplete->ncmd,
				BluetoothCommandOpcode(commandComplete->opcode));

			request = FindPetition(event->ecode, commandComplete->opcode,
				&eventIndexLocation);

			if (request != NULL)
				CommandComplete(commandComplete, request, eventIndexLocation);

			break;
		}
		case HCI_EVENT_CMD_STATUS:
		{
			struct hci_ev_cmd_status* commandStatus
				= JumpEventHeader<struct hci_ev_cmd_status>(event);

			TRACE_BT("LocalDeviceImpl: Incoming CommandStatus(%d)(%s) for %s\n", commandStatus->ncmd,
				BluetoothError(commandStatus->status),
				BluetoothCommandOpcode(commandStatus->opcode));

			request = FindPetition(event->ecode, commandStatus->opcode,
				&eventIndexLocation);
			if (request != NULL)
				CommandStatus(commandStatus, request, eventIndexLocation);

			break;
		}
		default:
			TRACE_BT("LocalDeviceImpl: Incoming %s event\n", BluetoothEvent(event->ecode));

			request = FindPetition(event->ecode);
			if (request != NULL)
				HandleExpectedRequest(event, request);

			break;
	}

	if (event->ecode == HCI_EVENT_LE_META) {
		HandleLeMetaEvent(event);
		return;
	}

	if (request == NULL) {
		TRACE_BT("LocalDeviceImpl: Event %s could not be understood or delivered\n",
			BluetoothEvent(event->ecode));
		HandleUnexpectedEvent(event);
	}
}


#if 0
#pragma mark -
#endif


void
LocalDeviceImpl::CommandComplete(struct hci_ev_cmd_complete* event,
	BMessage* request, int32 index)
{
	int16 opcodeExpected;
	BMessage reply;
	status_t status;

	// Handle command complete information
	request->FindInt16("opcodeExpected", index, &opcodeExpected);

	if (request->IsSourceWaiting() == false) {
		TRACE_BT("LocalDeviceImpl: Nobody waiting for the event\n");
	}

	switch ((uint16)opcodeExpected) {

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_LOCAL_VERSION):
		{
			struct hci_rp_read_loc_version* version
				= JumpEventHeader<struct hci_rp_read_loc_version,
				struct hci_ev_cmd_complete>(event);


			if (version->status == BT_OK) {

				if (!IsPropertyAvailable("hci_version"))
					fProperties->AddInt8("hci_version", version->hci_version);

				if (!IsPropertyAvailable("hci_revision")) {
					fProperties->AddInt16("hci_revision",
						version->hci_revision);
				}

				if (!IsPropertyAvailable("lmp_version"))
					fProperties->AddInt8("lmp_version", version->lmp_version);

				if (!IsPropertyAvailable("lmp_subversion")) {
					fProperties->AddInt16("lmp_subversion",
						version->lmp_subversion);
				}

				if (!IsPropertyAvailable("manufacturer")) {
					fProperties->AddInt16("manufacturer",
						version->manufacturer);
				}
			}

			TRACE_BT("LocalDeviceImpl: Reply for Local Version %x\n", version->status);

			reply.AddInt8("status", version->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case  PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_PG_TIMEOUT):
		{
			struct hci_rp_read_page_timeout* pageTimeout
				= JumpEventHeader<struct hci_rp_read_page_timeout,
				struct hci_ev_cmd_complete>(event);

			if (pageTimeout->status == BT_OK) {
				fProperties->AddInt16("page_timeout",
					pageTimeout->page_timeout);

				TRACE_BT("LocalDeviceImpl: Page Timeout=%x\n", pageTimeout->page_timeout);
			}

			reply.AddInt8("status", pageTimeout->status);
			reply.AddInt32("result", pageTimeout->page_timeout);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_LOCAL_FEATURES):
		{
			struct hci_rp_read_loc_features* features
				= JumpEventHeader<struct hci_rp_read_loc_features,
				struct hci_ev_cmd_complete>(event);

			if (features->status == BT_OK) {

				if (!IsPropertyAvailable("features")) {
					fProperties->AddData("features", B_ANY_TYPE,
						&features->features, 8);

					uint16 packetType = HCI_DM1 | HCI_DH1 | HCI_HV1;

					bool roleSwitch
						= (features->features[0] & LMP_RSWITCH) != 0;
					bool encryptCapable
						= (features->features[0] & LMP_ENCRYPT) != 0;

					if (features->features[0] & LMP_3SLOT)
						packetType |= (HCI_DM3 | HCI_DH3);

					if (features->features[0] & LMP_5SLOT)
						packetType |= (HCI_DM5 | HCI_DH5);

					if (features->features[1] & LMP_HV2)
						packetType |= (HCI_HV2);

					if (features->features[1] & LMP_HV3)
						packetType |= (HCI_HV3);

					fProperties->AddInt16("packet_type", packetType);
					fProperties->AddBool("role_switch_capable", roleSwitch);
					fProperties->AddBool("encrypt_capable", encryptCapable);

					TRACE_BT("LocalDeviceImpl: Packet type %x role switch %d encrypt %d\n",
						packetType, roleSwitch, encryptCapable);
				}

			}

			TRACE_BT("LocalDeviceImpl: Reply for Local Features %x\n", features->status);

			reply.AddInt8("status", features->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BUFFER_SIZE):
		{
			struct hci_rp_read_buffer_size* buffer
				= JumpEventHeader<struct hci_rp_read_buffer_size,
				struct hci_ev_cmd_complete>(event);

			if (buffer->status == BT_OK) {

				if (!IsPropertyAvailable("acl_mtu"))
					fProperties->AddInt16("acl_mtu", buffer->acl_mtu);

				if (!IsPropertyAvailable("sco_mtu"))
					fProperties->AddInt8("sco_mtu", buffer->sco_mtu);

				if (!IsPropertyAvailable("acl_max_pkt"))
					fProperties->AddInt16("acl_max_pkt", buffer->acl_max_pkt);

				if (!IsPropertyAvailable("sco_max_pkt"))
					fProperties->AddInt16("sco_max_pkt", buffer->sco_max_pkt);

			}

			TRACE_BT("LocalDeviceImpl: Reply for Read Buffer Size %x\n", buffer->status);


			reply.AddInt8("status", buffer->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
		}
		break;

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BD_ADDR):
		{
			struct hci_rp_read_bd_addr* readbdaddr
				= JumpEventHeader<struct hci_rp_read_bd_addr,
				struct hci_ev_cmd_complete>(event);

			if (readbdaddr->status == BT_OK) {
				reply.AddData("bdaddr", B_ANY_TYPE, &readbdaddr->bdaddr,
					sizeof(bdaddr_t));
			}

			TRACE_BT("LocalDeviceImpl: Read bdaddr status = %x\n", readbdaddr->status);

			reply.AddInt8("status", readbdaddr->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
		}
		break;


		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_CLASS_OF_DEV):
		{
			struct hci_read_dev_class_reply* classDev
				= JumpEventHeader<struct hci_read_dev_class_reply,
				struct hci_ev_cmd_complete>(event);

			if (classDev->status == BT_OK) {
				reply.AddData("devclass", B_ANY_TYPE, &classDev->dev_class,
					sizeof(classDev->dev_class));
			}

			TRACE_BT("LocalDeviceImpl: Read DeviceClass status = %x DeviceClass = [%x][%x][%x]\n",
				classDev->status, classDev->dev_class[0],
				classDev->dev_class[1], classDev->dev_class[2]);


			reply.AddInt8("status", classDev->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_LOCAL_NAME):
		{
			struct hci_rp_read_local_name* readLocalName
				= JumpEventHeader<struct hci_rp_read_local_name,
				struct hci_ev_cmd_complete>(event);


			if (readLocalName->status == BT_OK) {
				reply.AddString("friendlyname",
					(const char*)readLocalName->local_name);
			}

			TRACE_BT("LocalDeviceImpl: Friendly name status %x\n", readLocalName->status);

			reply.AddInt8("status", readLocalName->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_PIN_CODE_REPLY):
		{
			uint8* statusReply = (uint8*)(event + 1);

			// TODO: This reply has to match the BDADDR of the outgoing message
			TRACE_BT("LocalDeviceImpl: pincode accept status %x\n", *statusReply);

			reply.AddInt8("status", *statusReply);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			//ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_PIN_CODE_NEG_REPLY):
		{
			uint8* statusReply = (uint8*)(event + 1);

			// TODO: This reply might match the BDADDR of the outgoing message
			// => FindPetition should be expanded....
			TRACE_BT("LocalDeviceImpl: pincode reject status %x\n", *statusReply);

			reply.AddInt8("status", *statusReply);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_STORED_LINK_KEY):
		{
			struct hci_read_stored_link_key_reply* linkKeyRetrieval
				= JumpEventHeader<struct hci_read_stored_link_key_reply,
				struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Status %s MaxKeys=%d, KeysRead=%d\n",
				BluetoothError(linkKeyRetrieval->status),
				linkKeyRetrieval->max_num_keys,
				linkKeyRetrieval->num_keys_read);

			reply.AddInt8("status", linkKeyRetrieval->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_NEG_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_REPLY):
		{
			struct hci_cp_link_key_reply_reply* linkKeyReply
				= JumpEventHeader<struct hci_cp_link_key_reply_reply,
				struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Status %s addresss=%s\n", BluetoothError(linkKeyReply->status),
				bdaddrUtils::ToString(linkKeyReply->bdaddr).String());

			ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			break;
		}

		case  PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_SCAN_ENABLE):
		{
			struct hci_read_scan_enable* scanEnable
				= JumpEventHeader<struct hci_read_scan_enable,
				struct hci_ev_cmd_complete>(event);

			if (scanEnable->status == BT_OK) {
				fProperties->AddInt8("scan_enable", scanEnable->enable);

				TRACE_BT("LocalDeviceImpl: enable = %x\n", scanEnable->enable);
			}

			reply.AddInt8("status", scanEnable->status);
			reply.AddInt8("scan_enable", scanEnable->enable);
			status = request->SendReply(&reply);
			printf("Sending reply. scan_enable = %d\n", scanEnable->enable);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_READ_BUFFER_SIZE):
		{
			struct hci_rp_le_read_buffer_size* leBuffer
				= JumpEventHeader<struct hci_rp_le_read_buffer_size,
				struct hci_ev_cmd_complete>(event);

			if (leBuffer->status == BT_OK) {
				if (!IsPropertyAvailable("le_mtu"))
					fProperties->AddInt16("le_mtu", leBuffer->le_mtu);
				if (!IsPropertyAvailable("le_max_pkt"))
					fProperties->AddInt8("le_max_pkt", leBuffer->le_max_pkt);
			}

			TRACE_BT("LocalDeviceImpl: LE Read Buffer Size status=%x mtu=%d max_pkt=%d\n",
				leBuffer->status, leBuffer->le_mtu, leBuffer->le_max_pkt);

			reply.AddInt8("status", leBuffer->status);
			status = request->SendReply(&reply);
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_SIMPLE_PAIRING_MODE):
		{
			struct hci_read_simple_pairing_mode_reply* modeReply
				= JumpEventHeader<struct hci_read_simple_pairing_mode_reply,
				struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Write Simple Pairing Mode status=%d\n",
				modeReply->status);

			reply.AddInt8("status", modeReply->status);
			status = request->SendReply(&reply);
			ClearWantedEvent(request);
			break;
		}

		// place here all CC that just replies a uint8 status
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_IO_CAPABILITY_REQUEST_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_USER_CONFIRMATION_REQUEST_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_USER_CONFIRMATION_NEG_REPLY):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_SET_EVENT_MASK):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_DELETE_STORED_LINK_KEY):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_RESET):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_SCAN_ENABLE):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CLASS_OF_DEV):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_PG_TIMEOUT):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CA_TIMEOUT):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_AUTH_ENABLE):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_EXT_INQUIRY_RSP):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_LOCAL_NAME):
		case PACK_OPCODE(OGF_VENDOR_CMD, OCF_WRITE_BCM2035_BDADDR):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_EVENT_MASK):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_PARAMS):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_ENABLE):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_ADV_PARAMS):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_ADV_DATA):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_ADV_ENABLE):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_RANDOM_ADDRESS):
		{
			reply.AddInt8("status", *(uint8*)(event + 1));

			TRACE_BT("LocalDeviceImpl: %s for %s status %x\n", __FUNCTION__,
				BluetoothCommandOpcode(opcodeExpected), *(uint8*)(event + 1));

			status = request->SendReply(&reply);
			printf("%s: Sending reply write...\n", __func__);
			if (status < B_OK)
				printf("%s: Error sending reply write!\n", __func__);

			ClearWantedEvent(request);
			break;
		}

		default:
			TRACE_BT("LocalDeviceImpl: Command Complete not handled\n");
			break;
	}
}


void
LocalDeviceImpl::CommandStatus(struct hci_ev_cmd_status* event,
	BMessage* request, int32 index)
{
	int16 opcodeExpected;
	BMessage reply;

	// Handle command complete information
	request->FindInt16("opcodeExpected", index, &opcodeExpected);

	if (request->IsSourceWaiting() == false) {
		TRACE_BT("LocalDeviceImpl: Nobody waiting for the event\n");
	}

	switch (opcodeExpected) {

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY):
		{
			reply.what = BT_MSG_INQUIRY_STARTED;

			TRACE_BT("LocalDeviceImpl: Inquiry status %x\n", event->status);

			reply.AddInt8("status", event->status);
			request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS,
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY));
		}
		break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_REMOTE_NAME_REQUEST):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN):
		case PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_CREATE_CONN):
		{
			if (event->status == BT_OK) {
				ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
			} else {
				TRACE_BT("LocalDeviceImpl: Command Status for %s error %x\n",
					BluetoothCommandOpcode(opcodeExpected), event->status);

				reply.AddInt8("status", event->status);
				request->SendReply(&reply);

				// Remove the ENTIRE petition — the command failed,
				// so none of the remaining expected events (CONN_COMPLETE,
				// LINK_KEY_REQ, etc.) will arrive for this request.
				// Leaving them as zombie entries would cause them to steal
				// events from future requests.
				ClearWantedEvent(request);
			}
		}
		break;
		/*
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_ACCEPT_CONN_REQ):
		{
			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS,
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_ACCEPT_CONN_REQ));
		}
		break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_REJECT_CONN_REQ):
		{
			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS,
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_REJECT_CONN_REQ));
		}
		break;*/

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT):
		{
			if (event->status == BT_OK) {
				ClearWantedEvent(request, HCI_EVENT_CMD_STATUS,
					opcodeExpected);
			} else {
				reply.AddInt8("status", event->status);
				request->SendReply(&reply);
				ClearWantedEvent(request);
			}
		}
		break;

		default:
			TRACE_BT("LocalDeviceImpl: Command Status not handled\n");
		break;
	}
}


void
LocalDeviceImpl::InquiryResult(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);

	// Always dump discovered addresses for diagnostics
	if (count > 0) {
		bdaddr_t* addrs = (bdaddr_t*)(numberOfResponses + 1);
		for (uint8 i = 0; i < count; i++) {
			fprintf(stderr, "InquiryResult: device %02x:%02x:%02x:%02x:%02x:%02x\n",
				addrs[i].b[5], addrs[i].b[4], addrs[i].b[3],
				addrs[i].b[2], addrs[i].b[1], addrs[i].b[0]);
		}
	}

	if (count == 0 || request == NULL)
		return;

	uint8* base_ptr = numberOfResponses + 1;

	// get pointers ready for the arrays
	bdaddr_t* bdaddr_array = (bdaddr_t*)base_ptr;
	uint8* page_repetition_mode_array = (uint8*)(bdaddr_array + count);
	uint8* scan_period_mode_array = page_repetition_mode_array + count;
	uint8* scan_mode_array = scan_period_mode_array + count;
	uint8* dev_class_array = scan_mode_array + count;
	uint16* clock_offset_array = (uint16*)(dev_class_array + (count * 3));

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	for (uint8 i = 0; i < count; i++) {
		TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d, scan=%d clock=%d\n",
			page_repetition_mode_array[i], scan_period_mode_array[i], scan_mode_array[i],
			clock_offset_array[i]);

		reply.AddData("bdaddr", B_ANY_TYPE, &bdaddr_array[i], sizeof(bdaddr_t));
		reply.AddData("dev_class", B_ANY_TYPE, &dev_class_array[i * 3], 3);
		reply.AddUInt8("page_repetition_mode", page_repetition_mode_array[i]);
		reply.AddUInt8("scan_period_mode", scan_period_mode_array[i]);
		reply.AddUInt8("scan_mode", scan_mode_array[i]);
		reply.AddUInt16("clock_offset", clock_offset_array[i]);
	}

	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::InquiryResultWithRSSI(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);
	if (count == 0 || request == NULL)
		return;

	uint8* base_ptr = numberOfResponses + 1;

	// get pointers ready for the parallel arrays
	bdaddr_t* bdaddr_array = (bdaddr_t*)base_ptr;
	uint8* page_repetition_mode_array = (uint8*)(bdaddr_array + count);
	uint8* scan_period_mode_array = page_repetition_mode_array + count;
	uint8* dev_class_array = scan_period_mode_array + count;
	uint16* clock_offset_array = (uint16*)(dev_class_array + (count * 3));
	int8* rssi_array = (int8*)(clock_offset_array + count);

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	for (uint8 i = 0; i < count; i++) {
		TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d clock=%d rssi=%d\n",
			page_repetition_mode_array[i], scan_period_mode_array[i], clock_offset_array[i],
			rssi_array[i]);

		reply.AddData("bdaddr", B_ANY_TYPE, &bdaddr_array[i], sizeof(bdaddr_t));
		reply.AddData("dev_class", B_ANY_TYPE, &dev_class_array[i * 3], 3);
		reply.AddUInt8("page_repetition_mode", page_repetition_mode_array[i]);
		reply.AddUInt8("scan_period_mode", scan_period_mode_array[i]);
		reply.AddUInt16("clock_offset", clock_offset_array[i]);
		reply.AddInt8("rssi", rssi_array[i]);
	}

	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::ExtendedInquiryResult(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);

	// the spec says count always equals 1
	if (count != 1 || request == NULL)
		return;

	hci_ev_extended_inquiry_info* info = (hci_ev_extended_inquiry_info*)(numberOfResponses + 1);

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d clock=%d rssi=%d\n",
		info->page_repetition_mode, info->scan_period_mode, info->clock_offset, info->rssi);

	reply.AddData("bdaddr", B_ANY_TYPE, &info->bdaddr, sizeof(bdaddr_t));
	reply.AddData("dev_class", B_ANY_TYPE, info->dev_class, 3);
	reply.AddUInt8("page_repetition_mode", info->page_repetition_mode);
	reply.AddUInt8("scan_period_mode", info->scan_period_mode);
	reply.AddUInt16("clock_offset", info->clock_offset);
	reply.AddInt8("rssi", info->rssi);

	// need to implement eir parsing
	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::InquiryComplete(uint8* status, BMessage* request)
{
	BMessage reply(BT_MSG_INQUIRY_COMPLETED);

	reply.AddInt8("status", *status);

	printf("%s: Sending reply...\n", __func__);
	status_t stat = request->SendReply(&reply);
	if (stat < B_OK)
		printf("%s: Error sending reply!\n", __func__);

	ClearWantedEvent(request);
}


void
LocalDeviceImpl::RemoteNameRequestComplete(
	struct hci_ev_remote_name_request_complete_reply* remotename,
	BMessage* request)
{
	BMessage reply;

	if (remotename->status == BT_OK) {
		reply.AddString("friendlyname", (const char*)remotename->remote_name );

		// Persist the name if this is a paired device
		if (fKeyStore != NULL
			&& fKeyStore->FindLinkKey(remotename->bdaddr, NULL, NULL)) {
			fKeyStore->AddDeviceName(remotename->bdaddr,
				(const char*)remotename->remote_name);
			fKeyStore->Save();
			TRACE_BT("LocalDeviceImpl: Device name \"%s\" persisted for %s\n",
				remotename->remote_name,
				bdaddrUtils::ToString(remotename->bdaddr).String());
		}
	}

	reply.AddInt8("status", remotename->status);

	TRACE_BT("LocalDeviceImpl: %s for %s with status %s\n",
		BluetoothEvent(HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE),
		bdaddrUtils::ToString(remotename->bdaddr).String(),
		BluetoothError(remotename->status));

	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply to BMessage request: %s!\n",
			__func__, strerror(status));

	// This request is not gonna be used anymore
	ClearWantedEvent(request);
}


void
LocalDeviceImpl::ConnectionRequest(struct hci_ev_conn_request* event,
	BMessage* request)
{
	size_t size;
	void* command;

	TRACE_BT("LocalDeviceImpl: Connection Incoming type %x from %s...\n",
		event->link_type, bdaddrUtils::ToString(event->bdaddr).String());

	// Persist the remote device's CoD from the connection request
	if (fKeyStore != NULL) {
		uint32 cod = (event->dev_class[2] << 16)
			| (event->dev_class[1] << 8) | event->dev_class[0];
		if (cod != 0) {
			fKeyStore->AddDeviceClass(event->bdaddr, cod);
			fKeyStore->Save();
		}
	}

	// TODO: add a possible request in the queue
	if (true) { // Check Preferences if we are to accept this connection

		// Keep ourselves as slave
		command = buildAcceptConnectionRequest(event->bdaddr, 0x01 , &size);

		BMessage* newrequest = new BMessage;
		newrequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		newrequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL,
			OCF_ACCEPT_CONN_REQ));

		newrequest->AddInt16("eventExpected", HCI_EVENT_CONN_COMPLETE);
		newrequest->AddInt16("eventExpected", HCI_EVENT_PIN_CODE_REQ);
		newrequest->AddInt16("eventExpected", HCI_EVENT_ROLE_CHANGE);
		newrequest->AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
		newrequest->AddInt16("eventExpected",
			HCI_EVENT_PAGE_SCAN_REP_MODE_CHANGE);
		// SSP events
		newrequest->AddInt16("eventExpected",
			HCI_EVENT_IO_CAPABILITY_REQUEST);
		newrequest->AddInt16("eventExpected",
			HCI_EVENT_IO_CAPABILITY_RESPONSE);
		newrequest->AddInt16("eventExpected",
			HCI_EVENT_USER_CONFIRMATION_REQUEST);
		newrequest->AddInt16("eventExpected",
			HCI_EVENT_SIMPLE_PAIRING_COMPLETE);

		#if 0
		newrequest->AddInt16("eventExpected", HCI_EVENT_MAX_SLOT_CHANGE);
		newrequest->AddInt16("eventExpected", HCI_EVENT_DISCONNECTION_COMPLETE);
		#endif

		AddWantedEvent(newrequest);

		if ((fHCIDelegate)->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Command issued error for Accepting connection\n");
				// remove the request?
		} else {
			TRACE_BT("LocalDeviceImpl: Command issued for Accepting connection\n");
		}
	}
}


void
LocalDeviceImpl::ConnectionComplete(struct hci_ev_conn_complete* event,
	BMessage* request)
{

	if (event->status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: %s: Address %s handle=%#x type=%d encrypt=%d\n", __FUNCTION__,
				bdaddrUtils::ToString(event->bdaddr).String(), event->handle,
				event->link_type, event->encrypt_mode);

		// Don't initiate auth from our side — let the phone's
		// security manager handle auth/encrypt when it needs to
		// (e.g. before opening RFCOMM). We still respond to
		// Link_Key_Request with our stored key.
		if (event->link_type == 0x01 && event->encrypt_mode == 0) {
			TRACE_BT("LocalDeviceImpl: ACL connected unencrypted, "
				"waiting for remote to initiate security "
				"(handle=%#x)\n", event->handle);
		}

	} else {
		TRACE_BT("LocalDeviceImpl: %s: failed with error %s\n", __FUNCTION__,
			BluetoothError(event->status));
	}

	// it was expected
	if (request != NULL) {
		BMessage reply;
		reply.AddInt8("status", event->status);

		if (event->status == BT_OK)
			reply.AddInt16("handle", event->handle);

		TRACE_BT("LocalDeviceImpl: %s: Sending reply (status=%d, "
			"handle=%#x, source_waiting=%d)...\n", __FUNCTION__,
			event->status, event->handle,
			request->IsSourceWaiting());
		status_t status = request->SendReply(&reply);
		TRACE_BT("LocalDeviceImpl: %s: SendReply result: %s\n",
			__FUNCTION__, strerror(status));

		// This request is not gonna be used anymore
		ClearWantedEvent(request);
	} else {
		TRACE_BT("LocalDeviceImpl: %s: No petition (request==NULL)\n",
			__FUNCTION__);
	}

}


void
LocalDeviceImpl::SyncConnectionComplete(
	struct hci_ev_sychronous_connection_completed* event, BMessage* request)
{
	if (event->status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: %s: SCO handle=%#x link_type=%d "
			"tx_interval=%u rx_pkt_len=%u tx_pkt_len=%u air_mode=%u\n",
			__FUNCTION__, event->handle, event->link_type,
			event->transmission_interval, event->rx_packet_length,
			event->tx_packet_length, event->air_mode);
	} else {
		TRACE_BT("LocalDeviceImpl: %s: failed status=%s\n",
			__FUNCTION__, BluetoothError(event->status));
	}

	if (request != NULL) {
		BMessage reply;
		reply.AddInt8("status", event->status);

		if (event->status == BT_OK) {
			reply.AddInt16("handle", event->handle);
			reply.AddInt8("link_type", event->link_type);
			reply.AddInt16("rx_pkt_len", event->rx_packet_length);
			reply.AddInt16("tx_pkt_len", event->tx_packet_length);
		}

		request->SendReply(&reply);
		ClearWantedEvent(request);
	}
}


void
LocalDeviceImpl::DisconnectionComplete(
	struct hci_ev_disconnection_complete_reply* event, BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Handle=%#x, reason=%s status=%x\n", __FUNCTION__, event->handle,
		BluetoothError(event->reason), event->status);

	if (request != NULL) {
		BMessage reply;
		reply.AddInt8("status", event->status);

		printf("%s: Sending reply...\n", __func__);
		status_t status = request->SendReply(&reply);
		if (status < B_OK)
			printf("%s: Error sending reply!\n", __func__);
		// debug reply.PrintToStream();

		ClearWantedEvent(request);
	}
}


void
LocalDeviceImpl::PinCodeRequest(struct hci_ev_pin_code_req* event,
	BMessage* request)
{
	PincodeWindow* iPincode = new PincodeWindow(event->bdaddr, GetID());
	iPincode->Show();
}


void
LocalDeviceImpl::RoleChange(hci_ev_role_change* event, BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s role=%d status=%d\n", __FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(), event->role, event->status);
}


void
LocalDeviceImpl::PageScanRepetitionModeChange(
	struct hci_ev_page_scan_rep_mode_change* event, BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s type=%d\n",	__FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(), event->page_scan_rep_mode);
}


void
LocalDeviceImpl::LinkKeyNotify(hci_ev_link_key_notify* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s, key=%s, type=%d\n", __FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(),
		LinkKeyUtils::ToString(event->link_key).String(), event->key_type);

	if (fKeyStore != NULL) {
		fKeyStore->AddLinkKey(event->bdaddr, event->link_key, event->key_type);
		fKeyStore->Save();
		TRACE_BT("LocalDeviceImpl: Link key persisted for %s\n",
			bdaddrUtils::ToString(event->bdaddr).String());
	}

	// Issue a Remote Name Request so we can persist the device name
	size_t size;
	void* command = buildRemoteNameRequest(event->bdaddr, 0x01, 0x0000,
		&size);
	if (command != NULL) {
		BMessage* nameRequest = new BMessage;
		nameRequest->AddInt16("eventExpected",
			HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE);
		nameRequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		nameRequest->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_REMOTE_NAME_REQUEST));
		AddWantedEvent(nameRequest);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing Remote Name Request "
				"after pairing\n");
			ClearWantedEvent(nameRequest);
		} else {
			TRACE_BT("LocalDeviceImpl: Remote Name Request issued for %s "
				"after pairing\n",
				bdaddrUtils::ToString(event->bdaddr).String());
		}
	}
}


void
LocalDeviceImpl::LinkKeyRequested(struct hci_ev_link_key_req* keyRequested,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s\n", __FUNCTION__,
		bdaddrUtils::ToString(keyRequested->bdaddr).String());

	linkkey_t key;
	uint8 type;

	if (fKeyStore != NULL
		&& fKeyStore->FindLinkKey(keyRequested->bdaddr, &key, &type)) {
		// Found a persisted link key — reply with it
		TRACE_BT("LocalDeviceImpl: Found persisted link key for %s "
			"(type=%d)\n",
			bdaddrUtils::ToString(keyRequested->bdaddr).String(), type);

		BluetoothCommand<typed_command(hci_cp_link_key_reply)>
			reply(OGF_LINK_CONTROL, OCF_LINK_KEY_REPLY);
		bdaddrUtils::Copy(reply->bdaddr, keyRequested->bdaddr);
		memcpy(reply->link_key, key.l, 16);

		if (fHCIDelegate->IssueCommand(reply.Data(),
				reply.Size()) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LINK_KEY_REPLY\n");
		} else {
			TRACE_BT("LocalDeviceImpl: LINK_KEY_REPLY sent for %s\n",
				bdaddrUtils::ToString(keyRequested->bdaddr).String());
		}
	} else {
		// No key found — force new pairing
		TRACE_BT("LocalDeviceImpl: No link key for %s, sending NEG_REPLY\n",
			bdaddrUtils::ToString(keyRequested->bdaddr).String());

		BluetoothCommand<typed_command(hci_cp_link_key_neg_reply)>
			negReply(OGF_LINK_CONTROL, OCF_LINK_KEY_NEG_REPLY);
		bdaddrUtils::Copy(negReply->bdaddr, keyRequested->bdaddr);

		if (fHCIDelegate->IssueCommand(negReply.Data(),
				negReply.Size()) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LINK_KEY_NEG_REPLY\n");
		} else {
			TRACE_BT("LocalDeviceImpl: LINK_KEY_NEG_REPLY sent for %s\n",
				bdaddrUtils::ToString(keyRequested->bdaddr).String());
		}
	}

	if (request != NULL)
		ClearWantedEvent(request, HCI_EVENT_LINK_KEY_REQ);
}


void
LocalDeviceImpl::ReturnLinkKeys(struct hci_ev_return_link_keys* returnedKeys)
{
	TRACE_BT("LocalDeviceImpl: %s: #keys=%d\n",
		__FUNCTION__, returnedKeys->num_keys);

	uint8 numKeys = returnedKeys->num_keys;

	struct link_key_info* linkKeys = &returnedKeys->link_keys;

	while (numKeys > 0) {

		TRACE_BT("LocalDeviceImpl: Address=%s key=%s\n",
			bdaddrUtils::ToString(linkKeys->bdaddr).String(),
			LinkKeyUtils::ToString(linkKeys->link_key).String());

		linkKeys++;
		numKeys--;
	}
}


void
LocalDeviceImpl::MaxSlotChange(struct hci_ev_max_slot_change* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Handle=%#x, max slots=%d\n", __FUNCTION__,
		event->handle, event->lmp_max_slots);
}


void
LocalDeviceImpl::HardwareError(struct hci_ev_hardware_error* event)
{
	TRACE_BT("LocalDeviceImpl: %s: hardware code=%#x\n",	__FUNCTION__, event->hardware_code);
}


void
LocalDeviceImpl::NumberOfCompletedPackets(struct hci_ev_num_comp_pkts* event)
{
	TRACE_BT("LocalDeviceImpl: %s: #Handles=%d\n", __FUNCTION__, event->num_hndl);

	struct handle_and_number* numberPackets
		= JumpEventHeader<handle_and_number, hci_ev_num_comp_pkts>(event);

	for (uint8 i = 0; i < event->num_hndl; i++) {

		TRACE_BT("LocalDeviceImpl: %s: Handle=%d #packets=%d\n", __FUNCTION__, numberPackets->handle,
			numberPackets->num_completed);

			numberPackets++;
	}
}


#if 0
#pragma mark - LE Event Methods -
#endif


void
LocalDeviceImpl::HandleLeMetaEvent(struct hci_event_header* event)
{
	struct hci_ev_le_meta* leMeta
		= JumpEventHeader<struct hci_ev_le_meta>(event);

	TRACE_BT("LocalDeviceImpl: LE Meta subevent=%#x\n", leMeta->subevent);

	switch (leMeta->subevent) {
		case HCI_LE_SUBEVENT_CONN_COMPLETE:
			LeConnectionComplete(
				JumpEventHeader<struct hci_ev_le_conn_complete,
					struct hci_ev_le_meta>(leMeta));
			break;

		case HCI_LE_SUBEVENT_ADVERTISING_REPORT:
		{
			uint8* data = (uint8*)(leMeta + 1);
			uint8 eventLength = event->elen
				- sizeof(struct hci_ev_le_meta);
			LeAdvertisingReport(data, eventLength);
			break;
		}

		case HCI_LE_SUBEVENT_CONN_UPDATE_COMPLETE:
			TRACE_BT("LocalDeviceImpl: LE Connection Update Complete\n");
			break;

		case HCI_LE_SUBEVENT_READ_REMOTE_FEATURES_COMPLETE:
			TRACE_BT("LocalDeviceImpl: LE Read Remote Features Complete\n");
			break;

		case HCI_LE_SUBEVENT_LTK_REQUEST:
			LeLtkRequest(
				JumpEventHeader<struct hci_ev_le_ltk_request,
					struct hci_ev_le_meta>(leMeta));
			break;

		default:
			TRACE_BT("LocalDeviceImpl: Unknown LE subevent %#x\n",
				leMeta->subevent);
			break;
	}
}


void
LocalDeviceImpl::LeConnectionComplete(struct hci_ev_le_conn_complete* event)
{
	if (event->status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: LE Connection Complete handle=%#x "
			"role=%d addr=%s\n", event->handle, event->role,
			bdaddrUtils::ToString(event->peer_address).String());

		// Cache peer address for LTK lookup on reconnect
		bdaddrUtils::Copy(fLeConnectionAddress, event->peer_address);
		fLeConnectionHandle = event->handle;

		BMessage notify(BT_MSG_LE_CONN_COMPLETE);
		notify.AddInt8("status", event->status);
		notify.AddInt16("handle", event->handle);
		notify.AddInt8("role", event->role);
		notify.AddData("bdaddr", B_ANY_TYPE, &event->peer_address,
			sizeof(bdaddr_t));
		be_app_messenger.SendMessage(&notify);
	} else {
		TRACE_BT("LocalDeviceImpl: LE Connection Complete failed: %s\n",
			BluetoothError(event->status));
	}
}


void
LocalDeviceImpl::LeAdvertisingReport(uint8* data, uint8 eventLength)
{
	if (eventLength < 1)
		return;

	uint8 numReports = data[0];
	uint8* ptr = data + 1;

	TRACE_BT("LocalDeviceImpl: LE Advertising Report numReports=%d\n",
		numReports);

	for (uint8 i = 0; i < numReports; i++) {
		if (ptr + sizeof(struct hci_ev_le_advertising_info) > data + eventLength)
			break;

		struct hci_ev_le_advertising_info* info
			= (struct hci_ev_le_advertising_info*)ptr;

		uint8 dataLen = info->data_length;
		uint8* advData = ptr + sizeof(struct hci_ev_le_advertising_info);
		int8 rssi = (int8)advData[dataLen];

		TRACE_BT("LocalDeviceImpl: LE Adv type=%d addr=%s rssi=%d datalen=%d\n",
			info->event_type,
			bdaddrUtils::ToString(info->address).String(),
			rssi, dataLen);

		BMessage report(BT_MSG_LE_SCAN_RESULT);
		report.AddInt8("event_type", info->event_type);
		report.AddInt8("address_type", info->address_type);
		report.AddData("bdaddr", B_ANY_TYPE, &info->address,
			sizeof(bdaddr_t));
		report.AddData("adv_data", B_ANY_TYPE, advData, dataLen);
		report.AddInt8("rssi", rssi);
		be_app_messenger.SendMessage(&report);

		ptr = advData + dataLen + 1; /* +1 for rssi */
	}
}


void
LocalDeviceImpl::LeLtkRequest(struct hci_ev_le_ltk_request* event)
{
	TRACE_BT("LocalDeviceImpl: %s: handle=%#x ediv=%#x\n", __FUNCTION__,
		event->handle, event->ediv);

	// We need the peer bdaddr to look up the LTK.
	// The LeConnectionComplete event stored it in a BMessage sent to the
	// app; for the key lookup we need to find the connection.
	// In the server context we don't have direct btCoreData access,
	// but we do have the peer address cached from LeConnectionComplete.
	// For now, we use the LE connection bdaddr stored in the server's
	// connection-complete message. Since the server only supports one LE
	// connection at a time currently, we store/retrieve via the handle.

	// Look up peer address from connection handle via a message to the app
	// For simplicity, we keep the bdaddr from the last LE connection in the
	// server. The proper solution is to maintain a handle→bdaddr map.

	if (fKeyStore == NULL) {
		TRACE_BT("LocalDeviceImpl: No key store, sending LTK NEG_REPLY\n");
		goto neg_reply;
	}

	{
		// Try all known LTK entries - match by ediv/rand from the event
		// For LE Secure Connections, ediv=0 and rand=0
		// We need the bdaddr - walk through the key store isn't practical
		// without the address. Use the LE connection peer address.
		// TODO: maintain a connection table mapping handle→bdaddr
		// For now, use fLeConnectionAddress set by LeConnectionComplete
		uint8 ltk[16];
		uint16 ediv;
		uint8 rand[8];

		if (fKeyStore->FindLtk(fLeConnectionAddress, ltk, &ediv, rand)) {
			TRACE_BT("LocalDeviceImpl: Found LTK for %s, sending REPLY\n",
				bdaddrUtils::ToString(fLeConnectionAddress).String());

			BluetoothCommand<typed_command(hci_cp_le_ltk_request_reply)>
				reply(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_REPLY);
			reply->handle = event->handle;
			memcpy(reply->ltk, ltk, 16);

			if (fHCIDelegate->IssueCommand(reply.Data(),
					reply.Size()) == B_ERROR) {
				TRACE_BT("LocalDeviceImpl: Error issuing LE_LTK_REQUEST_REPLY\n");
			}
			return;
		}

		TRACE_BT("LocalDeviceImpl: No LTK found for %s\n",
			bdaddrUtils::ToString(fLeConnectionAddress).String());
	}

neg_reply:
	{
		BluetoothCommand<typed_command(hci_cp_le_ltk_request_neg_reply)>
			negReply(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_NEG_REPLY);
		negReply->handle = event->handle;

		if (fHCIDelegate->IssueCommand(negReply.Data(),
				negReply.Size()) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LE_LTK_REQUEST_NEG_REPLY\n");
		}
	}
}


void
LocalDeviceImpl::StartLeScan(uint8 type, uint16 interval, uint16 window,
	uint8 filterDup)
{
	size_t size;
	void* command;

	// Set scan parameters
	command = buildLeSetScanParameters(type, interval, window,
		HCI_LE_ADDR_PUBLIC, 0x00, &size);
	if (command != NULL) {
		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_PARAMS));
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LE Set Scan Parameters\n");
			ClearWantedEvent(request);
		}
	}

	// Enable scan
	command = buildLeSetScanEnable(0x01, filterDup, &size);
	if (command != NULL) {
		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_ENABLE));
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LE Set Scan Enable\n");
			ClearWantedEvent(request);
		}
	}
}


void
LocalDeviceImpl::StopLeScan()
{
	size_t size;
	void* command = buildLeSetScanEnable(0x00, 0x00, &size);
	if (command != NULL) {
		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_ENABLE));
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LE Set Scan Disable\n");
			ClearWantedEvent(request);
		}
	}
}


void
LocalDeviceImpl::CreateLeConnection(bdaddr_t bdaddr, uint8 addressType)
{
	size_t size;
	void* command = buildLeCreateConnection(
		0x0060,		/* scan interval: 60ms */
		0x0030,		/* scan window: 30ms */
		0x00,		/* filter policy: no whitelist */
		addressType,
		bdaddr,
		HCI_LE_ADDR_PUBLIC,
		0x0018,		/* conn interval min: 30ms */
		0x0028,		/* conn interval max: 50ms */
		0x0000,		/* latency: 0 */
		0x002C,		/* supervision timeout: 440ms */
		&size);

	if (command != NULL) {
		BMessage* request = new BMessage;
		request->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		request->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_CREATE_CONN));
		request->AddInt16("eventExpected", HCI_EVENT_LE_META);
		AddWantedEvent(request);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Error issuing LE Create Connection\n");
			ClearWantedEvent(request);
		}
	}
}


#if 0
#pragma mark - LE GATT Methods -
#endif


status_t
LocalDeviceImpl::LeGattExchangeMtu(bt_le_gatt_mtu_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_EXCHANGE_MTU, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattDiscoverServices(
	bt_le_gatt_discover_services_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_DISCOVER_SERVICES, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattDiscoverCharacteristics(
	bt_le_gatt_discover_chars_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_DISCOVER_CHARACTERISTICS, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattDiscoverDescriptors(
	bt_le_gatt_discover_descriptors_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_DISCOVER_DESCRIPTORS, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattRead(bt_le_gatt_read_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_READ, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattWrite(bt_le_gatt_write_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_WRITE, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattWriteNoResponse(bt_le_gatt_write_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_WRITE_NO_RESPONSE, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeGattSubscribe(bt_le_gatt_subscribe_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_GATT_SUBSCRIBE, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeSmPairInitiate(bt_le_smp_pair_params* params)
{
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_SMP_INITIATE_PAIRING, params, sizeof(*params));
}


status_t
LocalDeviceImpl::LeNcConfirm(uint16 connHandle, bool confirmed)
{
	bt_le_nc_confirm_params params;
	params.conn_handle = connHandle;
	params.confirmed = confirmed;
	return ((HCITransportAccessor*)fHCIDelegate)->GattIoctl(
		BT_LE_SMP_NC_CONFIRM, &params, sizeof(params));
}


#if 0
#pragma mark - SSP (Secure Simple Pairing) Methods -
#endif


void
LocalDeviceImpl::IoCapabilityRequest(
	struct hci_ev_io_capability_request* event, BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: IO Capability Request from %s\n",
		bdaddrUtils::ToString(event->bdaddr).String());

	// Reply with our IO capabilities: DisplayYesNo, no OOB, MITM protection
	size_t size;
	void* command = buildIoCapabilityRequestReply(event->bdaddr,
		HCI_IO_DISPLAY_YES_NO, 0x00, 0x01, &size);

	if (command != NULL) {
		BMessage* newRequest = new BMessage;
		newRequest->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		newRequest->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL,
				OCF_IO_CAPABILITY_REQUEST_REPLY));
		AddWantedEvent(newRequest);

		if (fHCIDelegate->IssueCommand(command, size) == B_ERROR)
			TRACE_BT("LocalDeviceImpl: IO Capability Reply failed\n");
		else
			TRACE_BT("LocalDeviceImpl: IO Capability Reply sent\n");
	}
}


void
LocalDeviceImpl::IoCapabilityResponse(
	struct hci_ev_io_capability_response* event)
{
	TRACE_BT("LocalDeviceImpl: IO Capability Response from %s: "
		"io=%d oob=%d auth=%d\n",
		bdaddrUtils::ToString(event->bdaddr).String(),
		event->io_capability, event->oob_data_present,
		event->auth_requirements);
}


void
LocalDeviceImpl::UserConfirmationRequest(
	struct hci_ev_user_confirmation_request* event, BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: User Confirmation Request from %s "
		"value=%" B_PRIu32 "\n",
		bdaddrUtils::ToString(event->bdaddr).String(),
		event->numeric_value);

	// Show numeric comparison dialog for user confirmation.
	// The dialog itself sends the HCI reply (accept or reject)
	// via BT_MSG_HANDLE_SIMPLE_REQUEST, same as PincodeWindow.
	NumericComparisonWindow* ncWindow = new NumericComparisonWindow(
		event->bdaddr, GetID(), event->numeric_value);
	ncWindow->Show();
}


void
LocalDeviceImpl::SimplePairingComplete(
	struct hci_ev_simple_pairing_complete* event, BMessage* request)
{
	if (event->status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: Simple Pairing Complete OK with %s\n",
			bdaddrUtils::ToString(event->bdaddr).String());
	} else {
		TRACE_BT("LocalDeviceImpl: Simple Pairing FAILED status=%d "
			"with %s\n", event->status,
			bdaddrUtils::ToString(event->bdaddr).String());
	}

	if (request != NULL)
		ClearWantedEvent(request, HCI_EVENT_SIMPLE_PAIRING_COMPLETE);
}


void
LocalDeviceImpl::SmpNumericComparisonRequest(
	struct hci_ev_smp_nc_request* event)
{
	TRACE_BT("LocalDeviceImpl: SMP NC Request from %s "
		"handle=%#x value=%" B_PRIu32 "\n",
		bdaddrUtils::ToString(event->bdaddr).String(),
		event->conn_handle, event->numeric_value);

	NumericComparisonWindow* ncWindow = new NumericComparisonWindow(
		event->bdaddr, GetID(), event->numeric_value,
		event->conn_handle);
	ncWindow->Show();
}


#if 0
#pragma mark - Request Methods -
#endif

status_t
LocalDeviceImpl::ProcessSimpleRequest(BMessage* request)
{
	ssize_t size;
	void* command = NULL;

	if (request->FindData("raw command", B_ANY_TYPE, 0,
		(const void **)&command, &size) == B_OK) {

		// Give the chance of just issuing the command
		int16 eventFound;
		if (request->FindInt16("eventExpected", &eventFound) == B_OK)
			AddWantedEvent(request);
		// LEAK: is command buffer freed within the Message?
		if (((HCITransportAccessor*)fHCIDelegate)->IssueCommand(command, size)
			== B_ERROR) {
			// TODO:
			// Reply the request with error!
			// Remove the just added request
			TRACE_BT("LocalDeviceImpl: ## ERROR Command issue, REMOVING!\n");
			ClearWantedEvent(request);

		} else {
			return B_OK;
		}
	} else {
		TRACE_BT("LocalDeviceImpl: No command specified for simple request\n");
	}

	return B_ERROR;
}
