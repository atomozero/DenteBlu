/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <GattClient.h>
#include "AttChannel.h"

#include <string.h>

#include <ByteOrder.h>

#include <btDebug.h>


GattClient::GattClient(AttChannel* channel)
	:
	fChannel(channel)
{
}


GattClient::~GattClient()
{
}


status_t
GattClient::ExchangeMtu(uint16 desiredMtu)
{
	uint16 serverMtu;
	return fChannel->ExchangeMtu(desiredMtu, &serverMtu);
}


status_t
GattClient::DiscoverPrimaryServices(gatt_service_t* _services,
	uint16* _count)
{
	uint16 startHandle = 0x0001;
	uint16 count = 0;
	uint16 maxCount = *_count;
	uint8 uuid16[2];

	uuid16[0] = (uint8)(GATT_UUID_PRIMARY_SERVICE & 0xFF);
	uuid16[1] = (uint8)(GATT_UUID_PRIMARY_SERVICE >> 8);

	while (startHandle != 0 && count < maxCount) {
		uint8 response[ATT_MAX_PDU_SIZE];
		uint16 responseLength;

		status_t status = fChannel->ReadByGroupType(startHandle, 0xFFFF,
			uuid16, 2, response, &responseLength);
		if (status != B_OK)
			break;

		if (response[0] == ATT_OP_ERROR_RSP) {
			if (responseLength >= sizeof(struct att_error_rsp)) {
				struct att_error_rsp* err = (struct att_error_rsp*)response;
				if (err->error_code == ATT_ERR_ATTRIBUTE_NOT_FOUND)
					break;
			}
			break;
		}

		if (response[0] != ATT_OP_READ_BY_GROUP_TYPE_RSP || responseLength < 2)
			break;

		uint8 itemLength = response[1];
		uint8* data = response + 2;
		uint16 dataLength = responseLength - 2;

		while (dataLength >= itemLength && count < maxCount) {
			gatt_service_t* svc = &_services[count];
			svc->start_handle = B_LENDIAN_TO_HOST_INT16(*(uint16*)data);
			svc->end_handle = B_LENDIAN_TO_HOST_INT16(*(uint16*)(data + 2));

			uint8 uuidLen = itemLength - 4;
			_ParseUuid(data + 4, uuidLen, &svc->uuid);

			TRACE("%s: service handle=[%#x,%#x] uuid_type=%d\n",
				__func__, svc->start_handle, svc->end_handle,
				svc->uuid.type);

			count++;
			startHandle = svc->end_handle + 1;
			if (startHandle == 0)
				break;

			data += itemLength;
			dataLength -= itemLength;
		}
	}

	*_count = count;
	return B_OK;
}


status_t
GattClient::DiscoverPrimaryServiceByUuid(const bt_uuid_t& uuid,
	gatt_service_t* _service)
{
	gatt_service_t services[GATT_MAX_SERVICES];
	uint16 count = GATT_MAX_SERVICES;

	status_t status = DiscoverPrimaryServices(services, &count);
	if (status != B_OK)
		return status;

	for (uint16 i = 0; i < count; i++) {
		if (_UuidEquals(services[i].uuid, uuid)) {
			*_service = services[i];
			return B_OK;
		}
	}

	return B_NAME_NOT_FOUND;
}


status_t
GattClient::DiscoverCharacteristics(uint16 startHandle, uint16 endHandle,
	gatt_characteristic_t* _chars, uint16* _count)
{
	uint16 searchHandle = startHandle;
	uint16 count = 0;
	uint16 maxCount = *_count;
	uint8 uuid16[2];

	uuid16[0] = (uint8)(GATT_UUID_CHARACTERISTIC & 0xFF);
	uuid16[1] = (uint8)(GATT_UUID_CHARACTERISTIC >> 8);

	while (searchHandle <= endHandle && count < maxCount) {
		uint8 response[ATT_MAX_PDU_SIZE];
		uint16 responseLength;

		status_t status = fChannel->ReadByType(searchHandle, endHandle,
			uuid16, 2, response, &responseLength);
		if (status != B_OK)
			break;

		if (response[0] == ATT_OP_ERROR_RSP) {
			if (responseLength >= sizeof(struct att_error_rsp)) {
				struct att_error_rsp* err = (struct att_error_rsp*)response;
				if (err->error_code == ATT_ERR_ATTRIBUTE_NOT_FOUND)
					break;
			}
			break;
		}

		if (response[0] != ATT_OP_READ_BY_TYPE_RSP || responseLength < 2)
			break;

		uint8 itemLength = response[1];
		uint8* data = response + 2;
		uint16 dataLength = responseLength - 2;

		while (dataLength >= itemLength && count < maxCount) {
			gatt_characteristic_t* chr = &_chars[count];
			chr->handle = B_LENDIAN_TO_HOST_INT16(*(uint16*)data);
			chr->properties = data[2];
			chr->value_handle = B_LENDIAN_TO_HOST_INT16(*(uint16*)(data + 3));
			chr->ccc_handle = 0;

			uint8 uuidLen = itemLength - 5;
			_ParseUuid(data + 5, uuidLen, &chr->uuid);

			TRACE("%s: char handle=%#x value=%#x props=%#x uuid_type=%d\n",
				__func__, chr->handle, chr->value_handle,
				chr->properties, chr->uuid.type);

			count++;
			searchHandle = chr->handle + 1;

			data += itemLength;
			dataLength -= itemLength;
		}
	}

	*_count = count;
	return B_OK;
}


status_t
GattClient::DiscoverDescriptors(uint16 startHandle, uint16 endHandle,
	gatt_characteristic_t* _char)
{
	uint16 searchHandle = startHandle;

	while (searchHandle <= endHandle) {
		uint8 response[ATT_MAX_PDU_SIZE];
		uint16 responseLength;

		status_t status = fChannel->FindInformation(searchHandle, endHandle,
			response, &responseLength);
		if (status != B_OK)
			break;

		if (response[0] == ATT_OP_ERROR_RSP)
			break;

		if (response[0] != ATT_OP_FIND_INFO_RSP || responseLength < 2)
			break;

		uint8 format = response[1];
		uint8* data = response + 2;
		uint16 dataLength = responseLength - 2;
		uint8 pairSize = (format == 1) ? 4 : 20; /* 2+2 or 2+16 */

		while (dataLength >= pairSize) {
			uint16 handle = B_LENDIAN_TO_HOST_INT16(*(uint16*)data);

			if (format == 1) {
				uint16 uuid16 = B_LENDIAN_TO_HOST_INT16(*(uint16*)(data + 2));
				if (uuid16 == GATT_UUID_CCC) {
					_char->ccc_handle = handle;
					TRACE("%s: found CCC descriptor at handle=%#x\n",
						__func__, handle);
					return B_OK;
				}
			}

			searchHandle = handle + 1;
			data += pairSize;
			dataLength -= pairSize;
		}
	}

	return B_OK;
}


status_t
GattClient::ReadCharacteristic(uint16 handle, uint8* _value,
	uint16* _length)
{
	return fChannel->ReadAttribute(handle, _value, _length);
}


status_t
GattClient::WriteCharacteristic(uint16 handle, const uint8* value,
	uint16 length)
{
	return fChannel->WriteAttribute(handle, value, length);
}


status_t
GattClient::WriteCharacteristicNoResponse(uint16 handle, const uint8* value,
	uint16 length)
{
	return fChannel->WriteCommand(handle, value, length);
}


status_t
GattClient::SubscribeNotifications(uint16 cccHandle, bool enable)
{
	return fChannel->EnableNotifications(cccHandle, enable);
}


/* static */ void
GattClient::_ParseUuid(const uint8* data, uint8 length, bt_uuid_t* _uuid)
{
	if (length == 2) {
		_uuid->type = BT_UUID_16;
		_uuid->uuid16 = B_LENDIAN_TO_HOST_INT16(*(uint16*)data);
	} else if (length == 4) {
		_uuid->type = BT_UUID_32;
		_uuid->uuid32 = B_LENDIAN_TO_HOST_INT32(*(uint32*)data);
	} else if (length == 16) {
		_uuid->type = BT_UUID_128;
		memcpy(_uuid->uuid128, data, 16);
	} else {
		_uuid->type = BT_UUID_16;
		_uuid->uuid16 = 0;
	}
}


/* static */ bool
GattClient::_UuidEquals(const bt_uuid_t& a, const bt_uuid_t& b)
{
	if (a.type != b.type)
		return false;

	switch (a.type) {
		case BT_UUID_16:
			return a.uuid16 == b.uuid16;
		case BT_UUID_32:
			return a.uuid32 == b.uuid32;
		case BT_UUID_128:
			return memcmp(a.uuid128, b.uuid128, 16) == 0;
		default:
			return false;
	}
}
