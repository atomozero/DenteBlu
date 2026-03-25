/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist
 * Copyright 2012 Fredrik Modéen [firstname]@[lastname].se
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _COMMAND_MANAGER_H
#define _COMMAND_MANAGER_H

#include <malloc.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bluetooth_error.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_command_le.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_event_le.h>

#include <Message.h>
#include <Messenger.h>

#include <bluetoothserver_p.h>

#define typed_command(type) type, sizeof(type)

template <typename Type = void, int paramSize = 0,
	int HeaderSize = HCI_COMMAND_HDR_SIZE>
class BluetoothCommand {

public:
	BluetoothCommand(uint8 ogf, uint8 ocf)
	{
		fHeader = (struct hci_command_header*) fBuffer;

		if (paramSize != 0)
			fContent = (Type*)(fHeader + 1);
		else
			// avoid pointing outside in case of not having parameters
			fContent = (Type*)fHeader;

		fHeader->opcode = B_HOST_TO_LENDIAN_INT16(PACK_OPCODE(ogf, ocf));
		fHeader->clen = paramSize;
	}

	Type*
	operator->() const
	{
 		return fContent;
	}

	void*
	Data() const
	{
		return (void*)fBuffer;
	}

	size_t Size() const
	{
		return HeaderSize + paramSize;
	}

private:
	char fBuffer[paramSize + HeaderSize];
	Type* fContent;
	struct hci_command_header* fHeader;
};


status_t
NonParameterCommandRequest(uint8 ofg, uint8 ocf, int32* result, hci_id hId,
	BMessenger* messenger);

template<typename PARAMETERCONTAINER, typename PARAMETERTYPE>
status_t
SingleParameterCommandRequest(uint8 ofg, uint8 ocf, PARAMETERTYPE parameter,
	int32* result, hci_id hId, BMessenger* messenger)
{
	int8 bt_status = BT_ERROR;

	BluetoothCommand<typed_command(PARAMETERCONTAINER)>
		simpleCommand(ofg, ocf);

	simpleCommand->param = parameter;

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	simpleCommand->param = parameter;

	request.AddInt32("hci_id", hId);
	request.AddData("raw command", B_ANY_TYPE, simpleCommand.Data(),
		simpleCommand.Size());
	request.AddInt16("eventExpected",  HCI_EVENT_CMD_COMPLETE);
	request.AddInt16("opcodeExpected", PACK_OPCODE(ofg, ocf));

	if (messenger->SendMessage(&request, &reply) == B_OK) {
		reply.FindInt8("status", &bt_status);
		if (result != NULL)
			reply.FindInt32("result", result);
	}

	return bt_status;
}


/* CONTROL BASEBAND */
void* buildSetEventMask(uint8 mask[8], size_t* outsize);
void* buildReset(size_t* outsize);
void* buildReadLocalName(size_t* outsize);
void* buildReadScan(size_t* outsize);
void* buildWriteScan(uint8 scanmode, size_t* outsize);
void* buildReadClassOfDevice(size_t* outsize);
void* buildWriteClassOfDevice(uint8 devClass[3], size_t* outsize);
void* buildWriteStoredLinkKey(bdaddr_t bdaddr, const uint8 key[16],
	size_t* outsize);
void* buildDeleteStoredLinkKey(bool deleteAll, size_t* outsize);

/* LINK CONTROL */
void* buildReadRemoteFeatures(uint16 handle, size_t* outsize);
void* buildReadRemoteExtendedFeatures(uint16 handle, uint8 pageNumber,
	size_t* outsize);
void* buildAuthenticationRequested(uint16 handle, size_t* outsize);
void* buildSetConnectionEncryption(uint16 handle, uint8 enable,
	size_t* outsize);
void* buildRemoteNameRequest(bdaddr_t bdaddr, uint8 pscan_rep_mode,
	uint16 clock_offset, size_t* outsize);
void* buildInquiry(uint32 lap, uint8 length, uint8 num_rsp, size_t* outsize);
void* buildInquiryCancel(size_t* outsize);
void* buildPinCodeRequestReply(bdaddr_t bdaddr, uint8 length, char pincode[16],
	size_t* outsize);
void* buildPinCodeRequestNegativeReply(bdaddr_t bdaddr, size_t* outsize);
void* buildAcceptConnectionRequest(bdaddr_t bdaddr, uint8 role,
	size_t* outsize);
void* buildRejectConnectionRequest(bdaddr_t bdaddr, size_t* outsize);

/* OGF_INFORMATIONAL_PARAM */
void* buildReadLocalVersionInformation(size_t* outsize);
void* buildReadBufferSize(size_t* outsize);
void* buildReadBdAddr(size_t* outsize);

/* OGF_LE_CONTROL */
void* buildLeSetEventMask(uint8 mask[8], size_t* outsize);
void* buildLeReadBufferSize(size_t* outsize);
void* buildLeSetScanParameters(uint8 type, uint16 interval, uint16 window,
	uint8 ownAddressType, uint8 filterPolicy, size_t* outsize);
void* buildLeSetScanEnable(uint8 enable, uint8 filterDup, size_t* outsize);
void* buildLeCreateConnection(uint16 scanInterval, uint16 scanWindow,
	uint8 filterPolicy, uint8 peerAddressType, bdaddr_t peerAddress,
	uint8 ownAddressType, uint16 connIntervalMin, uint16 connIntervalMax,
	uint16 connLatency, uint16 supervisionTimeout, size_t* outsize);
void* buildLeCreateConnectionCancel(size_t* outsize);
void* buildLeStartEncryption(uint16 handle, const uint8 random[8],
	uint16 ediv, const uint8 ltk[16], size_t* outsize);
void* buildLeSetDataLength(uint16 handle, uint16 txOctets,
	uint16 txTime, size_t* outsize);
void* buildLeLtkRequestReply(uint16 handle, const uint8 ltk[16],
	size_t* outsize);
void* buildLeLtkRequestNegReply(uint16 handle, size_t* outsize);

/* EIR (Extended Inquiry Response) */
void* buildWriteExtendedInquiryResponse(const uint16* uuids16,
	uint8 uuidCount, const char* name, size_t* outsize);

/* SSP Commands */
void* buildWriteSimplePairingMode(uint8 mode, size_t* outsize);
void* buildIoCapabilityRequestReply(bdaddr_t bdaddr, uint8 ioCapability,
	uint8 oobDataPresent, uint8 authRequirements, size_t* outsize);
void* buildUserConfirmationRequestReply(bdaddr_t bdaddr, size_t* outsize);
void* buildUserConfirmationRequestNegReply(bdaddr_t bdaddr, size_t* outsize);

#endif
