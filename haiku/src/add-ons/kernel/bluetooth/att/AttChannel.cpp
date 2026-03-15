/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AttChannel.h"

#include <string.h>

#include <ByteOrder.h>
#include <NetBufferUtilities.h>

#include <btDebug.h>
#include <btModules.h>


extern net_buffer_module_info* gBufferModule;
extern bt_hci_module_info* btDevices;


AttChannel::AttChannel(HciConnection* connection)
	:
	fConnection(connection),
	fMtu(ATT_DEFAULT_LE_MTU),
	fResponseLength(0),
	fExpectedOpcode(0),
	fNotificationCallback(NULL),
	fNotificationCookie(NULL)
{
	fResponseSem = create_sem(0, "att response");
}


AttChannel::~AttChannel()
{
	delete_sem(fResponseSem);
}


status_t
AttChannel::ReceiveData(net_buffer* buffer)
{
	if (buffer->size < 1) {
		gBufferModule->free(buffer);
		return B_ERROR;
	}

	uint8 opcode;
	gBufferModule->read(buffer, 0, &opcode, 1);

	TRACE("%s: opcode=%#x size=%" B_PRIu32 "\n", __func__, opcode,
		buffer->size);

	switch (opcode) {
		case ATT_OP_HANDLE_VALUE_NTF:
		{
			if (buffer->size < 3) {
				gBufferModule->free(buffer);
				return B_ERROR;
			}

			struct att_handle_value_ntf ntf;
			gBufferModule->read(buffer, 0, &ntf, sizeof(ntf));

			uint16 handle = B_LENDIAN_TO_HOST_INT16(ntf.handle);
			uint16 dataLength = buffer->size - sizeof(ntf);

			if (fNotificationCallback != NULL && dataLength > 0) {
				uint8 data[ATT_MAX_PDU_SIZE];
				gBufferModule->read(buffer, sizeof(ntf), data, dataLength);
				fNotificationCallback(handle, data, dataLength,
					fNotificationCookie);
			}

			gBufferModule->free(buffer);
			return B_OK;
		}

		case ATT_OP_HANDLE_VALUE_IND:
		{
			if (buffer->size < 3) {
				gBufferModule->free(buffer);
				return B_ERROR;
			}

			struct att_handle_value_ind ind;
			gBufferModule->read(buffer, 0, &ind, sizeof(ind));

			uint16 handle = B_LENDIAN_TO_HOST_INT16(ind.handle);
			uint16 dataLength = buffer->size - sizeof(ind);

			if (fNotificationCallback != NULL && dataLength > 0) {
				uint8 data[ATT_MAX_PDU_SIZE];
				gBufferModule->read(buffer, sizeof(ind), data, dataLength);
				fNotificationCallback(handle, data, dataLength,
					fNotificationCookie);
			}

			// Send confirmation
			uint8 cfm = ATT_OP_HANDLE_VALUE_CFM;
			_SendPdu(&cfm, 1);

			gBufferModule->free(buffer);
			return B_OK;
		}

		default:
			break;
	}

	// Response to a pending request
	if (opcode == fExpectedOpcode || opcode == ATT_OP_ERROR_RSP) {
		uint16 copySize = buffer->size;
		if (copySize > ATT_MAX_PDU_SIZE)
			copySize = ATT_MAX_PDU_SIZE;

		gBufferModule->read(buffer, 0, fResponseBuffer, copySize);
		fResponseLength = copySize;
		gBufferModule->free(buffer);

		release_sem(fResponseSem);
		return B_OK;
	}

	TRACE("%s: unexpected opcode %#x (expected %#x)\n", __func__,
		opcode, fExpectedOpcode);
	gBufferModule->free(buffer);
	return B_ERROR;
}


status_t
AttChannel::ExchangeMtu(uint16 clientMtu, uint16* _serverMtu)
{
	struct att_exchange_mtu_req req;
	req.opcode = ATT_OP_EXCHANGE_MTU_REQ;
	req.client_mtu = B_HOST_TO_LENDIAN_INT16(clientMtu);

	status_t status = _SendPdu((uint8*)&req, sizeof(req));
	if (status != B_OK)
		return status;

	uint8 response[ATT_MAX_PDU_SIZE];
	uint16 responseLength;
	status = _WaitForResponse(ATT_OP_EXCHANGE_MTU_RSP, response,
		&responseLength);
	if (status != B_OK)
		return status;

	if (response[0] == ATT_OP_ERROR_RSP) {
		if (responseLength >= sizeof(struct att_error_rsp)) {
			struct att_error_rsp* err = (struct att_error_rsp*)response;
			ERROR("%s: MTU exchange error %#x\n", __func__, err->error_code);
		}
		return B_ERROR;
	}

	if (responseLength < sizeof(struct att_exchange_mtu_rsp))
		return B_ERROR;

	struct att_exchange_mtu_rsp* rsp = (struct att_exchange_mtu_rsp*)response;
	uint16 serverMtu = B_LENDIAN_TO_HOST_INT16(rsp->server_mtu);

	fMtu = clientMtu < serverMtu ? clientMtu : serverMtu;
	if (fMtu < ATT_DEFAULT_LE_MTU)
		fMtu = ATT_DEFAULT_LE_MTU;

	if (_serverMtu != NULL)
		*_serverMtu = serverMtu;

	TRACE("%s: negotiated MTU=%d (client=%d server=%d)\n", __func__,
		fMtu, clientMtu, serverMtu);

	return B_OK;
}


status_t
AttChannel::FindInformation(uint16 startHandle, uint16 endHandle,
	uint8* _response, uint16* _responseLength)
{
	struct att_find_info_req req;
	req.opcode = ATT_OP_FIND_INFO_REQ;
	req.start_handle = B_HOST_TO_LENDIAN_INT16(startHandle);
	req.end_handle = B_HOST_TO_LENDIAN_INT16(endHandle);

	status_t status = _SendPdu((uint8*)&req, sizeof(req));
	if (status != B_OK)
		return status;

	return _WaitForResponse(ATT_OP_FIND_INFO_RSP, _response,
		_responseLength);
}


status_t
AttChannel::ReadByGroupType(uint16 startHandle, uint16 endHandle,
	const uint8* uuid, uint8 uuidLength, uint8* _response,
	uint16* _responseLength)
{
	uint8 pdu[sizeof(struct att_read_by_group_type_req) + 16];
	struct att_read_by_group_type_req* req
		= (struct att_read_by_group_type_req*)pdu;
	req->opcode = ATT_OP_READ_BY_GROUP_TYPE_REQ;
	req->start_handle = B_HOST_TO_LENDIAN_INT16(startHandle);
	req->end_handle = B_HOST_TO_LENDIAN_INT16(endHandle);
	memcpy(pdu + sizeof(*req), uuid, uuidLength);

	status_t status = _SendPdu(pdu, sizeof(*req) + uuidLength);
	if (status != B_OK)
		return status;

	return _WaitForResponse(ATT_OP_READ_BY_GROUP_TYPE_RSP, _response,
		_responseLength);
}


status_t
AttChannel::ReadByType(uint16 startHandle, uint16 endHandle,
	const uint8* uuid, uint8 uuidLength, uint8* _response,
	uint16* _responseLength)
{
	uint8 pdu[sizeof(struct att_read_by_type_req) + 16];
	struct att_read_by_type_req* req = (struct att_read_by_type_req*)pdu;
	req->opcode = ATT_OP_READ_BY_TYPE_REQ;
	req->start_handle = B_HOST_TO_LENDIAN_INT16(startHandle);
	req->end_handle = B_HOST_TO_LENDIAN_INT16(endHandle);
	memcpy(pdu + sizeof(*req), uuid, uuidLength);

	status_t status = _SendPdu(pdu, sizeof(*req) + uuidLength);
	if (status != B_OK)
		return status;

	return _WaitForResponse(ATT_OP_READ_BY_TYPE_RSP, _response,
		_responseLength);
}


status_t
AttChannel::ReadAttribute(uint16 handle, uint8* _value, uint16* _valueLength)
{
	struct att_read_req req;
	req.opcode = ATT_OP_READ_REQ;
	req.handle = B_HOST_TO_LENDIAN_INT16(handle);

	status_t status = _SendPdu((uint8*)&req, sizeof(req));
	if (status != B_OK)
		return status;

	uint8 response[ATT_MAX_PDU_SIZE];
	uint16 responseLength;
	status = _WaitForResponse(ATT_OP_READ_RSP, response, &responseLength);
	if (status != B_OK)
		return status;

	if (response[0] == ATT_OP_ERROR_RSP)
		return B_ERROR;

	uint16 valueLength = responseLength - 1; /* minus opcode */
	memcpy(_value, response + 1, valueLength);
	*_valueLength = valueLength;

	return B_OK;
}


status_t
AttChannel::WriteAttribute(uint16 handle, const uint8* value,
	uint16 valueLength)
{
	uint8 pdu[ATT_MAX_PDU_SIZE];
	struct att_write_req* req = (struct att_write_req*)pdu;
	req->opcode = ATT_OP_WRITE_REQ;
	req->handle = B_HOST_TO_LENDIAN_INT16(handle);
	memcpy(pdu + sizeof(*req), value, valueLength);

	status_t status = _SendPdu(pdu, sizeof(*req) + valueLength);
	if (status != B_OK)
		return status;

	uint8 response[ATT_MAX_PDU_SIZE];
	uint16 responseLength;
	status = _WaitForResponse(ATT_OP_WRITE_RSP, response, &responseLength);
	if (status != B_OK)
		return status;

	if (response[0] == ATT_OP_ERROR_RSP)
		return B_ERROR;

	return B_OK;
}


status_t
AttChannel::WriteCommand(uint16 handle, const uint8* value,
	uint16 valueLength)
{
	uint8 pdu[ATT_MAX_PDU_SIZE];
	struct att_write_cmd* cmd = (struct att_write_cmd*)pdu;
	cmd->opcode = ATT_OP_WRITE_CMD;
	cmd->handle = B_HOST_TO_LENDIAN_INT16(handle);
	memcpy(pdu + sizeof(*cmd), value, valueLength);

	return _SendPdu(pdu, sizeof(*cmd) + valueLength);
}


status_t
AttChannel::EnableNotifications(uint16 cccHandle, bool enable)
{
	uint16 value = enable ? B_HOST_TO_LENDIAN_INT16(GATT_CCC_NOTIFY) : 0;
	return WriteAttribute(cccHandle, (uint8*)&value, sizeof(value));
}


void
AttChannel::SetNotificationCallback(att_notification_callback callback,
	void* cookie)
{
	fNotificationCallback = callback;
	fNotificationCookie = cookie;
}


status_t
AttChannel::_SendPdu(const uint8* pdu, uint16 length)
{
	net_buffer* buffer = gBufferModule->create(
		length + sizeof(l2cap_basic_header));
	if (buffer == NULL)
		return B_NO_MEMORY;

	/* Write L2CAP basic header */
	l2cap_basic_header l2capHeader;
	l2capHeader.length = B_HOST_TO_LENDIAN_INT16(length);
	l2capHeader.dcid = B_HOST_TO_LENDIAN_INT16(L2CAP_ATT_CID);
	gBufferModule->append(buffer, &l2capHeader, sizeof(l2capHeader));

	/* Write ATT PDU */
	gBufferModule->append(buffer, pdu, length);

	buffer->type = fConnection->handle;
	status_t status = btDevices->PostACL(fConnection->Hid, buffer);
	if (status != B_OK) {
		gBufferModule->free(buffer);
		return status;
	}

	return B_OK;
}


status_t
AttChannel::_WaitForResponse(uint8 expectedOpcode, uint8* _response,
	uint16* _responseLength)
{
	fExpectedOpcode = expectedOpcode;
	fResponseLength = 0;

	status_t status = acquire_sem_etc(fResponseSem, 1,
		B_RELATIVE_TIMEOUT, ATT_RESPONSE_TIMEOUT);

	fExpectedOpcode = 0;

	if (status != B_OK) {
		ERROR("%s: timeout waiting for opcode %#x\n", __func__,
			expectedOpcode);
		return status;
	}

	memcpy(_response, fResponseBuffer, fResponseLength);
	*_responseLength = fResponseLength;

	return B_OK;
}
