/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _LOCALDEVICE_IMPL_H_
#define _LOCALDEVICE_IMPL_H_

#include <String.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI_event_le.h>

#include "LocalDeviceHandler.h"

#include "HCIDelegate.h"
#include "HCIControllerAccessor.h"
#include "HCITransportAccessor.h"

class BluetoothKeyStore;

class LocalDeviceImpl : public LocalDeviceHandler {

protected:
	LocalDeviceImpl(HCIDelegate* hd);

public:

	// Factory methods
	static LocalDeviceImpl* CreateControllerAccessor(BPath* path);
	static LocalDeviceImpl* CreateTransportAccessor(BPath* path);
	~LocalDeviceImpl();
	void Unregister();

	void HandleEvent(struct hci_event_header* event);

	// Request handling
	status_t ProcessSimpleRequest(BMessage* request);

private:
	void HandleUnexpectedEvent(struct hci_event_header* event);
	void HandleExpectedRequest(struct hci_event_header* event,
		BMessage* request);

	// Events handling
	void CommandComplete(struct hci_ev_cmd_complete* event, BMessage* request,
		int32 index);
	void CommandStatus(struct hci_ev_cmd_status* event, BMessage* request,
		int32 index);

	void NumberOfCompletedPackets(struct hci_ev_num_comp_pkts* event);

	// Inquiry
	void InquiryResult(uint8* numberOfResponses, BMessage* request);
	void InquiryResultWithRSSI(uint8* numberOfResponses, BMessage* request);
	void ExtendedInquiryResult(uint8* numberOfResponses, BMessage* request);
	void InquiryComplete(uint8* status, BMessage* request);
	void RemoteNameRequestComplete(struct hci_ev_remote_name_request_complete_reply*
		remotename, BMessage* request);

	// Connection
	void ConnectionComplete(struct hci_ev_conn_complete* event, BMessage* request);
	void ConnectionRequest(struct hci_ev_conn_request* event, BMessage* request);
	void DisconnectionComplete(struct hci_ev_disconnection_complete_reply* event,
		BMessage* request);

	// Pairing
	void PinCodeRequest(struct hci_ev_pin_code_req* event, BMessage* request);
	void RoleChange(struct hci_ev_role_change* event, BMessage* request);
	void LinkKeyNotify(struct hci_ev_link_key_notify* event, BMessage* request);
	void ReturnLinkKeys(struct hci_ev_return_link_keys* returnedKeys);

	void LinkKeyRequested(struct hci_ev_link_key_req* keyReqyested,
		BMessage* request);

	// SSP (Secure Simple Pairing)
	void IoCapabilityRequest(struct hci_ev_io_capability_request* event,
		BMessage* request);
	void IoCapabilityResponse(struct hci_ev_io_capability_response* event);
	void UserConfirmationRequest(
		struct hci_ev_user_confirmation_request* event, BMessage* request);
	void SimplePairingComplete(struct hci_ev_simple_pairing_complete* event,
		BMessage* request);

	// Synchronous connections (SCO/eSCO)
	void SyncConnectionComplete(
		struct hci_ev_sychronous_connection_completed* event,
		BMessage* request);

	void PageScanRepetitionModeChange(struct hci_ev_page_scan_rep_mode_change* event,
		BMessage* request);
	void MaxSlotChange(struct hci_ev_max_slot_change* event, BMessage* request);

	void HardwareError(struct hci_ev_hardware_error* event);

	// LE Events
	void HandleLeMetaEvent(struct hci_event_header* event);
	void SmpNumericComparisonRequest(
		struct hci_ev_smp_nc_request* event);
	void LeConnectionComplete(struct hci_ev_le_conn_complete* event);
	void LeAdvertisingReport(uint8* data, uint8 eventLength);
	void LeLtkRequest(struct hci_ev_le_ltk_request* event);

public:
	// Key store
	void SetKeyStore(BluetoothKeyStore* keyStore);
	void PushStoredLinkKeys();
	void ResetController();
	void DeleteControllerLinkKeys();

	// LE Operations (called by BluetoothServer)
	void StartLeScan(uint8 type, uint16 interval, uint16 window,
		uint8 filterDup);
	void StopLeScan();
	void CreateLeConnection(bdaddr_t bdaddr, uint8 addressType);

	// LE GATT operations (via ioctl to kernel)
	status_t LeGattExchangeMtu(bt_le_gatt_mtu_params* params);
	status_t LeGattDiscoverServices(
		bt_le_gatt_discover_services_params* params);
	status_t LeGattDiscoverCharacteristics(
		bt_le_gatt_discover_chars_params* params);
	status_t LeGattDiscoverDescriptors(
		bt_le_gatt_discover_descriptors_params* params);
	status_t LeGattRead(bt_le_gatt_read_params* params);
	status_t LeGattWrite(bt_le_gatt_write_params* params);
	status_t LeGattWriteNoResponse(bt_le_gatt_write_params* params);
	status_t LeGattSubscribe(bt_le_gatt_subscribe_params* params);

	// LE SMP operations
	status_t LeSmPairInitiate(bt_le_smp_pair_params* params);
	status_t LeNcConfirm(uint16 connHandle, bool confirmed);

private:
	BluetoothKeyStore*		fKeyStore;
	bdaddr_t				fLeConnectionAddress;
	uint16					fLeConnectionHandle;
};

#endif
